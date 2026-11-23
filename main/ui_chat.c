// Connecting to a node, and the Chat screen while connected: station details,
// a large hold-to-talk button (green: free to talk, red: someone else is
// talking) and Leave.

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "audio.h"
#include "display.h"
#include "el_directory.h"
#include "el_qso.h"
#include "net_wifi.h"
#include "settings.h"
#include "ui_internal.h"

#if UI_HW_KEYBOARD
// The CardputerZero's talk button is one line high.
#define TALK_LABEL LV_SYMBOL_AUDIO "  HOLD SPACE TO TALK"
#define TX_LABEL LV_SYMBOL_AUDIO "  TRANSMITTING"
#define BUSY_LABEL LV_SYMBOL_VOLUME_MAX "  BUSY"
#else
#define TALK_LABEL LV_SYMBOL_AUDIO "  HOLD TO TALK"
#define TX_LABEL LV_SYMBOL_AUDIO "  TRANSMITTING\nrelease to listen"
#define BUSY_LABEL LV_SYMBOL_VOLUME_MAX "  BUSY\nsomeone else is talking"
#endif

#define CONNECT_TIMEOUT_MS 45000

static server_entry_t s_target;
static bool s_connecting;

static lv_obj_t *s_scr, *s_title, *s_name, *s_node, *s_info, *s_volume_value, *s_talk, *s_talk_label;
static char s_info_cache[512];
static char s_peer_call[16], s_description[48];
static uint32_t s_peer_ip;
static bool s_disconnected;

static void chat_show(void);

// ---- connecting ------------------------------------------------------------

static void connect_fail(const char *msg)
{
    display_lock();
    ui_modal_message(msg, UI_COLOR_STOP, true, NULL);
    s_connecting = false;
    display_unlock();
}

// Picks what to hand el_qso_connect(): a fixed host for manual entries,
// otherwise the callsign (or node number) as the directory knows it.
static bool resolve_target(char *out, size_t size, char *err, size_t err_size)
{
    if (s_target.host[0]) {
        struct addrinfo hints = {.ai_family = AF_INET}, *res = NULL;
        if (getaddrinfo(s_target.host, NULL, &hints, &res) != 0 || !res) {
            snprintf(err, err_size, "Couldn't look up\n%s", s_target.host);
            return false;
        }
        strlcpy(out, inet_ntoa(((struct sockaddr_in *)res->ai_addr)->sin_addr), size);
        freeaddrinfo(res);
        return true;
    }

    if (!el_dir_count()) {
        snprintf(err, err_size,
                 "The EchoLink directory hasn't loaded yet.\n\nCheck that your callsign and password are set, "
                 "then try again.");
        return false;
    }
    el_station_t st;
    if (el_dir_find(s_target.call, &st)) {
        strlcpy(out, s_target.call, size);
        return true;
    }
    if (s_target.node) {
        char node[12];
        snprintf(node, sizeof(node), "%lu", (unsigned long)s_target.node);
        if (el_dir_find(node, &st)) {
            strlcpy(out, node, size);
            return true;
        }
    }
    snprintf(err, err_size, "%s isn't online right now.", s_target.call);
    return false;
}

static void connected_continue(void)
{
    s_connecting = false;
    ui_chat_update();
}

static void connect_worker(void *arg)
{
    char target[64], err[160];
    if (!net_wifi_has_ip()) {
        connect_fail("Not connected to Wi-Fi.");
        return;
    }
    if (!resolve_target(target, sizeof(target), err, sizeof(err))) {
        connect_fail(err);
        return;
    }
    esp_err_t e = el_qso_connect(target);
    if (e != ESP_OK) {
        connect_fail(e == ESP_ERR_INVALID_STATE ? "Already in a QSO. Leave it first." : "Couldn't start the connection.");
        return;
    }

    qso_status_t st;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CONNECT_TIMEOUT_MS);
    do {
        vTaskDelay(pdMS_TO_TICKS(200));
        el_qso_get_status(&st);
    } while (st.state == QSO_CONNECTING && (int32_t)(deadline - xTaskGetTickCount()) > 0);

    if (st.state == QSO_CONNECTED) {
        char msg[64];
        snprintf(msg, sizeof(msg), LV_SYMBOL_OK "  Connected!\n%s", s_target.call);
        display_lock();
        ui_modal_message(msg, UI_COLOR_GO_LIT, false, NULL);
        ui_modal_close_after(1000, connected_continue);
        display_unlock();
    } else {
        if (st.state == QSO_CONNECTING) el_qso_disconnect();
        if (!strcmp(st.end_reason, "no answer") || st.state == QSO_CONNECTING) {
            snprintf(err, sizeof(err), "%s didn't answer.\n\nIt may be busy, or not accepting connections.",
                     s_target.call);
        } else {
            snprintf(err, sizeof(err), "Connection to %s ended:\n%s", s_target.call, st.end_reason);
        }
        connect_fail(err);
    }
}

