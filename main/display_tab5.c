#include "display.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "hal/axi_icm_ll.h"
#include "hal/dw_gdma_ll.h"
#include "board.h"
#include "panel_tab5_init.h"

static const char *TAG = "display";

// The Tab5 ships with one of three 720x1280 MIPI-DSI panels, told apart by
// the touch controller beside it (as M5GFX's board detection does): the
// ST7123's or the ST7121's own at 0x55 (firmware 3 or 1), or a GT911 beside
// the ILI9881C. Timings and init lists are M5GFX's, as monster-cyberspace's
// and scheff-zello's Tab5 ports use them.
#define DSI_LANES        2
#define DSI_PHY_LDO_CHAN 3
#define DSI_PHY_LDO_MV   2500

#define BL_CHANNEL  LEDC_CHANNEL_0
#define BL_RES      LEDC_TIMER_12_BIT
#define BL_MAX_DUTY ((1u << 12) - 1)

#define BRIGHTNESS_DEFAULT 180
#define DRAW_BUF_LINES     12

#define ST_TOUCH_ADDR     0x55
#define ST_REG_FW_VERSION 0x0000
#define ST_REG_MAX_TOUCHES 0x0009
#define ST_REG_ADV_INFO   0x0010
#define ST_REG_REPORT     0x0014
#define ST_ADV_WITH_COORD 0x08
#define ST_MAX_POINTS     10
#define ST_REPORT_LEN     7
#define GT911_REG_STATUS  0x814E
#define TOUCH_POLL_US     (15 * 1000)

typedef enum { PANEL_ST7123, PANEL_ST7121, PANEL_ILI9881C } panel_kind_t;

typedef struct {
    const char *name;
    uint32_t lane_mbps, dpi_mhz;
    uint16_t hsync, hbp, hfp, vsync, vbp, vfp;
    const tab5_init_list_t *lists;
    size_t list_count;
    bool m5_demo;  // the ST7121's bring-up from M5Stack's Tab5 UserDemo
} panel_timing_t;

static const panel_timing_t s_timings[] = {
    [PANEL_ST7123] = {"ST7123", 1040, 80, 2, 40, 40, 2, 8, 220, kSt7123, sizeof(kSt7123) / sizeof(kSt7123[0]), false},
    [PANEL_ST7121] = {"ST7121", 965, 70, 2, 40, 40, 20, 24, 200, kSt7121, sizeof(kSt7121) / sizeof(kSt7121[0]), true},
    [PANEL_ILI9881C] = {"ILI9881C", 1040, 80, 40, 140, 40, 4, 20, 20, kIli9881c,
                        sizeof(kIli9881c) / sizeof(kIli9881c[0]), false},
};

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static i2c_master_dev_handle_t s_touch;
static bool s_touch_gt911;

// ---- Touch -------------------------------------------------------------------

// Both controllers take a 16-bit big-endian register address.
static esp_err_t touch_read(uint16_t reg, uint8_t *buf, size_t len)
{
    uint8_t a[2] = {reg >> 8, reg & 0xff};
    return i2c_master_transmit_receive(s_touch, a, sizeof(a), buf, len, 50);
}

static esp_err_t touch_add(uint16_t addr)
{
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100000,
    };
    return i2c_master_bus_add_device(board_main_i2c_bus(), &dev, &s_touch);
}

static panel_kind_t panel_detect(void)
{
    i2c_master_bus_handle_t bus = board_main_i2c_bus();
    if (touch_add(ST_TOUCH_ADDR) != ESP_OK) s_touch = NULL;
    // The controller takes a moment to start after board_init() lets it out
    // of reset.
    for (int i = 0; i < 60; i++) {
        uint8_t fw = 0;
        if (s_touch && touch_read(ST_REG_FW_VERSION, &fw, 1) == ESP_OK) {
            if (fw == 1 || fw == 3) {
                ESP_LOGI(TAG, "ST touch, firmware %u", fw);
                return fw == 1 ? PANEL_ST7121 : PANEL_ST7123;
            }
        } else {
            uint16_t gt = i2c_master_probe(bus, 0x14, 20) == ESP_OK ? 0x14
                        : i2c_master_probe(bus, 0x5D, 20) == ESP_OK ? 0x5D : 0;
            if (gt) {
                if (s_touch) i2c_master_bus_rm_device(s_touch);
                s_touch = NULL;
                if (touch_add(gt) == ESP_OK) s_touch_gt911 = true;
                ESP_LOGI(TAG, "GT911 touch at 0x%02x", gt);
                return PANEL_ILI9881C;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGW(TAG, "touch not identified, assuming the ST7123 panel");
    return PANEL_ST7123;
}

static bool touch_poll(int32_t *x, int32_t *y)
{
    if (s_touch_gt911) {
        uint8_t r[6];
        if (touch_read(GT911_REG_STATUS, r, sizeof(r)) != ESP_OK || !(r[0] & 0x80)) return false;
        uint8_t clear[3] = {GT911_REG_STATUS >> 8, GT911_REG_STATUS & 0xff, 0};
        i2c_master_transmit(s_touch, clear, sizeof(clear), 50);
        if ((r[0] & 0x0F) == 0) return false;
        *x = r[2] | (r[3] << 8);
        *y = r[4] | (r[5] << 8);
        return true;
    }
    uint8_t adv = 0, m = 0;
    if (touch_read(ST_REG_ADV_INFO, &adv, 1) != ESP_OK || !(adv & ST_ADV_WITH_COORD)) return false;
    if (touch_read(ST_REG_MAX_TOUCHES, &m, 1) != ESP_OK || !m) return false;
    if (m > ST_MAX_POINTS) m = ST_MAX_POINTS;
    // Every slot is read, or the report stays latched.
    uint8_t r[ST_MAX_POINTS * ST_REPORT_LEN];
    if (touch_read(ST_REG_REPORT, r, m * ST_REPORT_LEN) != ESP_OK) return false;
    for (int i = 0; i < m; i++) {
        const uint8_t *p = r + i * ST_REPORT_LEN;
        if (!(p[0] & 0x80)) continue;
        *x = ((p[0] & 0x3F) << 8) | p[1];
        *y = (p[2] << 8) | p[3];
        return true;
    }
    return false;
}

// Polls at most every TOUCH_POLL_US; between polls the last state stands.
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static int32_t last_x, last_y;
    static bool pressed;
    static int64_t last_poll_us;

    int64_t now = esp_timer_get_time();
    if (now - last_poll_us >= TOUCH_POLL_US) {
        last_poll_us = now;
        int32_t x, y;
        pressed = touch_poll(&x, &y);
        if (pressed) {
            last_x = x < DISPLAY_H_RES ? x : DISPLAY_H_RES - 1;
            last_y = y < DISPLAY_V_RES ? y : DISPLAY_V_RES - 1;
        }
    }
    data->point.x = last_x;
    data->point.y = last_y;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// ---- Panel -------------------------------------------------------------------

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_RES,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    ledc_channel_config_t chan = {
        .gpio_num = BOARD_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_CHANNEL,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,  // dark until the first frame is drawn
    };
    return ledc_channel_config(&chan);
}

void display_set_brightness(uint8_t level)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL, (uint32_t)level * BL_MAX_DUTY / 255);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL);
}

