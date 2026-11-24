// The CardputerZero's keyboard (TCA8418, evdev) as an LVGL keypad.
//
// The UI was made for touch, so focus is kept to what's on top: before each
// key the group is rebuilt, if the screen changed, from the focusable widgets
// of the top-most full-screen layer (a modal or form on lv_layer_top(), else
// the active screen). Then:
//   Up/Down, Tab        move the focus (Up/Down pick in an open dropdown)
//   Left/Right          move a text cursor or a slider, else the focus
//   Enter               press the focused button (held: held), or leave a
//                       text field for the next one
//   Esc, Backspace      back: Cancel/Back/OK on the top layer, or close an
//                       open dropdown (Backspace edits in a text field)
//   letters, digits     type, into the first text field if none has focus
//   Space               hold to talk, unless typing
//   Esc held 3 s        quit (the launcher sends SIGINT for this too)
// USB keyboards work too. The launcher keeps reading the keyboard while we
// run, so nothing is grabbed.
//
// In a desktop window only the built-in keyboard is read here, grabbed while
// the window has focus so the desktop doesn't get its keys too. The desktop's
// own keyboards arrive through SDL (sdl_window.c), already laid out:
// cp0_keys_tap(), cp0_keys_space() and cp0_keys_char().

#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "cp0.h"
#include "el_qso.h"
#include "esp_log.h"
#include "src/themes/lv_theme_private.h"

static const char *TAG = "keys";

#define MODIFIER_STATE "/sys/bus/i2c/devices/1-0034/modifier_state"
#define SYM_MAP        "/usr/share/keymaps/tca8418_keypad_m5stack_keymap.map"
#define KEY_FN_CP0     0x1d0
#define QUIT_HOLD_MS   3000
#define MAX_FOCUSABLE  128
#define KEY_INPUT_RESET (KEY_MAX + 1)
#define BUILTIN_NAME    "tca8418"

typedef struct {
    uint16_t code;
    uint8_t value;   // 1 down, 0 up
    uint8_t shift;   // the driver's one-shot or locked shift, at a press
    char ch;         // a character typed on a desktop keyboard (code unused)
    uint32_t ms;
} raw_key_t;

static atomic_bool s_focused;

static pthread_mutex_t s_raw_mu = PTHREAD_MUTEX_INITIALIZER;
static raw_key_t s_raw[128];
static unsigned s_raw_head, s_raw_tail;

static void raw_push(raw_key_t key)
{
    pthread_mutex_lock(&s_raw_mu);
    bool overflow = s_raw_head - s_raw_tail == 128;
    if (key.code == KEY_INPUT_RESET || overflow) {
        s_raw_tail = s_raw_head;
        key = (raw_key_t){.code = KEY_INPUT_RESET};
    }
    s_raw[s_raw_head++ % 128] = key;
    pthread_mutex_unlock(&s_raw_mu);
    if (overflow) ESP_LOGW(TAG, "keyboard queue overflow; releasing keys");
}

// ---- evdev thread ----------------------------------------------------------

static bool has_key_a(int fd)
{
    unsigned long bits[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) return false;
    return (bits[KEY_A / (8 * sizeof(long))] >> (KEY_A % (8 * sizeof(long)))) & 1;
}

// Every keyboard: the built-in one and any USB keyboard, found again every
// few seconds so plugging one in works.
#define MAX_KEYBOARDS 8

typedef struct {
    int fd;
    char path[32];
    bool sync_lost;
    bool grabbed;
} kbd_t;

static void scan_keyboards(kbd_t *k, int *n)
{
    for (int i = 0; i < 32 && *n < MAX_KEYBOARDS; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        bool open_already = false;
        for (int j = 0; j < *n; j++) open_already |= !strcmp(k[j].path, path);
        if (open_already) continue;
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (!has_key_a(fd)) {
            close(fd);
            continue;
        }
        char name[64] = "?";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        if (cp0_windowed() && !strstr(name, BUILTIN_NAME)) {
            close(fd);  // the desktop's, which SDL brings
            continue;
        }
        ESP_LOGI(TAG, "keyboard %s (%s)", path, name);
        k[*n].fd = fd;
        k[*n].sync_lost = false;
        k[*n].grabbed = false;
        snprintf(k[*n].path, sizeof(k[*n].path), "%s", path);
        (*n)++;
    }
}