void ui_chat_connect(const server_entry_t *entry)
{
    if (s_connecting) return;
    s_connecting = true;
    s_target = *entry;
    char msg[64];
    snprintf(msg, sizeof(msg), "Connecting to\n%s...", entry->call);
    ui_modal_busy(msg);
    if (!ui_run_async("qso_connect", connect_worker, NULL)) {
        connect_fail("Couldn't start the connection.\nTry again.");
    }
}

// ---- chat screen -----------------------------------------------------------

static void chat_close(void)
{
    ui_home_show();
    if (s_scr) lv_obj_delete_async(s_scr);
    s_scr = NULL;
    s_title = s_name = s_node = s_info = s_volume_value = s_talk = s_talk_label = NULL;
}

static void leave_clicked(lv_event_t *e)
{
    el_qso_disconnect();
    chat_close();
}

static void talk_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        qso_status_t st;
        el_qso_get_status(&st);
        // Half duplex: don't key up over someone else.
        if (st.state == QSO_CONNECTED && !st.receiving) el_qso_set_ptt(true);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        el_qso_set_ptt(false);
    }
}

static void set_talk(lv_color_t color, const char *text)
{
    lv_obj_set_style_bg_color(s_talk, color, 0);
    lv_obj_set_style_bg_color(s_talk, color, LV_STATE_PRESSED);
    lv_label_set_text(s_talk_label, text);
}

static void volume_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        int volume = lv_slider_get_value(lv_event_get_target(e));
        g_settings.volume = volume;
        audio_set_volume(volume);
        lv_label_set_text_fmt(s_volume_value, "%d%%", volume);
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST ||
        (UI_HW_KEYBOARD && code == LV_EVENT_VALUE_CHANGED)) {
        if (settings_save() != ESP_OK) {
            ui_modal_message("Couldn't save the volume setting.", UI_COLOR_STOP, true, NULL);
        }
    }
}

