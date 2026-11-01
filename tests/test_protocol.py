import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from host_harness import COMMON, ROOT, defines, function, header, run_c, source


class ProtocolTests(unittest.TestCase):
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
