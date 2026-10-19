#include "server_list.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "lwip/inet.h"
#include "wear_levelling.h"

static const char *TAG = "servers";

#define MOUNT_POINT    "/data"
#define PARTITION      "storage"
#define TMP_SUFFIX     ".tmp"
#define FILE_MAX_BYTES (2 * 1024 * 1024)

static SemaphoreHandle_t s_lock;
static server_entry_t *s_list;
static size_t s_count, s_cap;
static bool s_mounted;
static void (*s_changed_cb)(void);

static const char *const TYPE_NAMES[] = {"conference", "link", "repeater", "other"};

// ---- helpers ---------------------------------------------------------------

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

// Callsign without the "*...*" conference decoration, for sorting.
static const char *sort_key(const char *call)
{
    while (*call == '*') call++;
    return call;
}

static int entry_cmp(const void *a, const void *b)
{
    const server_entry_t *x = a, *y = b;
    int c = strcasecmp(sort_key(x->call), sort_key(y->call));
    return c ? c : strcmp(x->call, y->call);
}

char server_list_bucket(const char *call)
{
    call = sort_key(call);
    char c = toupper((unsigned char)*call);
    return (c >= 'A' && c <= 'Z') ? c : '#';
}

static server_type_t type_from_call(const char *call)
{
    size_t n = strlen(call);
    if (call[0] == '*') return SERVER_CONFERENCE;
    if (n > 2 && call[n - 2] == '-' && toupper((unsigned char)call[n - 1]) == 'L') return SERVER_LINK;
    if (n > 2 && call[n - 2] == '-' && toupper((unsigned char)call[n - 1]) == 'R') return SERVER_REPEATER;
    return SERVER_OTHER;
}

static server_type_t type_from_name(const char *name)
{
    for (int i = 0; i < 4; i++) {
        if (!strcmp(name, TYPE_NAMES[i])) return i;
    }
    return SERVER_OTHER;
}

typedef struct {
    server_entry_t *v;
    size_t n, cap;
} vec_t;

static bool vec_reserve(vec_t *vec, size_t n)
{
    if (n <= vec->cap) return true;
    size_t cap = vec->cap ? vec->cap : 256;
    while (cap < n) cap *= 2;
    server_entry_t *p = heap_caps_realloc(vec->v, cap * sizeof(*p), MALLOC_CAP_SPIRAM);
    if (!p) return false;
    vec->v = p;
    vec->cap = cap;
    return true;
}

static bool reserve(size_t n)
{
    vec_t vec = {s_list, s_count, s_cap};
    if (!vec_reserve(&vec, n)) return false;
    s_list = vec.v;
    s_cap = vec.cap;
    return true;
}

static void notify_changed(void)
{
    if (s_changed_cb) s_changed_cb();
}

// ---- XML -------------------------------------------------------------------

static void xml_escape(FILE *f, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&': fputs("&amp;", f); break;
        case '<': fputs("&lt;", f); break;
        case '>': fputs("&gt;", f); break;
        case '"': fputs("&quot;", f); break;
        case '\'': fputs("&apos;", f); break;
        default:
            // Directory text is Latin-1-ish; keep the file valid UTF-8 by
            // dropping anything outside printable ASCII.
            if ((unsigned char)*s >= 0x20 && (unsigned char)*s < 0x7f) fputc(*s, f);
        }
    }
}

// Copies attribute `name` of the element in [el, el_end) into `out`,
// decoding the five predefined entities. Returns false if absent.
static bool xml_attr(const char *el, const char *el_end, const char *name, char *out, size_t size)
{
    size_t nlen = strlen(name);
    for (const char *p = el; p + nlen + 2 < el_end; p++) {
        if (!isspace((unsigned char)p[-1]) || strncmp(p, name, nlen) || p[nlen] != '=') continue;
        char q = p[nlen + 1];
        if (q != '"' && q != '\'') continue;
        const char *v = p + nlen + 2;
        size_t o = 0;
        while (v < el_end && *v != q) {
            char c = *v++;
            if (c == '&') {
                static const struct { const char *ent; char ch; } ents[] = {
                    {"amp;", '&'}, {"lt;", '<'}, {"gt;", '>'}, {"quot;", '"'}, {"apos;", '\''},
                };
                for (int i = 0; i < 5; i++) {
                    size_t el_len = strlen(ents[i].ent);
                    if (v + el_len <= el_end && !strncmp(v, ents[i].ent, el_len)) {
                        c = ents[i].ch;
                        v += el_len;
                        break;
                    }
                }
            }
            if (o + 1 < size) out[o++] = c;
        }
        out[o] = 0;
        return true;
    }
    return false;
}

