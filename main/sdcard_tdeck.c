#include "sdcard.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "board.h"

static const char *TAG = "sdcard";

static sdmmc_card_t *s_card;
static SemaphoreHandle_t s_lock;

bool sdcard_mounted(void)
{
    return s_card != NULL;
}

esp_err_t sdcard_mount(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_card) {
        if (sdmmc_get_status(s_card) == ESP_OK) {
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "card gone, remounting");
        esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT, s_card);
        s_card = NULL;
    }

    esp_err_t err = board_sd_power(true);
    if (err == ESP_OK) {
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.slot = BOARD_SPI_HOST;
        sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot.host_id = BOARD_SPI_HOST;
        slot.gpio_cs = BOARD_SDCARD_CS;
        esp_vfs_fat_sdmmc_mount_config_t mount = {
            .format_if_mount_failed = false,
            .max_files = 4,
            .allocation_unit_size = 16 * 1024,
        };
        err = esp_vfs_fat_sdspi_mount(SDCARD_MOUNT, &host, &slot, &mount, &s_card);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "mounted %s, %llu MB", s_card->cid.name,
                 (unsigned long long)s_card->csd.capacity * s_card->csd.sector_size / (1024 * 1024));
    } else {
        ESP_LOGW(TAG, "no card: %s", esp_err_to_name(err));
        s_card = NULL;
    }
    xSemaphoreGive(s_lock);
    return err;
}