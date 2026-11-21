#include "web_config.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "audio.h"
#include "el_directory.h"
#include "el_qso.h"
#include "file_store.h"
#include "net_wifi.h"
#include "onboard.h"
#include "sdcard.h"
#include "server_list.h"
#include "settings.h"
#include "xml.h"

static const char *TAG = "web";

#define JOIN_TIMEOUT_MS   20000
// After station Wi-Fi connects, keep the hotspot a little longer so the phone
// can show the device's new address before it drops.
#define AP_LINGER_MS      60000
#define BODY_MAX          1024
#define SCAN_MAX          30
#define IMPORT_MAX        ((2 * 1024 * 1024) + 4096)
#define IMPORT_TIMEOUT_MS 30000
#define BUCKET_MAX        3000
#define BACKUP_FILE_NAME  "scheff-echolink-backup.xml"
#define SD_BACKUP_PATH    SDCARD_MOUNT "/" BACKUP_FILE_NAME
// Before the rename to scheff-echolink the SD card backup was
// echolink-p4-backup.xml; a card may still hold one, and import reads it.
#define SD_LEGACY_BACKUP_PATH SDCARD_MOUNT "/echolink-p4-backup.xml"
#define SD_LEGACY_PATH    SDCARD_MOUNT "/echolink-servers.xml"
#define AUTH_COOKIE      "scheff_session="
#define AUTH_CODE_BYTES  8
#define AUTH_TOKEN_BYTES 16
#define AUTH_MAX_FAILURES 5
#define AUTH_RETRY_US    (30 * 1000 * 1000LL)

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t s_httpd;
static volatile bool s_dns_run;
static bool s_ap_stop_pending;
static char s_access_code[AUTH_CODE_BYTES * 2 + 1];
static char s_access_display[AUTH_CODE_BYTES * 2 + 4];
static char s_session_token[AUTH_TOKEN_BYTES * 2 + 1];
static unsigned s_auth_failures;
static int64_t s_auth_retry_us;

static struct {
    enum { JOIN_IDLE, JOIN_BUSY, JOIN_OK, JOIN_FAILED } state;
    char ssid[33];
    char pass[65];
    char msg[96];
} s_join;

// ---- captive DNS: every name resolves to the setup hotspot ------------------

static void dns_task(void *arg)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        if (fd >= 0) close(fd);
        s_dns_run = false;
        vTaskDelete(NULL);
    }
    struct timeval tv = {.tv_sec = 1};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[512];
    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(fd, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12) continue;
        if (buf[2] & 0x80 || (buf[2] & 0x78) || buf[5] != 1) continue;  // queries with one question only

        // Walk the question name to find its type.
        int p = 12;
        while (p < n && buf[p]) p += buf[p] + 1;
        if (p + 5 > n) continue;
        uint16_t qtype = (buf[p + 1] << 8) | buf[p + 2];
        int qend = p + 5;

        buf[2] = 0x84 | (buf[2] & 0x01);  // response, authoritative, keep RD
        buf[3] = 0x80;                    // RA, no error
        buf[6] = 0, buf[7] = qtype == 1 ? 1 : 0;
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        int len = qend;
        if (qtype == 1) {  // A: 192.168.4.1; anything else gets no answers
            static const uint8_t ans[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
            memcpy(buf + qend, ans, sizeof(ans));
            len += sizeof(ans);
        }
        sendto(fd, buf, len, 0, (struct sockaddr *)&from, fl);
    }
    close(fd);
    vTaskDelete(NULL);
}

// ---- helpers ---------------------------------------------------------------

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_sendstr(req, txt);
    free(txt);
    return e;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", msg);
    httpd_resp_set_status(req, status);
    return send_json(req, root);
}

static cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > BODY_MAX) return NULL;
    char *body = malloc(req->content_len + 1);
    if (!body) return NULL;
    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            free(body);
            return NULL;
        }
        got += n;
    }
    body[got] = 0;
    cJSON *j = cJSON_Parse(body);
    free(body);
    return j;
}

