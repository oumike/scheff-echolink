#pragma once

#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_err.h"

#if CONFIG_EL_LINUX
// The app's data directory (~/.local/share/scheff-echolink), which main()
// makes the working directory.
#define SDCARD_MOUNT "."
#else
#define SDCARD_MOUNT "/sdcard"
#endif

// Mounts the board's microSD card at /sdcard if it isn't already. Safe to call
// repeatedly: a card inserted after boot is picked up on the next call.
esp_err_t sdcard_mount(void);
bool sdcard_mounted(void);