// True when the driver's sticky shift is on (one-shot or locked).
static bool sticky_shift(void)
{
    FILE *f = fopen(MODIFIER_STATE, "r");
    if (!f) return false;
    bool on = false;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        char name[16] = {0};
        int v = 0;
        if (sscanf(line, " %15[a-z_] %*[:=]%d", name, &v) != 2 && sscanf(line, " %15[a-z_] %d", name, &v) != 2)
            continue;
        if (!strcmp(name, "shift") && v > 0) on = true;
    }
    fclose(f);
    return on;
}

static void *reader(void *arg)
{
    kbd_t kbd[MAX_KEYBOARDS];
    int n = 0;  // open keyboards, 0..MAX_KEYBOARDS
    uint32_t last_scan = 0;
    bool told = false;
    for (;;) {
        if (cp0_millis() - last_scan > 3000 || !n) {
            scan_keyboards(kbd, &n);
            last_scan = cp0_millis();
            if (!n) {
                if (!told) ESP_LOGW(TAG, "no keyboard found; retrying");
                told = true;
                sleep(1);
                continue;
            }
        }
        bool focused = atomic_load(&s_focused);
        for (int i = 0; cp0_windowed() && i < n; i++) {
            if (kbd[i].grabbed == focused) continue;
            if (ioctl(kbd[i].fd, EVIOCGRAB, focused ? 1 : 0) == 0 || !focused) kbd[i].grabbed = focused;
            if (!focused) raw_push((raw_key_t){.code = KEY_INPUT_RESET});
        }
        struct pollfd p[MAX_KEYBOARDS];
        for (int i = 0; i < n; i++) p[i] = (struct pollfd){kbd[i].fd, POLLIN, 0};
        if (n < 1 || n > MAX_KEYBOARDS || poll(p, (nfds_t)n, cp0_windowed() ? 100 : 500) <= 0) continue;
        for (int d = 0; d < n; d++) {
            if (!(p[d].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) continue;
            struct input_event ev[16];
            ssize_t r = read(kbd[d].fd, ev, sizeof(ev));
            if (r <= 0) {
                if (r < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                ESP_LOGI(TAG, "keyboard %s gone", kbd[d].path);
                raw_push((raw_key_t){.code = KEY_INPUT_RESET});
                close(kbd[d].fd);
                kbd[d] = kbd[--n];
                break;  // p[] no longer matches kbd[]
            }
            for (size_t i = 0; i < (size_t)r / sizeof(ev[0]); i++) {
                if (ev[i].type == EV_SYN && ev[i].code == SYN_DROPPED) {
                    ESP_LOGW(TAG, "keyboard %s lost event synchronization; releasing keys", kbd[d].path);
                    kbd[d].sync_lost = true;
                    raw_push((raw_key_t){.code = KEY_INPUT_RESET});
                } else if (ev[i].type == EV_SYN && ev[i].code == SYN_REPORT) {
                    kbd[d].sync_lost = false;
                }
                if (kbd[d].sync_lost || ev[i].type != EV_KEY || ev[i].code > KEY_MAX ||
                    ev[i].value < 0 || ev[i].value > 1) continue;  // LVGL repeats held keys itself
                if (cp0_windowed() && !kbd[d].grabbed) continue;  // the desktop has these
                raw_key_t k = {.code = ev[i].code, .value = ev[i].value, .ms = cp0_millis()};
                if (k.value == 1 && k.code >= KEY_Q && k.code <= KEY_M) k.shift = sticky_shift();
                raw_push(k);
            }
        }
    }
    return NULL;
}

static bool raw_pop(raw_key_t *k)
{
    pthread_mutex_lock(&s_raw_mu);
    bool ok = s_raw_tail != s_raw_head;
    if (ok) *k = s_raw[s_raw_tail++ % 128];
    pthread_mutex_unlock(&s_raw_mu);
    return ok;
}

// ---- characters ------------------------------------------------------------

typedef struct {
    uint16_t code;
    char ch;
} sym_t;

// The launcher's built-in sym layer, for when the system keymap is missing.
static sym_t s_sym[64] = {
    {26, '!'}, {27, '@'}, {39, '#'}, {40, '$'}, {41, '%'}, {43, '^'}, {51, '&'}, {52, '*'},
    {53, '('}, {94, ')'}, {55, '~'}, {69, '`'}, {70, '_'}, {71, '-'}, {72, '+'}, {73, '='},
    {74, '['}, {75, ']'}, {76, '{'}, {77, '}'}, {79, ';'}, {80, ':'}, {81, '\''}, {82, '"'},
    {83, '<'}, {85, '>'}, {86, '\\'}, {89, '|'}, {90, ','}, {91, '.'}, {92, '/'}, {93, '?'},
};
static size_t s_sym_count = 32;

static char keysym_char(const char *name)
{
    static const struct {
        const char *name;
        char ch;
    } names[] = {
        {"space", ' '},       {"exclam", '!'},       {"quotedbl", '"'},     {"numbersign", '#'},
        {"dollar", '$'},      {"percent", '%'},      {"ampersand", '&'},    {"apostrophe", '\''},
        {"quoteright", '\''}, {"parenleft", '('},    {"parenright", ')'},   {"asterisk", '*'},
        {"plus", '+'},        {"comma", ','},        {"minus", '-'},        {"period", '.'},
        {"slash", '/'},       {"colon", ':'},        {"semicolon", ';'},    {"less", '<'},
        {"equal", '='},       {"greater", '>'},      {"question", '?'},     {"at", '@'},
        {"bracketleft", '['}, {"backslash", '\\'},   {"bracketright", ']'}, {"asciicircum", '^'},
        {"underscore", '_'},  {"grave", '`'},        {"quoteleft", '`'},    {"braceleft", '{'},
        {"bar", '|'},         {"braceright", '}'},   {"asciitilde", '~'},
    };
    if (strlen(name) == 1 && ((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z') ||
                              (name[0] >= '0' && name[0] <= '9')))
        return name[0];
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!strcmp(names[i].name, name)) return names[i].ch;
    }
    return 0;
}

static void load_sym_map(void)
{
    FILE *f = fopen(SYM_MAP, "r");
    if (!f) return;
    sym_t parsed[64];
    size_t n = 0;
    char line[256];
    while (n < 64 && fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        unsigned code;
        char name[64];
        if (sscanf(line, " keycode %u = %63s", &code, name) != 2 || code > 0xffff) continue;
        char ch = keysym_char(name);
        if (ch) parsed[n++] = (sym_t){(uint16_t)code, ch};
    }
    fclose(f);
    if (n) {
        memcpy(s_sym, parsed, n * sizeof(parsed[0]));
        s_sym_count = n;
    }
}

static char sym_char(uint16_t code)
{
    for (size_t i = 0; i < s_sym_count; i++) {
        if (s_sym[i].code == code) return s_sym[i].ch;
    }
    return 0;
}

static char us_char(uint16_t code, bool shift)
{
    static const char top[] = "qwertyuiop", home[] = "asdfghjkl", bottom[] = "zxcvbnm";
    static const char digits[] = "1234567890", shifted[] = "!@#$%^&*()";
    char c = 0;
    if (code >= KEY_Q && code <= KEY_P) c = top[code - KEY_Q];
    else if (code >= KEY_A && code <= KEY_L) c = home[code - KEY_A];
    else if (code >= KEY_Z && code <= KEY_M) c = bottom[code - KEY_Z];
    if (c) return shift ? c - 32 : c;
    if (code >= KEY_1 && code <= KEY_0) return (shift ? shifted : digits)[code - KEY_1];
    switch (code) {
    case KEY_MINUS: return shift ? '_' : '-';
    case KEY_EQUAL: return shift ? '+' : '=';
    case KEY_COMMA: return shift ? '<' : ',';
    case KEY_DOT: return shift ? '>' : '.';
    case KEY_SLASH: return shift ? '?' : '/';
    case KEY_SEMICOLON: return shift ? ':' : ';';
    case KEY_APOSTROPHE: return shift ? '"' : '\'';
    default: return 0;
    }
}

// ---- focus -----------------------------------------------------------------

static void press(uint16_t code, uint32_t key);

static lv_group_t *s_group;
static lv_obj_t *s_focusable[MAX_FOCUSABLE];
static size_t s_focusable_count;
static lv_obj_t *s_last_root;

static bool is_type(lv_obj_t *o, const lv_obj_class_t *c)
{
    return o && lv_obj_check_type(o, c);
}

// The layer keys act on: the top-most visible full-width child of the top
// layer (a modal, or a form drawn over Home), else the active screen.
static lv_obj_t *top_root(void)
{
    lv_obj_t *top = lv_layer_top();
    int32_t w = lv_display_get_horizontal_resolution(lv_obj_get_display(top));
    for (int32_t i = (int32_t)lv_obj_get_child_count(top) - 1; i >= 0; i--) {
        lv_obj_t *c = lv_obj_get_child(top, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
        if (lv_obj_get_width(c) >= w) return c;
    }
    return lv_screen_active();
}

static void collect(lv_obj_t *o, lv_obj_t **out, size_t *n)
{
    uint32_t count = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *c = lv_obj_get_child(o, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN) || lv_obj_has_state(c, LV_STATE_DISABLED)) continue;
        if (is_type(c, &lv_button_class) || is_type(c, &lv_textarea_class) || is_type(c, &lv_dropdown_class) ||
            is_type(c, &lv_slider_class)) {
            if (*n < MAX_FOCUSABLE) out[(*n)++] = c;
        }
        if (!is_type(c, &lv_textarea_class) && !is_type(c, &lv_dropdown_class)) collect(c, out, n);
    }
}

