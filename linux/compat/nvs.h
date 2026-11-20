#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// NVS as one key=value file per namespace in ~/.config/scheff-echolink
// (mode 0600: it holds the EchoLink and Wi-Fi passwords).

typedef uintptr_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

#ifdef __cplusplus
extern "C" {
#endif
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out);
void nvs_close(nvs_handle_t h);
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len);
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out);
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value);
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t value);
esp_err_t nvs_commit(nvs_handle_t h);
#ifdef __cplusplus
}
#endif
