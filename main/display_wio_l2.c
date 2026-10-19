#include "display.h"

#include <stdlib.h>
#include <sys/cdefs.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "board.h"

static const char *TAG = "display";

#define LCD_PIXEL_CLOCK_HZ 40000000
#define LCD_DRAW_ROWS      40

// Seeed's config asks LovyanGFX for inversion, but LovyanGFX's NV3031B driver
// sends INVOFF regardless, so that is what this panel has been shown with.
// Flip this if colours come out inverted.
#define LCD_INVERT_COLORS false

// NV3031B QSPI framing: a command goes out on one data line as 02 00 <cmd> 00
// followed by its parameters; pixels go out as 32 00 2C 00 followed by RGB565
// on all four lines.
#define LCD_OPCODE_WRITE_CMD   0x02
#define LCD_OPCODE_WRITE_COLOR 0x32
#define LCD_FRAME(opcode, cmd) (((uint32_t)(opcode) << 24) | ((uint32_t)(cmd) << 8))

// The panel is 240x320 portrait. Exchanging rows and columns, with rows
// reversed, gives the 320x240 landscape view Seeed's firmware uses.
#define LCD_MADCTL_LANDSCAPE (LCD_CMD_MY_BIT | LCD_CMD_MV_BIT)

#define GT911_ADDR         0x5d
#define GT911_RESOLUTION   0x8048
#define GT911_PRODUCT_ID   0x8140
#define GT911_STATUS       0x814e
#define GT911_POINT_1      0x8150
#define TOUCH_RELEASE_US   (250 * 1000)

#define LP5814_ENABLE      0x00
#define LP5814_MAX_CURRENT 0x01
#define LP5814_OUT_ENABLE  0x02
#define LP5814_DIM_MODE    0x04
#define LP5814_ENGINE_MODE 0x05
#define LP5814_UPDATE      0x0f
#define LP5814_DC0         0x14
#define LP5814_PWM0        0x18
#define LP5814_UPDATE_KEY  0x55
#define LP5814_CHANNELS    4

#define BACKLIGHT_DEFAULT  160

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int x_gap;
    int y_gap;
    bool swap_xy;
    bool mirror_x;
    bool mirror_y;
} nv3031b_panel_t;

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[8];
    uint16_t delay_ms;
} lcd_init_cmd_t;

// Vendor sequence from LovyanGFX's Panel_NV3031B, which Seeed's firmware uses.
static const lcd_init_cmd_t s_init_cmds[] = {
    {0xfd, 2, {0x06, 0x08}},                                     // unlock factory registers
    {0x60, 1, {0x0c}},                                           // source timing
    {0x61, 2, {0x07, 0x04}},                                     // gate timing
    {0xb4, 1, {0x01}},                                           // 1-dot column inversion
    {0xb1, 3, {0x0f, 0x02, 0x03}},                               // frame rate
    {0xb5, 4, {0x02, 0x02, 0x0a, 0x14}},                         // blanking porch
    {0xb6, 5, {0x44, 0x01, 0x9f, 0x00, 0x02}},                   // display function
    {0xdf, 1, {0x11}},                                           // bias/oscillator trim
    {0x67, 1, {0x21}},                                           // bias current
    {0x68, 4, {0x90, 0x4f, 0x27, 0x21}},                         // VCOM/power
    {0xe1, 2, {0x20, 0x69}},                                     // positive gamma voltage
    {0xe4, 2, {0x69, 0x20}},                                     // negative gamma voltage
    {0xe2, 6, {0x10, 0x12, 0x12, 0x30, 0x39, 0x3f}},             // positive gamma mid-tones
    {0xe5, 6, {0x3f, 0x33, 0x2d, 0x12, 0x12, 0x10}},             // negative gamma mid-tones
    {0xe0, 8, {0x06, 0x06, 0x0b, 0x12, 0x11, 0x11, 0x0e, 0x19}}, // positive gamma curve
    {0xe3, 8, {0x19, 0x13, 0x14, 0x14, 0x14, 0x12, 0x08, 0x05}}, // negative gamma curve
    {0xe6, 2, {0x00, 0xff}},                                     // AVDD/AVCL slope
    {0xe7, 6, {0x01, 0x04, 0x03, 0x03, 0x00, 0x12}},             // source output/EQ timing
    {0xe8, 3, {0x00, 0x70, 0x00}},                               // source driver level
    {0xec, 1, {0x54}},                                           // gate EQ/charge pump
    {0xfd, 2, {0xfa, 0xfc}},                                     // lock factory registers
    {LCD_CMD_COLMOD, 1, {0x55}},                                 // RGB565
    {LCD_CMD_SLPOUT, 0, {0}, 100},
};

