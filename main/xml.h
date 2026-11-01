#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "settings.h"

typedef struct {
    bool backup;
    settings_t settings;
} xml_document_t;

typedef esp_err_t (*xml_server_cb_t)(void *ctx, const char **attributes);

// Validates the whole EchoLink document before the caller applies any changes.
esp_err_t xml_read(const char *text, xml_server_cb_t server, void *ctx, xml_document_t *document);
const char *xml_attribute(const char **attributes, const char *name);
// 1: copied, 0: absent, -1: too long.
int xml_copy_attribute(const char **attributes, const char *name, char *out, size_t size);
