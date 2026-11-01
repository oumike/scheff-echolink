"""Navigation regressions, executing the production UI with headless boundaries."""

from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from host_harness import COMMON, header, implementation, run_c, source


BOUNDARIES = r"""
settings_t g_settings;
static qso_status_t host_qso;
static char host_info[512];
static bool host_wifi = true;
static unsigned host_ap_starts, host_saves, host_restarts, host_disconnects;
static char host_join_ssid[33], host_join_pass[65];
static net_join_result_t host_join_result = NET_JOIN_OK;

static esp_err_t board_battery_percent(uint8_t *p) { *p = 75; return ESP_OK; }
static void esp_restart(void) { ++host_restarts; }
esp_err_t settings_save(void) { ++host_saves; return ESP_OK; }
void audio_set_volume(int volume) {}
bool net_wifi_has_ip(void) { return host_wifi; }
bool net_wifi_ap_active(void) { return false; }
const char *net_wifi_ap_ssid(void) { return "test-ap"; }
esp_err_t net_wifi_ap_start(void) { ++host_ap_starts; return ESP_OK; }
void net_wifi_ap_stop(void) {}
bool net_wifi_sta_ip(char *out, size_t size) {
    if (host_wifi) strlcpy(out, "192.0.2.1", size);
    return host_wifi;
}
int net_wifi_scan(net_wifi_ap_t *out, int max) {
    assert(max > 0);
    *out = (net_wifi_ap_t){.ssid = "Scanned network", .rssi = -42, .secure = false};
    return 1;
}
net_join_result_t net_wifi_join(const char *ssid, const char *pass, uint32_t timeout) {
    strlcpy(host_join_ssid, ssid, sizeof(host_join_ssid));
    strlcpy(host_join_pass, pass, sizeof(host_join_pass));
    return host_join_result;
}
void el_dir_relogin(void) {}
bool el_dir_logged_in(void) { return true; }
size_t el_dir_count(void) { return 1; }
bool el_dir_find(const char *call, el_station_t *out) {
    *out = (el_station_t){.node = 1234, .ip = inet_addr("192.0.2.2")};
    strlcpy(out->call, call, sizeof(out->call));
    strlcpy(out->desc, "Directory description", sizeof(out->desc));
    return true;
}
bool el_dir_find_ip(uint32_t ip, el_station_t *out) {
    return el_dir_find(host_qso.remote_call, out);
}
void el_qso_update_identity(void) {}
void el_qso_get_status(qso_status_t *out) { *out = host_qso; }
void el_qso_get_info(char *out, size_t size) { strlcpy(out, host_info, size); }
esp_err_t el_qso_connect(const char *target) {
    if (host_qso.state != QSO_IDLE) return ESP_ERR_INVALID_STATE;
    host_qso = (qso_status_t){.state = QSO_CONNECTING, .remote_ip = inet_addr("192.0.2.2")};
    strlcpy(host_qso.remote_call, target, sizeof(host_qso.remote_call));
    return ESP_OK;
}
void el_qso_disconnect(void) {
    ++host_disconnects;
    host_qso.state = QSO_IDLE;
    host_qso.ptt = false;
}
void el_qso_set_ptt(bool on) { host_qso.ptt = on; }
size_t server_list_count(void) { return 0; }
size_t server_list_favorite_count(void) { return 0; }
size_t server_list_search(const char *query, server_entry_t *out, size_t max) { return 0; }
size_t server_list_get_bucket(char bucket, server_entry_t *out, size_t max) { return 0; }
char server_list_bucket(const char *call) { return call[0]; }
void server_list_set_changed_cb(void (*cb)(void)) {}
esp_err_t server_list_set_favorite(const char *call, bool favorite) { return ESP_OK; }
esp_err_t server_list_put(const char *orig_call, const server_entry_t *entry) { return ESP_OK; }
"""

