#include "console_cmds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_console.h"
#include "lwip/inet.h"
#include "audio.h"
#include "board.h"
#include "el_directory.h"
#include "el_qso.h"
#include "net_wifi.h"
#include "settings.h"

// Join argv[from..] with spaces.
static void join(char *out, size_t len, int argc, char **argv, int from)
{
    out[0] = 0;
    for (int i = from; i < argc; i++) {
        if (i > from) strlcat(out, " ", len);
        strlcat(out, argv[i], len);
    }
}

static int on_off(const char *s, bool cur)
{
    if (!s) return !cur;
    return !strcasecmp(s, "on") || !strcmp(s, "1");
}

static int cmd_status(int argc, char **argv)
{
    static const char *states[] = {"idle", "connecting", "connected"};
    qso_status_t st;
    el_qso_get_status(&st);
    net_wifi_print_status();
    printf("Directory: %s as %s, %u stations loaded\n", el_dir_logged_in() ? "logged in" : "not logged in",
           g_settings.callsign[0] ? g_settings.callsign : "(no callsign)", (unsigned)el_dir_count());
    printf("QSO: %s", states[st.state]);
    if (st.state != QSO_IDLE) {
        struct in_addr a = {.s_addr = st.remote_ip};
        printf(" %s %s [%s]  %s  rx %lu / tx %lu pkts", st.remote_call, st.remote_name, inet_ntoa(a),
               st.ptt ? "TX" : st.receiving ? "RX" : "--", (unsigned long)st.rx_packets, (unsigned long)st.tx_packets);
    }
    printf("\n");
    return 0;
}

static int cmd_set(int argc, char **argv)
{
    if (argc < 2) {
        settings_print();
        return 0;
    }
    char value[128];
    join(value, sizeof(value), argc, argv, 2);
    esp_err_t err = settings_set(argv[1], value);
    if (err == ESP_ERR_NOT_FOUND) {
        printf("Unknown key '%s'. Keys:\n", argv[1]);
        settings_print();
        return 1;
    }
    if (err != ESP_OK) {
        printf("Invalid value\n");
        return 1;
    }
    settings_save();
    if (!strcmp(argv[1], "callsign") || !strcmp(argv[1], "name")) el_qso_update_identity();
    if (!strcmp(argv[1], "volume")) audio_set_volume(g_settings.volume);
    if (!strcmp(argv[1], "mic_gain")) audio_set_mic_gain(g_settings.mic_gain);
    if (!strncmp(argv[1], "wifi_", 5)) printf("Saved. Run 'wifi' to reconnect.\n");
    return 0;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc >= 2) {
        strlcpy(g_settings.wifi_ssid, argv[1], sizeof(g_settings.wifi_ssid));
        strlcpy(g_settings.wifi_pass, argc >= 3 ? argv[2] : "", sizeof(g_settings.wifi_pass));
        settings_save();
    }
    if (!g_settings.wifi_ssid[0]) {
        net_wifi_print_status();
        printf("Usage: wifi <ssid> [password]\n");
        return 1;
    }
    return net_wifi_connect(g_settings.wifi_ssid, g_settings.wifi_pass) == ESP_OK ? 0 : 1;
}

static int cmd_login(int argc, char **argv)
{
    el_dir_set_status(EL_DIR_ONLINE);
    return el_dir_login(EL_DIR_ONLINE) == ESP_OK ? 0 : 1;
}

static int cmd_logout(int argc, char **argv)
{
    el_dir_set_status(EL_DIR_OFFLINE);
    return 0;
}

static int cmd_refresh(int argc, char **argv)
{
    return el_dir_fetch_list() == ESP_OK ? 0 : 1;
}

static int cmd_list(int argc, char **argv)
{
    el_dir_print(argc >= 2 ? argv[1] : NULL, argc >= 3 ? atoi(argv[2]) : 40);
    return 0;
}

static int cmd_connect(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: connect <callsign | node number | IP>   e.g. connect *ECHOTEST*\n");
        return 1;
    }
    return el_qso_connect(argv[1]) == ESP_OK ? 0 : 1;
}

static int cmd_disconnect(int argc, char **argv)
{
    el_qso_disconnect();
    return 0;
}

static int cmd_ptt(int argc, char **argv)
{
    el_qso_set_ptt(on_off(argc >= 2 ? argv[1] : NULL, el_qso_ptt()));
    return 0;
}

static int cmd_chat(int argc, char **argv)
{
    char msg[256];
    join(msg, sizeof(msg), argc, argv, 1);
    if (el_qso_send_chat(msg) != ESP_OK) {
        printf("Not connected\n");
        return 1;
    }
    return 0;
}

static int cmd_loopback(int argc, char **argv)
{
    bool on = on_off(argc >= 2 ? argv[1] : NULL, audio_loopback());
    audio_set_loopback(on);
    printf("Mic->speaker loopback %s\n", on ? "on (keep the volume low to avoid howl)" : "off");
    return 0;
}

static void reg(const char *cmd, const char *help, const char *hint, esp_console_cmd_func_t fn)
{
    const esp_console_cmd_t c = {.command = cmd, .help = help, .hint = hint, .func = fn};
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}

void console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "echolink>";
    rc.max_cmdline_length = 256;

#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    esp_console_dev_uart_config_t hw = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw, &rc, &repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t hw = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw, &rc, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw, &rc, &repl));
#else
    printf("No console transport configured\n");
    return;
#endif

    esp_console_register_help_command();
    reg("status", "Show Wi-Fi, directory and QSO status", NULL, cmd_status);
    reg("set", "Show settings, or set one and save to flash", "[<key> <value>]", cmd_set);
    reg("wifi", "Connect to Wi-Fi (saves credentials)", "[<ssid> [<password>]]", cmd_wifi);
    reg("login", "Register with the EchoLink directory now", NULL, cmd_login);
    reg("logout", "Mark this station offline in the directory", NULL, cmd_logout);
    reg("refresh", "Re-download the station list", NULL, cmd_refresh);
    reg("list", "List stations matching a filter", "[<filter> [<max>]]", cmd_list);
    reg("connect", "Connect to a station/node/conference", "<call | node | ip>", cmd_connect);
    reg("c", "Alias for connect", "<call | node | ip>", cmd_connect);
    reg("disconnect", "Hang up", NULL, cmd_disconnect);
    reg("dc", "Alias for disconnect", NULL, cmd_disconnect);
    reg("ptt", "Toggle transmit (or hold " BOARD_PTT_HINT ")", "[on|off]", cmd_ptt);
    reg("chat", "Send a chat message", "<text>", cmd_chat);
    reg("loopback", "Mic to speaker loopback test", "[on|off]", cmd_loopback);

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
