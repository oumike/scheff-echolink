#pragma once

// ESP-IDF's esp_http_server, as far as web_config.c uses it, over POSIX
// sockets: one thread serves one request per connection (Connection: close).
// Adapted from monster-cyberspace's linux/compat/httpd_compat.cpp.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "esp_err.h"

#define ESP_ERR_HTTPD_BASE           0xb000
#define ESP_ERR_HTTPD_HANDLERS_FULL  (ESP_ERR_HTTPD_BASE + 1)
#define ESP_ERR_HTTPD_HANDLER_EXISTS (ESP_ERR_HTTPD_BASE + 2)
#define ESP_ERR_HTTPD_INVALID_REQ    (ESP_ERR_HTTPD_BASE + 3)
#define ESP_ERR_HTTPD_RESULT_TRUNC   (ESP_ERR_HTTPD_BASE + 4)

#define HTTPD_SOCK_ERR_FAIL    -1
#define HTTPD_SOCK_ERR_INVALID -2
#define HTTPD_SOCK_ERR_TIMEOUT -3

#define HTTPD_RESP_USE_STRLEN -1
#define HTTPD_MAX_URI_LEN     512

typedef void *httpd_handle_t;

typedef enum { HTTP_DELETE = 0, HTTP_GET = 1, HTTP_HEAD = 2, HTTP_POST = 3, HTTP_PUT = 4 } httpd_method_t;

typedef enum {
    HTTPD_500_INTERNAL_SERVER_ERROR = 0,
    HTTPD_400_BAD_REQUEST,
    HTTPD_404_NOT_FOUND,
    HTTPD_405_METHOD_NOT_ALLOWED,
    HTTPD_ERR_CODE_MAX
} httpd_err_code_t;

typedef struct {
    unsigned task_priority;
    size_t stack_size;
    int core_id;
    uint16_t server_port;
    uint16_t max_open_sockets;
    uint16_t max_uri_handlers;
    uint16_t recv_wait_timeout;  // seconds
    uint16_t send_wait_timeout;  // seconds
    bool lru_purge_enable;
} httpd_config_t;

// CONFIG_EL_WEB_PORT (8573): port 80 needs root, the app runs as the login
// user, and common ports like 8080 are often taken by other services.
#define HTTPD_DEFAULT_CONFIG()                                                                             \
    {.task_priority = 5, .stack_size = 4096, .core_id = 0, .server_port = CONFIG_EL_WEB_PORT, .max_open_sockets = 7,     \
     .max_uri_handlers = 8, .recv_wait_timeout = 5, .send_wait_timeout = 5, .lru_purge_enable = false}

typedef struct httpd_req {
    httpd_handle_t handle;
    int method;
    char uri[HTTPD_MAX_URI_LEN + 1];
    size_t content_len;
    void *aux;
    void *user_ctx;
} httpd_req_t;

typedef struct {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *r);
    void *user_ctx;
} httpd_uri_t;

typedef esp_err_t (*httpd_err_handler_func_t)(httpd_req_t *req, httpd_err_code_t error);

#ifdef __cplusplus
extern "C" {
#endif
esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config);
esp_err_t httpd_stop(httpd_handle_t handle);
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler);
esp_err_t httpd_register_err_handler(httpd_handle_t handle, httpd_err_code_t error, httpd_err_handler_func_t fn);

int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len);
size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t val_size);
size_t httpd_req_get_url_query_len(httpd_req_t *r);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t buf_len);
esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t val_size);

esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status);
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value);
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t buf_len);
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t buf_len);
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t error, const char *msg);
#ifdef __cplusplus
}
#endif

static inline esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *str)
{
    return httpd_resp_send(r, str, HTTPD_RESP_USE_STRLEN);
}
static inline esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *r, const char *str)
{
    return httpd_resp_send_chunk(r, str, str ? HTTPD_RESP_USE_STRLEN : 0);
}
static inline esp_err_t httpd_resp_send_404(httpd_req_t *r)
{
    return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, NULL);
}
static inline esp_err_t httpd_resp_send_500(httpd_req_t *r)
{
    return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
}
