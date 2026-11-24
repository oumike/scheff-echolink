// The CardputerZero's 320x170 LCD through the Linux framebuffer, or a
// desktop window (sdl_window.c), and the thread that runs LVGL. Keys come
// from keys_cp0.c. In a window the screen is the window: it follows resizes,
// the mouse wheel scrolls what is under the pointer, and controls light up
// under the mouse.

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "cp0.h"
#include "display.h"
#include "esp_log.h"
#include "lvgl.h"
#include "src/themes/lv_theme_private.h"

static const char *TAG = "display";

static pthread_mutex_t s_lock;
static lv_display_t *s_disp;
static bool s_window;
static uint16_t *s_buf;
static lv_indev_t *s_pointer;
static atomic_int s_w = DISPLAY_LCD_H_RES, s_h = DISPLAY_LCD_V_RES;

void cp0_choose_window(bool window) { s_window = window; }
bool cp0_windowed(void) { return s_window; }
bool display_windowed(void) { return s_window; }
int32_t display_h_res(void) { return atomic_load(&s_w); }
int32_t display_v_res(void) { return atomic_load(&s_h); }

uint32_t cp0_millis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

bool display_lock(void)
{
    pthread_mutex_lock(&s_lock);
    return true;
}

void display_unlock(void)
{
    pthread_mutex_unlock(&s_lock);
}

// The launcher owns the backlight (its brightness keys and settings).
void display_set_brightness(uint8_t level)
{
}

// ---- the window --------------------------------------------------------------

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    sdlwin_push(s_buf, atomic_load(&s_w), atomic_load(&s_h), area->y1, area->y2 + 1);
    lv_display_flush_ready(disp);
}

static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    int16_t x, y;
    bool down = sdlwin_pointer(&x, &y);
    // Outside the window: the corner, which is page padding, so nothing hovers.
    if (x < 0 || y < 0) x = y = 0;
    data->point.x = x;
    data->point.y = y;
    data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// A new buffer at the window's new size; LVGL then lays every screen out again
// and redraws it all.
static void resize(int w, int h)
{
    if (w < 1 || h < 1 || (w == atomic_load(&s_w) && h == atomic_load(&s_h))) return;
    size_t bytes = (size_t)w * h * 2;
    uint16_t *buf = calloc(1, bytes);
    if (!buf) {
        ESP_LOGE(TAG, "no memory for a %dx%d window", w, h);
        return;
    }
    lv_display_set_resolution(s_disp, w, h);
    lv_display_set_buffers(s_disp, buf, NULL, bytes, LV_DISPLAY_RENDER_MODE_DIRECT);
    free(s_buf);
    s_buf = buf;
    atomic_store(&s_w, w);
    atomic_store(&s_h, h);
}

// Whether `o` can scroll by dy (positive: towards the top) or dx (towards the
// left).
static bool can_scroll(lv_obj_t *o, int32_t dx, int32_t dy)
{
    if (!lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE)) return false;
    lv_dir_t dir = lv_obj_get_scroll_dir(o);
    if (dy > 0 && (dir & LV_DIR_TOP) && lv_obj_get_scroll_top(o) > 0) return true;
    if (dy < 0 && (dir & LV_DIR_BOTTOM) && lv_obj_get_scroll_bottom(o) > 0) return true;
    if (dx > 0 && (dir & LV_DIR_LEFT) && lv_obj_get_scroll_left(o) > 0) return true;
    if (dx < 0 && (dir & LV_DIR_RIGHT) && lv_obj_get_scroll_right(o) > 0) return true;
    return false;
}

// The wheel scrolls the innermost thing under the pointer that has room to go
// that way, as a touch drag would: an open list before the page behind it.
static void wheel_scroll(void)
{
    int x100, y100;
    sdlwin_take_wheel(&x100, &y100);
    if (!x100 && !y100) return;
    int32_t step = lv_font_get_line_height(&lv_font_montserrat_20) * 3;
    int32_t dx = x100 * step / 100, dy = y100 * step / 100;
    if (!dx && !dy) return;
    lv_point_t p;
    lv_indev_get_point(s_pointer, &p);
    lv_obj_t *o = lv_indev_search_obj(lv_layer_top(), &p);
    if (!o) o = lv_indev_search_obj(lv_screen_active(), &p);
    for (; o; o = lv_obj_get_parent(o)) {
        if (!can_scroll(o, dx, dy)) continue;
        lv_obj_scroll_by_bounded(o, dx, dy, LV_ANIM_OFF);
        break;
    }
}

// The mouse pointer for what is under it: a hand over controls, a text bar
// over text fields.
static void hover_event(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_param(e);
    sdlwin_cursor_t c = SDLWIN_CURSOR_ARROW;
    if (o && !lv_obj_has_state(o, LV_STATE_DISABLED)) {
        if (lv_obj_has_class(o, &lv_textarea_class)) c = SDLWIN_CURSOR_TEXT;
        else if (lv_obj_has_class(o, &lv_button_class) || lv_obj_has_class(o, &lv_dropdown_class) ||
                 lv_obj_has_class(o, &lv_dropdownlist_class) || lv_obj_has_class(o, &lv_checkbox_class) ||
                 lv_obj_has_class(o, &lv_switch_class) || lv_obj_has_class(o, &lv_slider_class) ||
                 lv_obj_has_class(o, &lv_buttonmatrix_class))
            c = SDLWIN_CURSOR_HAND;
    }
    sdlwin_set_cursor(c);
}

