"""Real LVGL regressions, run by CTest against the CardputerZero build."""

import os
import unittest

from host_harness import COMMON, ROOT, function, header, implementation, run_c


def controls_program():
    return COMMON + r"""
#include <stdarg.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include "lvgl.h"
#include "src/themes/lv_theme_private.h"
static uint32_t now_ms = 4000;
static unsigned quits, reads, closes;
static bool transmitting, present = true, sync_loss;
static jmp_buf reader_done;
static lv_indev_t *keypad;
static void drain_keys(void);
uint32_t cp0_millis(void) { return now_ms; }
bool cp0_windowed(void) { return false; }
void cp0_request_quit(void) { quits++; }
void el_qso_set_ptt(bool on) { transmitting = on; }
static int keyboard_open(const char *path, int flags, ...) {
    return present && !strcmp(path, "/dev/input/event0") ? 7 : -1;
}
static int keyboard_close(int fd) { assert(fd == 7); closes++; return 0; }
static int keyboard_ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    memset(arg, 0, _IOC_SIZE(request));
    if (_IOC_NR(request) == 0x20 + EV_KEY) {
        ((unsigned long *)arg)[KEY_A / (8 * sizeof(long))] |= 1UL << (KEY_A % (8 * sizeof(long)));
    }
    return 0;
}
static int keyboard_poll(struct pollfd *fds, nfds_t count, int timeout) {
    assert(count == 1 && fds[0].fd == 7);
    if (reads == 2) now_ms += 4000;
    drain_keys();
    if (reads == 1) assert(transmitting);
    if (reads == 2) {
        assert(sync_loss && !transmitting);
        longjmp(reader_done, 1);
    }
    fds[0].revents = POLLIN;
    return 1;
}
static ssize_t keyboard_read(int fd, void *out, size_t size) {
    assert(fd == 7);
    if (reads++ == 0) {
        struct input_event keys[] = {
            {.type = EV_KEY, .code = KEY_ENTER, .value = 1},
            {.type = EV_KEY, .code = KEY_LEFTSHIFT, .value = 1},
            {.type = EV_KEY, .code = KEY_SPACE, .value = 1},
            {.type = EV_KEY, .code = KEY_ESC, .value = 1},
        };
        assert(size >= sizeof(keys));
        memcpy(out, keys, sizeof(keys));
        return sizeof(keys);
    }
    if (sync_loss) {
        struct input_event dropped[] = {
            {.type = EV_SYN, .code = SYN_DROPPED},
            {.type = EV_KEY, .code = KEY_SPACE, .value = 1},
            {.type = EV_SYN, .code = SYN_REPORT},
        };
        assert(size >= sizeof(dropped));
        memcpy(out, dropped, sizeof(dropped));
        return sizeof(dropped);
    }
    present = false;
    errno = ENODEV;
    return -1;
}
static unsigned keyboard_sleep(unsigned seconds) { longjmp(reader_done, 1); }
static FILE *keyboard_fopen(const char *path, const char *mode) { return NULL; }
#define open keyboard_open
#define close keyboard_close
#define ioctl keyboard_ioctl
#define poll keyboard_poll
#define read keyboard_read
#define sleep keyboard_sleep
#define fopen keyboard_fopen
""" + implementation("linux/src/keys_cp0.c") + r"""
#undef open
#undef close
#undef ioctl
#undef poll
#undef read
#undef sleep
#undef fopen
static void drain_keys(void) { lv_indev_read(keypad); }
static void setup(void) {
    lv_init();
    lv_display_t *display = lv_display_create(320, 240);
    s_group = lv_group_create();
    keypad = lv_indev_create();
    lv_indev_set_type(keypad, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_display(keypad, display);
    lv_indev_set_group(keypad, s_group);
    lv_indev_set_read_cb(keypad, read_cb);
}
static lv_obj_t *button(lv_obj_t *parent, const char *text) {
    lv_obj_t *obj = lv_button_create(parent);
    lv_label_set_text(lv_label_create(obj), text);
    return obj;
}
static void key(uint16_t code, bool down) {
    raw_push((raw_key_t){.code = code, .value = down, .ms = now_ms});
    drain_keys();
}
"""


