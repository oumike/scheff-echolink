#include "el_directory.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "net_wifi.h"
#include "server_list.h"
#include "settings.h"

static const char *TAG = "el_dir";

#define DIR_PORT          "5200"
#define REFRESH_MS        (5 * 60 * 1000)
#define LIST_REFRESH_MS   (15 * 60 * 1000)
#define IO_TIMEOUT_S      15
#define LIST_MAX_BYTES    (2 * 1024 * 1024)

static SemaphoreHandle_t s_lock;       // guards the station list
static SemaphoreHandle_t s_io_lock;    // one directory transaction at a time
static TaskHandle_t s_task;
static el_station_t *s_list;
static size_t s_count;
static volatile el_dir_status_t s_want = EL_DIR_ONLINE;
static volatile el_dir_status_t s_status = EL_DIR_OFFLINE;
static volatile bool s_logged_in;
static volatile bool s_relogin;

static int dir_connect(void)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    int err = getaddrinfo(g_settings.dir_server, DIR_PORT, &hints, &res);
    if (err != 0 || !res) {
        ESP_LOGE(TAG, "DNS lookup for %s failed (%d)", g_settings.dir_server, err);
        return -1;
    }
    // servers.echolink.org resolves to several hosts; try them in order.
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, 0);
        if (fd < 0) continue;
        struct timeval tv = {.tv_sec = IO_TIMEOUT_S};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) ESP_LOGE(TAG, "could not connect to %s:%s", g_settings.dir_server, DIR_PORT);
    return fd;
}

static bool send_all(int fd, const char *buf, size_t len)
{
    while (len) {
        int n = send(fd, buf, len, 0);
        if (n <= 0) return false;
        buf += n;
        len -= n;
    }
    return true;
}

esp_err_t el_dir_login(el_dir_status_t status)
{
    if (!g_settings.callsign[0] || !g_settings.password[0]) {
        ESP_LOGW(TAG, "callsign/password not set; use 'set callsign ...' and 'set password ...'");
        return ESP_ERR_INVALID_STATE;
    }

    // "l" CALL 0xAC 0xAC PASSWORD CR STATUS CR LOCATION CR
    char tstr[6] = "00:00";
    time_t now = time(NULL);
    struct tm tm;
    if (now > 1700000000) strftime(tstr, sizeof(tstr), "%H:%M", localtime_r(&now, &tm));
    const char *st = status == EL_DIR_OFFLINE ? "OFF-V3.40" : status == EL_DIR_BUSY ? "BUSY3.40(" : "ONLINE3.38(";
    char cmd[256];
    int len = snprintf(cmd, sizeof(cmd), "l%s\xac\xac%s\r%s%s%s\r%s\r", g_settings.callsign, g_settings.password, st,
                       status == EL_DIR_OFFLINE ? "" : tstr, status == EL_DIR_OFFLINE ? "" : ")", g_settings.location);
    if (len <= 0 || len >= sizeof(cmd)) return ESP_ERR_INVALID_SIZE;

    xSemaphoreTake(s_io_lock, portMAX_DELAY);
    esp_err_t ret = ESP_FAIL;
    int fd = dir_connect();
    if (fd >= 0) {
        char resp[64] = {0};
        int got = 0;
        if (send_all(fd, cmd, len)) {
            while (got < sizeof(resp) - 1) {
                int n = recv(fd, resp + got, sizeof(resp) - 1 - got, 0);
                if (n <= 0) break;
                got += n;
                if (got >= 2) break;
            }
        }
        close(fd);
        if (got >= 2 && memcmp(resp, "OK", 2) == 0) {
            ret = ESP_OK;
        } else {
            for (int i = 0; i < got; i++) if (!isprint((unsigned char)resp[i])) resp[i] = ' ';
            ESP_LOGE(TAG, "login rejected: \"%s\"", got ? resp : "(no reply)");
        }
    }
    xSemaphoreGive(s_io_lock);

    if (ret == ESP_OK) {
        s_status = status;
        s_logged_in = status != EL_DIR_OFFLINE;
        ESP_LOGI(TAG, "registered as %s (%s)", g_settings.callsign,
                 status == EL_DIR_OFFLINE ? "offline" : status == EL_DIR_BUSY ? "busy" : "online");
    }
    return ret;
}