static const char *json_str(cJSON *obj, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

// ---- local authorization ---------------------------------------------------

const char *web_config_access_code(void)
{
    return s_httpd ? s_access_display : "";
}

static void auth_init(void)
{
    static const char hex[] = "0123456789ABCDEF";
    uint8_t random[AUTH_CODE_BYTES + AUTH_TOKEN_BYTES];
    esp_fill_random(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); i++) {
        char *out = i < AUTH_CODE_BYTES ? s_access_code + i * 2
                                        : s_session_token + (i - AUTH_CODE_BYTES) * 2;
        out[0] = hex[random[i] >> 4];
        out[1] = hex[random[i] & 15];
    }
    snprintf(s_access_display, sizeof(s_access_display), "%.4s-%.4s-%.4s-%.4s",
             s_access_code, s_access_code + 4, s_access_code + 8, s_access_code + 12);
    s_auth_failures = 0;
    s_auth_retry_us = 0;
}

static bool auth_equal(const char *left, const char *right, size_t len)
{
    unsigned diff = 0;
    for (size_t i = 0; i < len; i++) diff |= (unsigned char)left[i] ^ (unsigned char)right[i];
    return diff == 0;
}

static bool auth_code_matches(const char *input)
{
    char code[sizeof(s_access_code)] = {0};
    size_t n = 0;
    for (; *input; input++) {
        if (*input == '-' || *input == ' ') continue;
        if (!isxdigit((unsigned char)*input) || n == sizeof(code) - 1) return false;
        code[n++] = toupper((unsigned char)*input);
    }
    return n == sizeof(code) - 1 && auth_equal(code, s_access_code, n);
}

static bool same_origin(httpd_req_t *req)
{
    char site[32], origin[160], host[128], expected[sizeof(host) + 8];
    if (httpd_req_get_hdr_value_len(req, "Sec-Fetch-Site")) {
        if (httpd_req_get_hdr_value_str(req, "Sec-Fetch-Site", site, sizeof(site)) != ESP_OK ||
            (strcmp(site, "same-origin") && strcmp(site, "none"))) return false;
    }
    if (!httpd_req_get_hdr_value_len(req, "Origin")) return true;
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) return false;
    snprintf(expected, sizeof(expected), "http://%s", host);
    return !strcmp(origin, expected);
}

static bool authenticated(httpd_req_t *req)
{
    char cookies[512];
    if (!s_session_token[0] ||
        httpd_req_get_hdr_value_str(req, "Cookie", cookies, sizeof(cookies)) != ESP_OK) return false;
    const size_t prefix = sizeof(AUTH_COOKIE) - 1, token_len = sizeof(s_session_token) - 1;
    for (char *p = cookies; *p;) {
        while (*p == ' ' || *p == ';') p++;
        char *end = strchr(p, ';');
        if (!end) end = p + strlen(p);
        if ((size_t)(end - p) == prefix + token_len && !memcmp(p, AUTH_COOKIE, prefix) &&
            auth_equal(p + prefix, s_session_token, token_len)) return true;
        p = end;
    }
    return false;
}

static esp_err_t h_auth(httpd_req_t *req)
{
    if (!same_origin(req)) return send_error(req, "403 Forbidden", "Cross-site requests are not allowed.");
    int64_t now = esp_timer_get_time();
    if (now < s_auth_retry_us) {
        char retry[12];
        snprintf(retry, sizeof(retry), "%u", (unsigned)((s_auth_retry_us - now + 999999) / 1000000));
        httpd_resp_set_hdr(req, "Retry-After", retry);
        return send_error(req, "429 Too Many Requests", "Too many attempts. Wait 30 seconds and try again.");
    }
    if (s_auth_retry_us) {
        s_auth_failures = 0;
        s_auth_retry_us = 0;
    }
    cJSON *body = read_json(req);
    if (!body) return send_error(req, "400 Bad Request", "Expected an access code.");
    bool valid = auth_code_matches(json_str(body, "code"));
    cJSON_Delete(body);
    if (!valid) {
        if (++s_auth_failures >= AUTH_MAX_FAILURES) s_auth_retry_us = now + AUTH_RETRY_US;
        return send_error(req, "401 Unauthorized", "Enter the current access code shown on the device.");
    }
    s_auth_failures = 0;
    char cookie[128];
    snprintf(cookie, sizeof(cookie), AUTH_COOKIE "%s; Path=/; HttpOnly; SameSite=Strict", s_session_token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "authenticated", true);
    return send_json(req, result);
}

static esp_err_t h_authorized(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (!authenticated(req)) {
        return send_error(req, "401 Unauthorized", "Enter the current access code shown on the device.");
    }
    if (!same_origin(req)) return send_error(req, "403 Forbidden", "Cross-site requests are not allowed.");
    const httpd_uri_t *route = req->user_ctx;
    return route ? route->handler(req) : httpd_resp_send_500(req);
}

