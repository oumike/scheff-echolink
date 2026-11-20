// "SD card" storage on the CardputerZero is the app's data directory, which
// main() has made the working directory (SDCARD_MOUNT is ".").

#include "sdcard.h"

esp_err_t sdcard_mount(void)
{
    return ESP_OK;
}

bool sdcard_mounted(void)
{
    return true;
}
