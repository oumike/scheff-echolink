#include "board.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "board";

// PCA9555 expander bits: port 0 is bits 0-7, port 1 bits 8-15.
#define EXP_WAKE_BUTTON  0   // input
#define EXP_I2C_INT      1   // input
#define EXP_SD_DETECT    2   // input
#define EXP_TOUCH_INT    3   // held low: GT911 comes out of reset at 0x5D
#define EXP_LCD_CS       4
#define EXP_LCD_POWER    5   // LCD and touch supply
#define EXP_LCD_RESET    6
#define EXP_GROVE_POWER  7
#define EXP_TOUCH_RESET  8
#define EXP_GNSS_RESET   9   // active high
#define EXP_USER_LED     10
#define EXP_USB_OTG      11
#define EXP_AUDIO_PA     12
#define EXP_GNSS_POWER   13
#define EXP_SD_POWER     14
#define EXP_BAT_ADC_EN   15

#define EXP_BIT(bit) (1u << (bit))

#define PCA9555_OUTPUT 0x02
#define PCA9555_CONFIG 0x06  // 1 = input

#define ADS1115_CONVERSION 0x00
#define ADS1115_CONFIG     0x01
// AIN0 to GND, +/-4.096 V range, continuous conversion, 8 SPS, comparator off.
#define ADS1115_BATTERY_CONFIG 0x4203

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_expander;
static i2c_master_dev_handle_t s_battery_adc;
static SemaphoreHandle_t s_expander_lock;
static uint16_t s_exp_out = 0xffff;
static uint16_t s_exp_config = 0xffff;

static i2c_master_dev_handle_t add_device(uint8_t address)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = BOARD_I2C_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(s_i2c, &cfg, &dev) != ESP_OK) return NULL;
    return dev;
}

// Writes the output latches before the direction, so a pin turned into an
// output starts at the staged level.
static esp_err_t expander_write(void)
{
    uint8_t out[3] = {PCA9555_OUTPUT, s_exp_out & 0xff, s_exp_out >> 8};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_expander, out, sizeof(out), 50), TAG, "expander output");
    uint8_t config[3] = {PCA9555_CONFIG, s_exp_config & 0xff, s_exp_config >> 8};
    return i2c_master_transmit(s_expander, config, sizeof(config), 50);
}

static void expander_stage(int bit, bool high)
{
    if (high) {
        s_exp_out |= EXP_BIT(bit);
    } else {
        s_exp_out &= ~EXP_BIT(bit);
    }
    s_exp_config &= ~EXP_BIT(bit);
}

static esp_err_t expander_set(int bit, bool high)
{
    if (!s_expander) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_expander_lock, portMAX_DELAY);
    uint16_t out = s_exp_out;
    uint16_t config = s_exp_config;
    expander_stage(bit, high);
    esp_err_t err = expander_write();
    if (err != ESP_OK) {
        s_exp_out = out;
        s_exp_config = config;
    }
    xSemaphoreGive(s_expander_lock);
    return err;
}

// Power and reset sequence from Seeed's Meshtastic variant: LCD and touch
// powered, LCD reset pulsed, then the touch controller released from reset
// while its INT line is low.
static esp_err_t expander_init(void)
{
    s_expander_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_expander_lock, ESP_ERR_NO_MEM, TAG, "expander lock");
    s_expander = add_device(BOARD_EXPANDER_ADDR);
    ESP_RETURN_ON_FALSE(s_expander, ESP_FAIL, TAG, "expander add");

    // Inputs keep config 1. GNSS, Grove, USB OTG power, the amp and the SD
    // card stay off until something needs them.
    expander_stage(EXP_TOUCH_INT, false);
    expander_stage(EXP_TOUCH_RESET, false);
    expander_stage(EXP_LCD_POWER, true);
    expander_stage(EXP_LCD_RESET, true);
    expander_stage(EXP_GROVE_POWER, false);
    expander_stage(EXP_GNSS_RESET, false);
    expander_stage(EXP_GNSS_POWER, false);
    expander_stage(EXP_USER_LED, false);
    expander_stage(EXP_USB_OTG, false);
    expander_stage(EXP_AUDIO_PA, false);
    expander_stage(EXP_SD_POWER, false);
    expander_stage(EXP_BAT_ADC_EN, true);
    ESP_RETURN_ON_ERROR(expander_write(), TAG, "expander not responding at 0x%02x", BOARD_EXPANDER_ADDR);

    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(expander_set(EXP_LCD_RESET, false), TAG, "LCD reset");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(expander_set(EXP_LCD_RESET, true), TAG, "LCD reset release");
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_RETURN_ON_ERROR(expander_set(EXP_LCD_CS, true), TAG, "LCD CS");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(expander_set(EXP_TOUCH_RESET, true), TAG, "touch reset release");
    vTaskDelay(pdMS_TO_TICKS(60));
    return ESP_OK;
}

