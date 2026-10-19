// Home: search, or pick a letter (or "*" for favorites), then a node from the
// list. Tap a row to connect; its gear opens Edit / Favorite.
// "Create Manual" adds a node by hand.

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_heap_caps.h"
#include "lwip/inet.h"
#include "display.h"
#include "el_directory.h"
#include "net_wifi.h"
#include "server_list.h"
#include "settings.h"
#include "ui_internal.h"

#define BUCKET_MAX    3000
#define PAGE_SIZE     20

// Dropdown order: favorites first, then A-Z, then digits.
static const char LETTERS[] = "*ABCDEFGHIJKLMNOPQRSTUVWXYZ#";
#define SEARCH_DEBOUNCE_MS 300

static lv_obj_t *s_scr, *s_callsign, *s_status, *s_web, *s_letter, *s_list, *s_search, *s_kb;
static lv_timer_t *s_search_timer;
static server_entry_t s_menu_entry;  // the entry a long press opened the menu for
static lv_timer_t *s_status_timer;
static server_entry_t *s_entries;
static size_t s_entry_count;

// Paging: PAGE_SIZE entries at a time, with Prev/Next under the list.
static lv_obj_t *s_pager, *s_prev, *s_next, *s_page_label;
static size_t s_page;

// Long press: a timer started on press, cancelled on release or scroll.

// Create/edit form (full screen, on the top layer).
static lv_obj_t *s_form, *s_f_call, *s_f_node, *s_f_desc, *s_f_host;
static char s_form_orig_call[16];
static server_entry_t s_form_base;

static void refresh_list(void);

// ---- status line -----------------------------------------------------------

static void update_status(lv_timer_t *t)
{
    ui_web_hint_update(s_web);
    if (!net_wifi_has_ip()) {
        lv_label_set_text_fmt(s_status, LV_SYMBOL_WIFI " Connecting to %s...", g_settings.wifi_ssid);
        lv_obj_set_style_text_color(s_status, UI_COLOR_WARN, 0);
    } else if (!g_settings.callsign[0] || !g_settings.password[0]) {
        lv_label_set_text(s_status, "No EchoLink account yet: tap Edit to set your callsign.");
        lv_obj_set_style_text_color(s_status, UI_COLOR_WARN, 0);
    } else if (!el_dir_logged_in()) {
        lv_label_set_text_fmt(s_status, "Logging in to EchoLink as %s...", g_settings.callsign);
        lv_obj_set_style_text_color(s_status, UI_COLOR_WARN, 0);
    } else if (!server_list_count()) {
        lv_label_set_text_fmt(s_status, LV_SYMBOL_OK " %s online  -  %u stations in directory, no servers saved yet",
                              g_settings.callsign, (unsigned)el_dir_count());
        lv_obj_set_style_text_color(s_status, UI_COLOR_WARN, 0);
    } else {
        lv_label_set_text_fmt(s_status, LV_SYMBOL_OK " %s online  -  %u servers saved", g_settings.callsign,
                              (unsigned)server_list_count());
        lv_obj_set_style_text_color(s_status, UI_COLOR_MUTED, 0);
    }
}

// ---- list ------------------------------------------------------------------

static char selected_bucket(void)
{
    return LETTERS[lv_dropdown_get_selected(s_letter)];
}

static void open_form(const server_entry_t *entry);

static void entry_menu_chosen(int index)
{
    if (index == 0) {
        open_form(&s_menu_entry);
    } else if (index == 1) {
        esp_err_t err = server_list_set_favorite(s_menu_entry.call, !s_menu_entry.favorite);
        if (err != ESP_OK) ui_modal_message("Couldn't save the change.", UI_COLOR_STOP, true, NULL);
    }
}

// Copies the entry: the list (and s_entries) may be rebuilt while it's open.
static void open_entry_menu(const server_entry_t *entry)
{
    s_menu_entry = *entry;
    const char *items[] = {LV_SYMBOL_EDIT "  Edit", entry->favorite ? LV_SYMBOL_CLOSE "  Unfavorite" : LV_SYMBOL_PLUS "  Favorite"};
    const lv_color_t colors[] = {UI_COLOR_ACCENT, UI_COLOR_WARN};
    ui_modal_menu(entry->call, items, colors, 2, entry_menu_chosen);
}

static void item_clicked(lv_event_t *e)
{
    ui_chat_connect(&s_entries[(size_t)(intptr_t)lv_event_get_user_data(e)]);
}

