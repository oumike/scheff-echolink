#include "onboard.h"

#include <ctype.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "el_directory.h"
#include "el_qso.h"
#include "settings.h"

static const char *TAG = "onboard";

#define WIFI_RESTART_DELAY_MS 3000

static void (*s_ui_cb)(void);
static void (*s_web_cb)(void);
static bool s_restart_pending;

static void wifi_restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(WIFI_RESTART_DELAY_MS));
    esp_restart();
}

static void notify(void)
{
    if (s_ui_cb) s_ui_cb();
    if (s_web_cb) s_web_cb();
}

bool onboard_wifi_done(void)
{
    return g_settings.wifi_ssid[0] != 0;
}

bool onboard_account_done(void)
{
    return g_settings.callsign[0] && g_settings.password[0];
}

esp_err_t onboard_save_wifi(const char *ssid, const char *pass)
{
    strlcpy(g_settings.wifi_ssid, ssid, sizeof(g_settings.wifi_ssid));
    strlcpy(g_settings.wifi_pass, pass ? pass : "", sizeof(g_settings.wifi_pass));
    esp_err_t err = settings_save();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not save Wi-Fi settings: %s", esp_err_to_name(err));
        return err;
    }
    notify();
    if (!s_restart_pending) {
        s_restart_pending = true;
        ESP_LOGI(TAG, "Wi-Fi onboarding complete; restarting in %d ms", WIFI_RESTART_DELAY_MS);
        if (xTaskCreate(wifi_restart_task, "wifi_restart", 2048, NULL, 3, NULL) != pdPASS) esp_restart();
    }
    return ESP_OK;
}

esp_err_t onboard_save_account(const char *call, const char *pass, const char *name, const char *location,
                               const char **err)
{
    char up[sizeof(g_settings.callsign)];
    strlcpy(up, call ? call : "", sizeof(up));
    for (char *c = up; *c; c++) *c = toupper((unsigned char)*c);

    if (!up[0] || strchr(up, ' ')) {
        if (err) *err = "Enter your callsign.";
        return ESP_ERR_INVALID_ARG;
    }
    if (!pass || !pass[0]) {
        if (err) *err = "Enter your EchoLink password.";
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(g_settings.callsign, up, sizeof(g_settings.callsign));
    strlcpy(g_settings.password, pass, sizeof(g_settings.password));
    strlcpy(g_settings.name, name ? name : "", sizeof(g_settings.name));
    strlcpy(g_settings.location, location ? location : "", sizeof(g_settings.location));
    esp_err_t e = settings_save();
    if (e != ESP_OK) {
        if (err) *err = "Couldn't save the settings.";
        return e;
    }
    el_qso_update_identity();
    el_dir_relogin();
    notify();
    return ESP_OK;
}

void onboard_settings_changed(void)
{
    notify();
}

void onboard_set_ui_cb(void (*cb)(void))
{
    s_ui_cb = cb;
}

void onboard_set_web_cb(void (*cb)(void))
{
    s_web_cb = cb;
}
