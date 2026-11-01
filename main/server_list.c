#include "server_list.h"

#include <ctype.h>
#include <errno.h>
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
#include "lwip/sockets.h"
#include "wear_levelling.h"
#include "file_store.h"
#include "xml.h"

static const char *TAG = "servers";

#define MOUNT_POINT    "/data"
#define PARTITION      "storage"
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

static char *xml_escape(char *out, const char *s)
{
    for (; *s; s++) {
        const char *entity;
        switch (*s) {
        case '&': entity = "&amp;"; break;
        case '<': entity = "&lt;"; break;
        case '>': entity = "&gt;"; break;
        case '"': entity = "&quot;"; break;
        case '\'': entity = "&apos;"; break;
        default:
            // Directory text is Latin-1-ish; keep the file valid UTF-8 by
            // dropping anything outside printable ASCII.
            if ((unsigned char)*s >= 0x20 && (unsigned char)*s < 0x7f) *out++ = *s;
            continue;
        }
        size_t len = strlen(entity);
        memcpy(out, entity, len);
        out += len;
    }
    *out = 0;
    return out;
}

static esp_err_t write_server_entry(server_list_write_cb_t write, void *ctx, const server_entry_t *entry)
{
    ESP_RETURN_ON_FALSE(entry->type >= SERVER_CONFERENCE && entry->type <= SERVER_OTHER &&
                        strnlen(entry->call, sizeof(entry->call)) < sizeof(entry->call) &&
                        strnlen(entry->desc, sizeof(entry->desc)) < sizeof(entry->desc) &&
                        strnlen(entry->host, sizeof(entry->host)) < sizeof(entry->host),
                        ESP_ERR_INVALID_ARG, TAG, "invalid server entry");
    // Every string byte expands to at most six bytes; fixed fields fit in 192.
    char line[6 * (sizeof(entry->call) + sizeof(entry->desc) + sizeof(entry->host)) + 192];
    char *out = line;
    out += snprintf(out, sizeof(line), "  <server call=\"");
    out = xml_escape(out, entry->call);
    out += snprintf(out, sizeof(line) - (out - line), "\" node=\"%lu\" type=\"%s\" desc=\"",
                    (unsigned long)entry->node, TYPE_NAMES[entry->type]);
    out = xml_escape(out, entry->desc);
    char ip[16] = "";
    if (entry->ip) inet_ntop(AF_INET, &entry->ip, ip, sizeof(ip));
    out += snprintf(out, sizeof(line) - (out - line), "\" ip=\"%s\" host=\"", ip);
    out = xml_escape(out, entry->host);
    out += snprintf(out, sizeof(line) - (out - line), "\"%s%s/>\n",
                    entry->manual ? " manual=\"true\"" : "", entry->favorite ? " favorite=\"true\"" : "");
    return write(ctx, line, out - line);
}

static esp_err_t parse_server(void *ctx, const char **attributes)
{
    vec_t *out = ctx;
    server_entry_t entry = {0};
    if (xml_copy_attribute(attributes, "call", entry.call, sizeof(entry.call)) != 1 || !entry.call[0] ||
        xml_copy_attribute(attributes, "desc", entry.desc, sizeof(entry.desc)) < 0 ||
        xml_copy_attribute(attributes, "host", entry.host, sizeof(entry.host)) < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *value = xml_attribute(attributes, "node");
    if (value) {
        char *end;
        errno = 0;
        unsigned long node = strtoul(value, &end, 10);
        if (!isdigit((unsigned char)*value) || *end || errno || node > UINT32_MAX) return ESP_ERR_INVALID_ARG;
        entry.node = node;
    }
    value = xml_attribute(attributes, "ip");
    if (value && *value) {
        struct in_addr address;
        if (!inet_aton(value, &address)) return ESP_ERR_INVALID_ARG;
        entry.ip = address.s_addr;
    }
    value = xml_attribute(attributes, "type");
    entry.type = value ? type_from_name(value) : type_from_call(entry.call);
    value = xml_attribute(attributes, "manual");
    if (value && strcmp(value, "true") && strcmp(value, "false")) return ESP_ERR_INVALID_ARG;
    entry.manual = value && !strcmp(value, "true");
    value = xml_attribute(attributes, "favorite");
    if (value && strcmp(value, "true") && strcmp(value, "false")) return ESP_ERR_INVALID_ARG;
    entry.favorite = value && !strcmp(value, "true");
    if (!vec_reserve(out, out->n + 1)) return ESP_ERR_NO_MEM;
    out->v[out->n++] = entry;
    return ESP_OK;
}

static esp_err_t parse_xml(const char *buf, vec_t *out, bool *backup)
{
    out->n = 0;
    xml_document_t document;
    esp_err_t err = xml_read(buf, parse_server, out, &document);
    if (err != ESP_OK) return err;
    if (backup) *backup = document.backup;
    if (out->n > 1) qsort(out->v, out->n, sizeof(*out->v), entry_cmp);
    return ESP_OK;
}

static esp_err_t read_file(const char *path, char **out, size_t *len)
{
    return file_store_read_text(path, FILE_MAX_BYTES, out, len);
}

// Replaces the in-memory list with the contents of `path`. Call with s_lock.
static esp_err_t load_file(const char *path)
{
    char *buf;
    size_t len;
    esp_err_t err = read_file(path, &buf, &len);
    if (err != ESP_OK) return err;
    vec_t vec = {0};
    err = parse_xml(buf, &vec, NULL);
    free(buf);
    if (err != ESP_OK) {
        free(vec.v);
        return err;
    }
    free(s_list);
    s_list = vec.v;
    s_cap = vec.cap;
    s_count = vec.n;
    ESP_LOGI(TAG, "loaded %u entries from %s", (unsigned)s_count, path);
    return err;
}

typedef struct {
    const server_entry_t *entries;
    size_t count;
} server_xml_t;

static esp_err_t server_file_write(void *ctx, const char *data, size_t len)
{
    return fwrite(data, 1, len, ctx) == len ? ESP_OK : ESP_FAIL;
}

static esp_err_t write_server_file(FILE *f, void *ctx)
{
    const server_xml_t *data = ctx;
    char when[32] = "";
    time_t now = time(NULL);
    if (now > 1700000000) {
        struct tm tm;
        strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", gmtime_r(&now, &tm));
    }
    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f, "<echolink-servers version=\"1\" generated=\"%s\">\n", when);
    for (size_t i = 0; i < data->count; i++) {
        esp_err_t err = write_server_entry(server_file_write, f, &data->entries[i]);
        if (err != ESP_OK) return err;
    }
    fprintf(f, "</echolink-servers>\n");
    return ferror(f) ? ESP_FAIL : ESP_OK;
}

