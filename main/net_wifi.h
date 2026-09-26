#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Brings up the ESP32-C6 coprocessor (ESP-Hosted over SDIO) and the STA
// interface. Safe to call with an empty SSID; connect later via net_wifi_connect.
esp_err_t net_wifi_init(void);
esp_err_t net_wifi_connect(const char *ssid, const char *pass);
bool net_wifi_wait_ip(uint32_t timeout_ms);
bool net_wifi_has_ip(void);
void net_wifi_print_status(void);
