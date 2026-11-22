/* Headless LVGL/RTOS boundaries for executing the real UI controllers. */
#include <stdarg.h>
#include <netdb.h>

#define CONFIG_IDF_TARGET_ESP32S3 1
#define DISPLAY_H_RES 320
#define DISPLAY_V_RES 240

typedef unsigned lv_color_t;
typedef int lv_font_t;
static lv_font_t lv_font_montserrat_14, lv_font_montserrat_16, lv_font_montserrat_20;
static lv_font_t lv_font_montserrat_28, lv_font_montserrat_36;
#define lv_color_hex(c) (c)
#define lv_color_white() 0xffffffu
#define lv_color_black() 0u
#define LV_PCT(n) (n)
#define LV_SIZE_CONTENT -1
#define LV_SYMBOL_WIFI "[wifi]"
#define LV_SYMBOL_UPLOAD "[ap]"
#define LV_SYMBOL_BATTERY_FULL "[full]"
#define LV_SYMBOL_BATTERY_3 "[3]"
#define LV_SYMBOL_BATTERY_2 "[2]"
#define LV_SYMBOL_BATTERY_1 "[1]"
#define LV_SYMBOL_BATTERY_EMPTY "[empty]"
#define LV_SYMBOL_SETTINGS "[settings]"
#define LV_SYMBOL_EDIT "[edit]"
#define LV_SYMBOL_CLOSE "[close]"
#define LV_SYMBOL_PLUS "[plus]"
#define LV_SYMBOL_OK "[ok]"
#define LV_SYMBOL_SAVE "[save]"
#define LV_SYMBOL_LEFT "[left]"
#define LV_SYMBOL_RIGHT "[right]"
#define LV_SYMBOL_REFRESH "[refresh]"
#define LV_SYMBOL_AUDIO "[mic]"
#define LV_SYMBOL_VOLUME_MAX "[speaker]"

enum {
    LV_FLEX_FLOW_ROW, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
    LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_ALIGN_TOP_RIGHT,
    LV_OPA_TRANSP, LV_OPA_COVER, LV_OPA_70, LV_DIR_VER, LV_ANIM_ON, LV_ANIM_OFF,
    LV_LABEL_LONG_WRAP, LV_LABEL_LONG_DOT, LV_TEXT_ALIGN_CENTER, LV_TEXT_ALIGN_RIGHT,
    LV_KEYBOARD_MODE_NUMBER, LV_KEYBOARD_MODE_TEXT_UPPER, LV_KEYBOARD_MODE_TEXT_LOWER,
    LV_SCR_LOAD_ANIM_MOVE_LEFT, LV_SCR_LOAD_ANIM_MOVE_RIGHT
};
enum { LV_OBJ_FLAG_HIDDEN = 1, LV_OBJ_FLAG_CLICKABLE = 2, LV_OBJ_FLAG_SCROLLABLE = 4 };
enum { LV_STATE_FOCUSED = 1, LV_STATE_DISABLED = 2, LV_STATE_PRESSED = 4 };
typedef enum {
    LV_EVENT_ALL, LV_EVENT_CLICKED, LV_EVENT_READY, LV_EVENT_CANCEL,
    LV_EVENT_FOCUSED, LV_EVENT_DEFOCUSED, LV_EVENT_VALUE_CHANGED,
    LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST
} lv_event_code_t;

typedef struct lv_obj_t lv_obj_t;
typedef struct {
    lv_obj_t *target;
    lv_event_code_t code;
    void *user_data;
} lv_event_t;
typedef void (*lv_event_cb_t)(lv_event_t *);
typedef struct {
    lv_event_cb_t cb;
    lv_event_code_t code;
    void *user_data;
} host_event_t;
enum { HOST_OBJECT, HOST_LABEL, HOST_BUTTON, HOST_FIELD };
struct lv_obj_t {
    bool alive, delete_pending;
    int kind, value;
    unsigned flags, state;
    char *text;
    lv_obj_t *parent, *child, *next, *textarea;
    host_event_t events[8];
    unsigned event_count;
};

static int host_lock_depth;
static bool display_lock(void) { ++host_lock_depth; return true; }
static void display_unlock(void) { assert(host_lock_depth > 0); --host_lock_depth; }
static lv_obj_t host_top = {.alive = true};
static lv_obj_t *host_active, *host_delete_queue[128];
static unsigned host_delete_count, host_screen_count, host_screens_created;

static void host_object(lv_obj_t *obj)
{
    assert(host_lock_depth > 0);
    assert(obj && obj->alive);
}