// Parses every <server .../> element of `buf` (NUL-terminated) into `out`,
// sorted. Unknown attributes are ignored; an element without a callsign is
// skipped.
static esp_err_t parse_xml(const char *buf, vec_t *out)
{
    out->n = 0;
    for (const char *p = strstr(buf, "<server"); p; p = strstr(p, "<server")) {
        const char *el_end = strstr(p, "/>");
        if (!el_end) break;
        if (!isspace((unsigned char)p[7])) {  // e.g. "<servers"
            p += 7;
            continue;
        }
        if (!vec_reserve(out, out->n + 1)) return ESP_ERR_NO_MEM;
        server_entry_t *e = &out->v[out->n];
        memset(e, 0, sizeof(*e));
        char tmp[64];
        if (xml_attr(p, el_end, "call", e->call, sizeof(e->call)) && e->call[0]) {
            xml_attr(p, el_end, "desc", e->desc, sizeof(e->desc));
            xml_attr(p, el_end, "host", e->host, sizeof(e->host));
            if (xml_attr(p, el_end, "node", tmp, sizeof(tmp))) e->node = strtoul(tmp, NULL, 10);
            if (xml_attr(p, el_end, "ip", tmp, sizeof(tmp)) && tmp[0]) e->ip = inet_addr(tmp);
            e->type = xml_attr(p, el_end, "type", tmp, sizeof(tmp)) ? type_from_name(tmp) : type_from_call(e->call);
            e->manual = xml_attr(p, el_end, "manual", tmp, sizeof(tmp)) && !strcmp(tmp, "true");
            e->favorite = xml_attr(p, el_end, "favorite", tmp, sizeof(tmp)) && !strcmp(tmp, "true");
            out->n++;
        }
        p = el_end + 2;
    }
    qsort(out->v, out->n, sizeof(*out->v), entry_cmp);
    return ESP_OK;
}

static esp_err_t read_file(const char *path, char **out, size_t *len)
{
    FILE *f = fopen(path, "r");
    if (!f) return ESP_ERR_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > FILE_MAX_BYTES) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    char *buf = heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t got = fread(buf, 1, size, f);
    fclose(f);
    buf[got] = 0;
    *out = buf;
    *len = got;
    return ESP_OK;
}

// Replaces the in-memory list with the contents of `path`. Call with s_lock.
static esp_err_t load_file(const char *path)
{
    char *buf;
    size_t len;
    esp_err_t err = read_file(path, &buf, &len);
    if (err != ESP_OK) return err;
    vec_t vec = {s_list, 0, s_cap};
    err = parse_xml(buf, &vec);
    free(buf);
    s_list = vec.v;
    s_cap = vec.cap;
    s_count = err == ESP_OK ? vec.n : 0;
    ESP_LOGI(TAG, "loaded %u entries from %s", (unsigned)s_count, path);
    return err;
}

// Writes to "<path>.tmp" and swaps it in, so a power cut mid-write leaves the
// previous file (or the .tmp, which load picks up) rather than a torn one.
static esp_err_t save_file(const char *path)
{
    char tmp_path[128];
    snprintf(tmp_path, sizeof(tmp_path), "%s" TMP_SUFFIX, path);
    FILE *f = fopen(tmp_path, "w");
    ESP_RETURN_ON_FALSE(f, ESP_FAIL, TAG, "open %s", tmp_path);

    char when[32] = "";
    time_t now = time(NULL);
    if (now > 1700000000) {
        struct tm tm;
        strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", gmtime_r(&now, &tm));
    }
    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f, "<echolink-servers version=\"1\" generated=\"%s\">\n", when);
    for (size_t i = 0; i < s_count; i++) {
        const server_entry_t *e = &s_list[i];
        fputs("  <server call=\"", f);
        xml_escape(f, e->call);
        fprintf(f, "\" node=\"%lu\" type=\"%s\" desc=\"", (unsigned long)e->node, TYPE_NAMES[e->type]);
        xml_escape(f, e->desc);
        fputs("\" ip=\"", f);
        if (e->ip) {
            struct in_addr a = {.s_addr = e->ip};
            fputs(inet_ntoa(a), f);
        }
        fputs("\" host=\"", f);
        xml_escape(f, e->host);
        fprintf(f, "\"%s%s/>\n", e->manual ? " manual=\"true\"" : "", e->favorite ? " favorite=\"true\"" : "");
    }
    fprintf(f, "</echolink-servers>\n");
    bool ok = !ferror(f);
    ok = (fclose(f) == 0) && ok;
    ESP_RETURN_ON_FALSE(ok, ESP_FAIL, TAG, "write %s", tmp_path);

    remove(path);
    ESP_RETURN_ON_FALSE(rename(tmp_path, path) == 0, ESP_FAIL, TAG, "rename to %s", path);
    ESP_LOGI(TAG, "saved %u entries to %s", (unsigned)s_count, path);
    return ESP_OK;
}

