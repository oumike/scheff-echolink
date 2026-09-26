#include "net_wifi.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_hosted_transport_config.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "board.h"

static const char *TAG = "wifi";

#define BIT_GOT_IP BIT0

static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static bool s_want_connect;

static esp_err_t c6_reset_cb(void *arg, bool level)
{
    return board_c6_enable(level);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, BIT_GOT_IP);
        if (s_want_connect) {
            wifi_event_sta_disconnected_t *d = data;
            ESP_LOGW(TAG, "disconnected (reason %d), retrying", d->reason);
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

esp_err_t net_wifi_init(void)
{
    s_events = xEventGroupCreate();

    // The C6 EN pin is on the XL9535, so hand ESP-Hosted a callback instead
    // of a reset GPIO.
    ESP_RETURN_ON_FALSE(esp_hosted_sdio_set_reset_callback(c6_reset_cb, NULL) == ESP_TRANSPORT_OK,
                        ESP_FAIL, TAG, "hosted reset cb");
    ESP_RETURN_ON_ERROR(board_c6_enable(true), TAG, "c6 power");
    ESP_RETURN_ON_ERROR(esp_hosted_init(), TAG, "esp_hosted_init");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, "echolink-p4");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL), TAG, "evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL), TAG, "evt");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    // Voice over UDP: modem power save adds latency and packet bunching.
    esp_wifi_set_ps(WIFI_PS_NONE);

    // The directory login carries local time; SNTP keeps it honest.
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);
    return ESP_OK;
}

esp_err_t net_wifi_connect(const char *ssid, const char *pass)
{
    if (!ssid || !*ssid) return ESP_ERR_INVALID_ARG;
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass ? pass : "", sizeof(wc.sta.password));
    wc.sta.threshold.authmode = (pass && *pass) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;

    s_want_connect = false;
    esp_wifi_disconnect();
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "config");
    s_want_connect = true;
    ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    return esp_wifi_connect();
}

bool net_wifi_wait_ip(uint32_t timeout_ms)
{
    return xEventGroupWaitBits(s_events, BIT_GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & BIT_GOT_IP;
}

bool net_wifi_has_ip(void)
{
    return s_events && (xEventGroupGetBits(s_events) & BIT_GOT_IP);
}

void net_wifi_print_status(void)
{
    wifi_ap_record_t ap;
    esp_netif_ip_info_t ip;
    if (net_wifi_has_ip() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
        esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
        printf("Wi-Fi: \"%s\" RSSI %d dBm, IP " IPSTR "\n", (char *)ap.ssid, ap.rssi, IP2STR(&ip.ip));
    } else {
        printf("Wi-Fi: not connected\n");
    }
}
