#include "file_store.h"

#include <stdbool.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "file_store";

#define STORE_PATH_MAX 128

static esp_err_t store_paths(const char *path, char *tmp, char *backup)
{
    ESP_RETURN_ON_FALSE(path && *path, ESP_ERR_INVALID_ARG, TAG, "missing path");
    ESP_RETURN_ON_FALSE(strlen(path) + 5 <= STORE_PATH_MAX, ESP_ERR_INVALID_SIZE, TAG, "path too long");
    snprintf(tmp, STORE_PATH_MAX, "%s.tmp", path);
    snprintf(backup, STORE_PATH_MAX, "%s.bak", path);
    return ESP_OK;
}

static esp_err_t store_exists(const char *path, bool *exists)
{
    struct stat st;
    *exists = stat(path, &st) == 0;
    ESP_RETURN_ON_FALSE(*exists || errno == ENOENT, ESP_FAIL, TAG, "stat %s: %d", path, errno);
    return ESP_OK;
}

static esp_err_t store_remove(const char *path)
{
    ESP_RETURN_ON_FALSE(remove(path) == 0 || errno == ENOENT, ESP_FAIL, TAG,
                        "remove %s: %d", path, errno);
    return ESP_OK;
}

esp_err_t file_store_recover(const char *path)
{
    char tmp[STORE_PATH_MAX], backup[STORE_PATH_MAX];
    ESP_RETURN_ON_ERROR(store_paths(path, tmp, backup), TAG, "recovery paths");
    bool current, previous;
    ESP_RETURN_ON_ERROR(store_exists(path, &current), TAG, "current file");
    ESP_RETURN_ON_ERROR(store_exists(backup, &previous), TAG, "rollback file");
    if (previous && !current) {
        ESP_LOGW(TAG, "restoring %s", path);
        ESP_RETURN_ON_FALSE(rename(backup, path) == 0, ESP_FAIL, TAG, "restore %s: %d", path, errno);
    } else if (previous) {
        ESP_RETURN_ON_ERROR(store_remove(backup), TAG, "remove old rollback file");
    }
    bool pending;
    ESP_RETURN_ON_ERROR(store_exists(tmp, &pending), TAG, "temporary file");
    return pending ? store_remove(tmp) : ESP_OK;
}

esp_err_t file_store_write(const char *path, file_store_writer_t write, void *ctx)
{
    ESP_RETURN_ON_FALSE(write, ESP_ERR_INVALID_ARG, TAG, "missing writer");
    char tmp[STORE_PATH_MAX], backup[STORE_PATH_MAX];
    ESP_RETURN_ON_ERROR(store_paths(path, tmp, backup), TAG, "write paths");
    ESP_RETURN_ON_ERROR(file_store_recover(path), TAG, "recover %s", path);
    FILE *file = fopen(tmp, "w");
    ESP_RETURN_ON_FALSE(file, ESP_FAIL, TAG, "open %s: %d", tmp, errno);
    esp_err_t err = write(file, ctx);
    if (ferror(file)) err = ESP_FAIL;
    if (fclose(file) != 0) err = ESP_FAIL;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write %s: %s", tmp, esp_err_to_name(err));
        store_remove(tmp);
        return err;
    }
    bool previous;
    err = store_exists(path, &previous);
    if (err == ESP_OK && previous && rename(path, backup) != 0) {
        ESP_LOGE(TAG, "retain %s: %d", path, errno);
        err = ESP_FAIL;
    }
    if (err != ESP_OK) {
        store_remove(tmp);
        return err;
    }
    if (rename(tmp, path) != 0) {
        ESP_LOGE(TAG, "replace %s: %d", path, errno);
        if (previous && rename(backup, path) != 0) ESP_LOGE(TAG, "rollback %s: %d", path, errno);
        store_remove(tmp);
        return ESP_FAIL;
    }
    // The new file is committed even if cleanup must wait until the next access.
    if (previous) store_remove(backup);
    return ESP_OK;
}

esp_err_t file_store_read_text(const char *path, size_t max, char **out, size_t *len)
{
    FILE *file = fopen(path, "r");
    if (!file) {
        if (errno == ENOENT) return ESP_ERR_NOT_FOUND;
        ESP_LOGE(TAG, "open %s: %d", path, errno);
        return ESP_FAIL;
    }
    esp_err_t err = ESP_FAIL;
    char *data = NULL;
    long size = 0;
    if (fseek(file, 0, SEEK_END) != 0) goto done;
    size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) goto done;
    if (!size || (size_t)size > max) {
        err = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    data = heap_caps_malloc((size_t)size + 1, MALLOC_CAP_SPIRAM);
    if (!data) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    if (fread(data, 1, size, file) != (size_t)size || ferror(file)) goto done;
    err = memchr(data, 0, size) ? ESP_ERR_INVALID_ARG : ESP_OK;
    data[size] = 0;
done:
    if (fclose(file) != 0) err = ESP_FAIL;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read %s: %s", path, esp_err_to_name(err));
        free(data);
        return err;
    }
    *out = data;
    if (len) *len = size;
    return ESP_OK;
}
