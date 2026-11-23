#pragma once

#include "esp_err.h"

// Browser-based setup (Wi-Fi and EchoLink account), on port 8573 on Linux
// and port 80 on ESP32. A fresh local access code is required each startup. Until
// onboarding is complete it also brings up a board-named open setup hotspot
// with captive DNS. The hotspot turns off a minute after station Wi-Fi gets
// an IP; the web server itself remains available there. Call after
// net_wifi_init().
esp_err_t web_config_start(void);

// For the device UI only; never include this in HTTP responses or backups.
const char *web_config_access_code(void);