typedef esp_err_t (*backup_write_cb_t)(void *ctx, const char *data, size_t len);

static esp_err_t backup_write_text(backup_write_cb_t write, void *ctx, const char *text)
{
    return write(ctx, text, strlen(text));
}

static esp_err_t backup_write_xml(backup_write_cb_t write, void *ctx, const char *text)
{
    const char *start = text;
    for (const char *p = text; ; p++) {
        const char *entity = NULL;
        switch (*p) {
        case '&': entity = "&amp;"; break;
        case '<': entity = "&lt;"; break;
        case '>': entity = "&gt;"; break;
        case '"': entity = "&quot;"; break;
        case '\'': entity = "&apos;"; break;
        case '\t': entity = "&#9;"; break;
        case '\n': entity = "&#10;"; break;
        case '\r': entity = "&#13;"; break;
        default: break;
        }
        if (entity) {
            if (p > start && write(ctx, start, p - start) != ESP_OK) return ESP_FAIL;
            if (backup_write_text(write, ctx, entity) != ESP_OK) return ESP_FAIL;
            start = p + 1;
        }
        if (!*p) return p > start ? write(ctx, start, p - start) : ESP_OK;
    }
}

static esp_err_t backup_write_attr(backup_write_cb_t write, void *ctx, const char *name, const char *value)
{
    if (backup_write_text(write, ctx, " ") != ESP_OK || backup_write_text(write, ctx, name) != ESP_OK ||
        backup_write_text(write, ctx, "=\"") != ESP_OK || backup_write_xml(write, ctx, value) != ESP_OK) {
        return ESP_FAIL;
    }
    return backup_write_text(write, ctx, "\"");
}

static esp_err_t backup_write(backup_write_cb_t write, void *ctx)
{
    esp_err_t err = backup_write_text(write, ctx,
                                      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                      "<echolink-backup version=\"1\">\n"
                                      "  <settings version=\"1\"");
#define WRITE_SETTING(name, value) do { if (err == ESP_OK) err = backup_write_attr(write, ctx, name, value); } while (0)
    WRITE_SETTING("callsign", g_settings.callsign);
    WRITE_SETTING("password", g_settings.password);
    WRITE_SETTING("name", g_settings.name);
    WRITE_SETTING("location", g_settings.location);
    WRITE_SETTING("dir_server", g_settings.dir_server);
    WRITE_SETTING("wifi_ssid", g_settings.wifi_ssid);
    WRITE_SETTING("wifi_pass", g_settings.wifi_pass);
    WRITE_SETTING("accept_incoming", g_settings.accept_incoming ? "true" : "false");
    char number[8];
    snprintf(number, sizeof(number), "%u", g_settings.volume);
    WRITE_SETTING("volume", number);
    snprintf(number, sizeof(number), "%u", g_settings.mic_gain);
    WRITE_SETTING("mic_gain", number);
#undef WRITE_SETTING
    if (err == ESP_OK) err = backup_write_text(write, ctx, "/>\n  <servers>\n");

    if (err == ESP_OK) err = server_list_export_entries(write, ctx);
    if (err == ESP_OK) err = backup_write_text(write, ctx, "  </servers>\n</echolink-backup>\n");
    return err;
}

static esp_err_t backup_parse_settings(const char *xml, settings_t *settings, bool *found)
{
    *found = false;
    xml_document_t document;
    esp_err_t err = xml_read(xml, NULL, NULL, &document);
    if (err == ESP_OK) {
        *found = document.backup;
        if (*found) *settings = document.settings;
    }
    return err;
}

static void restored_wifi_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_err_t err = net_wifi_connect(g_settings.wifi_ssid, g_settings.wifi_pass);
    if (err != ESP_OK) ESP_LOGE(TAG, "could not connect to restored Wi-Fi: %s", esp_err_to_name(err));
    onboard_settings_changed();
    vTaskDelete(NULL);
}