static void refresh_group(void)
{
    lv_obj_t *root = top_root();
    lv_obj_t *found[MAX_FOCUSABLE];
    size_t n = 0;
    collect(root, found, &n);
    if (root == s_last_root && n == s_focusable_count && n == lv_group_get_obj_count(s_group) &&
        !memcmp(found, s_focusable, n * sizeof(found[0])))
        return;

    lv_obj_t *prev = lv_group_get_focused(s_group);
    lv_group_remove_all_objs(s_group);
    bool keep = false;
    for (size_t i = 0; i < n; i++) {
        lv_group_add_obj(s_group, found[i]);
        if (found[i] == prev) keep = true;
    }
    if (keep) lv_group_focus_obj(prev);
    memcpy(s_focusable, found, n * sizeof(found[0]));
    s_focusable_count = n;
    s_last_root = root;
}

static lv_obj_t *focused(void)
{
    return lv_group_get_focused(s_group);
}

// The focusable below (dir 1) or above (dir -1) the focused one: the nearest
// row, then the nearest in that row.
static lv_obj_t *spatial(int dir)
{
    lv_obj_t *f = focused();
    if (!f) return NULL;
    lv_area_t a;
    lv_obj_get_coords(f, &a);
    int32_t cy = (a.y1 + a.y2) / 2, cx = (a.x1 + a.x2) / 2;
    lv_obj_t *best = NULL;
    int64_t best_score = INT64_MAX;
    for (size_t i = 0; i < s_focusable_count; i++) {
        lv_obj_t *o = s_focusable[i];
        if (o == f) continue;
        lv_area_t b;
        lv_obj_get_coords(o, &b);
        int32_t dy = ((b.y1 + b.y2) / 2 - cy) * dir;
        if (dy <= 2 || (dir > 0 ? b.y1 < a.y1 : b.y2 > a.y2)) continue;
        int32_t dx = LV_ABS((b.x1 + b.x2) / 2 - cx);
        int64_t score = (int64_t)dy * 4 + dx;
        if (score < best_score) {
            best_score = score;
            best = o;
        }
    }
    return best;
}

