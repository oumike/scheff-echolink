// The CardputerZero's 320x170 LCD through the Linux framebuffer, and the
// thread that runs LVGL. Keys come from keys_cp0.c.

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "cp0.h"
#include "display.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "display";

static pthread_mutex_t s_lock;
static lv_display_t *s_disp;

uint32_t cp0_millis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

bool display_lock(void)
{
    pthread_mutex_lock(&s_lock);
    return true;
}

void display_unlock(void)
{
    pthread_mutex_unlock(&s_lock);
}

// The launcher owns the backlight (its brightness keys and settings).
void display_set_brightness(uint8_t level)
{
}

static void *lvgl_thread(void *arg)
{
    for (;;) {
        display_lock();
        uint32_t wait = lv_timer_handler();
        display_unlock();
        if (wait > 10) wait = 10;
        if (wait < 1) wait = 1;
        usleep(wait * 1000);
    }
    return NULL;
}

// The panel's node: $LV_LINUX_FBDEV_DEVICE, else the device's /dev/fb_lcd
// link, else /dev/fb0.
static const char *fb_path(void)
{
    const char *env = getenv("LV_LINUX_FBDEV_DEVICE");
    if (env && *env) return env;
    if (access("/dev/fb_lcd", R_OK | W_OK) == 0) return "/dev/fb_lcd";
    return "/dev/fb0";
}

esp_err_t display_init(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_lock, &a);
    pthread_mutexattr_destroy(&a);

    lv_init();
    lv_tick_set_cb(cp0_millis);

    const char *path = fb_path();
    if (access(path, R_OK | W_OK) != 0) {
        ESP_LOGE(TAG, "%s not writable (is the user in the video group?)", path);
        return ESP_ERR_NOT_FOUND;
    }
    s_disp = lv_linux_fbdev_create();
    if (!s_disp) return ESP_FAIL;
    lv_linux_fbdev_set_file(s_disp, path);
    if (lv_display_get_horizontal_resolution(s_disp) <= 0) {
        ESP_LOGE(TAG, "%s: no usable framebuffer", path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "%s: %dx%d", path, (int)lv_display_get_horizontal_resolution(s_disp),
             (int)lv_display_get_vertical_resolution(s_disp));

    cp0_keys_start(s_disp);

    pthread_t t;
    if (pthread_create(&t, NULL, lvgl_thread, NULL) != 0) return ESP_FAIL;
    pthread_detach(t);
    return ESP_OK;
}
