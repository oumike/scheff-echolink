#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char callsign[16];
    char password[40];
    char name[32];
    char location[48];
    char dir_server[64];
    char wifi_ssid[33];
    char wifi_pass[65];
    bool accept_incoming;
    uint8_t volume;    // 0-100
    uint8_t mic_gain;  // dB, 0-42
} settings_t;

// Loaded at boot from NVS, falling back to Kconfig defaults.
extern settings_t g_settings;

esp_err_t settings_load(void);
esp_err_t settings_save(void);

// Set a field by name from a string (used by the console). Returns
// ESP_ERR_NOT_FOUND for an unknown key, ESP_ERR_INVALID_ARG for a bad value.
esp_err_t settings_set(const char *key, const char *value);
void settings_print(void);