static esp_err_t backup_apply_settings(const settings_t *settings, bool *wifi_changed, bool *reconnecting)
{
    settings_t previous = g_settings;
    *wifi_changed = strcmp(settings->wifi_ssid, previous.wifi_ssid) ||
                    strcmp(settings->wifi_pass, previous.wifi_pass);
    *reconnecting = *wifi_changed && settings->wifi_ssid[0];
    g_settings = *settings;
    esp_err_t err = settings_save();
    if (err != ESP_OK) {
        g_settings = previous;
        return err;
    }

    audio_set_volume(g_settings.volume);
    audio_set_mic_gain(g_settings.mic_gain);
    el_qso_update_identity();
    el_dir_relogin();
    if (*reconnecting) {
        if (xTaskCreate(restored_wifi_task, "restore_wifi", 4096, NULL, 5, NULL) != pdPASS) {
            *reconnecting = false;
            onboard_settings_changed();
        }
    } else {
        onboard_settings_changed();
    }
    return ESP_OK;
}

// ---- handlers --------------------------------------------------------------

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t h_status(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    char ip[16] = "";
    bool sta = net_wifi_sta_ip(ip, sizeof(ip));

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wifi, "ssid", g_settings.wifi_ssid);
    cJSON_AddBoolToObject(wifi, "connected", sta);
    cJSON_AddStringToObject(wifi, "ip", ip);

    // Passwords never leave the device; only whether one is set.
    cJSON *acct = cJSON_AddObjectToObject(root, "account");
    cJSON_AddStringToObject(acct, "callsign", g_settings.callsign);
    cJSON_AddStringToObject(acct, "name", g_settings.name);
    cJSON_AddStringToObject(acct, "location", g_settings.location);
    cJSON_AddBoolToObject(acct, "has_password", g_settings.password[0] != 0);

    cJSON *dir = cJSON_AddObjectToObject(root, "dir");
    cJSON_AddBoolToObject(dir, "logged_in", el_dir_logged_in());
    cJSON_AddNumberToObject(dir, "count", el_dir_count());

    cJSON *ap = cJSON_AddObjectToObject(root, "ap");
    cJSON_AddBoolToObject(ap, "active", net_wifi_ap_active());
    cJSON_AddStringToObject(ap, "ssid", net_wifi_ap_active() ? net_wifi_ap_ssid() : "");

    static const char *const STATES[] = {"idle", "joining", "ok", "failed"};
    cJSON *join = cJSON_AddObjectToObject(root, "join");
    cJSON_AddStringToObject(join, "state", STATES[s_join.state]);
    cJSON_AddStringToObject(join, "ssid", s_join.ssid);
    cJSON_AddStringToObject(join, "msg", s_join.msg);

    cJSON_AddNumberToObject(root, "servers", server_list_count());
    cJSON_AddNumberToObject(root, "favorites", server_list_favorite_count());
    cJSON_AddBoolToObject(root, "onboarded", onboard_complete());
    return send_json(req, root);
}

static esp_err_t h_scan(httpd_req_t *req)
{
    net_wifi_ap_t *aps = calloc(SCAN_MAX, sizeof(*aps));
    if (!aps) return httpd_resp_send_500(req);
    int n = net_wifi_scan(aps, SCAN_MAX);
    if (n < 0) {
        free(aps);
        return send_error(req, "503 Service Unavailable", "Scan failed. Try again.");
    }
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(o, "secure", aps[i].secure);
        cJSON_AddItemToArray(arr, o);
    }
    free(aps);
    return send_json(req, arr);
}

static void join_task(void *arg)
{
    net_join_result_t res = net_wifi_join(s_join.ssid, s_join.pass, JOIN_TIMEOUT_MS);
    switch (res) {
    case NET_JOIN_OK: s_join.msg[0] = 0; break;
    case NET_JOIN_BAD_PASSWORD: strlcpy(s_join.msg, "The password was rejected.", sizeof(s_join.msg)); break;
    case NET_JOIN_NOT_FOUND:
        snprintf(s_join.msg, sizeof(s_join.msg), "Couldn't find \"%s\". Check the name, or scan again.", s_join.ssid);
        break;
    case NET_JOIN_TIMEOUT: strlcpy(s_join.msg, "Timed out waiting for the network.", sizeof(s_join.msg)); break;
    default: strlcpy(s_join.msg, "Couldn't connect to the network.", sizeof(s_join.msg)); break;
    }
    if (res == NET_JOIN_OK && onboard_save_wifi(s_join.ssid, s_join.pass) != ESP_OK) {
        res = NET_JOIN_FAILED;
        strlcpy(s_join.msg, "Connected, but couldn't save the Wi-Fi settings.", sizeof(s_join.msg));
    }
    memset(s_join.pass, 0, sizeof(s_join.pass));
    s_join.state = res == NET_JOIN_OK ? JOIN_OK : JOIN_FAILED;
    vTaskDelete(NULL);
}

