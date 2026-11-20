// LVGL settings for the CardputerZero build; everything else is LVGL 9.3's
// default (lv_conf_internal.h). The ESP32 builds set theirs in sdkconfig.
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

#define LV_USE_STDLIB_MALLOC  LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING  LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

// LVGL runs on one thread (display_cp0.c), behind display_lock().
#define LV_USE_OS LV_OS_NONE

#define LV_DEF_REFR_PERIOD 16

#define LV_USE_LOG      1
#define LV_LOG_LEVEL    LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF   1

#define LV_USE_LINUX_FBDEV         1
#define LV_LINUX_FBDEV_BSD         0
#define LV_LINUX_FBDEV_RENDER_MODE LV_DISPLAY_RENDER_MODE_PARTIAL
#define LV_LINUX_FBDEV_BUFFER_COUNT 0
#define LV_LINUX_FBDEV_BUFFER_SIZE 60

#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_DEFAULT       &lv_font_montserrat_14

#define LV_USE_THEME_DEFAULT       1
#define LV_THEME_DEFAULT_DARK      1
#define LV_THEME_DEFAULT_GROW      0

// Unused widgets and extras, to keep the binary small.
#define LV_USE_ANIMIMG    0
#define LV_USE_CALENDAR   0
#define LV_USE_CANVAS     0
#define LV_USE_CHART      0
#define LV_USE_IMAGEBUTTON 0
#define LV_USE_LED        0
#define LV_USE_LIST       0
#define LV_USE_MENU       0
#define LV_USE_MSGBOX     0
#define LV_USE_ROLLER     0
#define LV_USE_SCALE      0
#define LV_USE_SPAN       0
#define LV_USE_SPINBOX    0
#define LV_USE_TABLE      0
#define LV_USE_TABVIEW    0
#define LV_USE_TILEVIEW   0
#define LV_USE_WIN        0
#define LV_USE_ARCLABEL   0
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS    0
#endif
