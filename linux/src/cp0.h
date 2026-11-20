#pragma once

// Pieces linux/src's files share.

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

// Set by SIGINT/SIGTERM (the launcher's long Esc) or our own long Esc;
// main() then exits.
void cp0_request_quit(void);

// Keyboard: the evdev reader thread, and the LVGL keypad it feeds.
void cp0_keys_start(lv_display_t *disp);

uint32_t cp0_millis(void);
