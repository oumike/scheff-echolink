#pragma once

// Onboarding state shared by the touch UI and the web config: Wi-Fi first,
// then the EchoLink account. Either side can complete a step; the other
// follows along through the change callbacks.

#include <stdbool.h>
#include "esp_err.h"

bool onboard_wifi_done(void);
bool onboard_account_done(void);
static inline bool onboard_complete(void) { return onboard_wifi_done() && onboard_account_done(); }

// Stores a network that was just joined successfully and schedules a reboot.
esp_err_t onboard_save_wifi(const char *ssid, const char *pass);

// Validates, saves and applies the account (callsign upper-cased; name and
// location optional). Returns ESP_ERR_INVALID_ARG with `err` filled in when
// the callsign or password is missing.
esp_err_t onboard_save_account(const char *call, const char *pass, const char *name, const char *location,
                               const char **err);

// Refreshes onboarding consumers after settings are restored in bulk.
void onboard_settings_changed(void);

// Called (from the task that made the change) after any step is saved.
void onboard_set_ui_cb(void (*cb)(void));
void onboard_set_web_cb(void (*cb)(void));
