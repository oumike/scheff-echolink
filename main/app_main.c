// EchoLink client shared by the LilyGO T-Display-P4, LilyGO T-Deck Plus and
// Seeed Wio Tracker L2 targets.
// Board-specific display, audio, network transport and PTT implementations
// sit behind the common module APIs used here.

#include "esp_log.h"
#include "audio.h"
#include "board.h"
#include "console_cmds.h"
#include "display.h"
#include "el_directory.h"
#include "el_qso.h"
#include "net_wifi.h"
#include "ptt.h"
#include "server_list.h"
#include "settings.h"
#include "ui.h"
#include "web_config.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_ERROR_CHECK(settings_load());
    ESP_ERROR_CHECK(board_init());

    // A dead display must not take the radio down with it: the serial
    // console still works without it.
    esp_err_t disp_err = display_init();
    if (disp_err != ESP_OK) ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(disp_err));
    if (server_list_init() != ESP_OK) ESP_LOGE(TAG, "server list storage unavailable");

    ESP_ERROR_CHECK(audio_init(el_qso_mic_frame));
    audio_set_volume(g_settings.volume);
    audio_set_mic_gain(g_settings.mic_gain);

    ESP_ERROR_CHECK(net_wifi_init());
    if (g_settings.wifi_ssid[0]) {
        net_wifi_connect(g_settings.wifi_ssid, g_settings.wifi_pass);
#if !CONFIG_EL_LINUX
    } else {
        ESP_LOGW(TAG, "no Wi-Fi configured: set it up on screen, or use 'wifi <ssid> <password>'");
#endif
    }

    web_config_start();

    ESP_ERROR_CHECK(el_qso_init());
    el_dir_start();
    ptt_button_start();
    if (disp_err == ESP_OK) ui_start();

    if (!g_settings.callsign[0]) {
        ESP_LOGW(TAG, "no callsign configured: use 'set callsign <CALL>' and 'set password <pw>'");
    }
    console_start();
}
