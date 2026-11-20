#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
// Microseconds since start (CLOCK_MONOTONIC).
int64_t esp_timer_get_time(void);
#ifdef __cplusplus
}
#endif
