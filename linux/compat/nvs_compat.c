// NVS over files: one "key=value" line per key in
// $XDG_CONFIG_HOME/scheff-echolink/<namespace>.nvs (default ~/.config),
// written to a temporary file and renamed, mode 0600. Values are escaped
// (\\, \n) so any string survives.

#include <errno.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "nvs.h"
#include "nvs_flash.h"

#define MAX_KEYS 64

typedef struct {
    char key[16];
    char *value;
} entry_t;

typedef struct {
    char path[512];
    bool writable;
    entry_t e[MAX_KEYS];
    int n;
} handle_t;

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static char s_dir[400];

static int mkdirs(const char *path, mode_t mode)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    return mkdir(tmp, mode) != 0 && errno != EEXIST ? -1 : 0;
}

esp_err_t nvs_flash_init(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) {
        snprintf(s_dir, sizeof(s_dir), "%s/scheff-echolink", xdg);
    } else if (home && *home) {
        snprintf(s_dir, sizeof(s_dir), "%s/.config/scheff-echolink", home);
    } else {
        snprintf(s_dir, sizeof(s_dir), "/tmp/scheff-echolink-config");
    }
    return mkdirs(s_dir, 0700) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t nvs_flash_erase(void)
{
    return ESP_OK;
}

static char *unescape(const char *s)
{
    char *out = malloc(strlen(s) + 1), *o = out;
    if (!out) return NULL;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            *o++ = *s == 'n' ? '\n' : *s;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
    return out;
}

static void put(handle_t *h, const char *key, char *value)
{
    for (int i = 0; i < h->n; i++) {
        if (!strcmp(h->e[i].key, key)) {
            free(h->e[i].value);
            h->e[i].value = value;
            return;
        }
    }
    if (h->n >= MAX_KEYS) {
        free(value);
        return;
    }
    snprintf(h->e[h->n].key, sizeof(h->e[h->n].key), "%s", key);
    h->e[h->n++].value = value;
}

static const char *get(handle_t *h, const char *key)
{
    for (int i = 0; i < h->n; i++) {
        if (!strcmp(h->e[i].key, key)) return h->e[i].value;
    }
    return NULL;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    if (!s_dir[0] && nvs_flash_init() != ESP_OK) return ESP_FAIL;
    handle_t *h = calloc(1, sizeof(*h));
    if (!h) return ESP_ERR_NO_MEM;
    snprintf(h->path, sizeof(h->path), "%s/%s.nvs", s_dir, ns);
    h->writable = mode == NVS_READWRITE;

    pthread_mutex_lock(&s_mu);
    FILE *f = fopen(h->path, "r");
    pthread_mutex_unlock(&s_mu);
    if (!f) {
        if (!h->writable) {
            free(h);
            return ESP_ERR_NVS_NOT_FOUND;
        }
    } else {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\n")] = 0;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            char *v = unescape(eq + 1);
            if (v) put(h, line, v);
        }
        fclose(f);
    }
    *out = (nvs_handle_t)h;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    handle_t *h = (handle_t *)handle;
    if (!h) return;
    for (int i = 0; i < h->n; i++) {
        if (h->e[i].value) {
            memset(h->e[i].value, 0, strlen(h->e[i].value));
            free(h->e[i].value);
        }
    }
    free(h);
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *len)
{
    const char *v = get((handle_t *)handle, key);
    if (!v) return ESP_ERR_NVS_NOT_FOUND;
    size_t need = strlen(v) + 1;
    if (!out) {
        *len = need;
        return ESP_OK;
    }
    if (*len < need) return ESP_ERR_INVALID_SIZE;
    memcpy(out, v, need);
    *len = need;
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out)
{
    const char *v = get((handle_t *)handle, key);
    if (!v) return ESP_ERR_NVS_NOT_FOUND;
    *out = (uint8_t)strtoul(v, NULL, 10);
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    handle_t *h = (handle_t *)handle;
    if (!h->writable) return ESP_ERR_INVALID_STATE;
    char *v = strdup(value);
    if (!v) return ESP_ERR_NO_MEM;
    put(h, key, v);
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    char buf[4];
    snprintf(buf, sizeof(buf), "%u", value);
    return nvs_set_str(handle, key, buf);
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    handle_t *h = (handle_t *)handle;
    if (!h->writable) return ESP_ERR_INVALID_STATE;
    char tmp[530];
    snprintf(tmp, sizeof(tmp), "%s.tmp", h->path);
    pthread_mutex_lock(&s_mu);
    mode_t old = umask(077);
    FILE *f = fopen(tmp, "w");
    umask(old);
    esp_err_t err = ESP_FAIL;
    if (f) {
        for (int i = 0; i < h->n; i++) {
            fprintf(f, "%s=", h->e[i].key);
            for (const char *c = h->e[i].value; *c; c++) {
                if (*c == '\\') fputs("\\\\", f);
                else if (*c == '\n') fputs("\\n", f);
                else fputc(*c, f);
            }
            fputc('\n', f);
        }
        bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
        ok = fclose(f) == 0 && ok;
        if (ok && rename(tmp, h->path) == 0) err = ESP_OK;
        else unlink(tmp);
    }
    pthread_mutex_unlock(&s_mu);
    return err;
}
