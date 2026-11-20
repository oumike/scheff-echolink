#pragma once

#include "FreeRTOS.h"

typedef struct el_sem *SemaphoreHandle_t;

#ifdef __cplusplus
extern "C" {
#endif
// A binary semaphore that starts given, as FreeRTOS's mutex does (not
// recursive).
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
void vSemaphoreDelete(SemaphoreHandle_t s);
#ifdef __cplusplus
}
#endif
