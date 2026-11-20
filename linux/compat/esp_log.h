#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
void esp_log_write_linux(char level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
#ifdef __cplusplus
}
#endif

#define ESP_LOGE(tag, fmt, ...) esp_log_write_linux('E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) esp_log_write_linux('W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) esp_log_write_linux('I', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) do { if (0) esp_log_write_linux('D', tag, fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGV(tag, fmt, ...) do { if (0) esp_log_write_linux('V', tag, fmt, ##__VA_ARGS__); } while (0)