static esp_lcd_panel_io_handle_t s_lcd_io;
static esp_lcd_panel_handle_t s_panel;
static i2c_master_dev_handle_t s_touch;
static i2c_master_dev_handle_t s_backlight;

static esp_err_t lcd_write(esp_lcd_panel_io_handle_t io, uint8_t cmd, const void *data, size_t len)
{
    return esp_lcd_panel_io_tx_param(io, LCD_FRAME(LCD_OPCODE_WRITE_CMD, cmd), data, len);
}

static esp_err_t panel_write_madctl(nv3031b_panel_t *lcd)
{
    uint8_t madctl = LCD_MADCTL_LANDSCAPE;
    if (lcd->swap_xy) madctl ^= LCD_CMD_MV_BIT;
    // Which address-order bit flips a screen axis depends on the exchange.
    bool exchanged = madctl & LCD_CMD_MV_BIT;
    if (lcd->mirror_x) madctl ^= exchanged ? LCD_CMD_MY_BIT : LCD_CMD_MX_BIT;
    if (lcd->mirror_y) madctl ^= exchanged ? LCD_CMD_MX_BIT : LCD_CMD_MY_BIT;
    return lcd_write(lcd->io, LCD_CMD_MADCTL, &madctl, 1);
}

static esp_err_t panel_del(esp_lcd_panel_t *panel)
{
    free(__containerof(panel, nv3031b_panel_t, base));
    return ESP_OK;
}

// board_init() pulses the reset line on the I/O expander; this is the
// software equivalent.
static esp_err_t panel_reset(esp_lcd_panel_t *panel)
{
    nv3031b_panel_t *lcd = __containerof(panel, nv3031b_panel_t, base);
    ESP_RETURN_ON_ERROR(lcd_write(lcd->io, LCD_CMD_SWRESET, NULL, 0), TAG, "SWRESET");
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

static esp_err_t panel_invert_color(esp_lcd_panel_t *panel, bool invert)
{
    nv3031b_panel_t *lcd = __containerof(panel, nv3031b_panel_t, base);
    return lcd_write(lcd->io, invert != LCD_INVERT_COLORS ? LCD_CMD_INVON : LCD_CMD_INVOFF, NULL, 0);
}

static esp_err_t panel_init(esp_lcd_panel_t *panel)
{
    nv3031b_panel_t *lcd = __containerof(panel, nv3031b_panel_t, base);
    // A lone NOP frame first flushes the panel's QSPI interface.
    ESP_RETURN_ON_ERROR(lcd_write(lcd->io, LCD_CMD_NOP, NULL, 0), TAG, "NOP");
    for (size_t i = 0; i < sizeof(s_init_cmds) / sizeof(s_init_cmds[0]); i++) {
        const lcd_init_cmd_t *cmd = &s_init_cmds[i];
        ESP_RETURN_ON_ERROR(lcd_write(lcd->io, cmd->cmd, cmd->data, cmd->len), TAG, "init 0x%02x", cmd->cmd);
        if (cmd->delay_ms) vTaskDelay(pdMS_TO_TICKS(cmd->delay_ms));
    }
    ESP_RETURN_ON_ERROR(panel_write_madctl(lcd), TAG, "MADCTL");
    return panel_invert_color(panel, false);
}

static esp_err_t panel_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                   const void *color_data)
{
    nv3031b_panel_t *lcd = __containerof(panel, nv3031b_panel_t, base);
    ESP_RETURN_ON_FALSE(x_start < x_end && y_start < y_end, ESP_ERR_INVALID_ARG, TAG, "empty area");
    size_t bytes = (size_t)(x_end - x_start) * (y_end - y_start) * sizeof(uint16_t);
    int x0 = x_start + lcd->x_gap, x1 = x_end - 1 + lcd->x_gap;
    int y0 = y_start + lcd->y_gap, y1 = y_end - 1 + lcd->y_gap;
    const uint8_t columns[] = {x0 >> 8, x0 & 0xff, x1 >> 8, x1 & 0xff};
    const uint8_t rows[] = {y0 >> 8, y0 & 0xff, y1 >> 8, y1 & 0xff};
    ESP_RETURN_ON_ERROR(lcd_write(lcd->io, LCD_CMD_CASET, columns, sizeof(columns)), TAG, "CASET");
    ESP_RETURN_ON_ERROR(lcd_write(lcd->io, LCD_CMD_RASET, rows, sizeof(rows)), TAG, "RASET");
    ESP_RETURN_ON_ERROR(lcd_write(lcd->io, LCD_CMD_RAMWR, NULL, 0), TAG, "RAMWR");
    return esp_lcd_panel_io_tx_color(lcd->io, LCD_FRAME(LCD_OPCODE_WRITE_COLOR, LCD_CMD_RAMWR), color_data, bytes);
}