// Up/Down: the open dropdown's own keys, else spatial focus.
static void vertical(uint16_t code, int dir, bool dd_open)
{
    if (dd_open) {
        press(code, dir > 0 ? LV_KEY_DOWN : LV_KEY_UP);
        return;
    }
    lv_obj_t *to = spatial(dir);
    if (to) lv_group_focus_obj(to);
    else if (!focused()) press(code, LV_KEY_NEXT);
}

static lv_obj_t *first_textarea(void)
{
    for (size_t i = 0; i < s_focusable_count; i++) {
        if (is_type(s_focusable[i], &lv_textarea_class)) return s_focusable[i];
    }
    return NULL;
}

static bool button_text_is(lv_obj_t *b, const char *want, bool suffix)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(b); i++) {
        lv_obj_t *l = lv_obj_get_child(b, i);
        if (!is_type(l, &lv_label_class)) continue;
        const char *t = lv_label_get_text(l);
        if (!t) continue;
        if (!suffix) return !strcmp(t, want);
        size_t lt = strlen(t), lw = strlen(want);
        return lt >= lw && !strcmp(t + lt - lw, want);
    }
    return false;
}

// Back: Cancel, then ...Back, then OK on the top layer. Never Leave: hanging
// up is a deliberate choice.
static void back_async(void *arg)
{
    refresh_group();
    lv_obj_t *f = focused();
    if (is_type(f, &lv_dropdown_class) && lv_dropdown_is_open(f)) {
        lv_dropdown_close(f);
        return;
    }
    static const struct {
        const char *text;
        bool suffix;
    } order[] = {{"Cancel", false}, {"Back", true}, {"OK", false}};
    for (size_t k = 0; k < sizeof(order) / sizeof(order[0]); k++) {
        for (size_t i = 0; i < s_focusable_count; i++) {
            lv_obj_t *b = s_focusable[i];
            if (is_type(b, &lv_button_class) && button_text_is(b, order[k].text, order[k].suffix)) {
                lv_obj_send_event(b, LV_EVENT_CLICKED, NULL);
                return;
            }
        }
    }
}