static esp_err_t h_wifi(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "Bad request.");
    const char *ssid = json_str(j, "ssid");
    if (!ssid[0] || strlen(ssid) > 32) {
        cJSON_Delete(j);
        return send_error(req, "400 Bad Request", "Enter a network name.");
    }
    if (s_join.state == JOIN_BUSY) {
        cJSON_Delete(j);
        return send_error(req, "409 Conflict", "Already connecting. Wait a moment.");
    }
    strlcpy(s_join.ssid, ssid, sizeof(s_join.ssid));
    strlcpy(s_join.pass, json_str(j, "pass"), sizeof(s_join.pass));
    cJSON_Delete(j);
    s_join.msg[0] = 0;
    s_join.state = JOIN_BUSY;
    // Answer first: joining can move the hotspot's channel and drop the phone
    // for a moment. The page polls /api/status for the outcome.
    if (xTaskCreate(join_task, "web_join", 4096, NULL, 5, NULL) != pdPASS) {
        s_join.state = JOIN_FAILED;
        return send_error(req, "500 Internal Server Error", "Couldn't start the join.");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", "joining");
    return send_json(req, root);
}

static esp_err_t h_account(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "Bad request.");
    // A blank password keeps the saved one, since the page never sees it.
    const char *pass = json_str(j, "password");
    char keep[sizeof(g_settings.password)];
    strlcpy(keep, pass[0] ? pass : g_settings.password, sizeof(keep));

    const char *err = NULL;
    esp_err_t e = onboard_save_account(json_str(j, "callsign"), keep, json_str(j, "name"), json_str(j, "location"),
                                       &err);
    cJSON_Delete(j);
    memset(keep, 0, sizeof(keep));
    if (e != ESP_OK) return send_error(req, "400 Bad Request", err ? err : "Couldn't save.");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

// ---- servers ---------------------------------------------------------------

static const char *const TYPE_NAMES[] = {"conference", "link", "repeater", "other"};

// Appends `v` to `out` as a JSON string literal.
static size_t json_quote(char *out, size_t size, const char *v)
{
    size_t o = 0;
    if (o < size) out[o++] = '"';
    for (; *v && o + 7 < size; v++) {
        unsigned char c = *v;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = c;
        } else if (c < 0x20 || c >= 0x7f) {
            o += snprintf(out + o, size - o, "\\u%04x", c);
        } else {
            out[o++] = c;
        }
    }
    if (o < size) out[o++] = '"';
    return o;
}

// Decodes %XX and '+' in a query value, in place.
static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

// GET /api/servers?letter=A (or "*" for favorites, "#" for digits), or
// ?q=text to search. Streamed, since a busy letter runs to hundreds.
static esp_err_t h_servers(httpd_req_t *req)
{
    char qs[160] = "", letter[8] = "A", query[64] = "";
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
        httpd_query_key_value(qs, "letter", letter, sizeof(letter));
        httpd_query_key_value(qs, "q", query, sizeof(query));
    }
    url_decode(letter);
    url_decode(query);
    char bucket = toupper((unsigned char)letter[0]);
    if (bucket != SERVER_BUCKET_FAVORITES && !(bucket >= 'A' && bucket <= 'Z')) bucket = '#';

    server_entry_t *list = heap_caps_malloc(BUCKET_MAX * sizeof(*list), MALLOC_CAP_SPIRAM);
    if (!list) return httpd_resp_send_500(req);
    size_t n = query[0] ? server_list_search(query, list, BUCKET_MAX) : server_list_get_bucket(bucket, list, BUCKET_MAX);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char buf[512];
    httpd_resp_send_chunk(req, "[", 1);
    for (size_t i = 0; i < n; i++) {
        const server_entry_t *e = &list[i];
        size_t o = snprintf(buf, sizeof(buf), "%s{\"call\":", i ? "," : "");
        o += json_quote(buf + o, sizeof(buf) - o, e->call);
        o += snprintf(buf + o, sizeof(buf) - o, ",\"node\":%lu,\"type\":\"%s\",\"manual\":%s,\"favorite\":%s,\"desc\":",
                      (unsigned long)e->node, TYPE_NAMES[e->type], e->manual ? "true" : "false",
                      e->favorite ? "true" : "false");
        o += json_quote(buf + o, sizeof(buf) - o, e->desc);
        o += snprintf(buf + o, sizeof(buf) - o, ",\"host\":");
        o += json_quote(buf + o, sizeof(buf) - o, e->host);
        o += snprintf(buf + o, sizeof(buf) - o, "}");
        if (httpd_resp_send_chunk(req, buf, o) != ESP_OK) break;
    }
    free(list);
    httpd_resp_send_chunk(req, "]", 1);
    return httpd_resp_send_chunk(req, NULL, 0);
}

