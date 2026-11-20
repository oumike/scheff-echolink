#include "board.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "hal/wdt_hal.h"

// M5Stack Tab5, after scheff-zello's and monster-cyberspace's Tab5 ports and
// M5Stack's BSP.

static const char *TAG = "board";

#define I2C_HZ 100000  // as M5Stack's factory firmware

// PI4IOE5V6408 8-bit I/O expanders. Pin n is bit n.
#define PI4_REG_DIR      0x03  // 1 = output
#define PI4_REG_OUT      0x05
#define PI4_REG_HIZ      0x07
#define PI4_REG_PULL_EN  0x0B
#define PI4_REG_PULL_SEL 0x0D  // 1 = pull-up

#define EXP1_ADDR 0x43
#define EXP2_ADDR 0x44
#define EXP1_SPK_AMP  1
#define EXP1_LCD_RST  4
#define EXP1_TP_RST   5
#define EXP2_C6_POWER 0
#define EXP2_USB_5V   3

// INA226 on the 2S battery pack: bus voltage, 1.25 mV per bit.
#define INA226_ADDR       0x41
#define INA226_REG_BUS_MV 0x02

static const struct {
    uint8_t addr, out, hiz, pull, dir;
} s_exp_init[] = {
    // 0x43: antenna internal, amplifier off, 5 V out on, LCD_RST an input with
    // a pull-up (it must never be driven high), touch and camera out of reset.
    {EXP1_ADDR, 0x74, 0x08, 0x10, 0x67},
    // 0x44: C6 powered, USB-A 5 V on, charge current 1 A, charger on.
    {EXP2_ADDR, 0x89, 0x06, 0x00, 0xB9},
};

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_exp_dev[2], s_ina226;
static uint8_t s_exp_out[2];
static SemaphoreHandle_t s_exp_lock;

// Resting LiPo voltage (mV) against charge, from full down to empty.
static const struct {
    uint16_t mv;
    uint8_t percent;
} s_lipo[] = {
    {4200, 100}, {4100, 90}, {4000, 80}, {3920, 70}, {3850, 60}, {3800, 50},
    {3750, 40},  {3700, 30}, {3650, 20}, {3550, 10}, {3450, 5},  {3300, 0},
};

static uint8_t lipo_percent(int mv)
{
    const size_t n = sizeof(s_lipo) / sizeof(s_lipo[0]);
    if (mv >= s_lipo[0].mv) return 100;
    if (mv <= s_lipo[n - 1].mv) return 0;
    size_t i = 1;
    while (mv < s_lipo[i].mv) i++;
    int hi_mv = s_lipo[i - 1].mv, lo_mv = s_lipo[i].mv;
    int hi_p = s_lipo[i - 1].percent, lo_p = s_lipo[i].percent;
    return lo_p + (mv - lo_mv) * (hi_p - lo_p) / (hi_mv - lo_mv);
}

static esp_err_t pi4_write(int i, uint8_t reg, uint8_t v)
{
    uint8_t buf[2] = {reg, v};
    return i2c_master_transmit(s_exp_dev[i], buf, sizeof(buf), 100);
}

static esp_err_t pi4_set(int i, int pin, bool level)
{
    xSemaphoreTake(s_exp_lock, portMAX_DELAY);
    uint8_t v = level ? (s_exp_out[i] | (1u << pin)) : (s_exp_out[i] & ~(1u << pin));
    esp_err_t err = pi4_write(i, PI4_REG_OUT, v);
    if (err == ESP_OK) s_exp_out[i] = v;
    xSemaphoreGive(s_exp_lock);
    return err;
}

// esp_restart() on the P4 is a CPU reset that leaves the MIPI-DSI host half set
// up, and the panel then stays dark. As in monster-cyberspace, every software
// restart becomes a full system reset through the RTC watchdog.
static void full_restart(void)
{
    wdt_hal_context_t rwdt = RWDT_HAL_CONTEXT_DEFAULT();
    wdt_hal_write_protect_disable(&rwdt);
    wdt_hal_config_stage(&rwdt, WDT_STAGE0, 20000, WDT_STAGE_ACTION_RESET_SYSTEM);
    wdt_hal_enable(&rwdt);
    wdt_hal_write_protect_enable(&rwdt);
    for (;;) {
    }
}