// A save interrupted between remove() and rename() leaves only the .tmp.
static void recover_tmp(const char *path)
{
    char tmp_path[128];
    snprintf(tmp_path, sizeof(tmp_path), "%s" TMP_SUFFIX, path);
    if (!file_exists(path) && file_exists(tmp_path)) {
        ESP_LOGW(TAG, "recovering %s from %s", path, tmp_path);
        rename(tmp_path, path);
    }
}

// ---- public ----------------------------------------------------------------

esp_err_t server_list_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    esp_vfs_fat_mount_config_t mcfg = {
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    wl_handle_t wl;
    ESP_RETURN_ON_ERROR(esp_vfs_fat_spiflash_mount_rw_wl(MOUNT_POINT, PARTITION, &mcfg, &wl), TAG,
                        "mount " MOUNT_POINT);
    s_mounted = true;

    recover_tmp(SERVER_LIST_DEFAULT_PATH);
    recover_tmp(SERVER_LIST_PATH);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = load_file(SERVER_LIST_PATH);
    if (err == ESP_ERR_NOT_FOUND && load_file(SERVER_LIST_DEFAULT_PATH) == ESP_OK) {
        save_file(SERVER_LIST_PATH);
    } else if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "%s unreadable: %s", SERVER_LIST_PATH, esp_err_to_name(err));
    }
    xSemaphoreGive(s_lock);

    if (!s_count) ESP_LOGI(TAG, "no server list yet; it is created on the first directory download");
    return ESP_OK;
}

void server_list_on_directory(const el_station_t *stations, size_t count)
{
    if (!s_mounted || file_exists(SERVER_LIST_DEFAULT_PATH)) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    // Keep any manual entries made before the first download.
    size_t kept = 0;
    for (size_t i = 0; i < s_count; i++) {
        if (s_list[i].manual) s_list[kept++] = s_list[i];
    }
    s_count = kept;

    for (size_t i = 0; i < count; i++) {
        server_type_t t = type_from_call(stations[i].call);
        if (t == SERVER_OTHER) continue;  // individual stations
        if (!reserve(s_count + 1)) break;
        server_entry_t *e = &s_list[s_count++];
        memset(e, 0, sizeof(*e));
        strlcpy(e->call, stations[i].call, sizeof(e->call));
        strlcpy(e->desc, stations[i].desc, sizeof(e->desc));
        e->node = stations[i].node;
        e->ip = stations[i].ip;
        e->type = t;
    }
    qsort(s_list, s_count, sizeof(*s_list), entry_cmp);
    size_t nodes = s_count - kept;
    ESP_LOGI(TAG, "directory: %u stations, %u conferences/links/repeaters", (unsigned)count, (unsigned)nodes);
    if (!nodes) {
        // Don't freeze an empty snapshot as the default; try the next list.
        xSemaphoreGive(s_lock);
        return;
    }

    // The default holds the directory snapshot only; manual entries live in
    // the working copy.
    server_entry_t *all = s_list;
    size_t all_count = s_count;
    server_entry_t *snap = heap_caps_malloc((all_count ? all_count : 1) * sizeof(*snap), MALLOC_CAP_SPIRAM);
    if (snap) {
        size_t n = 0;
        for (size_t i = 0; i < all_count; i++) {
            if (!all[i].manual) snap[n++] = all[i];
        }
        s_list = snap;
        s_count = n;
        save_file(SERVER_LIST_DEFAULT_PATH);
        s_list = all;
        s_count = all_count;
        free(snap);
    }
    save_file(SERVER_LIST_PATH);
    xSemaphoreGive(s_lock);
    notify_changed();
}

size_t server_list_count(void)
{
    return s_count;
}

size_t server_list_get_bucket(char bucket, server_entry_t *out, size_t max)
{
    size_t n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_count && n < max; i++) {
        bool match = bucket == SERVER_BUCKET_FAVORITES ? s_list[i].favorite
                                                       : server_list_bucket(s_list[i].call) == bucket;
        if (match) out[n++] = s_list[i];
    }
    xSemaphoreGive(s_lock);
    return n;
}

