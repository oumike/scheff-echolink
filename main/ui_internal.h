#pragma once

// Shared pieces of the touch UI. Everything here must be called with the LVGL
// lock held: from LVGL callbacks (which already hold it), or between
// display_lock()/display_unlock().

#include <stdbool.h>
#include "sdkconfig.h"
#include "lvgl.h"
#include "display.h"
#include "server_list.h"

#if CONFIG_EL_BOARD_CARDPUTER_ZERO
// 320x170 with a full keyboard: arrows/Tab move the focus, Enter presses,
// Esc goes back (linux/src/keys_cp0.c), so no on-screen keyboard.
#define UI_COMPACT          1
#define UI_HW_KEYBOARD      1
#define UI_PAD              4
#define UI_GAP              3
#define UI_BTN_H            26
#define UI_FIELD_H          26
#define UI_RADIUS           4
#define UI_SAFE_TOP         2
#define UI_SAFE_BOTTOM      0
#define UI_FONT_SMALL       (&lv_font_montserrat_12)
#define UI_FONT             (&lv_font_montserrat_14)
#define UI_FONT_TITLE       (&lv_font_montserrat_16)
#define UI_STATUS_BAR_W     72
#define UI_STATUS_BAR_H     20
#define UI_SPINNER_SIZE     32
#define UI_ICON_BUTTON_SIZE 26
#define UI_EDIT_BUTTON_H    24
#define UI_CLEAR_BUTTON_W   30
#define UI_CHAT_TALK_H      40
#define UI_SLIDER_H         12
#define UI_VOLUME_VALUE_W   40
#define UI_COMPACT_LIST_H   86
#define UI_LETTER_W         64
#define UI_COMPACT_INFO_H   36
#elif CONFIG_IDF_TARGET_ESP32S3
#define UI_COMPACT          1
#define UI_PAD              6
#define UI_GAP              4
#define UI_BTN_H            36
#define UI_FIELD_H          34
#define UI_RADIUS           6
#define UI_SAFE_TOP         4
#define UI_SAFE_BOTTOM      4
#define UI_FONT_SMALL       (&lv_font_montserrat_14)
#define UI_FONT             (&lv_font_montserrat_16)
#define UI_FONT_TITLE       (&lv_font_montserrat_20)
#define UI_STATUS_BAR_W     92
#define UI_STATUS_BAR_H     28
#define UI_SPINNER_SIZE     44
#define UI_ICON_BUTTON_SIZE 36
#define UI_EDIT_BUTTON_H    30
#define UI_CLEAR_BUTTON_W   38
#define UI_CHAT_TALK_H      72
#define UI_SLIDER_H         16
#define UI_VOLUME_VALUE_W   44
#define UI_COMPACT_LIST_H   104
#define UI_COMPACT_INFO_H   48
#else
#define UI_COMPACT          0
#define UI_PAD              16
#define UI_GAP              14
#define UI_BTN_H            96
#define UI_FIELD_H          72
#define UI_RADIUS           14
#if CONFIG_EL_BOARD_TAB5
// The Tab5's LCD has square corners: only a small margin.
#define UI_SAFE_TOP    16
#define UI_SAFE_BOTTOM 16
#else
// The AMOLED's corners are curved: keep content this far from the top and
// bottom edges. Every screen and full-screen form gets it from
// ui_layout_create(); anything placed outside that layout must add it itself.
#define UI_SAFE_TOP    72
#define UI_SAFE_BOTTOM 32
#endif

#define UI_FONT_SMALL (&lv_font_montserrat_20)
#define UI_FONT       (&lv_font_montserrat_28)
#define UI_FONT_TITLE (&lv_font_montserrat_36)
#define UI_STATUS_BAR_W     128
#define UI_STATUS_BAR_H     48
#define UI_SPINNER_SIZE     96
#define UI_ICON_BUTTON_SIZE 80
#define UI_EDIT_BUTTON_H    56
#define UI_CLEAR_BUTTON_W   80
#define UI_CHAT_TALK_H      210
#define UI_SLIDER_H         28
#define UI_VOLUME_VALUE_W   72
#endif