static esp_err_t send_list(const uint8_t *p, uint32_t gap_ms)
{
    while (*p) {
        uint8_t len = *p++;
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, p[0], len > 1 ? p + 1 : NULL, len - 1), TAG,
                            "init cmd 0x%02x", p[0]);
        if (gap_ms) vTaskDelay(pdMS_TO_TICKS(gap_ms));
        p += len;
    }
    return ESP_OK;
}

static esp_err_t panel_init(const panel_timing_t *t)
{
    esp_ldo_channel_handle_t ldo;
    esp_ldo_channel_config_t ldo_cfg = {.chan_id = DSI_PHY_LDO_CHAN, .voltage_mv = DSI_PHY_LDO_MV};
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo), TAG, "dsi phy ldo");

    esp_lcd_dsi_bus_handle_t bus;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = DSI_LANES,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = t->lane_mbps,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &bus), TAG, "dsi bus");

    esp_lcd_dbi_io_config_t io_cfg = {.virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8};
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(bus, &io_cfg, &s_io), TAG, "dbi io");

    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = t->dpi_mhz,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = DISPLAY_H_RES,
            .v_size = DISPLAY_V_RES,
            .hsync_pulse_width = t->hsync,
            .hsync_back_porch = t->hbp,
            .hsync_front_porch = t->hfp,
            .vsync_pulse_width = t->vsync,
            .vsync_back_porch = t->vbp,
            .vsync_front_porch = t->vfp,
        },
        .flags.use_dma2d = true,
    };
    // Scan-out reads the frame buffer from PSRAM; give it top priority on the
    // AXI bus so the DSI bridge's FIFO can't underrun.
    axi_icm_ll_set_dw_gdma_qos_arbiter_prio(DW_GDMA_LL_MASTER_PORT_MEMORY, 0, 15);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_dpi(bus, &dpi_cfg, &s_panel), TAG, "dpi panel");

    // These panels take their vendor sequence before the video stream starts.
    if (t->m5_demo) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, LCD_CMD_SWRESET, NULL, 0), TAG, "swreset");
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    for (size_t i = 0; i < t->list_count; i++) {
        ESP_RETURN_ON_ERROR(send_list(t->lists[i].cmds, t->m5_demo ? 5 : 0), TAG, "init list");
        if (t->lists[i].delay_ms) vTaskDelay(pdMS_TO_TICKS(t->lists[i].delay_ms));
    }
    if (!t->m5_demo) {
        const uint8_t madctl = 0x00, colmod = 0x55;
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, LCD_CMD_MADCTL, &madctl, 1), TAG, "madctl");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, LCD_CMD_COLMOD, &colmod, 1), TAG, "colmod");
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "dpi init");
    return ESP_OK;
}

esp_err_t display_init(void)
{
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");
    const panel_timing_t *t = &s_timings[panel_detect()];
    ESP_RETURN_ON_ERROR(panel_init(t), TAG, "%s panel", t->name);

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 8192;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_io,
        .panel_handle = s_panel,
        .buffer_size = DISPLAY_H_RES * DRAW_BUF_LINES,
        .double_buffer = true,
        .hres = DISPLAY_H_RES,
        .vres = DISPLAY_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi_cfg = {
        .flags.avoid_tearing = false,
    };
    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    ESP_RETURN_ON_FALSE(disp, ESP_FAIL, TAG, "lvgl display");

    // Touch is optional: the UI still draws without it, and the serial
    // console remains a way in.
    if (s_touch) {
        lvgl_port_lock(0);
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, touch_read_cb);
        lv_indev_set_display(indev, disp);
        lv_indev_set_scroll_limit(indev, 24);  // ~300 ppi
        lvgl_port_unlock();
    } else {
        ESP_LOGE(TAG, "touch unavailable");
    }

    // Let the panel scan out a first frame before lighting it.
    if (lvgl_port_lock(0)) {
        lv_refr_now(NULL);
        lvgl_port_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    display_set_brightness(BRIGHTNESS_DEFAULT);
    ESP_LOGI(TAG, "%s %dx%d LCD ready, %u bytes internal RAM free", t->name, DISPLAY_H_RES, DISPLAY_V_RES,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
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
