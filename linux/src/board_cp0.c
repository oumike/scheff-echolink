// The CardputerZero's board layer: Linux owns the hardware, so only the
// battery gauge (the BQ27220's sysfs node) has anything to do.

#include <stdio.h>
#include "board.h"

#define BATTERY_CAPACITY "/sys/class/power_supply/bq27220-0/capacity"

esp_err_t board_init(void)
{
    return ESP_OK;
}

esp_err_t board_audio_power(bool on)
{
    return ESP_OK;
}

esp_err_t board_c6_enable(bool on)
{
    return ESP_OK;
}

esp_err_t board_sd_power(bool on)
{
    return ESP_OK;
}

esp_err_t board_usb_host_power(bool on)
{
    return ESP_OK;
}

esp_err_t board_screen_reset_release(bool released)
{
    return ESP_OK;
}

esp_err_t board_touch_reset_release(bool released)
{
    return ESP_OK;
}

esp_err_t board_battery_percent(uint8_t *percent)
{
    FILE *f = fopen(BATTERY_CAPACITY, "r");
    if (!f) return ESP_ERR_NOT_FOUND;
    int v = -1;
    int n = fscanf(f, "%d", &v);
    fclose(f);
    if (n != 1 || v < 0) return ESP_FAIL;
    *percent = v > 100 ? 100 : (uint8_t)v;
    return ESP_OK;
}