#ifndef UI_HW_KEYBOARD
#define UI_HW_KEYBOARD 0
#endif

#define UI_COLOR_BG      lv_color_hex(0x000000)
#define UI_COLOR_PANEL   lv_color_hex(0x1a1d23)
#define UI_COLOR_MUTED   lv_color_hex(0x9aa3ad)
#define UI_COLOR_ACCENT  lv_color_hex(0x2f7df6)
#define UI_COLOR_GO      lv_color_hex(0x1f9d55)
#define UI_COLOR_GO_LIT  lv_color_hex(0x35d07f)
#define UI_COLOR_STOP    lv_color_hex(0xd23c3c)
#define UI_COLOR_WARN    lv_color_hex(0xe0a526)

// ---- screens ----
// First-boot setup, or manual recovery from Home (with a Back action).
void ui_wifi_show(void);
void ui_home_show(void);
// True only on Home, without an active server edit form.
bool ui_home_is_active(void);
// Redraws the callsign header after the account changed.
void ui_home_refresh_account(void);
// Account form: full screen during onboarding, an overlay when editing.
void ui_account_onboard(void);
void ui_account_edit(void);
bool ui_account_is_open(void);
// Deletes the Wi-Fi setup screens / the account form once onboarding moves on.
void ui_wifi_destroy(void);
void ui_account_destroy(void);
// Fills `label` with how to reach the web config right now (setup hotspot
// and/or the device's IP); empty when neither is up.
void ui_web_hint_update(lv_obj_t *label);
void ui_web_access_show(void);
void ui_chat_connect(const server_entry_t *entry);
// The shared UI timer observes all QSO sources and updates Chat controls.
void ui_chat_update(void);

// ---- building blocks ----

// A screen laid out as a scrolling content column above an on-screen keyboard
// that appears while a text field has focus. Returns the content column.
lv_obj_t *ui_screen_create(lv_obj_t **out_screen, lv_obj_t **out_keyboard);
// The same layout as a full-size child of `parent` (e.g. lv_layer_top() for a
// full-screen form). Delete *out_root to close it.
lv_obj_t *ui_layout_create(lv_obj_t *parent, lv_obj_t **out_root, lv_obj_t **out_keyboard);
lv_obj_t *ui_title(lv_obj_t *parent, const char *text);
lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color);
lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_color_t color, lv_event_cb_t cb, void *user);
// Fits a one-line text area to UI_FIELD_H on the CardputerZero.
void ui_textarea_compact(lv_obj_t *ta);
// Labelled one-line text field; shows `kb` while focused.
lv_obj_t *ui_field(lv_obj_t *parent, const char *label, const char *placeholder, lv_obj_t *kb);
void ui_field_attach_keyboard(lv_obj_t *ta, lv_obj_t *kb);

// ---- temporary status modal (one at a time, on the top layer) ----
void ui_modal_busy(const char *text);
// Replaces the modal with `text`. With `ok`, adds an OK button that closes it
// and calls `on_close`; without, the caller closes it (ui_modal_close_after).
void ui_modal_message(const char *text, lv_color_t color, bool ok, void (*on_close)(void));
void ui_modal_close(void);
bool ui_modal_is_open(void);
// A choice: `title`, one stacked button per item, then Cancel. Tapping an item
// closes the modal and calls `cb` with its index; Cancel just closes it.
void ui_modal_menu(const char *title, const char *const *items, const lv_color_t *colors, int count,
                   void (*cb)(int index));
// Closes the modal after `ms`, then calls `then` (may be NULL).
void ui_modal_close_after(uint32_t ms, void (*then)(void));

// Runs `fn(arg)` on a short-lived worker task, for blocking work (scan, join,
// connect) that must not stall LVGL. `fn` takes display_lock() itself before
// touching the UI. Returns false if launch fails; the caller still owns `arg`.
bool ui_run_async(const char *name, void (*fn)(void *), void *arg);