static lv_obj_t *lv_obj_create(lv_obj_t *parent)
{
    assert(host_lock_depth > 0);
    lv_obj_t *obj = calloc(1, sizeof(*obj));
    assert(obj);
    obj->alive = true;
    obj->parent = parent;
    obj->text = strdup("");
    if (parent) {
        host_object(parent);
        lv_obj_t **slot = &parent->child;
        while (*slot) slot = &(*slot)->next;
        *slot = obj;
    } else {
        ++host_screen_count;
        ++host_screens_created;
    }
    return obj;
}

static lv_obj_t *host_create(lv_obj_t *parent, int kind)
{
    lv_obj_t *obj = lv_obj_create(parent);
    obj->kind = kind;
    return obj;
}
#define lv_label_create(p) host_create(p, HOST_LABEL)
#define lv_button_create(p) host_create(p, HOST_BUTTON)
#define lv_textarea_create(p) host_create(p, HOST_FIELD)
#define lv_keyboard_create(p) lv_obj_create(p)
#define lv_dropdown_create(p) lv_obj_create(p)
#define lv_slider_create(p) lv_obj_create(p)
#define lv_spinner_create(p) lv_obj_create(p)
#define lv_layer_top() (&host_top)
#define lv_screen_active() host_active

static void lv_obj_delete(lv_obj_t *obj)
{
    host_object(obj);
    assert(obj != host_active);
    while (obj->child) lv_obj_delete(obj->child);
    if (obj->parent) {
        lv_obj_t **slot = &obj->parent->child;
        while (*slot != obj) { assert(*slot); slot = &(*slot)->next; }
        *slot = obj->next;
    } else {
        --host_screen_count;
    }
    obj->alive = false;
    free(obj->text);
    free(obj);
}

static void lv_obj_clean(lv_obj_t *obj)
{
    host_object(obj);
    while (obj->child) lv_obj_delete(obj->child);
}

static void lv_obj_delete_async(lv_obj_t *obj)
{
    host_object(obj);
    assert(!obj->delete_pending && host_delete_count < 128);
    obj->delete_pending = true;
    host_delete_queue[host_delete_count++] = obj;
}

static void host_flush_deletes(void)
{
    display_lock();
    for (unsigned i = 0; i < host_delete_count; ++i) lv_obj_delete(host_delete_queue[i]);
    host_delete_count = 0;
    display_unlock();
}

static void lv_screen_load(lv_obj_t *obj) { host_object(obj); host_active = obj; }
static void lv_screen_load_anim(lv_obj_t *obj, int a, int b, int c, bool d)
{
    lv_screen_load(obj);
}
static lv_obj_t *lv_obj_get_screen(lv_obj_t *obj)
{
    host_object(obj);
    while (obj->parent) obj = obj->parent;
    return obj;
}
static lv_obj_t *lv_obj_get_child(lv_obj_t *obj, unsigned n)
{
    host_object(obj);
    lv_obj_t *child = obj->child;
    while (child && n--) child = child->next;
    assert(child);
    return child;
}

#define lv_obj_set_size(o, ...) host_object(o)
#define lv_obj_set_width(o, ...) host_object(o)
#define lv_obj_set_height(o, ...) host_object(o)
#define lv_obj_set_flex_flow(o, ...) host_object(o)
#define lv_obj_set_flex_align(o, ...) host_object(o)
#define lv_obj_set_flex_grow(o, ...) host_object(o)
#define lv_obj_set_scroll_dir(o, ...) host_object(o)
#define lv_obj_set_style_bg_opa(o, ...) host_object(o)
#define lv_obj_set_style_bg_color(o, ...) host_object(o)
#define lv_obj_set_style_border_width(o, ...) host_object(o)
#define lv_obj_set_style_radius(o, ...) host_object(o)
#define lv_obj_set_style_pad_all(o, ...) host_object(o)
#define lv_obj_set_style_pad_top(o, ...) host_object(o)
#define lv_obj_set_style_pad_bottom(o, ...) host_object(o)
#define lv_obj_set_style_pad_row(o, ...) host_object(o)
#define lv_obj_set_style_pad_column(o, ...) host_object(o)
#define lv_obj_set_style_pad_hor(o, ...) host_object(o)
#define lv_obj_set_style_text_font(o, ...) host_object(o)
#define lv_obj_set_style_text_color(o, ...) host_object(o)
#define lv_obj_set_style_text_align(o, ...) host_object(o)
#define lv_obj_set_style_text_letter_space(o, ...) host_object(o)
#define lv_obj_set_style_max_height(o, ...) host_object(o)
#define lv_obj_align(o, ...) host_object(o)
#define lv_obj_center(o) host_object(o)
#define lv_obj_move_foreground(o) host_object(o)
#define lv_obj_update_layout(o) host_object(o)
#define lv_obj_scroll_to_view_recursive(o, ...) host_object(o)
#define lv_obj_scroll_to_y(o, ...) host_object(o)
#define lv_label_set_long_mode(o, ...) host_object(o)
#define lv_textarea_set_one_line(o, ...) host_object(o)
#define lv_textarea_set_password_mode(o, ...) host_object(o)
#define lv_textarea_set_placeholder_text(o, ...) host_object(o)
#define lv_textarea_set_max_length(o, ...) host_object(o)
#define lv_textarea_set_accepted_chars(o, ...) host_object(o)
#define lv_keyboard_set_mode(o, ...) host_object(o)
#define lv_dropdown_set_options(o, ...) host_object(o)
#define lv_dropdown_get_list(o) (o)
#define lv_slider_set_range(o, ...) host_object(o)