// POST /api/servers {orig_call, call, node, desc, host}: add or edit.
static esp_err_t h_server_save(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "Bad request.");
    server_entry_t e = {0};
    strlcpy(e.call, json_str(j, "call"), sizeof(e.call));
    for (char *c = e.call; *c; c++) *c = toupper((unsigned char)*c);
    strlcpy(e.desc, json_str(j, "desc"), sizeof(e.desc));
    strlcpy(e.host, json_str(j, "host"), sizeof(e.host));
    cJSON *node = cJSON_GetObjectItemCaseSensitive(j, "node");
    e.node = cJSON_IsNumber(node) && node->valuedouble > 0 ? (uint32_t)node->valuedouble
             : cJSON_IsString(node) ? strtoul(node->valuestring, NULL, 10) : 0;
    char orig[sizeof(e.call)];
    strlcpy(orig, json_str(j, "orig_call"), sizeof(orig));
    cJSON_Delete(j);

    if (!e.call[0] || strchr(e.call, ' ')) return send_error(req, "400 Bad Request", "A callsign is required.");
    if (!e.node && !e.host[0]) {
        return send_error(req, "400 Bad Request", "Enter a node number, or a host / IP address to connect to.");
    }
    // Keep what the form doesn't carry (last-seen IP, manual flag) on edits.
    if (orig[0]) {
        server_entry_t *cur = heap_caps_malloc(BUCKET_MAX * sizeof(*cur), MALLOC_CAP_SPIRAM);
        if (cur) {
            size_t n = server_list_get_bucket(server_list_bucket(orig), cur, BUCKET_MAX);
            for (size_t i = 0; i < n; i++) {
                if (!strcasecmp(cur[i].call, orig)) {
                    e.ip = cur[i].ip;
                    e.manual = cur[i].manual;
                    e.favorite = cur[i].favorite;
                    break;
                }
            }
            free(cur);
        }
    }
    e.type = SERVER_OTHER;  // re-derived from the callsign
    esp_err_t err = server_list_put(orig[0] ? orig : NULL, &e);
    if (err != ESP_OK) return send_error(req, "500 Internal Server Error", "Couldn't save the server list.");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

// POST /api/servers/delete {call}
static esp_err_t h_server_delete(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "Bad request.");
    char call[16];
    strlcpy(call, json_str(j, "call"), sizeof(call));
    cJSON_Delete(j);
    esp_err_t err = server_list_delete(call);
    if (err == ESP_ERR_NOT_FOUND) return send_error(req, "404 Not Found", "No such server.");
    if (err != ESP_OK) return send_error(req, "500 Internal Server Error", "Couldn't save the server list.");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

// POST /api/servers/favorite {call, favorite}
static esp_err_t h_server_favorite(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "Bad request.");
    char call[16];
    strlcpy(call, json_str(j, "call"), sizeof(call));
    bool fav = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "favorite"));
    cJSON_Delete(j);
    esp_err_t err = server_list_set_favorite(call, fav);
    if (err == ESP_ERR_NOT_FOUND) return send_error(req, "404 Not Found", "No such server.");
    if (err != ESP_OK) return send_error(req, "500 Internal Server Error", "Couldn't save the server list.");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "favorite", fav);
    return send_json(req, root);
}

static esp_err_t backup_http_write(void *ctx, const char *data, size_t len)
{
    return httpd_resp_send_chunk(ctx, data, len);
}

static esp_err_t backup_file_write(void *ctx, const char *data, size_t len)
{
    return fwrite(data, 1, len, ctx) == len ? ESP_OK : ESP_FAIL;
}

static esp_err_t backup_write_file(FILE *file, void *ctx)
{
    return backup_write(backup_file_write, file);
}

