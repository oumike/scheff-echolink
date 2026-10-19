#include "ptt.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "board.h"
#include "el_qso.h"

static const char *TAG = "ptt";

#define KEYBOARD_ADDR            0x55
#define KEYBOARD_BRIGHTNESS_CMD  0x01
#define KEYBOARD_RAW_MODE_CMD    0x03
#define KEYBOARD_COLUMNS         5
#define SPACE_COLUMN             0
#define SPACE_ROW                5
#define READ_PERIOD_MS           10
#define RELEASE_AFTER_MISSES     30

static void ptt_task(void *arg)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = KEYBOARD_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t keyboard;
    if (i2c_master_bus_add_device(board_main_i2c_bus(), &cfg, &keyboard) != ESP_OK) {
        ESP_LOGE(TAG, "could not add T-Deck keyboard");
        vTaskDelete(NULL);
    }

    bool raw_mode = false;
    bool pressed = false;
    int stable = 0;
    int missed = 0;
    for (;;) {
        if (!raw_mode) {
            const uint8_t brightness[] = {KEYBOARD_BRIGHTNESS_CMD, 127};
            i2c_master_transmit(keyboard, brightness, sizeof(brightness), 100);
            const uint8_t command = KEYBOARD_RAW_MODE_CMD;
            raw_mode = i2c_master_transmit(keyboard, &command, 1, 100) == ESP_OK;
            if (!raw_mode) {
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
            ESP_LOGI(TAG, "Space bar hold-to-talk ready");
        }

        uint8_t matrix[KEYBOARD_COLUMNS];
        if (i2c_master_receive(keyboard, matrix, sizeof(matrix), 100) != ESP_OK) {
            if (++missed >= RELEASE_AFTER_MISSES && pressed) {
                pressed = false;
                stable = 0;
                el_qso_set_ptt(false);
            }
            vTaskDelay(pdMS_TO_TICKS(READ_PERIOD_MS));
            continue;
        }
        missed = 0;

        bool now = (matrix[SPACE_COLUMN] & (1U << SPACE_ROW)) != 0;
        if (now == pressed) {
            stable = 0;
        } else if (++stable >= 3) {
            stable = 0;
            pressed = now;
            el_qso_set_ptt(pressed);
        }
        vTaskDelay(pdMS_TO_TICKS(READ_PERIOD_MS));
    }
}

void ptt_button_start(void)
{
    xTaskCreate(ptt_task, "ptt_keyboard", 3072, NULL, 10, NULL);
}