static void lv_obj_add_flag(lv_obj_t *o, unsigned f) { host_object(o); o->flags |= f; }
static void lv_obj_remove_flag(lv_obj_t *o, unsigned f) { host_object(o); o->flags &= ~f; }
static void lv_obj_add_state(lv_obj_t *o, unsigned s) { host_object(o); o->state |= s; }
static void lv_obj_remove_state(lv_obj_t *o, unsigned s) { host_object(o); o->state &= ~s; }
static void lv_label_set_text(lv_obj_t *o, const char *s)
{
    host_object(o);
    char *text = strdup(s);
    assert(text);
    free(o->text);
    o->text = text;
}
static void lv_label_set_text_fmt(lv_obj_t *o, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lv_label_set_text(o, buf);
}
#define lv_textarea_set_text lv_label_set_text
static const char *lv_textarea_get_text(lv_obj_t *o) { host_object(o); return o->text; }
#define lv_label_get_text lv_textarea_get_text
static void lv_keyboard_set_textarea(lv_obj_t *o, lv_obj_t *ta) { host_object(o); o->textarea = ta; }
static lv_obj_t *lv_keyboard_get_textarea(lv_obj_t *o) { host_object(o); return o->textarea; }
static void lv_dropdown_set_selected(lv_obj_t *o, unsigned n) { host_object(o); o->value = n; }
static unsigned lv_dropdown_get_selected(lv_obj_t *o) { host_object(o); return o->value; }
static void lv_slider_set_value(lv_obj_t *o, int n, int anim) { host_object(o); o->value = n; }
static int lv_slider_get_value(lv_obj_t *o) { host_object(o); return o->value; }
static lv_obj_t *lv_event_get_target(lv_event_t *e) { return e->target; }
static lv_event_code_t lv_event_get_code(lv_event_t *e) { return e->code; }
static void *lv_event_get_user_data(lv_event_t *e) { return e->user_data; }
static void lv_obj_add_event_cb(lv_obj_t *o, lv_event_cb_t cb, lv_event_code_t code, void *data)
{
    host_object(o);
    assert(o->event_count < 8);
    o->events[o->event_count++] = (host_event_t){cb, code, data};
}
static void lv_obj_send_event(lv_obj_t *o, lv_event_code_t code, void *param)
{
    host_object(o);
    host_event_t events[8];
    unsigned count = o->event_count;
    memcpy(events, o->events, sizeof(events));
    for (unsigned i = 0; i < count; ++i) {
        if (events[i].code == LV_EVENT_ALL || events[i].code == code) {
            lv_event_t event = {o, code, events[i].user_data};
            events[i].cb(&event);
        }
    }
}