static esp_err_t battery_adc_init(void)
{
    s_battery_adc = add_device(BOARD_BAT_ADC_ADDR);
    ESP_RETURN_ON_FALSE(s_battery_adc, ESP_FAIL, TAG, "ADS1115 add");
    uint8_t config[3] = {ADS1115_CONFIG, ADS1115_BATTERY_CONFIG >> 8, ADS1115_BATTERY_CONFIG & 0xff};
    esp_err_t err = i2c_master_transmit(s_battery_adc, config, sizeof(config), 50);
    if (err != ESP_OK) {
        i2c_master_bus_rm_device(s_battery_adc);
        s_battery_adc = NULL;
    }
    return err;
}

esp_err_t board_init(void)
{
    // Keep the SX1262 deselected; nothing here drives the LoRa radio.
    gpio_set_level(BOARD_RADIO_CS, 1);
    gpio_config_t radio_cs = {
        .pin_bit_mask = 1ULL << BOARD_RADIO_CS,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&radio_cs), TAG, "radio CS");

    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "I2C bus");
    ESP_RETURN_ON_ERROR(expander_init(), TAG, "I/O expander");

    esp_err_t err = battery_adc_init();
    if (err != ESP_OK) ESP_LOGW(TAG, "battery ADC unavailable: %s", esp_err_to_name(err));
    ESP_LOGI(TAG, "Wio Tracker L2 peripherals powered");
    return ESP_OK;
}

esp_err_t board_audio_power(bool on)
{
    return expander_set(EXP_AUDIO_PA, on);
}

esp_err_t board_c6_enable(bool on)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_sd_power(bool on)
{
    return expander_set(EXP_SD_POWER, on);
}

esp_err_t board_screen_reset_release(bool released)
{
    return expander_set(EXP_LCD_RESET, released);
}

esp_err_t board_touch_reset_release(bool released)
{
    return expander_set(EXP_TOUCH_RESET, released);
}

// Seeed's open-circuit voltage curve for the L2's cell, 100% down to 0% in
// 10% steps.
static uint8_t battery_percent_from_mv(int mv)
{
    static const int ocv_mv[] = {4180, 4040, 3864, 3800, 3745, 3710, 3687, 3663, 3623, 3482, 3300};
    const int steps = sizeof(ocv_mv) / sizeof(ocv_mv[0]);
    if (mv >= ocv_mv[0]) return 100;
    for (int i = 1; i < steps; i++) {
        if (mv >= ocv_mv[i]) {
            return 100 - 10 * i + 10 * (mv - ocv_mv[i]) / (ocv_mv[i - 1] - ocv_mv[i]);
        }
    }
    return 0;
}

esp_err_t board_battery_percent(uint8_t *percent)
{
    if (!percent) return ESP_ERR_INVALID_ARG;
    if (!s_battery_adc) return ESP_ERR_INVALID_STATE;

    uint8_t reg = ADS1115_CONVERSION;
    uint8_t data[2];
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_battery_adc, &reg, 1, data, sizeof(data), 50), TAG,
                        "battery read");
    int16_t raw = (int16_t)((data[0] << 8) | data[1]);
    // 0.125 mV per count at +/-4.096 V, behind a 1:2 divider.
    int battery_mv = raw > 0 ? raw / 4 : 0;
    *percent = battery_percent_from_mv(battery_mv);
    return ESP_OK;
}

i2c_master_bus_handle_t board_main_i2c_bus(void)
{
    return s_i2c;
}

i2c_master_bus_handle_t board_codec_i2c_bus(void)
{
    return s_i2c;
}
