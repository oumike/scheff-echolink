#include "display.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "board.h"

static const char *TAG = "display";

#define LCD_PIXEL_CLOCK_HZ 40000000
#define LCD_DRAW_ROWS      40
#define GT911_ADDR         0x5d
#define GT911_PRODUCT_ID   0x8140
#define GT911_STATUS       0x814e
#define GT911_POINT_1      0x8150
#define TOUCH_RELEASE_US   (250 * 1000)

static esp_lcd_panel_io_handle_t s_lcd_io;
static esp_lcd_panel_handle_t s_panel;
static i2c_master_dev_handle_t s_touch;
static uint8_t s_backlight_level;

static esp_err_t gt_read(uint16_t reg, void *data, size_t size)
{
    uint8_t address[2] = {reg >> 8, reg};
    return i2c_master_transmit_receive(s_touch, address, sizeof(address), data, size, 50);
}

static esp_err_t gt_clear_status(void)
{
    uint8_t data[3] = {GT911_STATUS >> 8, GT911_STATUS, 0};
    return i2c_master_transmit(s_touch, data, sizeof(data), 50);
}

static esp_err_t touch_init(void)
{
    gpio_config_t interrupt = {
        .pin_bit_mask = 1ULL << BOARD_TOUCH_INT,
        .mode = GPIO_MODE_INPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&interrupt), TAG, "touch interrupt");
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = GT911_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(board_main_i2c_bus(), &cfg, &s_touch), TAG, "GT911 add");
    char product[5] = {0};
    ESP_RETURN_ON_ERROR(gt_read(GT911_PRODUCT_ID, product, 4), TAG, "GT911 probe");
    ESP_LOGI(TAG, "GT911 ready (product %s)", product);
    return ESP_OK;
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static bool pressed;
    static int32_t last_x, last_y;
    static int64_t last_report_us;

    uint8_t status;
    if (gt_read(GT911_STATUS, &status, 1) == ESP_OK && (status & 0x80)) {
        uint8_t count = status & 0x0f;
        if (count > 0 && count <= 5) {
            uint8_t point[8];
            if (gt_read(GT911_POINT_1, point, sizeof(point)) == ESP_OK) {
                int32_t raw_x = point[1] | (point[2] << 8);
                int32_t raw_y = point[3] | (point[4] << 8);
                // Official T-Deck transform: swap XY, then mirror Y.
                last_x = raw_y;
                last_y = DISPLAY_V_RES - 1 - raw_x;
                if (last_x < 0) last_x = 0;
                if (last_x >= DISPLAY_H_RES) last_x = DISPLAY_H_RES - 1;
                if (last_y < 0) last_y = 0;
                if (last_y >= DISPLAY_V_RES) last_y = DISPLAY_V_RES - 1;
                pressed = true;
                last_report_us = esp_timer_get_time();
            }
        } else {
            pressed = false;
        }
        gt_clear_status();
    } else if (pressed && esp_timer_get_time() - last_report_us > TOUCH_RELEASE_US) {
        pressed = false;
    }

    data->point.x = last_x;
    data->point.y = last_y;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void display_set_brightness(uint8_t level)
{
    uint8_t target = level ? 1 + (uint16_t)level * 15 / 255 : 0;
    if (target == s_backlight_level) return;
    if (!target) {
        gpio_set_level(BOARD_TFT_BACKLIGHT, 0);
        vTaskDelay(pdMS_TO_TICKS(3));
        s_backlight_level = 0;
        return;
    }
    if (!s_backlight_level) {
        gpio_set_level(BOARD_TFT_BACKLIGHT, 1);
        s_backlight_level = 16;
        esp_rom_delay_us(30);
    }
    int pulses = (16 + (16 - target) - (16 - s_backlight_level)) % 16;
    for (int i = 0; i < pulses; i++) {
        gpio_set_level(BOARD_TFT_BACKLIGHT, 0);
        gpio_set_level(BOARD_TFT_BACKLIGHT, 1);
    }
    s_backlight_level = target;
}

esp_err_t display_init(void)
{
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_TFT_CS,
        .dc_gpio_num = BOARD_TFT_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(BOARD_SPI_HOST, &io_cfg, &s_lcd_io), TAG, "LCD IO");
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_lcd_io, &panel_cfg, &s_panel), TAG, "ST7789");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "LCD reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "LCD init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "LCD invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, true), TAG, "LCD rotate");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, false), TAG, "LCD mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "LCD on");

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 6144;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "LVGL port");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = s_lcd_io,
        .panel_handle = s_panel,
        .buffer_size = DISPLAY_H_RES * LCD_DRAW_ROWS,
        .double_buffer = true,
        .hres = DISPLAY_H_RES,
        .vres = DISPLAY_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,
        },
    };
    lv_display_t *display = lvgl_port_add_disp(&display_cfg);
    ESP_RETURN_ON_FALSE(display, ESP_FAIL, TAG, "LVGL display");

    esp_err_t touch_err = touch_init();
    if (touch_err == ESP_OK) {
        lvgl_port_lock(0);
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, touch_read_cb);
        lv_indev_set_display(indev, display);
        lv_indev_set_scroll_limit(indev, 12);
        lvgl_port_unlock();
    } else {
        ESP_LOGW(TAG, "touch unavailable: %s", esp_err_to_name(touch_err));
    }

    display_set_brightness(192);
    ESP_LOGI(TAG, "%dx%d T-Deck LCD ready", DISPLAY_H_RES, DISPLAY_V_RES);
    return ESP_OK;
}

bool display_lock(void)
{
    return lvgl_port_lock(0);
}

void display_unlock(void)
{
    lvgl_port_unlock();
}