static void gear_clicked(lv_event_t *e)
{
    open_entry_menu(&s_entries[(size_t)(intptr_t)lv_event_get_user_data(e)]);
}

static const char *type_name(server_type_t t)
{
    return t == SERVER_CONFERENCE ? "Conference" : t == SERVER_LINK ? "Link" : t == SERVER_REPEATER ? "Repeater" : "Node";
}

static size_t page_count(void)
{
    return (s_entry_count + PAGE_SIZE - 1) / PAGE_SIZE;
}

static void update_pager(void)
{
    size_t pages = page_count();
    if (pages <= 1) {
        lv_obj_add_flag(s_pager, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_pager, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(s_page_label, "Page %u of %u", (unsigned)(s_page + 1), (unsigned)pages);
    if (s_page == 0) lv_obj_add_state(s_prev, LV_STATE_DISABLED);
    else lv_obj_remove_state(s_prev, LV_STATE_DISABLED);
    if (s_page + 1 >= pages) lv_obj_add_state(s_next, LV_STATE_DISABLED);
    else lv_obj_remove_state(s_next, LV_STATE_DISABLED);
}

static void refresh_list(void)
{
    lv_obj_clean(s_list);
    char bucket = selected_bucket();
    const char *query = lv_textarea_get_text(s_search);
    bool searching = query[0] != 0;
    s_entry_count = searching ? server_list_search(query, s_entries, BUCKET_MAX)
                              : server_list_get_bucket(bucket, s_entries, BUCKET_MAX);
    // Keep the page where it was (after an edit, say), unless the list shrank.
    if (s_page >= page_count()) s_page = page_count() ? page_count() - 1 : 0;
    update_pager();
    lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);

    if (!s_entry_count) {
        lv_obj_t *l;
        if (searching) {
            char msg[80];
            snprintf(msg, sizeof(msg), "No stations match \"%s\".", query);
            l = ui_label(s_list, msg, UI_FONT_SMALL, UI_COLOR_MUTED);
        } else if (bucket == SERVER_BUCKET_FAVORITES) {
            l = ui_label(s_list, "No favorites yet.\n\nHold a station for 3 seconds to add it.", UI_FONT_SMALL,
                         UI_COLOR_MUTED);
        } else if (!server_list_count()) {
            l = ui_label(s_list,
                         "No servers yet.\n\nThe list is saved after the first EchoLink directory login. "
                         "You can also add one with Create Manual.",
                         UI_FONT_SMALL, UI_COLOR_MUTED);
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), "No servers starting with %s.", bucket == '#' ? "a digit" : (char[]){bucket, 0});
            l = ui_label(s_list, msg, UI_FONT_SMALL, UI_COLOR_MUTED);
        }
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(l, UI_PAD * 3, 0);
        return;
    }

    size_t end = (s_page + 1) * PAGE_SIZE < s_entry_count ? (s_page + 1) * PAGE_SIZE : s_entry_count;
    for (size_t i = s_page * PAGE_SIZE; i < end; i++) {
        const server_entry_t *en = &s_entries[i];
        // Row: tap anywhere to connect; the gear on the right opens the menu.
        lv_obj_t *b = lv_button_create(s_list);
        lv_obj_set_size(b, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(b, UI_COLOR_PANEL, 0);
        lv_obj_set_style_radius(b, UI_RADIUS, 0);
        lv_obj_set_style_pad_all(b, UI_PAD, 0);
        lv_obj_set_style_pad_column(b, UI_GAP, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(b, item_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        // Text column; not clickable, so taps on it reach the row.
        lv_obj_t *txt = lv_obj_create(b);
        lv_obj_set_height(txt, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(txt, 1);
        lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(txt, 0, 0);
        lv_obj_set_style_pad_row(txt, 4, 0);
        lv_obj_set_style_bg_opa(txt, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(txt, 0, 0);
        lv_obj_remove_flag(txt, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *gear = lv_button_create(b);
        lv_obj_set_size(gear, UI_ICON_BUTTON_SIZE, UI_ICON_BUTTON_SIZE);
        lv_obj_set_style_bg_color(gear, UI_COLOR_BG, 0);
        lv_obj_set_style_radius(gear, UI_RADIUS, 0);
        lv_obj_add_event_cb(gear, gear_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *gl = lv_label_create(gear);
        lv_label_set_text(gl, LV_SYMBOL_SETTINGS);
        lv_obj_set_style_text_font(gl, UI_FONT, 0);
        lv_obj_center(gl);

        lv_obj_t *call = lv_label_create(txt);
        lv_label_set_text(call, en->call);
        lv_obj_set_style_text_font(call, UI_FONT, 0);
        if (en->favorite) lv_obj_set_style_text_color(call, UI_COLOR_WARN, 0);

        lv_obj_t *meta = lv_label_create(txt);
        char node[24] = "";
        if (en->node) snprintf(node, sizeof(node), "  -  node %lu", (unsigned long)en->node);
        lv_label_set_text_fmt(meta, "%s%s%s%s", en->favorite ? "Favorite  -  " : "", type_name(en->type), node,
                              en->manual ? "  -  manual" : "");
        lv_obj_set_style_text_font(meta, UI_FONT_SMALL, 0);
        lv_obj_set_style_text_color(meta, UI_COLOR_ACCENT, 0);

        if (en->desc[0]) {
            lv_obj_t *desc = lv_label_create(txt);
            lv_label_set_text(desc, en->desc);
            lv_obj_set_width(desc, LV_PCT(100));
            lv_label_set_long_mode(desc, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_font(desc, UI_FONT_SMALL, 0);
            lv_obj_set_style_text_color(desc, UI_COLOR_MUTED, 0);
        }
    }
}

static void letter_changed(lv_event_t *e)
{
    lv_textarea_set_text(s_search, "");  // a letter pick ends the search
    s_page = 0;
    refresh_list();
}

static void search_timer_cb(lv_timer_t *t)
{
    s_search_timer = NULL;  // one-shot
    s_page = 0;
    refresh_list();
}

// Typing restarts a short timer, so the list rebuilds once per pause rather
// than on every keystroke.
static void search_changed(lv_event_t *e)
{
    if (s_search_timer) lv_timer_delete(s_search_timer);
    s_search_timer = lv_timer_create(search_timer_cb, SEARCH_DEBOUNCE_MS, NULL);
    lv_timer_set_repeat_count(s_search_timer, 1);
}

static void search_clear(lv_event_t *e)
{
    lv_textarea_set_text(s_search, "");
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_state(s_search, LV_STATE_FOCUSED);
}

static void page_clicked(lv_event_t *e)
{
    int step = (int)(intptr_t)lv_event_get_user_data(e);
    if (step < 0 && s_page > 0) s_page--;
    if (step > 0 && s_page + 1 < page_count()) s_page++;
    refresh_list();
}

// Shows the page holding `call` (after a save, which may move it).
static void show_entry(const char *call)
{
    const char *p = strchr(LETTERS, server_list_bucket(call));
    lv_dropdown_set_selected(s_letter, p ? (uint32_t)(p - LETTERS) : 1);
    lv_textarea_set_text(s_search, "");
    s_page = 0;
    refresh_list();
    for (size_t i = 0; i < s_entry_count; i++) {
        if (!strcasecmp(s_entries[i].call, call)) {
            if (i / PAGE_SIZE != s_page) {
                s_page = i / PAGE_SIZE;
                refresh_list();
            }
            break;
        }
    }
}

// Runs on whichever task changed the list (directory task or LVGL itself).
static void list_changed(void)
{
    if (!display_lock()) return;
    if (s_list) refresh_list();
    display_unlock();
}

// ---- create / edit form ----------------------------------------------------

static void close_form(void)
{
    if (s_form) lv_obj_delete(s_form);
    s_form = NULL;
}

static void form_cancel(lv_event_t *e)
{
    close_form();
}

static void form_save(lv_event_t *e)
{
    server_entry_t en = s_form_base;
    strlcpy(en.call, lv_textarea_get_text(s_f_call), sizeof(en.call));
    for (char *c = en.call; *c; c++) *c = toupper((unsigned char)*c);
    strlcpy(en.desc, lv_textarea_get_text(s_f_desc), sizeof(en.desc));
    strlcpy(en.host, lv_textarea_get_text(s_f_host), sizeof(en.host));
    const char *node = lv_textarea_get_text(s_f_node);
    en.node = node[0] ? strtoul(node, NULL, 10) : 0;

    if (!en.call[0]) {
        ui_modal_message("A callsign is required.", UI_COLOR_WARN, true, NULL);
        return;
    }
    if (!en.node && !en.host[0]) {
        ui_modal_message("Enter a node number, or a host / IP address to connect to.", UI_COLOR_WARN, true, NULL);
        return;
    }
    en.type = SERVER_OTHER;  // re-derived from the callsign
    if (!s_form_orig_call[0]) en.manual = true;

    esp_err_t err = server_list_put(s_form_orig_call, &en);
    if (err != ESP_OK) {
        char msg[80];
        snprintf(msg, sizeof(msg), "Couldn't save:\n%s", esp_err_to_name(err));
        ui_modal_message(msg, UI_COLOR_STOP, true, NULL);
        return;
    }
    close_form();
    show_entry(en.call);
}

static void node_field_focused(lv_event_t *e)
{
    lv_keyboard_set_mode(lv_event_get_user_data(e), LV_KEYBOARD_MODE_NUMBER);
}

static void text_field_focused(lv_event_t *e)
{
    lv_keyboard_set_mode(lv_event_get_user_data(e), LV_KEYBOARD_MODE_TEXT_UPPER);
}

static void open_form(const server_entry_t *entry)
{
    close_form();
    lv_obj_t *kb;
    lv_obj_t *col = ui_layout_create(lv_layer_top(), &s_form, &kb);

    memset(&s_form_base, 0, sizeof(s_form_base));
    s_form_orig_call[0] = 0;
    if (entry) {
        s_form_base = *entry;
        strlcpy(s_form_orig_call, entry->call, sizeof(s_form_orig_call));
    }

    ui_title(col, entry ? "Edit Server" : "New Server");
    if (entry && !entry->manual) {
        ui_label(col, "Changes are saved to your list only; the default list is kept as a backup.",
                 UI_FONT_SMALL, UI_COLOR_MUTED);
    }

    s_f_call = ui_field(col, "Callsign", "e.g. *ECHOTEST* or W1ABC-R", kb);
    lv_textarea_set_max_length(s_f_call, sizeof(s_form_base.call) - 1);
    lv_obj_add_event_cb(s_f_call, text_field_focused, LV_EVENT_FOCUSED, kb);

    s_f_node = ui_field(col, "Node number", "e.g. 9999", kb);
    lv_textarea_set_accepted_chars(s_f_node, "0123456789");
    lv_textarea_set_max_length(s_f_node, 7);
    lv_obj_add_event_cb(s_f_node, node_field_focused, LV_EVENT_FOCUSED, kb);

    s_f_desc = ui_field(col, "Description", "Optional", kb);
    lv_textarea_set_max_length(s_f_desc, sizeof(s_form_base.desc) - 1);
    lv_obj_add_event_cb(s_f_desc, text_field_focused, LV_EVENT_FOCUSED, kb);

    s_f_host = ui_field(col, "Host or IP address", "Optional: connect directly", kb);
    lv_textarea_set_max_length(s_f_host, sizeof(s_form_base.host) - 1);
    lv_obj_add_event_cb(s_f_host, text_field_focused, LV_EVENT_FOCUSED, kb);

    if (entry) {
        lv_textarea_set_text(s_f_call, entry->call);
        if (entry->node) {
            char n[12];
            snprintf(n, sizeof(n), "%lu", (unsigned long)entry->node);
            lv_textarea_set_text(s_f_node, n);
        }
        lv_textarea_set_text(s_f_desc, entry->desc);
        lv_textarea_set_text(s_f_host, entry->host);
    }

    // Push the buttons to the bottom when the fields don't fill the screen.
    lv_obj_t *spacer = lv_obj_create(col);
    lv_obj_set_width(spacer, LV_PCT(100));
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_remove_flag(spacer, LV_OBJ_FLAG_CLICKABLE);

    ui_button(col, LV_SYMBOL_SAVE "  Save", UI_COLOR_GO, form_save, NULL);
    ui_button(col, "Cancel", UI_COLOR_PANEL, form_cancel, NULL);
}

static void edit_account_clicked(lv_event_t *e)
{
    ui_account_edit();
}

void ui_home_refresh_account(void)
{
    if (!s_callsign) return;
    lv_label_set_text(s_callsign, g_settings.callsign[0] ? g_settings.callsign : "No callsign");
    update_status(NULL);
}

static void create_clicked(lv_event_t *e)
{
    open_form(NULL);
}

// ---- screen ----------------------------------------------------------------

void ui_home_show(void)
{
    if (!s_scr) {
        s_entries = heap_caps_calloc(BUCKET_MAX, sizeof(*s_entries), MALLOC_CAP_SPIRAM);

        lv_obj_t *col = ui_screen_create(&s_scr, &s_kb);
    #if !UI_COMPACT
        lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    #endif

        // Callsign header with a small Edit button under it.
        s_callsign = ui_title(col, "");
        lv_obj_t *edit = ui_button(col, LV_SYMBOL_EDIT "  Edit", UI_COLOR_PANEL, edit_account_clicked, NULL);
        lv_obj_set_size(edit, LV_SIZE_CONTENT, UI_EDIT_BUTTON_H);
        lv_obj_set_style_pad_hor(edit, UI_PAD * 2, 0);
        lv_obj_set_style_text_font(lv_obj_get_child(edit, 0), UI_FONT_SMALL, 0);
        s_status = ui_label(col, "", UI_FONT_SMALL, UI_COLOR_MUTED);
        s_web = ui_label(col, "", UI_FONT_SMALL, UI_COLOR_MUTED);

        // Search bar with a clear button.
        lv_obj_t *row = lv_obj_create(col);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_column(row, UI_GAP, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        s_search = lv_textarea_create(row);
        lv_textarea_set_one_line(s_search, true);
        lv_textarea_set_placeholder_text(s_search, "Search stations");
        lv_textarea_set_max_length(s_search, 24);
        lv_obj_set_flex_grow(s_search, 1);
        lv_obj_set_style_text_font(s_search, UI_FONT, 0);
        lv_obj_set_style_radius(s_search, UI_RADIUS, 0);
        ui_field_attach_keyboard(s_search, s_kb);
        lv_obj_add_event_cb(s_search, search_changed, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_t *clr = ui_button(row, LV_SYMBOL_CLOSE, UI_COLOR_PANEL, search_clear, NULL);
        lv_obj_set_size(clr, UI_CLEAR_BUTTON_W, UI_FIELD_H);

        s_letter = lv_dropdown_create(col);
        lv_dropdown_set_options(s_letter,
                                "*\nA\nB\nC\nD\nE\nF\nG\nH\nI\nJ\nK\nL\nM\nN\nO\nP\nQ\nR\nS\nT\nU\nV\nW\nX\nY\nZ\n0-9");
        // Open on favorites when there are some, otherwise on A.
        lv_dropdown_set_selected(s_letter, server_list_favorite_count() ? 0 : 1);
        lv_obj_set_width(s_letter, LV_PCT(100));
        lv_obj_set_style_text_font(s_letter, UI_FONT, 0);
        lv_obj_set_style_radius(s_letter, UI_RADIUS, 0);
        lv_obj_set_style_text_font(lv_dropdown_get_list(s_letter), UI_FONT, 0);
        lv_obj_set_style_max_height(lv_dropdown_get_list(s_letter), DISPLAY_V_RES / 2, 0);
        lv_obj_add_event_cb(s_letter, letter_changed, LV_EVENT_VALUE_CHANGED, NULL);

        s_list = lv_obj_create(col);
        lv_obj_set_width(s_list, LV_PCT(100));
    #if UI_COMPACT
        lv_obj_set_height(s_list, 104);
    #else
        lv_obj_set_flex_grow(s_list, 1);
    #endif
        lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(s_list, 0, 0);
        lv_obj_set_style_pad_row(s_list, UI_GAP / 2, 0);
        lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_list, 0, 0);

        s_pager = lv_obj_create(col);
        lv_obj_set_size(s_pager, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(s_pager, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(s_pager, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(s_pager, 0, 0);
        lv_obj_set_style_bg_opa(s_pager, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_pager, 0, 0);
        lv_obj_remove_flag(s_pager, LV_OBJ_FLAG_SCROLLABLE);
        s_prev = ui_button(s_pager, LV_SYMBOL_LEFT " Prev", UI_COLOR_PANEL, page_clicked, (void *)(intptr_t)-1);
        lv_obj_set_width(s_prev, LV_PCT(30));
        s_page_label = lv_label_create(s_pager);
        lv_obj_set_style_text_font(s_page_label, UI_FONT_SMALL, 0);
        lv_obj_set_style_text_color(s_page_label, UI_COLOR_MUTED, 0);
        s_next = ui_button(s_pager, "Next " LV_SYMBOL_RIGHT, UI_COLOR_PANEL, page_clicked, (void *)(intptr_t)1);
        lv_obj_set_width(s_next, LV_PCT(30));

        ui_button(col, LV_SYMBOL_PLUS "  Create Manual", UI_COLOR_ACCENT, create_clicked, NULL);

        server_list_set_changed_cb(list_changed);
        s_status_timer = lv_timer_create(update_status, 1000, NULL);
        ui_home_refresh_account();
        refresh_list();
    }
    lv_screen_load(s_scr);
}
