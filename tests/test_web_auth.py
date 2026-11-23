import unittest

from host_harness import COMMON, defines, function, header, run_c, source


def auth_program():
    code = COMMON + header("linux/compat/sdkconfig.h") + header("linux/compat/esp_http_server.h")
    code += defines("main/web_config.c", "AUTH_")
    web = source("main/web_config.c")
    start = web.index("static char s_access_code[")
    code += web[start:web.index("static struct {", start)]
    code += r"""
static httpd_handle_t s_httpd;
static volatile bool s_dns_run;
static unsigned random_byte, actions, route_count;
static int64_t now = 1000000;
static const char *input_code = "", *request_cookie = "", *origin = "", *site = "";
static char response_cookie[128], retry_after[16];
static int response_status;
static bool fail_registration, stopped;
static httpd_uri_t routes[20];
typedef struct { const char *code; } cJSON;
static cJSON body;
static void esp_fill_random(void *out, size_t n) {
    for (size_t i = 0; i < n; i++) ((uint8_t *)out)[i] = ++random_byte;
}
static int64_t esp_timer_get_time(void) { return now; }
static cJSON *read_json(httpd_req_t *req) { body.code = input_code; return &body; }
static const char *json_str(cJSON *obj, const char *key) { return obj->code; }
static void cJSON_Delete(cJSON *obj) {}
static cJSON *cJSON_CreateObject(void) { return &body; }
static void cJSON_AddBoolToObject(cJSON *obj, const char *key, bool value) {}
static esp_err_t send_json(httpd_req_t *req, cJSON *obj) { return ESP_OK; }
static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message) {
    response_status = atoi(status);
    return ESP_OK;
}
esp_err_t httpd_resp_send_err(httpd_req_t *req, httpd_err_code_t error, const char *message) {
    response_status = 500;
    return ESP_OK;
}
static const char *request_header(const char *field) {
    if (!strcasecmp(field, "Cookie")) return request_cookie;
    if (!strcasecmp(field, "Origin")) return origin;
    if (!strcasecmp(field, "Host")) return "device:8573";
    if (!strcasecmp(field, "Sec-Fetch-Site")) return site;
    return "";
}
size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *field) {
    return strlen(request_header(field));
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *field, char *out, size_t n) {
    const char *value = request_header(field);
    if (!value[0]) return ESP_ERR_NOT_FOUND;
    strlcpy(out, value, n);
    return strlen(value) < n ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *field, const char *value) {
    if (!strcmp(field, "Set-Cookie")) strlcpy(response_cookie, value, sizeof(response_cookie));
    if (!strcmp(field, "Retry-After")) strlcpy(retry_after, value, sizeof(retry_after));
    return ESP_OK;
}
esp_err_t httpd_start(httpd_handle_t *out, const httpd_config_t *cfg) {
    *out = (void *)1;
    return ESP_OK;
}
esp_err_t httpd_stop(httpd_handle_t handle) { stopped = true; route_count = 0; return ESP_OK; }
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *route) {
    if (fail_registration) return ESP_FAIL;
    assert(route_count < sizeof(routes) / sizeof(routes[0]));
    routes[route_count++] = *route;
    return ESP_OK;
}
esp_err_t httpd_register_err_handler(httpd_handle_t handle, httpd_err_code_t code,
                                    httpd_err_handler_func_t handler) { return ESP_OK; }
static esp_err_t h_not_found(httpd_req_t *req, httpd_err_code_t error) { return ESP_OK; }
static esp_err_t h_index(httpd_req_t *req) { return ESP_OK; }
static esp_err_t action(httpd_req_t *req) { actions++; return ESP_OK; }
static void onboard_changed(void) {}
static void onboard_set_web_cb(void (*callback)(void)) {}
static bool onboard_complete(void) { return true; }
static bool net_wifi_ap_active(void) { return false; }
static esp_err_t net_wifi_ap_start(void) { return ESP_FAIL; }
static void dns_task(void *arg) {}
static int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg,
                       unsigned priority, void *handle) { return 1; }
static void reset_response(void) {
    response_status = 200;
    response_cookie[0] = retry_after[0] = 0;
}
"""
    for signature in (
        "const char *web_config_access_code(", "static void auth_init(", "static bool auth_equal(",
        "static bool auth_code_matches(", "static bool same_origin(", "static bool authenticated(",
        "static esp_err_t h_auth(", "static esp_err_t h_authorized(",
    ):
        code += function("main/web_config.c", signature)
    for name in (
        "h_status", "h_scan", "h_wifi", "h_account", "h_servers", "h_server_save", "h_server_delete",
        "h_server_favorite", "h_export_download", "h_import_upload", "h_sd_export", "h_sd_import",
    ):
        code += f"\n#define {name} action\n"
    code += function("main/web_config.c", "esp_err_t web_config_start(")
    return code