static esp_err_t panel_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    nv3031b_panel_t *lcd = __containerof(panel, nv3031b_panel_t, base);
    lcd->mirror_x = mirror_x;
    lcd->mirror_y = mirror_y;
    return panel_write_madctl(lcd);
}

static esp_err_t panel_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    nv3031b_panel_t *lcd = __containerof(panel, nv3031b_panel_t, base);
    lcd->swap_xy = swap_axes;
    return panel_write_madctl(lcd);
}

static esp_err_t panel_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    nv3031b_panel_t *lcd = __containerof(panel, nv3031b_panel_t, base);
    lcd->x_gap = x_gap;
    lcd->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_disp_on_off(esp_lcd_panel_t *panel, bool on)
{
    nv3031b_panel_t *lcd = __containerof(panel, nv3031b_panel_t, base);
    if (!on) return lcd_write(lcd->io, LCD_CMD_DISPOFF, NULL, 0);
    // The NV303x wants a dummy parameter after DISPON.
    const uint8_t dummy = 0;
    return lcd_write(lcd->io, LCD_CMD_DISPON, &dummy, 1);
}

static esp_err_t nv3031b_new_panel(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t *ret_panel)
{
    nv3031b_panel_t *lcd = calloc(1, sizeof(*lcd));
    ESP_RETURN_ON_FALSE(lcd, ESP_ERR_NO_MEM, TAG, "panel alloc");
    lcd->io = io;
    lcd->base.del = panel_del;
    lcd->base.reset = panel_reset;
    lcd->base.init = panel_init;
    lcd->base.draw_bitmap = panel_draw_bitmap;
    lcd->base.invert_color = panel_invert_color;
    lcd->base.mirror = panel_mirror;
    lcd->base.swap_xy = panel_swap_xy;
    lcd->base.set_gap = panel_set_gap;
    lcd->base.disp_on_off = panel_disp_on_off;
    *ret_panel = &lcd->base;
    return ESP_OK;
}

static esp_err_t lp5814_write(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    return i2c_master_transmit(s_backlight, data, sizeof(data), 50);
}

// LP5814 four-channel LED driver; all four channels feed the backlight.
// Starts dark so the panel's power-on contents never show.
static esp_err_t backlight_init(void)
{
    ESP_RETURN_ON_ERROR(i2c_master_probe(board_main_i2c_bus(), BOARD_BACKLIGHT_ADDR, 50), TAG,
                        "no LP5814 at 0x%02x", BOARD_BACKLIGHT_ADDR);
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_BACKLIGHT_ADDR,
        .scl_speed_hz = BOARD_I2C_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(board_main_i2c_bus(), &cfg, &s_backlight), TAG, "LP5814 add");

    esp_err_t err = ESP_OK;
    err |= lp5814_write(LP5814_ENABLE, 0x01);
    err |= lp5814_write(LP5814_MAX_CURRENT, 0x01);  // 51 mA
    err |= lp5814_write(LP5814_OUT_ENABLE, 0x00);
    err |= lp5814_write(LP5814_DIM_MODE, 0x4e);
    err |= lp5814_write(LP5814_ENGINE_MODE, 0xf0);
    for (int i = 0; i < LP5814_CHANNELS; i++) {
        err |= lp5814_write(LP5814_DC0 + i, 200);
        err |= lp5814_write(LP5814_PWM0 + i, 0);
    }
    err |= lp5814_write(LP5814_OUT_ENABLE, 0x0f);
    err |= lp5814_write(LP5814_UPDATE, LP5814_UPDATE_KEY);
    if (err != ESP_OK) {
        i2c_master_bus_rm_device(s_backlight);
        s_backlight = NULL;
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
    return ESP_OK;
}