// ---- LVGL keypad -----------------------------------------------------------

typedef struct {
    uint32_t key;
    lv_indev_state_t state;
} out_key_t;

static out_key_t s_out[32];
static unsigned s_out_head, s_out_tail;
static uint32_t s_cur_key;
static lv_indev_state_t s_cur_state = LV_INDEV_STATE_RELEASED;
static uint32_t s_held[KEY_MAX + 1];  // the LVGL key each held raw key sent
static bool s_shift_l, s_shift_r, s_sym_down, s_fn, s_ptt, s_esc;
static uint32_t s_esc_ms;

static void out_push(uint32_t key, lv_indev_state_t state)
{
    if (s_out_head - s_out_tail < 32) s_out[s_out_head++ % 32] = (out_key_t){key, state};
}

static void press(uint16_t code, uint32_t key)
{
    // One key at a time, as LVGL's keypad expects.
    if (s_cur_state == LV_INDEV_STATE_PRESSED) {
        out_push(s_cur_key, LV_INDEV_STATE_RELEASED);
        for (size_t i = 0; i <= KEY_MAX; i++) {
            if (s_held[i] == s_cur_key) s_held[i] = 0;
        }
    }
    out_push(key, LV_INDEV_STATE_PRESSED);
    s_cur_key = key;
    s_cur_state = LV_INDEV_STATE_PRESSED;
    s_held[code] = key;
}

