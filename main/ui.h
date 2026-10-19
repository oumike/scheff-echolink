#pragma once

// Touch UI. Call after display_init(), net_wifi_init() and server_list_init().
// Boots to Wi-Fi setup when no network is saved, otherwise to the home screen.
void ui_start(void);