static void update_identity(const qso_status_t *st)
{
    char call[sizeof(s_peer_call)];
    struct in_addr addr = {.s_addr = st->remote_ip};
    strlcpy(call, st->remote_call[0] ? st->remote_call : inet_ntoa(addr), sizeof(call));
    if (strcmp(call, s_peer_call) || st->remote_ip != s_peer_ip) {
        strlcpy(s_peer_call, call, sizeof(s_peer_call));
        s_peer_ip = st->remote_ip;
        lv_label_set_text(s_title, call);

        uint32_t node = 0;
        s_description[0] = 0;
        el_station_t station;
        if (!strcasecmp(call, s_target.call)) {
            node = s_target.node;
            strlcpy(s_description, s_target.desc, sizeof(s_description));
        } else if (el_dir_find(call, &station)) {
            node = station.node;
            strlcpy(s_description, station.desc, sizeof(s_description));
        }
        if (node) {
            lv_label_set_text_fmt(s_node, "Node %lu", (unsigned long)node);
            lv_obj_remove_flag(s_node, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_node, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_label_set_text(s_name, st->remote_name[0] ? st->remote_name : s_description);
}

void ui_chat_update(void)
{
    if (!s_scr && (s_connecting || !ui_home_is_active() || ui_account_is_open() || ui_modal_is_open())) return;

    qso_status_t st;
    el_qso_get_status(&st);
    if (!s_scr) {
        if (st.state != QSO_CONNECTED) return;
        chat_show();
    }
    if (s_disconnected) return;

    if (st.state != QSO_CONNECTED) {
        s_disconnected = true;
        char msg[96];
        snprintf(msg, sizeof(msg), "Disconnected from %s\n(%s)", s_peer_call,
                 st.end_reason[0] ? st.end_reason : "link lost");
        ui_modal_message(msg, UI_COLOR_WARN, true, chat_close);
        return;
    }

    if (st.ptt) {
        set_talk(UI_COLOR_GO_LIT, TX_LABEL);
    } else if (st.receiving) {
        set_talk(UI_COLOR_STOP, BUSY_LABEL);
    } else {
        set_talk(UI_COLOR_GO, TALK_LABEL);
    }

    update_identity(&st);

    char info[sizeof(s_info_cache)];
    el_qso_get_info(info, sizeof(info));
    if (strcmp(info, s_info_cache)) {
        strlcpy(s_info_cache, info, sizeof(s_info_cache));
        lv_label_set_text(s_info, info[0] ? info : "Waiting for station info...");
    }
}

static void chat_show(void)
{
    if (s_scr) return;
    s_disconnected = false;
    s_info_cache[0] = 0;
    s_peer_call[0] = 0;
    s_peer_ip = 0;

    lv_obj_t *col = ui_screen_create(&s_scr, NULL);
#if !UI_COMPACT
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
#endif

    s_title = ui_title(col, "");
    s_name = ui_label(col, "", UI_FONT, lv_color_white());
    s_node = ui_label(col, "", UI_FONT_SMALL, UI_COLOR_ACCENT);

    // Station info the remote sends on connect (users, location, ...).
    lv_obj_t *box = lv_obj_create(col);
    lv_obj_set_width(box, LV_PCT(100));
#if UI_COMPACT
    lv_obj_set_height(box, UI_COMPACT_INFO_H);
#else
    lv_obj_set_flex_grow(box, 1);
#endif
    lv_obj_set_style_bg_color(box, UI_COLOR_PANEL, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, UI_RADIUS, 0);
    lv_obj_set_style_pad_all(box, UI_PAD, 0);
    s_info = ui_label(box, "Waiting for station info...", UI_FONT_SMALL, UI_COLOR_MUTED);

    lv_obj_t *volume_row = lv_obj_create(col);
    lv_obj_set_size(volume_row, LV_PCT(100), UI_FIELD_H);
    lv_obj_set_flex_flow(volume_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(volume_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(volume_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(volume_row, 0, 0);
    lv_obj_set_style_pad_all(volume_row, 0, 0);
    lv_obj_set_style_pad_column(volume_row, UI_GAP, 0);
    lv_obj_remove_flag(volume_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *volume_icon = lv_label_create(volume_row);
    lv_label_set_text(volume_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_font(volume_icon, UI_FONT, 0);
    lv_obj_set_style_text_color(volume_icon, UI_COLOR_MUTED, 0);
    lv_obj_t *volume = lv_slider_create(volume_row);
    lv_slider_set_range(volume, 0, 100);
    lv_slider_set_value(volume, g_settings.volume, LV_ANIM_OFF);
    lv_obj_set_height(volume, UI_SLIDER_H);
    lv_obj_set_flex_grow(volume, 1);
    lv_obj_add_event_cb(volume, volume_event, LV_EVENT_ALL, NULL);
    s_volume_value = lv_label_create(volume_row);
    lv_label_set_text_fmt(s_volume_value, "%u%%", g_settings.volume);
    lv_obj_set_width(s_volume_value, UI_VOLUME_VALUE_W);
    lv_obj_set_style_text_font(s_volume_value, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_volume_value, UI_COLOR_MUTED, 0);
    lv_obj_set_style_text_align(s_volume_value, LV_TEXT_ALIGN_RIGHT, 0);

    s_talk = lv_button_create(col);
    lv_obj_set_size(s_talk, LV_PCT(100), UI_CHAT_TALK_H);
    lv_obj_set_style_radius(s_talk, UI_RADIUS * 3, 0);
    lv_obj_add_event_cb(s_talk, talk_event, LV_EVENT_ALL, NULL);
    s_talk_label = lv_label_create(s_talk);
    lv_obj_set_style_text_font(s_talk_label, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_align(s_talk_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_talk_label);
    set_talk(UI_COLOR_GO, TALK_LABEL);

    ui_button(col, LV_SYMBOL_CLOSE "  Leave", UI_COLOR_PANEL, leave_clicked, NULL);

    lv_screen_load(s_scr);
}
