#include "sdcard.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/sdmmc_host.h"
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
        // Still there? A pulled card fails this status query.
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
        vTaskDelay(pdMS_TO_TICKS(20));  // let the switched card supply settle
        // The slot is wired 1-bit (CLK, CMD, D0), routed through the GPIO matrix.
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.width = 1;
        slot.clk = BOARD_SD_CLK;
        slot.cmd = BOARD_SD_CMD;
        slot.d0 = BOARD_SD_D0;
        slot.d1 = slot.d2 = slot.d3 = GPIO_NUM_NC;
        slot.d4 = slot.d5 = slot.d6 = slot.d7 = GPIO_NUM_NC;
        slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
        esp_vfs_fat_sdmmc_mount_config_t mount = {
            .format_if_mount_failed = false,
            .max_files = 4,
            .allocation_unit_size = 16 * 1024,
        };
        err = esp_vfs_fat_sdmmc_mount(SDCARD_MOUNT, &host, &slot, &mount, &s_card);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "mounted %s, %llu MB", s_card->cid.name,
                 (unsigned long long)s_card->csd.capacity * s_card->csd.sector_size / (1024 * 1024));
    } else {
        ESP_LOGW(TAG, "no card: %s", esp_err_to_name(err));
        s_card = NULL;
        board_sd_power(false);
    }
    xSemaphoreGive(s_lock);
    return err;
}