static esp_err_t save_list(const char *path, const server_entry_t *entries, size_t count)
{
    server_xml_t data = {.entries = entries, .count = count};
    esp_err_t err = file_store_write(path, write_server_file, &data);
    if (err == ESP_OK) ESP_LOGI(TAG, "saved %u entries to %s", (unsigned)count, path);
    return err;
}

static esp_err_t save_file(const char *path)
{
    return save_list(path, s_list, s_count);
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

    esp_err_t default_recovery = file_store_recover(SERVER_LIST_DEFAULT_PATH);
    esp_err_t working_recovery = file_store_recover(SERVER_LIST_PATH);
    if (default_recovery != ESP_OK || working_recovery != ESP_OK) {
        ESP_LOGE(TAG, "server list recovery failed");
    }

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
    bool backup = false;
    esp_err_t err = parse_xml(xml, &vec, &backup);
    // A complete backup may intentionally contain an empty server list. Old
    // server-list XML still needs at least one entry to avoid accidental wipes.
    if (err == ESP_OK && !vec.n && !backup) err = ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) {
        free(vec.v);
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    err = save_list(SERVER_LIST_PATH, vec.v, vec.n);
    if (err == ESP_OK) {
        free(s_list);
        s_list = vec.v;
        s_count = vec.n;
        s_cap = vec.cap;
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        free(vec.v);
        return err;
    }
    if (count) *count = vec.n;
    ESP_LOGI(TAG, "imported %u entries", (unsigned)vec.n);
    notify_changed();
    return err;
}

esp_err_t server_list_import_file(const char *path, size_t *count)
{
    ESP_RETURN_ON_FALSE(s_mounted, ESP_ERR_INVALID_STATE, TAG, "storage not mounted");
    char *buf;
    size_t len;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = read_file(path, &buf, &len);
    xSemaphoreGive(s_lock);
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

esp_err_t server_list_export_entries(server_list_write_cb_t write, void *ctx)
{
    ESP_RETURN_ON_FALSE(s_mounted, ESP_ERR_INVALID_STATE, TAG, "storage not mounted");
    ESP_RETURN_ON_FALSE(write, ESP_ERR_INVALID_ARG, TAG, "missing backup writer");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t count = s_count;
    server_entry_t *snapshot = count ? heap_caps_malloc(count * sizeof(*snapshot), MALLOC_CAP_SPIRAM) : NULL;
    if (count && snapshot) memcpy(snapshot, s_list, count * sizeof(*snapshot));
    xSemaphoreGive(s_lock);
    ESP_RETURN_ON_FALSE(!count || snapshot, ESP_ERR_NO_MEM, TAG, "allocate backup snapshot");
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < count && err == ESP_OK; i++) err = write_server_entry(write, ctx, &snapshot[i]);
    free(snapshot);
    return err;
}

void server_list_set_changed_cb(void (*cb)(void))
{
    s_changed_cb = cb;
}
