import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from host_harness import COMMON, ROOT, defines, function, header, run_c, source


class ProtocolTests(unittest.TestCase):
    def test_tab5_gt911_keeps_held_touch_until_release_or_timeout(self):
        path = "main/display_tab5.c"
        code = COMMON + "\n#define CONFIG_EL_BOARD_TAB5 1\n" + header("main/display.h")
        code += defines(path, "GT911_", "ST_REG_", "ST_ADV_", "ST_MAX_", "ST_REPORT_", "TOUCH_")
        code += r"""
typedef void lv_indev_t;
typedef struct { struct { int32_t x, y; } point; int state; } lv_indev_data_t;
enum { LV_INDEV_STATE_RELEASED, LV_INDEV_STATE_PRESSED };
static bool s_touch_gt911 = true;
static void *s_touch;
static uint8_t status = 0x81;
static bool read_error;
static int64_t now = 1000000;
static int64_t esp_timer_get_time(void) { return now; }
static esp_err_t touch_read(uint16_t reg, void *out, size_t n) {
    if (read_error) return ESP_FAIL;
    assert(reg == GT911_REG_STATUS && n == 6);
    const uint8_t report[6] = {status, 0, 100, 0, 200, 0};
    memcpy(out, report, n);
    return ESP_OK;
}
static esp_err_t i2c_master_transmit(void *dev, const void *data, size_t n, int timeout) {
    return ESP_OK;
}
"""
        code += function(path, "static bool touch_poll(")
        code += function(path, "static void touch_read_cb(")
        code += r"""
int main(void) {
    lv_indev_data_t data;
    touch_read_cb(NULL, &data);
    assert(data.state == LV_INDEV_STATE_PRESSED && data.point.x == 100 && data.point.y == 200);
    now += TOUCH_POLL_US;
    status = 0;
    touch_read_cb(NULL, &data);
    assert(data.state == LV_INDEV_STATE_PRESSED);
    now += TOUCH_POLL_US;
    status = 0x80;
    touch_read_cb(NULL, &data);
    assert(data.state == LV_INDEV_STATE_RELEASED);
    now += TOUCH_POLL_US;
    status = 0x81;
    touch_read_cb(NULL, &data);
    assert(data.state == LV_INDEV_STATE_PRESSED);
    read_error = true;
    now += TOUCH_POLL_US;
    touch_read_cb(NULL, &data);
    assert(data.state == LV_INDEV_STATE_PRESSED);
    now += TOUCH_RELEASE_US;
    touch_read_cb(NULL, &data);
    assert(data.state == LV_INDEV_STATE_RELEASED);
}
"""
        run_c(code)

    def test_directory_rejects_incomplete_counts_records_and_terminators(self):
        code = COMMON + header("main/el_directory.h")
        for signature in ("static void parse_data(", "static char *next_line(", "static size_t parse_list("):
            code += function("main/el_directory.c", signature)
        code += r"""
int main(void) {
    const char complete[] = "@@@\n2\n.\n\n\n\nKEEP-R\nDescription [ON 12:34]\n10\n192.0.2.1\n"
                            " \nServer message\n\n\n+++";
    for (size_t cut = 0; cut < strlen(complete); cut++) {
        char *text = strndup(complete, cut);
        el_station_t *list = NULL;
        assert(parse_list(text, cut, &list) == 0 && !list);
        free(text);
    }
    const char *valid[] = {
        complete,
        "@@@\r\n1\r\nKEEP-R\r\nDescription [ON 12:34]\r\n10\r\n192.0.2.1\r\n+++\r\n",
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        char *text = strdup(valid[i]);
        el_station_t *list = NULL;
        assert(parse_list(text, strlen(text), &list) == 1);
        assert(list && !strcmp(list[0].call, "KEEP-R") && list[0].node == 10);
        free(list);
        free(text);
    }
    const char *invalid[] = {
        "@@@\n2\nKEEP-R\nDescription\n10\n192.0.2.1\n+++",
        "@@@\n1\nKEEP-R\nDescription\n10\n192.0.2.1\nwrong",
        "@@@\n1\nKEEP-R\nDescription\n10\n192.0.2.1\n+++\nextra",
        "@@@\n-1\n+++", "@@@\n9999999999999999999999\n+++", "@@@\n1junk\n+++",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        char *text = strdup(invalid[i]);
        el_station_t *list = NULL;
        assert(parse_list(text, strlen(text), &list) == 0 && !list);
        free(text);
    }
}
"""
        run_c(code)

    def test_incoming_call_clears_previous_session_information(self):
        code = COMMON
        for module in ("audio", "el_qso", "el_directory", "settings"):
            code += header(f"main/{module}.h")
        code += r"""
typedef void *gsm;
settings_t g_settings;
static int64_t esp_timer_get_time(void) { return 1000000; }
void audio_play_flush(void) {}
void el_dir_set_status(el_dir_status_t status) {}
bool el_dir_find_ip(uint32_t ip, el_station_t *out) {
    *out = (el_station_t){.ip = ip, .call = "NEW-R"};
    return true;
}
static void send_info(uint32_t ip) {}
static void send_sdes(uint32_t ip) {}
static void send_bye(uint32_t ip) {}
"""
        qso = source("main/el_qso.c")
        code += qso[qso.index("#define PORT_AUDIO"):qso.index("/* ---- RTCP")]
        for signature in (
            "static uint8_t *put_item(", "static uint8_t *put_null_rr(", "static size_t build_sdes(",
            "static const uint8_t *find_rtcp(", "static bool sdes_get(", "static void set_peer(",
            "static void set_idle(", "static void set_connected(", "static void parse_remote_name(",
            "static void handle_ctrl(", "void el_qso_get_status(",
        ):
            code += function("main/el_qso.c", signature)
        code += r"""
int main(void) {
    q.state = QSO_CONNECTED;
    q.last_audio_us = 999999;
    strcpy(q.remote_call, "OLD-R");
    strcpy(q.info, "Previous station information");
    set_idle("remote hung up");
    g_settings.accept_incoming = true;
    uint8_t sdes[256];
    size_t n = build_sdes(sdes, "NEW-R", "New operator");
    handle_ctrl(sdes, n, inet_addr("192.0.2.2"));
    qso_status_t status;
    el_qso_get_status(&status);
    assert(status.state == QSO_CONNECTED && !strcmp(status.remote_call, "NEW-R"));
    assert(!strcmp(status.remote_name, "New operator"));
    assert(!q.info[0] && !status.end_reason[0] && !status.receiving);
}
"""
        run_c(code)

    def test_linux_connect_applies_credentials_and_reports_failure(self):
        code = COMMON + header("main/net_wifi.h") + r"""
static net_join_result_t result = NET_JOIN_OK;
static unsigned calls;
net_join_result_t net_wifi_join(const char *ssid, const char *pass, uint32_t timeout) {
    calls++;
    assert(!strcmp(ssid, "Restored network") && !strcmp(pass, "restored password"));
    assert(timeout > 0);
    return result;
}
"""
        code += function("linux/src/net_cp0.c", "esp_err_t net_wifi_connect(")
        code += r"""
int main(void) {
    assert(net_wifi_connect("Restored network", "restored password") == ESP_OK && calls == 1);
    result = NET_JOIN_BAD_PASSWORD;
    assert(net_wifi_connect("Restored network", "restored password") != ESP_OK && calls == 2);
    assert(net_wifi_connect("", "") == ESP_ERR_INVALID_ARG && calls == 2);
}
"""
        run_c(code)

    @unittest.skipUnless(sys.platform == "linux", "Linux socket compatibility layer")
    def test_linux_restart_can_rebind_its_udp_port(self):
        code = COMMON + f'\n#include "{ROOT / "linux/compat/lwip/sockets.h"}"\n'
        code += function("main/el_qso.c", "static int bind_udp(")
        code += r"""
int main(int argc, char **argv) {
    if (argc > 1) {
        int fd = bind_udp((uint16_t)atoi(argv[1]));
        assert(fd >= 0);
        close(fd);
        return 0;
    }
    int fd = bind_udp(0);
    assert(fd >= 0);
    struct sockaddr_in address;
    socklen_t size = sizeof(address);
    assert(getsockname(fd, (struct sockaddr *)&address, &size) == 0);
    char port[24];
    snprintf(port, sizeof(port), "%u", ntohs(address.sin_port));
    char *args[] = {argv[0], port, NULL};
    execv(argv[0], args);
    assert(!"execv failed");
}
"""
        run_c(code)

    def test_gt911_coordinates_on_both_s3_boards(self):
        for board, expected in (
            ("tdeck", "{{0,239}, {160,119}, {319,0}}"),
            ("wio_l2", "{{319,0}, {159,120}, {0,239}}"),
        ):
            with self.subTest(board=board):
                path = f"main/display_{board}.c"
                code = COMMON + "\n#define CONFIG_IDF_TARGET_ESP32S3 1\n"
                code += header("main/display.h") + defines(path, "GT911_", "TOUCH_RELEASE_US")
                code += r"""
typedef void lv_indev_t;
typedef struct { struct { int32_t x, y; } point; int state; } lv_indev_data_t;
enum { LV_INDEV_STATE_RELEASED, LV_INDEV_STATE_PRESSED };
static uint16_t raw_x, raw_y;
static int64_t esp_timer_get_time(void) { return 1000000; }
static esp_err_t gt_read(uint16_t reg, void *out, size_t n) {
    if (reg == 0x814e) { *(uint8_t *)out = 0x81; return ESP_OK; }
    /* GT911 point at 0x814F: ID, X lo/hi, Y lo/hi, size lo/hi, reserved. */
    const uint8_t point[16] = {0, raw_x & 255, raw_x >> 8, raw_y & 255, raw_y >> 8, 10, 0, 0};
    assert(reg >= 0x814f && reg - 0x814f + n <= sizeof(point));
    memcpy(out, point + (reg - 0x814f), n);
    return ESP_OK;
}
static esp_err_t gt_clear_status(void) { return ESP_OK; }
"""
                code += function(path, "static void touch_read_cb(")
                code += f"\nstatic const int expected[3][2] = {expected};\n"
                code += r"""
int main(void) {
    const int touches[3][2] = {{0,0}, {120,160}, {239,319}};
    for (size_t i = 0; i < 3; i++) {
        raw_x = touches[i][0]; raw_y = touches[i][1];
        lv_indev_data_t data;
        touch_read_cb(NULL, &data);
        assert(data.state == LV_INDEV_STATE_PRESSED);
        assert(data.point.x == expected[i][0] && data.point.y == expected[i][1]);
    }
}
"""
                run_c(code)

    def test_wifi_preserves_maximum_length_credentials(self):
        code = COMMON + r"""
enum { WIFI_AUTH_OPEN, WIFI_AUTH_WPA2_PSK, WIFI_IF_STA };
typedef struct {
    struct {
        uint8_t ssid[32], password[64];
        struct { int authmode; } threshold;
        struct { bool capable; } pmf_cfg;
    } sta;
} wifi_config_t;
static bool s_want_connect;
static wifi_config_t captured;
static int esp_wifi_disconnect(void) { return ESP_OK; }
static int esp_wifi_set_config(int mode, wifi_config_t *cfg) { captured = *cfg; return ESP_OK; }
static int esp_wifi_connect(void) { return ESP_OK; }
"""
        code += function("main/net_wifi.c", "esp_err_t net_wifi_connect(")
        code += r"""
int main(void) {
    const char *ssid = "12345678901234567890123456789012";
    const char *psk = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    assert(net_wifi_connect(ssid, psk) == ESP_OK);
    assert(!memcmp(captured.sta.ssid, ssid, 32));
    assert(!memcmp(captured.sta.password, psk, 64));
    assert(net_wifi_connect("open-network", NULL) == ESP_OK);
    assert(!strcmp((char *)captured.sta.ssid, "open-network"));
    assert(captured.sta.password[0] == 0 && captured.sta.threshold.authmode == WIFI_AUTH_OPEN);
    assert(net_wifi_connect("123456789012345678901234567890123", psk) == ESP_ERR_INVALID_ARG);
    assert(net_wifi_connect(ssid, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0")
           == ESP_ERR_INVALID_ARG);
}
"""
        run_c(code)

    def directory_program(self):
        code = COMMON + header("main/el_directory.h") + r"""
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
#define REFRESH_MS 300000
#define LIST_REFRESH_MS 900000
static bool s_relogin, s_logged_in, change_status;
static size_t s_count;
static el_dir_status_t s_want = EL_DIR_ONLINE;
static unsigned ticks, attempts, at[32];
static jmp_buf done;
static int ulTaskNotifyTake(int clear, unsigned delay) {
    ticks += delay;
    if (ticks > 75000) longjmp(done, 1);
    if (change_status && ticks == 20000) s_want = EL_DIR_BUSY;
    return 0;
}
static bool net_wifi_has_ip(void) { return true; }
static unsigned xTaskGetTickCount(void) { return ticks; }
esp_err_t el_dir_login(el_dir_status_t want) {
    assert(attempts < 32);
    at[attempts++] = ticks;
    return ESP_FAIL;
}
esp_err_t el_dir_fetch_list(void) { return ESP_FAIL; }
"""
        return code + function("main/el_directory.c", "static void dir_task(")

    def test_failed_directory_login_honors_backoff(self):
        run_c(self.directory_program() + r"""
int main(void) {
    if (!setjmp(done)) dir_task(NULL);
    assert(attempts == 2 && at[0] == 5000 && at[1] == 65000);
}
""")

    def test_directory_status_change_interrupts_backoff(self):
        run_c(self.directory_program() + r"""
int main(void) {
    change_status = true;
    if (!setjmp(done)) dir_task(NULL);
    assert(attempts == 2 && at[0] == 5000 && at[1] == 20000);
}
""")

    def test_invalid_gsm_is_not_played(self):
        code = COMMON + '#include "gsm.h"\n' + header("main/audio.h") + header("main/el_qso.h")
        qso = source("main/el_qso.c")
        code += qso[qso.index("#define PORT_AUDIO") : qso.index("/* ---- RTCP")]
        code += r"""
static int64_t esp_timer_get_time(void) { return 1000000; }
static void handle_text(const uint8_t *buf, size_t n) { }
static size_t played;
void audio_play_push(const int16_t *pcm, size_t n) { played += n; }
"""
        code += function("main/el_qso.c", "static void handle_audio(")
        code += r"""
int main(void) {
    q.dec = gsm_create();
    assert(q.dec);
    q.state = QSO_CONNECTED;
    q.remote_ip = 1;
    uint8_t invalid[AUDIO_PKT_BYTES] = {0xc0, 3};
    handle_audio(invalid, sizeof(invalid), 1);
    assert(played == 0);
    uint8_t short_packet[] = {0xc0};
    handle_audio(short_packet, sizeof(short_packet), 1);
    assert(played == 0);
    uint8_t valid[AUDIO_PKT_BYTES] = {0xc0, 3};
    for (size_t i = 0; i < 4; i++) valid[RTP_HDR_BYTES + i * GSM_FRAME_BYTES] = 0xd0;
    handle_audio(valid, sizeof(valid), 1);
    assert(played == 640);
    gsm_destroy(q.dec);
}
"""
        flags = [
            "-I" + str(ROOT / "components/gsm/include"),
            "-I" + str(ROOT / "components/gsm/private"),
            "-DSASR", "-DNeedFunctionPrototypes=1", "-ftrivial-auto-var-init=pattern",
            "-Wno-deprecated-non-prototype", "-Wno-shift-negative-value",
            *sorted((ROOT / "components/gsm/src").glob("*.c")),
        ]
        # libgsm's legacy fixed-point arithmetic relies on signed shifts.
        run_c(code, extra=flags, sanitize="address")

    def upload_program(self):
        code = COMMON + defines("main/web_config.c", "IMPORT_") + r"""
typedef struct { int content_len; } httpd_req_t;
#define HTTPD_SOCK_ERR_TIMEOUT -11
static jmp_buf stopped;
static int64_t now;
static unsigned receives;
static bool drip;
static bool imported;
static const char *payload;
static const char *response;
static int64_t esp_timer_get_time(void) { return now; }
static esp_err_t send_error(httpd_req_t *r, const char *status, const char *msg) {
    response = status;
    return ESP_OK;
}
static esp_err_t httpd_resp_send_500(httpd_req_t *r) { return ESP_FAIL; }
static esp_err_t import_backup(httpd_req_t *r, char *xml, const char *where) {
    imported = true;
    return ESP_OK;
}
static int httpd_req_recv(httpd_req_t *r, char *out, size_t n) {
    now += 5000000;
    if (++receives == 100) longjmp(stopped, 1);
    if (payload) { memcpy(out, payload, n); return n; }
    if (!drip) return HTTPD_SOCK_ERR_TIMEOUT;
    *out = ' ';
    return 1;
}
"""
        return code + function("main/web_config.c", "static esp_err_t h_import_upload(")

    def test_upload_socket_timeout_finishes_request(self):
        run_c(self.upload_program() + r"""
int main(void) {
    httpd_req_t req = {.content_len = 128};
    if (!setjmp(stopped)) {
        assert(h_import_upload(&req) == ESP_FAIL);
        assert(response && !strncmp(response, "408", 3));
        return 0;
    }
    assert(!"upload never returned after repeated socket timeouts");
}
""")

    def test_slow_upload_has_an_overall_deadline(self):
        run_c(self.upload_program() + r"""
int main(void) {
    drip = true;
    httpd_req_t req = {.content_len = 128};
    if (!setjmp(stopped)) {
        assert(h_import_upload(&req) == ESP_FAIL);
        assert(response && !strncmp(response, "408", 3));
        assert(now < 120000000);
        return 0;
    }
    assert(!"slow upload still monopolizes the HTTP task after 8 minutes");
}
""")

    def test_flash_finds_image_with_either_optional_directory_missing(self):
        for core in (".platformio-p4", ".platformio"):
            with self.subTest(core=core), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                hosted = root / core / "packages/framework-arduinoespressif32-libs/hosted"
                hosted.mkdir(parents=True)
                (hosted / "esp32c6-v2.12.3.bin").write_bytes(b"fixture, not firmware")
                tools = root / "tools"
                tools.mkdir()
                stub = tools / "esptool.py"
                stub.write_text("#!/bin/sh\nprintf 'FAKE_FLASHER_SELECTED\\n'\n")
                stub.chmod(0o755)
                result = subprocess.run(
                    ["bash", str(ROOT / "scripts/flash-c6.sh"), "NO-DEVICE"],
                    env=dict(os.environ, HOME=str(root), PATH=str(tools) + os.pathsep + os.environ["PATH"]),
                    capture_output=True, text=True, timeout=15,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("FAKE_FLASHER_SELECTED", result.stdout)

    def test_upload_rejects_embedded_nul_instead_of_importing_prefix(self):
        run_c(self.upload_program() + r"""
int main(void) {
    const char data[] = "<server call='NEW-R'/>\0ignored suffix";
    payload = data;
    httpd_req_t req = {.content_len = sizeof(data) - 1};
    assert(h_import_upload(&req) == ESP_OK);
    assert(response && !strncmp(response, "400", 3) && !imported);
}
""")

    def test_complete_upload_reaches_import(self):
        run_c(self.upload_program() + r"""
int main(void) {
    payload = "<server call='NEW-R'/>";
    httpd_req_t req = {.content_len = strlen(payload)};
    assert(h_import_upload(&req) == ESP_OK && imported && !response);
}
""")


if __name__ == "__main__":
    unittest.main()
