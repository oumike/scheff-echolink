// EchoLink account: callsign and password (plus the optional name and
// location the directory shows). Part of onboarding after Wi-Fi, and editable
// later from Home.

#include <string.h>
#include "onboard.h"
#include "settings.h"
#include "ui_internal.h"

static lv_obj_t *s_root, *s_call, *s_pass, *s_name, *s_loc;
static bool s_onboarding;

static void upper_focused(lv_event_t *e)
{
    lv_keyboard_set_mode(lv_event_get_user_data(e), LV_KEYBOARD_MODE_TEXT_UPPER);
}

static void lower_focused(lv_event_t *e)
{
    lv_keyboard_set_mode(lv_event_get_user_data(e), LV_KEYBOARD_MODE_TEXT_LOWER);
}

static void build_fields(lv_obj_t *col, lv_obj_t *kb)
{
    s_call = ui_field(col, "Callsign", "e.g. N0CALL", kb);
    lv_textarea_set_max_length(s_call, sizeof(g_settings.callsign) - 1);
    lv_obj_add_event_cb(s_call, upper_focused, LV_EVENT_FOCUSED, kb);

    // Masked, unlike the Wi-Fi password: this one is an account secret.
    s_pass = ui_field(col, "EchoLink password", "Password", kb);
    lv_textarea_set_password_mode(s_pass, true);
    lv_textarea_set_max_length(s_pass, sizeof(g_settings.password) - 1);
    lv_obj_add_event_cb(s_pass, lower_focused, LV_EVENT_FOCUSED, kb);

    s_name = ui_field(col, "Your name (optional)", "e.g. Mike", kb);
    lv_textarea_set_max_length(s_name, sizeof(g_settings.name) - 1);
    lv_obj_add_event_cb(s_name, lower_focused, LV_EVENT_FOCUSED, kb);

    s_loc = ui_field(col, "Location (optional)", "Shown in the directory", kb);
    lv_textarea_set_max_length(s_loc, sizeof(g_settings.location) - 1);
    lv_obj_add_event_cb(s_loc, lower_focused, LV_EVENT_FOCUSED, kb);

    lv_textarea_set_text(s_call, g_settings.callsign);
    lv_textarea_set_text(s_pass, g_settings.password);
    lv_textarea_set_text(s_name, g_settings.name);
    lv_textarea_set_text(s_loc, g_settings.location);
}

// Validates and applies the form. Returns false (with a message up) if the
// form needs fixing.
static bool apply(void)
{
    const char *err = NULL;
    bool onboarding = s_onboarding;
    esp_err_t e = onboard_save_account(lv_textarea_get_text(s_call), lv_textarea_get_text(s_pass),
                                       lv_textarea_get_text(s_name), lv_textarea_get_text(s_loc), &err);
    if (e != ESP_OK) {
        ui_modal_message(err ? err : "Couldn't save.", UI_COLOR_WARN, true, NULL);
        return false;
    }
    // Onboarding moves on by itself (ui.c); an edit just closes.
    if (!onboarding) ui_account_destroy();
    return true;
}

void ui_account_destroy(void)
{
    lv_obj_t *root = s_root;
    s_root = NULL;
    if (!root) return;
    if (s_onboarding) {
        lv_obj_delete_async(root);  // a screen: deleted once the next one is up
    } else {
        lv_obj_delete(root);  // an overlay on the top layer
    }
}

bool ui_account_is_open(void)
{
    return s_root != NULL;
}

// Saving advances onboarding (ui.c) and closes this form.
static void save_clicked(lv_event_t *e)
{
    apply();
}

static void cancel_clicked(lv_event_t *e)
{
    ui_account_destroy();
}

static void web_clicked(lv_event_t *e)
{
    ui_web_access_show();
}

static lv_obj_t *spacer(lv_obj_t *col)
{
    lv_obj_t *sp = lv_obj_create(col);
    lv_obj_set_width(sp, LV_PCT(100));
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sp, 0, 0);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);
    return sp;
}

void ui_account_onboard(void)
{
    lv_obj_t *kb;
    lv_obj_t *col = ui_screen_create(&s_root, &kb);
    s_onboarding = true;

    ui_title(col, "EchoLink Account");
    ui_label(col,
             "Enter the callsign and password you use with EchoLink. The callsign must already be validated "
             "at echolink.org.",
             UI_FONT_SMALL, UI_COLOR_MUTED);
    ui_button(col, "Web access", UI_COLOR_PANEL, web_clicked, NULL);
    build_fields(col, kb);
    spacer(col);
    ui_button(col, "Continue", UI_COLOR_GO, save_clicked, NULL);
    ui_web_hint_update(ui_label(col, "", UI_FONT_SMALL, UI_COLOR_MUTED));
    lv_screen_load(s_root);
}

void ui_account_edit(void)
{
    if (s_root) return;
    lv_obj_t *kb;
    lv_obj_t *col = ui_layout_create(lv_layer_top(), &s_root, &kb);
    s_onboarding = false;

    ui_title(col, "EchoLink Account");
    ui_label(col, "Changes take effect right away: the device logs in to the directory again.", UI_FONT_SMALL,
             UI_COLOR_MUTED);
    ui_button(col, "Web access", UI_COLOR_PANEL, web_clicked, NULL);
    build_fields(col, kb);
    spacer(col);
    ui_button(col, LV_SYMBOL_SAVE "  Save", UI_COLOR_GO, save_clicked, NULL);
    ui_button(col, "Cancel", UI_COLOR_PANEL, cancel_clicked, NULL);
}