// Parse "description [ON 12:34]" style data lines.
static void parse_data(el_station_t *st, char *data)
{
    char *br = strrchr(data, '[');
    if (br) {
        st->busy = strstr(br, "BUSY") != NULL;
        char *end = strchr(br, ']');
        if (end && end - br >= 6) {
            memcpy(st->time, end - 5, 5);
            st->time[5] = 0;
        }
        *br = 0;
    }
    size_t n = strlen(data);
    while (n && data[n - 1] == ' ') data[--n] = 0;
    strlcpy(st->desc, data, sizeof(st->desc));
}

// Response to "s": "@@@\n" COUNT "\n" then COUNT x {call, data, id, ip} lines,
// terminated by "+++". Entries with call "." are padding; call " " carries a
// server message instead of a station.
// Returns the next '\n'-terminated line (NUL-terminated in place, trailing
// '\r' stripped), or NULL if no complete line remains. Empty lines are kept,
// since every record is exactly four lines.
static char *next_line(char **p, char *end)
{
    char *nl = memchr(*p, '\n', end - *p);
    if (!nl) return NULL;
    char *line = *p;
    *nl = 0;
    if (nl > line && nl[-1] == '\r') nl[-1] = 0;
    *p = nl + 1;
    return line;
}

static size_t parse_list(char *buf, size_t len, el_station_t **out)
{
    char *p = buf, *end = buf + len;
    char *hdr = next_line(&p, end);
    if (!hdr || strcmp(hdr, "@@@") != 0) {
        ESP_LOGE(TAG, "bad list header");
        return 0;
    }
    char *line = next_line(&p, end);
    int count = line ? atoi(line) : 0;
    if (count <= 0) return 0;

    el_station_t *list = heap_caps_calloc(count, sizeof(el_station_t), MALLOC_CAP_SPIRAM);
    if (!list) return 0;
    size_t n = 0;
    while (n < (size_t)count) {
        char *call = next_line(&p, end);
        char *data = next_line(&p, end);
        char *id = next_line(&p, end);
        char *ip = next_line(&p, end);
        if (!call || !data || !id || !ip) break;
        if (!strcmp(call, ".")) continue;
        if (!strcmp(call, " ")) {
            ESP_LOGW(TAG, "server: %s", data);
            count--;
            continue;
        }
        el_station_t *st = &list[n++];
        strlcpy(st->call, call, sizeof(st->call));
        parse_data(st, data);
        st->node = strtoul(id, NULL, 10);
        st->ip = inet_addr(ip);
    }
    *out = list;
    return n;
}

esp_err_t el_dir_fetch_list(void)
{
    char *buf = heap_caps_malloc(LIST_MAX_BYTES + 1, MALLOC_CAP_SPIRAM);
    if (!buf) return ESP_ERR_NO_MEM;

    xSemaphoreTake(s_io_lock, portMAX_DELAY);
    size_t got = 0;
    int fd = dir_connect();
    if (fd >= 0) {
        if (send_all(fd, "s", 1)) {
            for (;;) {
                int n = recv(fd, buf + got, LIST_MAX_BYTES - got, 0);
                if (n <= 0) break;
                got += n;
                if (got >= LIST_MAX_BYTES) break;
                if (got >= 4 && !memcmp(buf + got - 4, "+++", 3)) break;
                if (got >= 3 && !memcmp(buf + got - 3, "+++", 3)) break;
            }
        }
        close(fd);
    }
    xSemaphoreGive(s_io_lock);

    el_station_t *list = NULL;
    size_t n = got ? parse_list(buf, got, &list) : 0;
    free(buf);
    if (!n) {
        free(list);
        ESP_LOGE(TAG, "station list fetch failed (%u bytes)", (unsigned)got);
        return ESP_FAIL;
    }

    // First download on this device: saves echolink-default-servers.xml.
    server_list_on_directory(list, n);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    free(s_list);
    s_list = list;
    s_count = n;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "station list: %u entries", (unsigned)n);
    return ESP_OK;
}

