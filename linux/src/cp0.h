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
// Window mode: whether the window has the keyboard (the built-in one is
// grabbed then), and the desktop's keys: a press and release of an evdev
// KEY_*, the talk key held or let go, and a typed character.
void cp0_keys_set_focus(bool focused);
void cp0_keys_tap(uint16_t code, bool shift);
void cp0_keys_space(bool down);
void cp0_keys_char(char c);

// Whether the app runs in a desktop window rather than on the LCD; chosen
// once, before display_init().
bool cp0_windowed(void);
void cp0_choose_window(bool window);

// The desktop window (sdl_window.c). Its video runs on the main thread;
// LVGL's thread hands it frames and takes the pointer, wheel and new sizes.
bool sdlwin_open(const char *title);
void sdlwin_run(bool (*quit)(void));
void sdlwin_close(const char *screenshot_path);
void sdlwin_size(int *w, int *h);
bool sdlwin_take_resize(int *w, int *h);
void sdlwin_push(const uint16_t *frame, int w, int h, int y0, int y1);
bool sdlwin_pointer(int16_t *x, int16_t *y);
// Wheel movement since the last call, in hundredths of a notch: y positive
// towards the top, x towards the left.
void sdlwin_take_wheel(int *x100, int *y100);
typedef enum { SDLWIN_CURSOR_ARROW, SDLWIN_CURSOR_HAND, SDLWIN_CURSOR_TEXT } sdlwin_cursor_t;
void sdlwin_set_cursor(sdlwin_cursor_t cursor);

uint32_t cp0_millis(void);
