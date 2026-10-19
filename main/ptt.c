#include "ptt.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "board.h"
#include "el_qso.h"

// Hold-to-talk on the BOOT/user button, debounced by requiring a stable level for
// three consecutive 10 ms samples.
static void ptt_task(void *arg)
{
    bool pressed = false;
    int stable = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));
        bool now = gpio_get_level(BOARD_BOOT_BUTTON) == 0;
        if (now == pressed) {
            stable = 0;
            continue;
        }
        if (++stable < 3) continue;
        stable = 0;
        pressed = now;
        el_qso_set_ptt(pressed);
    }
}

void ptt_button_start(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_BOOT_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    xTaskCreate(ptt_task, "ptt", 2048, NULL, 10, NULL);
}