@unittest.skipUnless(os.environ.get("EL_LVGL_LIBRARY"), "run with the CardputerZero build's CTest")
class LinuxControlsTests(unittest.TestCase):
    def run_controls(self, body):
        run_c(controls_program() + body, extra=(
            "-DLV_CONF_INCLUDE_SIMPLE",
            "-I", os.environ["EL_LVGL_INCLUDE"],
            "-I", ROOT / "linux",
            os.environ["EL_LVGL_LIBRARY"], "-lm", "-pthread",
        ))

    def test_back_uses_the_current_layer_after_opening_and_deleting_modals(self):
        self.run_controls(r"""
static unsigned home_back;
static void close_layer(lv_event_t *event) { lv_obj_delete(lv_event_get_user_data(event)); }
static void back_home(lv_event_t *event) { home_back++; }
int main(void) {
    setup();
    lv_obj_t *home = button(lv_screen_active(), "Back");
    lv_obj_add_event_cb(home, back_home, LV_EVENT_CLICKED, NULL);
    lv_obj_t *form = lv_obj_create(lv_layer_top());
    lv_obj_set_size(form, 320, 240);
    lv_obj_t *cancel = button(form, "Cancel");
    lv_obj_add_event_cb(cancel, close_layer, LV_EVENT_CLICKED, form);
    lv_obj_update_layout(lv_layer_top());
    refresh_group();
    lv_obj_t *modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(modal, 320, 240);
    lv_obj_t *ok = button(modal, "OK");
    lv_obj_add_event_cb(ok, close_layer, LV_EVENT_CLICKED, modal);
    lv_obj_update_layout(lv_layer_top());
    back_async(NULL);
    assert(!lv_obj_is_valid(modal) && lv_obj_is_valid(form));
    back_async(NULL);
    assert(!lv_obj_is_valid(form));
    back_async(NULL);
    assert(home_back == 1);
    lv_deinit();
}
""")

    def test_device_removal_or_sync_loss_releases_ptt_modifiers_and_escape(self):
        for sync_loss in (False, True):
            with self.subTest(sync_loss=sync_loss):
                self.run_controls(r"""
int main(void) {
    setup();
    button(lv_screen_active(), "Talk");
""" + f"sync_loss = {int(sync_loss)};\n" + r"""
    if (!setjmp(reader_done)) reader(NULL);
    now_ms += 4000;
    drain_keys();
    assert(!transmitting && !s_ptt && !s_shift_l && !s_esc && !quits);
    assert(s_cur_state == LV_INDEV_STATE_RELEASED);
    for (size_t i = 0; i <= KEY_MAX; i++) assert(!s_held[i]);
    assert(closes == (sync_loss ? 0 : 1));
    lv_deinit();
}
""")

    def test_queue_overflow_cannot_leave_ptt_pressed(self):
        self.run_controls(r"""
int main(void) {
    setup();
    button(lv_screen_active(), "Talk");
    key(KEY_SPACE, true);
    assert(transmitting);
    for (unsigned i = 0; i < 129; i++) raw_push((raw_key_t){.code = KEY_RIGHT, .value = 1, .ms = now_ms});
    drain_keys();
    assert(!transmitting && !s_ptt && s_cur_state == LV_INDEV_STATE_RELEASED);
    lv_deinit();
}
""")

    def test_escape_holds_for_three_seconds_but_queued_release_takes_priority(self):
        self.run_controls(r"""
int main(void) {
    setup();
    button(lv_screen_active(), "Back");
    key(KEY_ESC, true);
    now_ms += 2999;
    drain_keys();
    assert(!quits);
    now_ms++;
    drain_keys();
    assert(quits == 1);
    key(KEY_ESC, false);
    key(KEY_ESC, true);
    raw_push((raw_key_t){.code = KEY_ESC, .value = 0, .ms = now_ms + 100});
    now_ms += 4000;
    drain_keys();
    assert(quits == 1 && !s_esc);
    assert(lv_async_call_cancel(back_async, NULL) == LV_RESULT_OK);
    lv_deinit();
}
""")

    def test_arrow_key_volume_changes_are_persisted_without_a_release_event(self):
        self.run_controls(header("main/settings.h") + r"""
#define UI_HW_KEYBOARD 1
#define UI_COLOR_STOP lv_color_hex(0xff0000)
settings_t g_settings;
static unsigned saves, errors;
static int audio_volume;
static bool fail_save;
static lv_obj_t *s_volume_value;
esp_err_t settings_save(void) { saves++; return fail_save ? ESP_FAIL : ESP_OK; }
void audio_set_volume(int value) { audio_volume = value; }
static void ui_modal_message(const char *text, lv_color_t color, bool ok, void (*cb)(void)) { errors++; }
""" + function("main/ui_chat.c", "static void volume_event(") + r"""
int main(void) {
    setup();
    lv_obj_t *slider = lv_slider_create(lv_screen_active());
    s_volume_value = lv_label_create(lv_screen_active());
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 50, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, volume_event, LV_EVENT_ALL, NULL);
    key(KEY_RIGHT, true);
    key(KEY_RIGHT, false);
    assert(g_settings.volume == 51 && audio_volume == 51 && saves == 1 && !errors);
    fail_save = true;
    key(KEY_LEFT, true);
    key(KEY_LEFT, false);
    assert(g_settings.volume == 50 && audio_volume == 50 && saves == 2 && errors == 1);
    lv_deinit();
}
""")


if __name__ == "__main__":
    unittest.main()