static void dir_task(void *arg)
{
    TickType_t next_login = 0, next_list = 0;
    el_dir_status_t attempted = EL_DIR_OFFLINE;
    bool retry = false;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
        if (!net_wifi_has_ip()) continue;

        TickType_t now = xTaskGetTickCount();
        bool relogin = s_relogin;
        if (relogin) s_relogin = false;
        el_dir_status_t want = s_want;
        if (relogin || want != attempted ||
            ((retry || want != EL_DIR_OFFLINE) && (int32_t)(now - next_login) >= 0)) {
            attempted = want;
            retry = el_dir_login(want) != ESP_OK;
            next_login = xTaskGetTickCount() + pdMS_TO_TICKS(retry ? 60 * 1000 : REFRESH_MS);
            if (retry && want != EL_DIR_OFFLINE) continue;
        }
        if (s_logged_in && (!s_count || (int32_t)(now - next_list) >= 0)) {
            next_list = now + pdMS_TO_TICKS(el_dir_fetch_list() == ESP_OK ? LIST_REFRESH_MS : 60 * 1000);
        }
    }
}

void el_dir_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_io_lock = xSemaphoreCreateMutex();
    // Roomy: the first list also writes the server snapshot (stdio + FAT).
    xTaskCreate(dir_task, "el_dir", 10240, NULL, 5, &s_task);
}

void el_dir_set_status(el_dir_status_t status)
{
    s_want = status;
    if (s_task) xTaskNotifyGive(s_task);
}

void el_dir_relogin(void)
{
    s_logged_in = false;
    s_relogin = true;
    if (s_task) xTaskNotifyGive(s_task);
}

el_dir_status_t el_dir_status(void)
{
    return s_status;
}

bool el_dir_logged_in(void)
{
    return s_logged_in;
}

size_t el_dir_count(void)
{
    return s_count;
}

static bool is_digits(const char *s)
{
    if (!*s) return false;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return false;
    return true;
}

// Compare callsigns ignoring case and the "*...*" conference decoration.
static bool call_eq(const char *a, const char *b)
{
    while (*a == '*') a++;
    while (*b == '*') b++;
    size_t la = strlen(a), lb = strlen(b);
    while (la && a[la - 1] == '*') la--;
    while (lb && b[lb - 1] == '*') lb--;
    return la == lb && strncasecmp(a, b, la) == 0;
}

bool el_dir_find(const char *query, el_station_t *out)
{
    bool by_node = is_digits(query);
    uint32_t node = by_node ? strtoul(query, NULL, 10) : 0;
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_count && !found; i++) {
        if (by_node ? s_list[i].node == node : call_eq(s_list[i].call, query)) {
            *out = s_list[i];
            found = true;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

bool el_dir_find_ip(uint32_t ip, el_station_t *out)
{
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_count && !found; i++) {
        if (s_list[i].ip == ip) {
            *out = s_list[i];
            found = true;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

void el_dir_print(const char *filter, int max)
{
    int shown = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_count && shown < max; i++) {
        const el_station_t *st = &s_list[i];
        if (filter && *filter && !strcasestr(st->call, filter) && !strcasestr(st->desc, filter)) continue;
        printf("%7lu  %-12s %-4s %-5s  %s\n", (unsigned long)st->node, st->call, st->busy ? "BUSY" : "ON",
               st->time, st->desc);
        shown++;
    }
    size_t total = s_count;
    xSemaphoreGive(s_lock);
    printf("(%d shown, %u in directory)\n", shown, (unsigned)total);
}