void display_set_brightness(uint8_t level)
{
    if (!s_backlight) return;
    for (int i = 0; i < LP5814_CHANNELS; i++) lp5814_write(LP5814_PWM0 + i, level);
    lp5814_write(LP5814_UPDATE, LP5814_UPDATE_KEY);
}

static esp_err_t gt_read(uint16_t reg, void *data, size_t size)
{
    uint8_t address[2] = {reg >> 8, reg};
    return i2c_master_transmit_receive(s_touch, address, sizeof(address), data, size, 50);
}

static esp_err_t gt_clear_status(void)
{
    uint8_t data[3] = {GT911_STATUS >> 8, GT911_STATUS & 0xff, 0};
    return i2c_master_transmit(s_touch, data, sizeof(data), 50);
}

// board_init() brings the GT911 out of reset at 0x5D. Its INT line stays
// driven by the expander, so touches are polled.
static esp_err_t touch_init(void)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = GT911_ADDR,
        .scl_speed_hz = BOARD_I2C_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(board_main_i2c_bus(), &cfg, &s_touch), TAG, "GT911 add");
    char product[5] = {0};
    ESP_RETURN_ON_ERROR(gt_read(GT911_PRODUCT_ID, product, 4), TAG, "GT911 probe");
    uint8_t res[4] = {0};
    gt_read(GT911_RESOLUTION, res, sizeof(res));
    ESP_LOGI(TAG, "GT911 ready (product %s, %dx%d)", product, res[0] | (res[1] << 8), res[2] | (res[3] << 8));
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
                // Raw x runs 0-239 across the portrait panel and raw y 0-319
                // along it; map them through the landscape MADCTL.
                int32_t raw_x = point[1] | (point[2] << 8);
                int32_t raw_y = point[3] | (point[4] << 8);
                last_x = DISPLAY_H_RES - 1 - raw_y;
                last_y = raw_x;
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

esp_err_t display_init(void)
{
    esp_err_t err = backlight_init();
    if (err != ESP_OK) ESP_LOGW(TAG, "backlight unavailable: %s", esp_err_to_name(err));

    spi_bus_config_t bus_cfg = {
        .sclk_io_num = BOARD_LCD_SCLK,
        .data0_io_num = BOARD_LCD_IO0,
        .data1_io_num = BOARD_LCD_IO1,
        .data2_io_num = BOARD_LCD_IO2,
        .data3_io_num = BOARD_LCD_IO3,
        .data4_io_num = GPIO_NUM_NC,
        .data5_io_num = GPIO_NUM_NC,
        .data6_io_num = GPIO_NUM_NC,
        .data7_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_H_RES * LCD_DRAW_ROWS * sizeof(uint16_t),
        .flags = SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO), TAG, "QSPI bus");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_LCD_CS,
        .dc_gpio_num = GPIO_NUM_NC,
        .spi_mode = 3,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = true,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(BOARD_LCD_SPI_HOST, &io_cfg, &s_lcd_io), TAG, "LCD IO");
    ESP_RETURN_ON_ERROR(nv3031b_new_panel(s_lcd_io, &s_panel), TAG, "NV3031B");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "LCD init");
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

    display_set_brightness(BACKLIGHT_DEFAULT);
    ESP_LOGI(TAG, "%dx%d Wio Tracker L2 LCD ready", DISPLAY_H_RES, DISPLAY_V_RES);
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