// GET /api/servers/export: all settings and the working list as one download.
static esp_err_t h_export_download(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/xml");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"" BACKUP_FILE_NAME "\"");
    esp_err_t err = backup_write(backup_http_write, req);
    return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
}

static esp_err_t import_result(httpd_req_t *req, esp_err_t err, size_t count, const char *what,
                               bool settings_restored, bool wifi_changed, bool reconnecting)
{
    if (err == ESP_ERR_NOT_FOUND) {
        return send_error(req, "400 Bad Request", "That file has no <server> entries; nothing was changed.");
    }
    if (err == ESP_ERR_INVALID_SIZE) return send_error(req, "400 Bad Request", "That file is empty or too large.");
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_VERSION) {
        return send_error(req, "400 Bad Request", "That file is not a valid server list; nothing was changed.");
    }
    if (err != ESP_OK) return send_error(req, "500 Internal Server Error", "Import failed; nothing was changed.");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "count", count);
    cJSON_AddStringToObject(root, "from", what);
    cJSON_AddBoolToObject(root, "settings", settings_restored);
    cJSON_AddBoolToObject(root, "wifi_changed", wifi_changed);
    cJSON_AddBoolToObject(root, "reconnecting", reconnecting);
    return send_json(req, root);
}

static esp_err_t import_backup(httpd_req_t *req, char *xml, const char *what)
{
    settings_t restored;
    bool has_settings;
    esp_err_t err = backup_parse_settings(xml, &restored, &has_settings);
    if (err == ESP_ERR_INVALID_VERSION) {
        return send_error(req, "400 Bad Request", "This backup uses an unsupported settings version.");
    }
    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", "The backup settings are incomplete or invalid; nothing was changed.");
    }

    size_t count = 0;
    err = server_list_import_buffer(xml, &count);
    if (err != ESP_OK) return import_result(req, err, count, what, false, false, false);

    bool wifi_changed = false, reconnecting = false;
    if (has_settings) {
        err = backup_apply_settings(&restored, &wifi_changed, &reconnecting);
        if (err != ESP_OK) {
            return send_error(req, "500 Internal Server Error",
                              "The server list was restored, but the settings could not be saved.");
        }
    }
    return import_result(req, ESP_OK, count, what, has_settings, wifi_changed, reconnecting);
}

// POST /api/servers/import: the request body is a complete backup or a legacy
// XML server list.
static esp_err_t h_import_upload(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > IMPORT_MAX) {
        return send_error(req, "400 Bad Request", "That file is empty or too large.");
    }
    char *buf = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM);
    if (!buf) return httpd_resp_send_500(req);
    int64_t deadline = esp_timer_get_time() + IMPORT_TIMEOUT_MS * 1000LL;
    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT || esp_timer_get_time() >= deadline) {
            free(buf);
            send_error(req, "408 Request Timeout", "The upload timed out; nothing was changed.");
            // Close the session rather than reuse a connection with an unread body.
            return ESP_FAIL;
        }
        if (n <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        got += n;
    }
    if (memchr(buf, 0, got)) {
        free(buf);
        return send_error(req, "400 Bad Request", "The backup contains invalid XML bytes.");
    }
    buf[got] = 0;
    esp_err_t err = import_backup(req, buf, "upload");
    free(buf);
    return err;
}

static esp_err_t sd_error(httpd_req_t *req)
{
    return send_error(req, "503 Service Unavailable", "No SD card found. Insert a FAT-formatted microSD card.");
}

// POST /api/sd/export and /api/sd/import: complete backup at the root of the
// microSD card. Import also accepts the pre-rename backup and the old
// server-list-only filename.
static esp_err_t h_sd_export(httpd_req_t *req)
{
    if (sdcard_mount() != ESP_OK) return sd_error(req);
    esp_err_t err = file_store_write(SD_BACKUP_PATH, backup_write_file, NULL);
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "Couldn't write to the SD card.");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "path", BACKUP_FILE_NAME);
    cJSON_AddNumberToObject(root, "count", server_list_count());
    cJSON_AddBoolToObject(root, "settings", true);
    return send_json(req, root);
}

static esp_err_t backup_read_file(const char *path, char **out)
{
    esp_err_t err = file_store_recover(path);
    return err == ESP_OK ? file_store_read_text(path, IMPORT_MAX, out, NULL) : err;
}

