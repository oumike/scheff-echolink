#pragma once

#include <stdio.h>
#include "esp_err.h"

typedef esp_err_t (*file_store_writer_t)(FILE *file, void *ctx);

// Callers serialize access to each path. Recovery discards uncommitted .tmp
// files and restores .bak if replacement was interrupted.
esp_err_t file_store_recover(const char *path);
esp_err_t file_store_write(const char *path, file_store_writer_t write, void *ctx);
esp_err_t file_store_read_text(const char *path, size_t max, char **out, size_t *len);