class WebAuthTests(unittest.TestCase):
    def test_every_configuration_route_requires_the_current_session(self):
        run_c(auth_program() + r"""
int main(void) {
    assert(web_config_start() == ESP_OK);
    httpd_req_t req = {0};
    for (unsigned i = 0; i < route_count; i++) {
        if (!strcmp(routes[i].uri, "/") || !strcmp(routes[i].uri, "/api/auth")) continue;
        reset_response();
        req.user_ctx = routes[i].user_ctx;
        assert(routes[i].handler(&req) == ESP_OK);
        assert(response_status == 401 && actions == 0);
    }
    input_code = web_config_access_code();
    reset_response();
    assert(h_auth(&req) == ESP_OK && response_status == 200);
    assert(strstr(response_cookie, "HttpOnly") && strstr(response_cookie, "SameSite=Strict"));
    char cookie[sizeof(response_cookie)];
    strcpy(cookie, response_cookie);
    request_cookie = cookie;
    for (unsigned i = 0; i < route_count; i++) {
        if (!strcmp(routes[i].uri, "/") || !strcmp(routes[i].uri, "/api/auth")) continue;
        reset_response();
        unsigned before = actions;
        req.user_ctx = routes[i].user_ctx;
        assert(routes[i].handler(&req) == ESP_OK);
        assert(response_status == 200 && actions == before + 1);
    }
}
""")

    def test_bad_codes_are_rate_limited_and_old_sessions_expire_on_restart(self):
        run_c(auth_program() + r"""
int main(void) {
    assert(web_config_start() == ESP_OK);
    httpd_req_t req = {0};
    char old_code[sizeof(s_access_code)];
    strcpy(old_code, web_config_access_code());
    input_code = "wrong";
    for (unsigned i = 0; i < AUTH_MAX_FAILURES; i++) {
        reset_response();
        assert(h_auth(&req) == ESP_OK && response_status == 401 && !response_cookie[0]);
    }
    input_code = old_code;
    reset_response();
    assert(h_auth(&req) == ESP_OK && response_status == 429 && retry_after[0]);
    now += AUTH_RETRY_US;
    reset_response();
    assert(h_auth(&req) == ESP_OK && response_status == 200 && response_cookie[0]);
    char old_cookie[sizeof(response_cookie)];
    strcpy(old_cookie, response_cookie);
    auth_init();
    request_cookie = old_cookie;
    assert(!authenticated(&req));
    reset_response();
    assert(h_auth(&req) == ESP_OK && response_status == 401 && !response_cookie[0]);
    input_code = web_config_access_code();
    reset_response();
    assert(h_auth(&req) == ESP_OK && response_status == 200);
}
""")

    def test_cookie_boundaries_and_cross_site_requests_cannot_authorize_actions(self):
        run_c(auth_program() + r"""
int main(void) {
    assert(web_config_start() == ESP_OK);
    httpd_uri_t target = {.handler = action};
    httpd_req_t req = {.user_ctx = &target};
    char cookie[256];
    snprintf(cookie, sizeof(cookie), "x" AUTH_COOKIE "%s", s_session_token);
    request_cookie = cookie;
    assert(!authenticated(&req));
    snprintf(cookie, sizeof(cookie), AUTH_COOKIE "%sX", s_session_token);
    assert(!authenticated(&req));
    snprintf(cookie, sizeof(cookie), "other=value; " AUTH_COOKIE "%s; unrelated=1", s_session_token);
    assert(authenticated(&req));
    const char *bad_origins[] = {"http://evil.invalid", "http://device:80", "null"};
    for (size_t i = 0; i < sizeof(bad_origins) / sizeof(bad_origins[0]); i++) {
        origin = bad_origins[i];
        reset_response();
        assert(h_authorized(&req) == ESP_OK && response_status == 403 && actions == 0);
        input_code = web_config_access_code();
        reset_response();
        assert(h_auth(&req) == ESP_OK && response_status == 403 && !response_cookie[0]);
    }
    origin = "";
    site = "cross-site";
    reset_response();
    assert(h_authorized(&req) == ESP_OK && response_status == 403 && actions == 0);
    origin = "http://device:8573";
    site = "same-origin";
    reset_response();
    assert(h_authorized(&req) == ESP_OK && response_status == 200 && actions == 1);
}
""")

    def test_route_registration_failure_stops_the_server(self):
        run_c(auth_program() + r"""
int main(void) {
    fail_registration = true;
    assert(web_config_start() == ESP_FAIL);
    assert(stopped && !s_httpd && !web_config_access_code()[0]);
}
""")


if __name__ == "__main__":
    unittest.main()