esp_err_t board_init(void)
{
    esp_register_shutdown_handler(full_restart);
    s_exp_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_exp_lock, ESP_ERR_NO_MEM, TAG, "expander lock");

    i2c_master_bus_config_t i2c = {
        .i2c_port = 0,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c, &s_i2c), TAG, "i2c");
    for (int i = 0; i < 2; i++) {
        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = s_exp_init[i].addr,
            .scl_speed_hz = I2C_HZ,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &dev, &s_exp_dev[i]), TAG, "expander add");
    }

    // M5Stack's bring-up (bsp_reset_tp): the panel and touch held in reset for
    // 100 ms with TP_INT pulled high, then 100 ms for the controller to start.
    // Without it the ST7121's touch reports one stuck point.
    gpio_config_t tp_int = {
        .pin_bit_mask = 1ULL << BOARD_TOUCH_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&tp_int);
    const uint8_t resets = (1u << EXP1_LCD_RST) | (1u << EXP1_TP_RST);
    for (int i = 0; i < 2; i++) {
        uint8_t out = s_exp_init[i].out, dir = s_exp_init[i].dir;
        if (i == 0) {
            out &= ~resets;
            dir |= resets;
        }
        // Levels first, then the direction, so no output starts at the wrong one.
        ESP_RETURN_ON_ERROR(pi4_write(i, PI4_REG_OUT, out), TAG, "expander 0x%02x not responding",
                            s_exp_init[i].addr);
        ESP_RETURN_ON_ERROR(pi4_write(i, PI4_REG_HIZ, s_exp_init[i].hiz), TAG, "expander hi-z");
        ESP_RETURN_ON_ERROR(pi4_write(i, PI4_REG_PULL_SEL, s_exp_init[i].pull), TAG, "expander pull");
        ESP_RETURN_ON_ERROR(pi4_write(i, PI4_REG_PULL_EN, s_exp_init[i].pull), TAG, "expander pull");
        ESP_RETURN_ON_ERROR(pi4_write(i, PI4_REG_DIR, dir), TAG, "expander dir");
        s_exp_out[i] = out;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(pi4_write(0, PI4_REG_DIR, s_exp_init[0].dir), TAG, "expander dir");
    ESP_RETURN_ON_ERROR(pi4_write(0, PI4_REG_OUT, s_exp_init[0].out), TAG, "expander out");
    s_exp_out[0] = s_exp_init[0].out;
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_pull_mode(BOARD_TOUCH_INT, GPIO_FLOATING);

    i2c_device_config_t ina = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = INA226_ADDR,
        .scl_speed_hz = I2C_HZ,
    };
    if (i2c_master_bus_add_device(s_i2c, &ina, &s_ina226) != ESP_OK) s_ina226 = NULL;

    ESP_LOGI(TAG, "Tab5 expanders ready");
    return ESP_OK;
}

esp_err_t board_audio_power(bool on)
{
    esp_err_t err = pi4_set(0, EXP1_SPK_AMP, on);
    if (err == ESP_OK && on) vTaskDelay(pdMS_TO_TICKS(20));
    return err;
}

esp_err_t board_c6_enable(bool on)
{
    return pi4_set(1, EXP2_C6_POWER, on);
}

esp_err_t board_sd_power(bool on)
{
    return ESP_OK;
}

esp_err_t board_usb_host_power(bool on)
{
    return pi4_set(1, EXP2_USB_5V, on);
}

esp_err_t board_battery_percent(uint8_t *percent)
{
    if (!percent) return ESP_ERR_INVALID_ARG;
    if (!s_ina226) return ESP_ERR_INVALID_STATE;

    uint8_t reg = INA226_REG_BUS_MV, raw[2];
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_ina226, &reg, 1, raw, sizeof(raw), 100), TAG, "ina226");
    int mv = ((raw[0] << 8) | raw[1]) * 5 / 4;
    if (mv < 4000) return ESP_ERR_NOT_FOUND;  // no pack fitted, running on USB
    // Two cells in series (NP-F550 pack): judge one.
    *percent = lipo_percent(mv / 2);
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