static esp_err_t h_sd_import(httpd_req_t *req)
{
    if (sdcard_mount() != ESP_OK) return sd_error(req);
    char *xml = NULL;
    esp_err_t err = backup_read_file(SD_BACKUP_PATH, &xml);
    if (err == ESP_ERR_NOT_FOUND) err = backup_read_file(SD_LEGACY_BACKUP_PATH, &xml);
    if (err == ESP_ERR_NOT_FOUND) err = backup_read_file(SD_LEGACY_PATH, &xml);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_error(req, "404 Not Found", "No " BACKUP_FILE_NAME " on the SD card.");
    }
    if (err == ESP_ERR_INVALID_SIZE) return send_error(req, "400 Bad Request", "That file is empty or too large.");
    if (err != ESP_OK) return send_error(req, "500 Internal Server Error", "Couldn't read the SD card backup.");
    err = import_backup(req, xml, "sd");
    free(xml);
    return err;
}

// Phones probe fixed URLs (generate_204, hotspot-detect.html, ...) to detect
// a captive portal; sending those to the page makes the setup sheet pop up.
static esp_err_t h_not_found(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

// ---- hotspot lifetime ------------------------------------------------------

static void ap_stop_task(void *arg)
{
    while (net_wifi_ap_active()) {
        while (net_wifi_ap_active() && !net_wifi_has_ip()) vTaskDelay(pdMS_TO_TICKS(500));
        if (!net_wifi_ap_active()) break;

        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(AP_LINGER_MS);
        while (net_wifi_ap_active() && net_wifi_has_ip() &&
               (int32_t)(deadline - xTaskGetTickCount()) > 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (net_wifi_has_ip()) {
            s_dns_run = false;
            net_wifi_ap_stop();
        }
    }
    s_ap_stop_pending = false;
    vTaskDelete(NULL);
}

static void onboard_changed(void)
{
    if (net_wifi_ap_active() && !s_ap_stop_pending) {
        s_ap_stop_pending = true;
        if (xTaskCreate(ap_stop_task, "web_ap_stop", 3072, NULL, 3, NULL) != pdPASS) s_ap_stop_pending = false;
    }
}

esp_err_t web_config_start(void)
{
    auth_init();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    esp_err_t e = httpd_start(&s_httpd, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "httpd start: %s", esp_err_to_name(e));
        return e;
    }

    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/api/auth", .method = HTTP_POST, .handler = h_auth},
        {.uri = "/api/status", .method = HTTP_GET, .handler = h_status},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = h_scan},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = h_wifi},
        {.uri = "/api/account", .method = HTTP_POST, .handler = h_account},
        {.uri = "/api/servers", .method = HTTP_GET, .handler = h_servers},
        {.uri = "/api/servers", .method = HTTP_POST, .handler = h_server_save},
        {.uri = "/api/servers/delete", .method = HTTP_POST, .handler = h_server_delete},
        {.uri = "/api/servers/favorite", .method = HTTP_POST, .handler = h_server_favorite},
        {.uri = "/api/servers/export", .method = HTTP_GET, .handler = h_export_download},
        {.uri = "/api/servers/import", .method = HTTP_POST, .handler = h_import_upload},
        {.uri = "/api/sd/export", .method = HTTP_POST, .handler = h_sd_export},
        {.uri = "/api/sd/import", .method = HTTP_POST, .handler = h_sd_import},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_uri_t route = uris[i];
        if (route.handler != h_index && route.handler != h_auth) {
            route.handler = h_authorized;
            route.user_ctx = (void *)&uris[i];
        }
        e = httpd_register_uri_handler(s_httpd, &route);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "registering %s failed: %s", route.uri, esp_err_to_name(e));
            httpd_stop(s_httpd);
            s_httpd = NULL;
            return e;
        }
    }
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, h_not_found);

    onboard_set_web_cb(onboard_changed);

    // Until onboarding is done, also offer the setup hotspot. Its monitor
    // turns it off after station Wi-Fi has a confirmed IP.
    if (!onboard_complete() && net_wifi_ap_start() == ESP_OK) {
        s_dns_run = true;
        xTaskCreate(dns_task, "web_dns", 3072, NULL, 4, NULL);
        onboard_changed();
    }
    ESP_LOGI(TAG, "web config on port %u%s", (unsigned)cfg.server_port, net_wifi_ap_active() ? " (setup hotspot up)" : "");
    printf("Web config access code: %s\n", s_access_display);
    return ESP_OK;
}
