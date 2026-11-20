#pragma once

#include "FreeRTOS.h"

typedef struct el_task *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

#ifdef __cplusplus
extern "C" {
#endif
// Stack size and priority are ignored; tasks are detached threads.
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                       TaskHandle_t *out);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                   UBaseType_t prio, TaskHandle_t *out, BaseType_t core);
// Only vTaskDelete(NULL), from the task itself.
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
#ifdef __cplusplus
}
#endif