static void release(uint16_t code)
{
    uint32_t key = s_held[code];
    s_held[code] = 0;
    if (!key || s_cur_state != LV_INDEV_STATE_PRESSED || key != s_cur_key) return;
    out_push(key, LV_INDEV_STATE_RELEASED);
    s_cur_state = LV_INDEV_STATE_RELEASED;
}

static void key_down(const raw_key_t *k)
{
    refresh_group();
    lv_obj_t *f = focused();
    bool in_text = is_type(f, &lv_textarea_class);
    bool dd_open = is_type(f, &lv_dropdown_class) && lv_dropdown_is_open(f);
    bool shift = s_shift_l || s_shift_r || k->shift;

    switch (k->code) {
    case KEY_UP: case KEY_PAGEUP:
        vertical(k->code, -1, dd_open);
        return;
    case KEY_DOWN: case KEY_PAGEDOWN:
        vertical(k->code, 1, dd_open);
        return;
    case KEY_LEFT: case KEY_RIGHT: {
        bool own = in_text || dd_open || is_type(f, &lv_slider_class);
        if (k->code == KEY_LEFT) press(k->code, own ? LV_KEY_LEFT : LV_KEY_PREV);
        else press(k->code, own ? LV_KEY_RIGHT : LV_KEY_NEXT);
        return;
    }
    case KEY_TAB:
        press(k->code, shift ? LV_KEY_PREV : LV_KEY_NEXT);
        return;
    case KEY_ENTER: case KEY_KPENTER:
        press(k->code, in_text ? LV_KEY_NEXT : LV_KEY_ENTER);
        return;
    case KEY_BACKSPACE: case KEY_DELETE:
        if (in_text) press(k->code, k->code == KEY_DELETE ? LV_KEY_DEL : LV_KEY_BACKSPACE);
        else lv_async_call(back_async, NULL);
        return;
    case KEY_HOME: case KEY_END:
        if (in_text) press(k->code, k->code == KEY_HOME ? LV_KEY_HOME : LV_KEY_END);
        return;
    case KEY_SPACE:
        if (in_text) {
            press(k->code, ' ');
        } else {
            s_ptt = true;
            el_qso_set_ptt(true);
        }
        return;
    default: break;
    }

    if (s_fn) return;
    char c = s_sym_down ? sym_char(k->code) : 0;
    if (!c) c = us_char(k->code, shift);
    if (!c) return;
    if (!in_text) {
        lv_obj_t *ta = first_textarea();
        if (!ta) return;
        lv_group_focus_obj(ta);
    }
    press(k->code, (uint32_t)(unsigned char)c);
}

// A character from a desktop keyboard: typed as is, into the first text
// field if none has focus.
static void type_char(char c)
{
    refresh_group();
    if (!is_type(focused(), &lv_textarea_class)) {
        lv_obj_t *ta = c == ' ' ? NULL : first_textarea();
        if (!ta) return;
        lv_group_focus_obj(ta);
    }
    press(KEY_RESERVED, (uint32_t)(unsigned char)c);
    release(KEY_RESERVED);
}

