// EchoLink client for the LilyGO T-Display-P4.
//
// Audio: ES8311 codec (onboard electret mic + NS4150B speaker amp) at 8 kHz.
// Network: Wi-Fi through the ESP32-C6 coprocessor (ESP-Hosted over SDIO).
// UI: serial console for now; BOOT button is hold-to-talk.

#include "esp_log.h"
#include "audio.h"
#include "board.h"
#include "console_cmds.h"
#include "el_directory.h"
#include "el_qso.h"
#include "net_wifi.h"
#include "ptt.h"
#include "settings.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_ERROR_CHECK(settings_load());
    ESP_ERROR_CHECK(board_init());

    ESP_ERROR_CHECK(audio_init(el_qso_mic_frame));
    audio_set_volume(g_settings.volume);
    audio_set_mic_gain(g_settings.mic_gain);

    ESP_ERROR_CHECK(net_wifi_init());
    if (g_settings.wifi_ssid[0]) {
        net_wifi_connect(g_settings.wifi_ssid, g_settings.wifi_pass);
    } else {
        ESP_LOGW(TAG, "no Wi-Fi configured: use 'wifi <ssid> <password>'");
    }

    ESP_ERROR_CHECK(el_qso_init());
    el_dir_start();
    ptt_button_start();

    if (!g_settings.callsign[0]) {
        ESP_LOGW(TAG, "no callsign configured: use 'set callsign <CALL>' and 'set password <pw>'");
    }
    console_start();
}
