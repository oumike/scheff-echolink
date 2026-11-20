// The FreeRTOS task, notification and semaphore calls main/ uses, over
// pthreads.

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"

struct el_task {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint32_t notify;
    TaskFunction_t fn;
    void *arg;
    char name[16];
};

struct el_sem {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    unsigned count;
};

static __thread struct el_task *t_self;

static struct el_task *task_new(void)
{
    struct el_task *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    pthread_mutex_init(&t->mu, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&t->cv, &ca);
    pthread_condattr_destroy(&ca);
    return t;
}

// Absolute CLOCK_MONOTONIC deadline `ticks` ms from now.
static struct timespec deadline(TickType_t ticks)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += ticks / 1000;
    ts.tv_nsec += (long)(ticks % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    return ts;
}

static void *trampoline(void *p)
{
    struct el_task *t = p;
    t_self = t;
    pthread_setname_np(pthread_self(), t->name);
    t->fn(t->arg);
    return NULL;
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                       TaskHandle_t *out)
{
    (void)prio;
    struct el_task *t = task_new();
    if (!t) return pdFAIL;
    t->fn = fn;
    t->arg = arg;
    strncpy(t->name, name ? name : "task", sizeof(t->name) - 1);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    // FreeRTOS sizes are bytes on ESP-IDF; leave generous headroom.
    size_t size = stack < 65536 ? 262144 : (size_t)stack * 4;
    pthread_attr_setstacksize(&attr, size);
    if (out) *out = t;
    pthread_t th;
    int rc = pthread_create(&th, &attr, trampoline, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        if (out) *out = NULL;
        free(t);
        return pdFAIL;
    }
    return pdPASS;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                   UBaseType_t prio, TaskHandle_t *out, BaseType_t core)
{
    (void)core;
    return xTaskCreate(fn, name, stack, arg, prio, out);
}

void vTaskDelete(TaskHandle_t task)
{
    // A task's struct stays allocated: a handle may still be notified.
    if (task == NULL || task == t_self) pthread_exit(NULL);
}

void vTaskDelay(TickType_t ticks)
{
    struct timespec ts = {.tv_sec = ticks / 1000, .tv_nsec = (long)(ticks % 1000) * 1000000};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

TickType_t xTaskGetTickCount(void)
{
    return (TickType_t)(esp_timer_get_time() / 1000);
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    if (!t_self) t_self = task_new();
    return t_self;
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    struct el_task *t = xTaskGetCurrentTaskHandle();
    struct timespec until = deadline(wait);
    pthread_mutex_lock(&t->mu);
    while (!t->notify && wait) {
        int rc = wait == portMAX_DELAY ? pthread_cond_wait(&t->cv, &t->mu)
                                       : pthread_cond_timedwait(&t->cv, &t->mu, &until);
        if (rc == ETIMEDOUT) break;
    }
    uint32_t v = t->notify;
    if (v) t->notify = clear ? 0 : v - 1;
    pthread_mutex_unlock(&t->mu);
    return v;
}

BaseType_t xTaskNotifyGive(TaskHandle_t t)
{
    if (!t) return pdFAIL;
    pthread_mutex_lock(&t->mu);
    t->notify++;
    pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&t->mu);
    return pdPASS;
}

static SemaphoreHandle_t sem_new(unsigned count)
{
    struct el_sem *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    pthread_mutex_init(&s->mu, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&s->cv, &ca);
    pthread_condattr_destroy(&ca);
    s->count = count;
    return s;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return sem_new(1);
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return sem_new(0);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait)
{
    if (!s) return pdFALSE;
    struct timespec until = deadline(wait);
    pthread_mutex_lock(&s->mu);
    while (!s->count && wait) {
        int rc = wait == portMAX_DELAY ? pthread_cond_wait(&s->cv, &s->mu)
                                       : pthread_cond_timedwait(&s->cv, &s->mu, &until);
        if (rc == ETIMEDOUT) break;
    }
    BaseType_t ok = s->count ? pdTRUE : pdFALSE;
    if (ok) s->count--;
    pthread_mutex_unlock(&s->mu);
    return ok;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    if (!s) return pdFALSE;
    pthread_mutex_lock(&s->mu);
    BaseType_t ok = s->count ? pdFALSE : pdTRUE;
    s->count = 1;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mu);
    return ok;
}

void vSemaphoreDelete(SemaphoreHandle_t s)
{
    if (!s) return;
    pthread_cond_destroy(&s->cv);
    pthread_mutex_destroy(&s->mu);
    free(s);
}
