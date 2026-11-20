#pragma once

// The CardputerZero build's answers to the Kconfig options main/ reads. There
// is no menuconfig on Linux: settings are changed on screen, in the web
// config or at the console, and saved under ~/.config/scheff-echolink.

#define CONFIG_EL_LINUX                1
#define CONFIG_EL_BOARD_CARDPUTER_ZERO 1

#define CONFIG_EL_CALLSIGN          ""
#define CONFIG_EL_PASSWORD          ""
#define CONFIG_EL_NAME              ""
#define CONFIG_EL_LOCATION          "CardputerZero"
#define CONFIG_EL_DIRECTORY_SERVER  "servers.echolink.org"
#define CONFIG_EL_WIFI_SSID         ""
#define CONFIG_EL_WIFI_PASSWORD     ""
#define CONFIG_EL_ACCEPT_INCOMING   1

#define CONFIG_WL_SECTOR_SIZE 4096

// The web config's port (esp_http_server.h's default): 80 needs root.
#define CONFIG_EL_WEB_PORT 8573