static void handle(const raw_key_t *k)
{
    if (k->ch) {
        type_char(k->ch);
        return;
    }
    bool down = k->value == 1;
    switch (k->code) {
    case KEY_INPUT_RESET:
        if (s_cur_state == LV_INDEV_STATE_PRESSED) out_push(s_cur_key, LV_INDEV_STATE_RELEASED);
        s_cur_state = LV_INDEV_STATE_RELEASED;
        memset(s_held, 0, sizeof(s_held));
        s_shift_l = s_shift_r = s_sym_down = s_fn = s_esc = false;
        s_ptt = false;
        el_qso_set_ptt(false);
        return;
    case KEY_LEFTSHIFT: s_shift_l = down; return;
    case KEY_RIGHTSHIFT: s_shift_r = down; return;
    case KEY_COMPOSE: s_sym_down = down; return;
    case KEY_FN_CP0: s_fn = down; return;
    case KEY_LEFTCTRL: case KEY_LEFTALT: case KEY_RIGHTALT: return;
    case KEY_ESC:
        if (down) {
            s_esc = true;
            s_esc_ms = k->ms;
        } else if (s_esc) {
            s_esc = false;
            if (k->ms - s_esc_ms < QUIT_HOLD_MS) lv_async_call(back_async, NULL);
        }
        return;
    case KEY_SPACE:
        if (!down && s_ptt) {
            s_ptt = false;
            el_qso_set_ptt(false);
            return;
        }
        break;
    default: break;
    }
    if (down) key_down(k);
    else release(k->code);
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    raw_key_t k;
    while (s_out_head == s_out_tail && raw_pop(&k)) handle(&k);

    if (s_out_head != s_out_tail) {
        out_key_t o = s_out[s_out_tail++ % 32];
        data->key = o.key;
        data->state = o.state;
    } else {
        data->key = s_cur_key;
        data->state = s_cur_state;
    }
    pthread_mutex_lock(&s_raw_mu);
    bool pending = s_raw_tail != s_raw_head;
    pthread_mutex_unlock(&s_raw_mu);
    data->continue_reading = pending || s_out_head != s_out_tail;
    if (!pending && s_esc && cp0_millis() - s_esc_ms >= QUIT_HOLD_MS) cp0_request_quit();
}

// ---- focus ring ------------------------------------------------------------

static lv_theme_t s_theme;
static lv_style_t s_focus_style;

static void theme_apply(lv_theme_t *th, lv_obj_t *o)
{
    (void)th;
    if (is_type(o, &lv_button_class) || is_type(o, &lv_textarea_class) || is_type(o, &lv_dropdown_class) ||
        is_type(o, &lv_slider_class))
        lv_obj_add_style(o, &s_focus_style, LV_STATE_FOCUS_KEY);
}

// The default theme's keypad focus is too faint to see on this screen.
static void focus_ring(lv_display_t *disp)
{
    lv_style_init(&s_focus_style);
    lv_style_set_outline_width(&s_focus_style, 2);
    lv_style_set_outline_pad(&s_focus_style, -2);  // inside, so lists don't clip it
    lv_style_set_outline_color(&s_focus_style, lv_color_hex(0xffc107));
    lv_style_set_outline_opa(&s_focus_style, LV_OPA_COVER);
    lv_theme_t *base = lv_display_get_theme(disp);
    if (!base) return;
    s_theme = *base;
    lv_theme_set_parent(&s_theme, base);
    lv_theme_set_apply_cb(&s_theme, theme_apply);
    lv_display_set_theme(disp, &s_theme);
}

// ---- desktop keyboards (window mode) -------------------------------------------

void cp0_keys_set_focus(bool focused)
{
    atomic_store(&s_focused, focused);
}

void cp0_keys_tap(uint16_t code, bool shift)
{
    uint32_t ms = cp0_millis();
    raw_push((raw_key_t){.code = code, .value = 1, .shift = shift, .ms = ms});
    raw_push((raw_key_t){.code = code, .value = 0, .ms = ms});
}

void cp0_keys_space(bool down)
{
    raw_push((raw_key_t){.code = KEY_SPACE, .value = down, .ms = cp0_millis()});
}

void cp0_keys_char(char c)
{
    if (c >= 0x20 && c < 0x7f) raw_push((raw_key_t){.ch = c, .ms = cp0_millis()});
}

void cp0_keys_start(lv_display_t *disp)
{
    load_sym_map();
    focus_ring(disp);
    s_group = lv_group_create();
    lv_indev_t *kp = lv_indev_create();
    lv_indev_set_type(kp, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(kp, read_cb);
    lv_indev_set_display(kp, disp);
    lv_indev_set_group(kp, s_group);

    pthread_t t;
    if (pthread_create(&t, NULL, reader, NULL) == 0) pthread_detach(t);
}
