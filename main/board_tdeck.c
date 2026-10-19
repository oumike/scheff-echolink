#include "board.h"

#include "driver/gpio.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "board";

static i2c_master_bus_handle_t s_i2c;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali;
static adc_channel_t s_battery_channel;

static esp_err_t battery_adc_init(void)
{
    adc_unit_t unit;
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(BOARD_BAT_ADC, &unit, &s_battery_channel), TAG,
                        "battery ADC pin");
    adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = unit};
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc), TAG, "battery ADC unit");
    adc_oneshot_chan_cfg_t channel_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, s_battery_channel, &channel_cfg), TAG,
                        "battery ADC channel");

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = unit,
        .chan = s_battery_channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali);
    if (err != ESP_OK) ESP_LOGW(TAG, "battery ADC calibration unavailable: %s", esp_err_to_name(err));
    return ESP_OK;
}

esp_err_t board_init(void)
{
    gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << BOARD_POWER_ON) | (1ULL << BOARD_SDCARD_CS) |
                        (1ULL << BOARD_TFT_CS) | (1ULL << BOARD_RADIO_CS) |
                        (1ULL << BOARD_TFT_BACKLIGHT),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&outputs), TAG, "output pins");
    gpio_set_level(BOARD_POWER_ON, 1);
    gpio_set_level(BOARD_SDCARD_CS, 1);
    gpio_set_level(BOARD_TFT_CS, 1);
    gpio_set_level(BOARD_RADIO_CS, 1);
    gpio_set_level(BOARD_TFT_BACKLIGHT, 0);

    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "I2C bus");

    spi_bus_config_t spi_cfg = {
        .mosi_io_num = BOARD_SPI_MOSI,
        .miso_io_num = BOARD_SPI_MISO,
        .sclk_io_num = BOARD_SPI_SCLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = 320 * 48 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_SPI_HOST, &spi_cfg, SPI_DMA_CH_AUTO), TAG, "SPI bus");
    battery_adc_init();
    ESP_LOGI(TAG, "T-Deck Plus peripherals powered");
    return ESP_OK;
}

esp_err_t board_audio_power(bool on)
{
    if (on) gpio_set_level(BOARD_POWER_ON, 1);
    return ESP_OK;
}

esp_err_t board_c6_enable(bool on)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_sd_power(bool on)
{
    if (on) gpio_set_level(BOARD_POWER_ON, 1);
    return ESP_OK;
}

esp_err_t board_screen_reset_release(bool released)
{
    return ESP_OK;
}

esp_err_t board_touch_reset_release(bool released)
{
    return ESP_OK;
}

esp_err_t board_battery_percent(uint8_t *percent)
{
    if (!percent) return ESP_ERR_INVALID_ARG;
    if (!s_adc) return ESP_ERR_INVALID_STATE;

    int pin_mv;
    esp_err_t err;
    if (s_adc_cali) {
        err = adc_oneshot_get_calibrated_result(s_adc, s_adc_cali, s_battery_channel, &pin_mv);
    } else {
        int raw;
        err = adc_oneshot_read(s_adc, s_battery_channel, &raw);
        pin_mv = raw * 3100 / 4095;
    }
    if (err != ESP_OK) return err;

    int battery_mv = pin_mv * 2;
    int value = (battery_mv - 3300) * 100 / (4200 - 3300);
    *percent = value < 0 ? 0 : value > 100 ? 100 : value;
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