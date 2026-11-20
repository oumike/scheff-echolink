#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
// Starts the app again in place (execv of /proc/self/exe): same PID, so the
// launcher keeps waiting on it.
void esp_restart(void) __attribute__((noreturn));
#ifdef __cplusplus
}
#endif
