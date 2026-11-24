#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_err.h"

#if CONFIG_EL_BOARD_CARDPUTER_ZERO
// ST7789V 1.9" LCD in landscape, as the Linux framebuffer shows it, or a
// desktop window of any size (linux/src/display_cp0.c).
#define DISPLAY_LCD_H_RES 320
#define DISPLAY_LCD_V_RES 170
int32_t display_h_res(void);
int32_t display_v_res(void);
bool display_windowed(void);
#define DISPLAY_H_RES display_h_res()
#define DISPLAY_V_RES display_v_res()
#elif CONFIG_IDF_TARGET_ESP32S3
#define DISPLAY_H_RES 320
#define DISPLAY_V_RES 240
#elif CONFIG_EL_BOARD_TAB5
// 5" LCD (720x1280 portrait, MIPI-DSI): ST7123, ST7121 or ILI9881C.
#define DISPLAY_H_RES 720
#define DISPLAY_V_RES 1280
#else
// RM69A10 4.1" AMOLED (568x1232 portrait, MIPI-DSI).
#define DISPLAY_H_RES 568
#define DISPLAY_V_RES 1232
#endif

esp_err_t display_init(void);

// LVGL is not thread safe: hold this around any lv_* call made outside LVGL's
// own callbacks (which already run with it held).
bool display_lock(void);
void display_unlock(void);

// Panel brightness, normalized to 0-255.
void display_set_brightness(uint8_t level);