HELPERS = r"""
static lv_obj_t *boot_home(void)
{
    strlcpy(g_settings.wifi_ssid, "Saved network", sizeof(g_settings.wifi_ssid));
    strlcpy(g_settings.wifi_pass, "saved pass", sizeof(g_settings.wifi_pass));
    strlcpy(g_settings.callsign, "N0LOCAL", sizeof(g_settings.callsign));
    strlcpy(g_settings.password, "account pass", sizeof(g_settings.password));
    g_settings.volume = 50;
    ui_start();
    assert(host_text(host_active, "N0LOCAL"));
    return host_active;
}

static void remote_connect(const char *call, const char *name)
{
    host_qso = (qso_status_t){.state = QSO_CONNECTED, .remote_ip = inet_addr("192.0.2.3")};
    strlcpy(host_qso.remote_call, call, sizeof(host_qso.remote_call));
    strlcpy(host_qso.remote_name, name, sizeof(host_qso.remote_name));
    strlcpy(host_info, "Live station info", sizeof(host_info));
}

static void outgoing_connected(void)
{
    host_qso.state = QSO_CONNECTED;
    strlcpy(host_qso.remote_name, "Outgoing operator", sizeof(host_qso.remote_name));
    host_delay_hook = NULL;
}

static void start_outgoing(void)
{
    server_entry_t entry = {.call = "K1OUT", .desc = "Saved outgoing target", .node = 9999};
    display_lock();
    ui_chat_connect(&entry);
    display_unlock();
    host_delay_hook = outgoing_connected;
    host_run_task("qso_connect");
}
"""


def firmware():
    parts = [COMMON, source("tests/ui_host_fixture.h")]
    for path in ("settings", "el_directory", "server_list", "net_wifi", "el_qso", "onboard", "audio", "ui_internal"):
        parts.append(header(f"main/{path}.h"))
    parts.extend([
        BOUNDARIES, implementation("main/onboard.c"),
        "#define malloc host_job_malloc\n", implementation("main/ui.c"), "#undef malloc\n",
    ])
    # Each controller is a separate translation unit on-device. Prefix only
    # colliding file-static names, leaving every production function unchanged.
    for path, names in (
        ("ui_home", ("s_scr", "s_kb")),
        ("ui_wifi", ("s_kb", "s_pass")),
        ("ui_account", ("s_pass", "s_name")),
        ("ui_chat", ("s_scr", "s_name")),
    ):
        parts.extend(f"#define {name} {path}_{name}\n" for name in names)
        parts.append(implementation(f"main/{path}.c"))
        parts.extend(f"#undef {name}\n" for name in names)
    parts.append(HELPERS)
    return "\n".join(parts)