typedef struct lv_timer_t {
    bool alive;
    uint32_t period;
    uint64_t due;
    int repeat;
    void (*cb)(struct lv_timer_t *);
} lv_timer_t;
static lv_timer_t *host_timers[256];
static unsigned host_timer_used;
static uint64_t host_now;
static lv_timer_t *lv_timer_create(void (*cb)(lv_timer_t *), uint32_t ms, void *arg)
{
    assert(host_lock_depth > 0 && host_timer_used < 256 && ms);
    lv_timer_t *timer = calloc(1, sizeof(*timer));
    *timer = (lv_timer_t){true, ms, host_now + ms, -1, cb};
    host_timers[host_timer_used++] = timer;
    return timer;
}
static void lv_timer_delete(lv_timer_t *t)
{
    assert(host_lock_depth > 0 && t && t->alive);
    t->alive = false;
}
static void lv_timer_set_repeat_count(lv_timer_t *t, int n) { assert(t->alive); t->repeat = n; }
static unsigned host_timer_count(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < host_timer_used; ++i) count += host_timers[i]->alive;
    return count;
}
static void host_tick(unsigned ms)
{
    uint64_t until = host_now + ms;
    host_flush_deletes();
    for (;;) {
        lv_timer_t *next = NULL;
        for (unsigned i = 0; i < host_timer_used; ++i) {
            lv_timer_t *t = host_timers[i];
            if (t->alive && t->due <= until && (!next || t->due < next->due)) next = t;
        }
        if (!next) break;
        host_now = next->due;
        display_lock();
        next->cb(next);
        if (next->alive) {
            if (next->repeat > 0 && --next->repeat == 0) next->alive = false;
            else next->due = host_now + next->period;
        }
        display_unlock();
        host_flush_deletes();
    }
    host_now = until;
}

typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(n) (n)
#define pdPASS 1
static TickType_t host_task_ticks, host_last_delay;
static void (*host_delay_hook)(void);
static TickType_t xTaskGetTickCount(void) { return host_task_ticks; }
static void vTaskDelay(TickType_t ticks)
{
    host_task_ticks += ticks;
    host_last_delay = ticks;
    if (host_delay_hook) host_delay_hook();
}
static void vTaskDelete(void *task) {}
typedef struct {
    char name[32];
    void (*fn)(void *);
    void *arg;
    bool pending;
} host_task_t;
static host_task_t host_tasks[64];
static unsigned host_task_used;
static bool host_fail_job_alloc, host_fail_task_create;
static void *host_job_malloc(size_t size)
{
    if (host_fail_job_alloc) {
        host_fail_job_alloc = false;
        return NULL;
    }
    return malloc(size);
}
static int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack,
                       void *arg, unsigned priority, void *handle)
{
    if (host_fail_task_create) {
        host_fail_task_create = false;
        return 0;
    }
    assert(host_task_used < 64);
    host_task_t *task = &host_tasks[host_task_used++];
    strlcpy(task->name, name, sizeof(task->name));
    task->fn = fn;
    task->arg = arg;
    task->pending = true;
    return pdPASS;
}
static void host_run_task(const char *name)
{
    assert(host_lock_depth == 0);
    for (unsigned i = 0; i < host_task_used; ++i) {
        host_task_t *task = &host_tasks[i];
        if (task->pending && !strcmp(task->name, name)) {
            task->pending = false;
            task->fn(task->arg);
            assert(host_lock_depth == 0);
            return;
        }
    }
    fprintf(stderr, "Missing worker: %s\n", name);
    assert(false);
}

static lv_obj_t *host_text(lv_obj_t *root, const char *text)
{
    if (!root || root->flags & LV_OBJ_FLAG_HIDDEN) return NULL;
    assert(root->alive);
    if (!strcmp(root->text ? root->text : "", text)) return root;
    for (lv_obj_t *o = root->child; o; o = o->next) {
        lv_obj_t *found = host_text(o, text);
        if (found) return found;
    }
    return NULL;
}
static lv_obj_t *host_button(lv_obj_t *root, const char *text)
{
    if (!root || root->flags & LV_OBJ_FLAG_HIDDEN) return NULL;
    if (root->kind == HOST_BUTTON && root->child && strstr(root->child->text, text)) return root;
    for (lv_obj_t *o = root->child; o; o = o->next) {
        lv_obj_t *found = host_button(o, text);
        if (found) return found;
    }
    return NULL;
}
static void host_click(lv_obj_t *root, const char *text)
{
    lv_obj_t *button = host_button(root, text);
    if (!button) fprintf(stderr, "Missing button: %s\n", text);
    assert(button && !(button->state & LV_STATE_DISABLED));
    display_lock();
    lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
    display_unlock();
}
static lv_obj_t *host_field(lv_obj_t *root, const char *label)
{
    lv_obj_t *field = host_text(root, label);
    assert(field && field->next && field->next->kind == HOST_FIELD);
    return field->next;
}
static void host_fill(lv_obj_t *root, const char *label, const char *value)
{
    display_lock();
    lv_textarea_set_text(host_field(root, label), value);
    display_unlock();
}
