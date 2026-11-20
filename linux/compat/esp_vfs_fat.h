#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "wear_levelling.h"

// No flash partition on Linux: the mount always fails, and server_list.c
// falls back to the "SD card" (the data directory, see sdcard.h).
typedef struct {
    bool format_if_mount_failed;
    int max_files;
    size_t allocation_unit_size;
} esp_vfs_fat_mount_config_t;

static inline esp_err_t esp_vfs_fat_spiflash_mount_rw_wl(const char *base_path, const char *partition_label,
                                                         const esp_vfs_fat_mount_config_t *cfg,
                                                         wl_handle_t *wl)
{
    (void)base_path, (void)partition_label, (void)cfg, (void)wl;
    return ESP_ERR_NOT_FOUND;
}