class UIFlowsTest(unittest.TestCase):
    def run_flow(self, body):
        # Keep all compiler artifacts in the project, including the shared
        # harness's per-case directory, and remove them after each execution.
        directory = Path("tests/.ui-flows-build")
        directory.mkdir(exist_ok=True)
        try:
            with patch.object(tempfile, "tempdir", str(directory.resolve())):
                run_c(firmware() + '\n#line 1 "ui-flow-case.c"\nint main(void) {\n' + body + "\nreturn 0;\n}\n")
        finally:
            directory.rmdir()

    def test_home_wifi_back_preserves_draft_across_settings_notifications(self):
        self.run_flow(r"""
            lv_obj_t *home = boot_home();
            host_wifi = false;
            host_tick(2000);
            assert(host_active == home && host_ap_starts == 0);
            host_click(home, "Wi-Fi");
            lv_obj_t *setup = host_active;
            assert(setup != home && host_text(setup, "Wi-Fi Setup"));
            assert(!strcmp(host_field(setup, "Network name (SSID)")->text, "Saved network"));
            host_fill(setup, "Network name (SSID)", "Unsaved draft");
            onboard_settings_changed();
            host_tick(1000);
            assert(host_active == setup);
            assert(!strcmp(host_field(setup, "Network name (SSID)")->text, "Unsaved draft"));
            host_click(setup, "Back");
            host_tick(1000);
            assert(host_active == home && host_screen_count == 1);
            assert(!strcmp(g_settings.wifi_ssid, "Saved network"));
            assert(host_saves == 0 && host_ap_starts == 0);
        """)

    def test_wifi_scan_completion_after_back_and_reopen_uses_live_widgets(self):
        self.run_flow(r"""
            lv_obj_t *home = boot_home();
            for (unsigned reopen = 0; reopen < 2; ++reopen) {
                host_click(host_active, "Wi-Fi");
                host_click(host_active, "Scan for networks");
                host_click(host_active, "Back");
                host_click(host_active, "Back");
                host_tick(1000);
                assert(host_active == home && host_screen_count == 1);
                if (reopen) {
                    host_click(home, "Wi-Fi");
                    host_click(host_active, "Scan for networks");
                    assert(host_text(host_active, "Scanning for networks..."));
                }
                host_run_task("wifi_scan");
                host_tick(1000);
                if (reopen) {
                    assert(host_text(host_active, "Scanned network"));
                    host_click(host_active, "Scanned network");
                    assert(!strcmp(host_field(host_active, "Network name (SSID)")->text, "Scanned network"));
                    host_click(host_active, "Back");
                    host_tick(1000);
                }
                assert(host_active == home && host_screen_count == 1);
            }
        """)

    def test_restoring_incomplete_settings_during_wifi_edit_keeps_onboarding_required(self):
        self.run_flow(r"""
            boot_home();
            host_click(host_active, "Wi-Fi");
            lv_obj_t *setup = host_active;
            host_fill(setup, "Network name (SSID)", "Recovery draft");
            g_settings.wifi_ssid[0] = 0;
            onboard_settings_changed();
            host_tick(1000);
            assert(host_active == setup && host_text(setup, "Wi-Fi Setup"));
            assert(!strcmp(host_field(setup, "Network name (SSID)")->text, "Recovery draft"));
            assert(!host_button(setup, "Back"));
        """)

    def test_manual_wifi_join_still_saves_and_schedules_reboot(self):
        self.run_flow(r"""
            boot_home();
            host_click(host_active, "Wi-Fi");
            host_fill(host_active, "Network name (SSID)", "Replacement network");
            host_fill(host_active, "Password", "replacement pass");
            host_click(host_active, "Join");
            host_run_task("wifi_join");
            assert(host_saves == 0);
            host_tick(1000);
            assert(host_saves == 1);
            assert(!strcmp(g_settings.wifi_ssid, "Replacement network"));
            assert(!strcmp(g_settings.wifi_pass, "replacement pass"));
            assert(!strcmp(host_join_ssid, "Replacement network"));
            assert(!strcmp(host_join_pass, "replacement pass"));
            assert(host_text(lv_layer_top(), "Wi-Fi saved.\nRestarting..."));
            host_run_task("wifi_restart");
            assert(host_restarts == 1 && host_last_delay == 3000);
            assert(host_ap_starts == 0);
        """)

    def test_first_boot_wifi_has_no_home_escape_and_advances_on_save(self):
        self.run_flow(r"""
            ui_start();
            assert(host_text(host_active, "Wi-Fi Setup"));
            assert(!host_button(host_active, "Back"));
            host_fill(host_active, "Network name (SSID)", "First network");
            host_fill(host_active, "Password", "first pass");
            host_click(host_active, "Join");
            host_run_task("wifi_join");
            host_tick(1000);
            assert(host_text(host_active, "EchoLink Account"));
            assert(host_screen_count == 1 && host_saves == 1);
            host_run_task("wifi_restart");
            assert(host_restarts == 1 && host_last_delay == 3000);
        """)

    def test_incoming_call_opens_live_identity_controls_and_cleans_up_on_leave(self):
        self.run_flow(r"""
            lv_obj_t *home = boot_home();
            unsigned timers = host_timer_count();
            remote_connect("K2IN", "Incoming operator");
            host_tick(1000);
            assert(host_active != home);
            assert(host_text(host_active, "K2IN"));
            assert(host_text(host_active, "Incoming operator"));
            assert(host_text(host_active, "Live station info"));
            lv_obj_t *talk = host_button(host_active, "HOLD TO TALK");
            assert(talk);
            host_qso.receiving = true;
            host_tick(100);
            assert(host_button(host_active, "BUSY"));
            display_lock();
            lv_obj_send_event(talk, LV_EVENT_PRESSED, NULL);
            display_unlock();
            assert(!host_qso.ptt);
            host_qso.receiving = false;
            display_lock();
            lv_obj_send_event(talk, LV_EVENT_PRESSED, NULL);
            display_unlock();
            host_tick(100);
            assert(host_qso.ptt && host_button(host_active, "TRANSMITTING"));
            display_lock();
            lv_obj_send_event(talk, LV_EVENT_RELEASED, NULL);
            display_unlock();
            assert(!host_qso.ptt);
            host_click(host_active, "Leave");
            host_tick(2000);
            assert(host_active == home && host_screen_count == 1);
            assert(host_timer_count() == timers && host_disconnects == 1);
        """)

    def test_console_call_waits_for_wifi_edit_then_opens_chat(self):
        self.run_flow(r"""
            boot_home();
            host_click(host_active, "Wi-Fi");
            lv_obj_t *setup = host_active;
            host_fill(setup, "Network name (SSID)", "Keep draft");
            host_qso.state = QSO_CONNECTING;
            host_tick(1000);
            assert(host_active == setup);
            remote_connect("K3CON", "Console operator");
            host_tick(1000);
            assert(host_active == setup);
            assert(!strcmp(host_field(setup, "Network name (SSID)")->text, "Keep draft"));
            host_click(setup, "Back");
            host_tick(1000);
            assert(host_text(host_active, "K3CON"));
            assert(host_text(host_active, "Console operator"));
        """)

    def test_incoming_call_preserves_account_and_server_edit_forms(self):
        for button, label in (("Edit", "Callsign"), ("Create Manual", "Callsign")):
            with self.subTest(form=button):
                self.run_flow(r"""
                    lv_obj_t *home = boot_home();
                    host_click(home, "%s");
                    lv_obj_t *field = host_field(lv_layer_top(), "%s");
                    display_lock();
                    lv_textarea_set_text(field, "EDIT-DRAFT");
                    display_unlock();
                    remote_connect("K2IN", "Incoming operator");
                    host_tick(1000);
                    assert(host_active == home && field->alive);
                    assert(!strcmp(field->text, "EDIT-DRAFT"));
                    host_click(lv_layer_top(), "Cancel");
                    host_tick(1000);
                    assert(host_text(host_active, "K2IN"));
                """ % (button, label))

    def test_outgoing_success_and_observer_share_one_chat_and_no_stale_identity(self):
        self.run_flow(r"""
            lv_obj_t *home = boot_home();
            unsigned timers = host_timer_count(), screens = host_screens_created;
            start_outgoing();
            host_tick(2000);
            assert(host_text(host_active, "K1OUT"));
            assert(host_screens_created == screens + 1);
            lv_obj_t *chat = host_active;
            host_tick(2000);
            assert(host_active == chat && host_screens_created == screens + 1);
            host_click(chat, "Leave");
            host_tick(1000);
            assert(host_active == home && host_timer_count() == timers);
            remote_connect("K4NEXT", "Next operator");
            host_tick(1000);
            assert(host_text(host_active, "K4NEXT"));
            assert(host_text(host_active, "Next operator"));
            assert(!host_text(host_active, "K1OUT"));
            assert(!host_text(host_active, "Saved outgoing target"));
            host_qso.state = QSO_IDLE;
            strlcpy(host_qso.end_reason, "remote hung up", sizeof(host_qso.end_reason));
            host_tick(100);
            assert(host_text(lv_layer_top(), "Disconnected from K4NEXT\n(remote hung up)"));
            host_click(lv_layer_top(), "OK");
            host_tick(2000);
            assert(host_active == home && host_screen_count == 1);
            assert(host_timer_count() == timers);
        """)

    def test_outgoing_success_modal_does_not_open_chat_after_disconnect(self):
        self.run_flow(r"""
            lv_obj_t *home = boot_home();
            unsigned timers = host_timer_count(), screens = host_screens_created;
            start_outgoing();
            host_qso.state = QSO_IDLE;
            strlcpy(host_qso.end_reason, "remote hung up", sizeof(host_qso.end_reason));
            host_tick(2000);
            assert(host_active == home);
            assert(host_screens_created == screens && host_timer_count() == timers);
            remote_connect("K5NEW", "New operator");
            host_tick(1000);
            assert(host_text(host_active, "K5NEW"));
        """)

    def test_outgoing_worker_launch_failure_allows_later_calls(self):
        for failure in ("host_fail_job_alloc", "host_fail_task_create"):
            with self.subTest(failure=failure):
                self.run_flow(r"""
                    lv_obj_t *home = boot_home();
                    server_entry_t entry = {.call = "K1OUT"};
                    %s = true;
                    display_lock();
                    ui_chat_connect(&entry);
                    display_unlock();
                    assert(host_active == home);
                    host_click(lv_layer_top(), "OK");
                    remote_connect("K6REC", "Recovered operator");
                    host_tick(1000);
                    assert(host_text(host_active, "K6REC"));
                    assert(host_text(host_active, "Recovered operator"));
                    host_click(host_active, "Leave");
                    host_tick(1000);
                    assert(host_active == home);
                    start_outgoing();
                    host_tick(2000);
                    assert(host_text(host_active, "K1OUT"));
                """ % failure)


if __name__ == "__main__":
    unittest.main()