// On top of the default theme: controls lighten while the mouse is over them.
static lv_theme_t s_theme;
static lv_style_t s_hover;
static lv_color_filter_dsc_t s_hover_filter;

static lv_color_t lighten(const lv_color_filter_dsc_t *f, lv_color_t c, lv_opa_t opa)
{
    return lv_color_lighten(c, opa);
}

static void theme_apply(lv_theme_t *th, lv_obj_t *o)
{
    if (lv_obj_has_class(o, &lv_button_class) || lv_obj_has_class(o, &lv_dropdown_class) ||
        lv_obj_has_class(o, &lv_checkbox_class) || lv_obj_has_class(o, &lv_switch_class)) {
        lv_obj_add_style(o, &s_hover, LV_STATE_HOVERED);
    } else if (lv_obj_has_class(o, &lv_slider_class)) {
        lv_obj_add_style(o, &s_hover, LV_PART_KNOB | LV_STATE_HOVERED);
    }
}

static void hover_theme(void)
{
    lv_color_filter_dsc_init(&s_hover_filter, lighten);
    lv_style_init(&s_hover);
    lv_style_set_color_filter_dsc(&s_hover, &s_hover_filter);
    lv_style_set_color_filter_opa(&s_hover, LV_OPA_20);
    lv_theme_t *base = lv_display_get_theme(s_disp);
    if (base) s_theme = *base;
    lv_theme_set_parent(&s_theme, base);
    lv_theme_set_apply_cb(&s_theme, theme_apply);
    lv_display_set_theme(s_disp, &s_theme);
}

static esp_err_t window_create(void)
{
    int w, h;
    sdlwin_size(&w, &h);
    sdlwin_take_resize(&w, &h);
    atomic_store(&s_w, w);
    atomic_store(&s_h, h);
    s_disp = lv_display_create(w, h);
    if (!s_disp) return ESP_ERR_NO_MEM;
    size_t bytes = (size_t)w * h * 2;
    s_buf = calloc(1, bytes);
    if (!s_buf) return ESP_ERR_NO_MEM;
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, s_buf, NULL, bytes, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(s_disp, flush_cb);
    s_pointer = lv_indev_create();
    lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_pointer, pointer_read);
    lv_indev_set_display(s_pointer, s_disp);
    lv_indev_add_event_cb(s_pointer, hover_event, LV_EVENT_HOVER_OVER, NULL);
    hover_theme();
    ESP_LOGI(TAG, "window: %dx%d", w, h);
    return ESP_OK;
}

static void *lvgl_thread(void *arg)
{
    for (;;) {
        display_lock();
        if (s_window) {
            int w, h;
            if (sdlwin_take_resize(&w, &h)) resize(w, h);
            wheel_scroll();
        }
        uint32_t wait = lv_timer_handler();
        display_unlock();
        if (wait > 10) wait = 10;
        if (wait < 1) wait = 1;
        usleep(wait * 1000);
    }
    return NULL;
}

// The panel's node: $LV_LINUX_FBDEV_DEVICE, else the device's /dev/fb_lcd
// link, else /dev/fb0.
static const char *fb_path(void)
{
    const char *env = getenv("LV_LINUX_FBDEV_DEVICE");
    if (env && *env) return env;
    if (access("/dev/fb_lcd", R_OK | W_OK) == 0) return "/dev/fb_lcd";
    return "/dev/fb0";
}

esp_err_t display_init(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_lock, &a);
    pthread_mutexattr_destroy(&a);

    lv_init();
    lv_tick_set_cb(cp0_millis);

    if (s_window) {
        esp_err_t err = window_create();
        if (err != ESP_OK) return err;
        cp0_keys_start(s_disp);
        pthread_t t;
        if (pthread_create(&t, NULL, lvgl_thread, NULL) != 0) return ESP_FAIL;
        pthread_detach(t);
        return ESP_OK;
    }

    const char *path = fb_path();
    if (access(path, R_OK | W_OK) != 0) {
        ESP_LOGE(TAG, "%s not writable (is the user in the video group?)", path);
        return ESP_ERR_NOT_FOUND;
    }
    s_disp = lv_linux_fbdev_create();
    if (!s_disp) return ESP_FAIL;
    lv_linux_fbdev_set_file(s_disp, path);
    if (lv_display_get_horizontal_resolution(s_disp) <= 0) {
        ESP_LOGE(TAG, "%s: no usable framebuffer", path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "%s: %dx%d", path, (int)lv_display_get_horizontal_resolution(s_disp),
             (int)lv_display_get_vertical_resolution(s_disp));

    cp0_keys_start(s_disp);

    pthread_t t;
    if (pthread_create(&t, NULL, lvgl_thread, NULL) != 0) return ESP_FAIL;
    pthread_detach(t);
    return ESP_OK;
}