size_t server_list_search(const char *query, server_entry_t *out, size_t max)
{
    size_t qlen = strlen(query);
    bool digits = qlen > 0;
    for (const char *c = query; *c; c++) digits = digits && isdigit((unsigned char)*c);

    size_t n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_count && n < max; i++) {
        const server_entry_t *e = &s_list[i];
        bool match = strcasestr(e->call, query) || strcasestr(e->desc, query);
        if (!match && digits && e->node) {
            char node[12];
            snprintf(node, sizeof(node), "%lu", (unsigned long)e->node);
            match = !strncmp(node, query, qlen);
        }
        if (match) out[n++] = *e;
    }
    xSemaphoreGive(s_lock);
    return n;
}

size_t server_list_favorite_count(void)
{
    size_t n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_count; i++) n += s_list[i].favorite;
    xSemaphoreGive(s_lock);
    return n;
}

static server_entry_t *find_locked(const char *call)
{
    for (size_t i = 0; i < s_count; i++) {
        if (!strcasecmp(s_list[i].call, call)) return &s_list[i];
    }
    return NULL;
}

esp_err_t server_list_put(const char *orig_call, const server_entry_t *entry)
{
    ESP_RETURN_ON_FALSE(s_mounted, ESP_ERR_INVALID_STATE, TAG, "storage not mounted");
    ESP_RETURN_ON_FALSE(entry && entry->call[0], ESP_ERR_INVALID_ARG, TAG, "empty callsign");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    server_entry_t *slot = orig_call && orig_call[0] ? find_locked(orig_call) : NULL;
    bool is_new = !slot;
    // Renaming onto another entry's callsign replaces that one; drop the
    // edited original so the call stays unique.
    server_entry_t *same = find_locked(entry->call);
    if (slot && same && same != slot) {
        *same = s_list[--s_count];
        if (slot == &s_list[s_count]) slot = same;
    } else if (!slot) {
        slot = same;
    }
    if (!slot && reserve(s_count + 1)) slot = &s_list[s_count++];

    esp_err_t err = ESP_ERR_NO_MEM;
    if (slot) {
        bool was_manual = slot->manual && !is_new;
        *slot = *entry;
        slot->manual = is_new || was_manual || entry->manual;
        if (slot->type == SERVER_OTHER) slot->type = type_from_call(slot->call);
        qsort(s_list, s_count, sizeof(*s_list), entry_cmp);
        err = save_file(SERVER_LIST_PATH);
    }
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) notify_changed();
    return err;
}

esp_err_t server_list_set_favorite(const char *call, bool favorite)
{
    ESP_RETURN_ON_FALSE(s_mounted, ESP_ERR_INVALID_STATE, TAG, "storage not mounted");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    server_entry_t *e = find_locked(call);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (e) {
        e->favorite = favorite;
        err = save_file(SERVER_LIST_PATH);
    }
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) notify_changed();
    return err;
}

esp_err_t server_list_delete(const char *call)
{
    ESP_RETURN_ON_FALSE(s_mounted, ESP_ERR_INVALID_STATE, TAG, "storage not mounted");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    server_entry_t *e = find_locked(call);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (e) {
        memmove(e, e + 1, (s_list + s_count - (e + 1)) * sizeof(*e));
        s_count--;
        err = save_file(SERVER_LIST_PATH);
    }
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) notify_changed();
    return err;
}

esp_err_t server_list_import_buffer(const char *xml, size_t *count)
{
    ESP_RETURN_ON_FALSE(s_mounted, ESP_ERR_INVALID_STATE, TAG, "storage not mounted");
    vec_t vec = {0};
    esp_err_t err = parse_xml(xml, &vec);
    // A complete backup may intentionally contain an empty server list. Old
    // server-list XML still needs at least one entry to avoid accidental wipes.
    if (err == ESP_OK && !vec.n && !strstr(xml, "<echolink-backup")) err = ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) {
        free(vec.v);
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    free(s_list);
    s_list = vec.v;
    s_count = vec.n;
    s_cap = vec.cap;
    err = save_file(SERVER_LIST_PATH);
    xSemaphoreGive(s_lock);
    if (count) *count = vec.n;
    ESP_LOGI(TAG, "imported %u entries", (unsigned)vec.n);
    notify_changed();
    return err;
}

esp_err_t server_list_import_file(const char *path, size_t *count)
{
    char *buf;
    size_t len;
    esp_err_t err = read_file(path, &buf, &len);
    if (err != ESP_OK) return err;
    err = server_list_import_buffer(buf, count);
    free(buf);
    return err;
}

esp_err_t server_list_export_file(const char *path)
{
    ESP_RETURN_ON_FALSE(s_mounted, ESP_ERR_INVALID_STATE, TAG, "storage not mounted");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = save_file(path);
    xSemaphoreGive(s_lock);
    return err;
}

void server_list_set_changed_cb(void (*cb)(void))
{
    s_changed_cb = cb;
}
