#include "xml.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "expat.h"

static const char *TAG = "xml";

typedef struct {
    XML_Parser parser;
    xml_server_cb_t server;
    void *ctx;
    xml_document_t document;
    esp_err_t error;
    enum { ROOT_NONE, ROOT_LIST, ROOT_BACKUP, ROOT_SERVER } root;
    unsigned depth;
    bool settings_seen, servers_seen, in_servers;
} xml_reader_t;

const char *xml_attribute(const char **attributes, const char *name)
{
    for (size_t i = 0; attributes[i]; i += 2) {
        if (!strcmp(attributes[i], name)) return attributes[i + 1];
    }
    return NULL;
}

int xml_copy_attribute(const char **attributes, const char *name, char *out, size_t size)
{
    const char *value = xml_attribute(attributes, name);
    if (!value) return 0;
    size_t len = strlen(value);
    if (len >= size) return -1;
    memcpy(out, value, len + 1);
    return 1;
}

static bool xml_u8(const char *text, uint8_t max, uint8_t *value)
{
    if (!text || !*text) return false;
    char *end;
    long parsed = strtol(text, &end, 10);
    if (*end || parsed < 0 || parsed > max) return false;
    *value = parsed;
    return true;
}

static esp_err_t xml_settings(const char **attributes, settings_t *settings)
{
    const char *version = xml_attribute(attributes, "version");
    if (!version || strcmp(version, "1")) return ESP_ERR_INVALID_VERSION;
#define READ_SETTING(field) \
    do { \
        if (xml_copy_attribute(attributes, #field, settings->field, sizeof(settings->field)) != 1) \
            return ESP_ERR_INVALID_ARG; \
    } while (0)
    READ_SETTING(callsign);
    READ_SETTING(password);
    READ_SETTING(name);
    READ_SETTING(location);
    READ_SETTING(dir_server);
    READ_SETTING(wifi_ssid);
    READ_SETTING(wifi_pass);
#undef READ_SETTING
    const char *incoming = xml_attribute(attributes, "accept_incoming");
    if (!incoming || (strcmp(incoming, "true") && strcmp(incoming, "false"))) return ESP_ERR_INVALID_ARG;
    settings->accept_incoming = !strcmp(incoming, "true");
    if (!xml_u8(xml_attribute(attributes, "volume"), 100, &settings->volume) ||
        !xml_u8(xml_attribute(attributes, "mic_gain"), 42, &settings->mic_gain)) return ESP_ERR_INVALID_ARG;
    for (char *p = settings->callsign; *p; p++) *p = toupper((unsigned char)*p);
    return ESP_OK;
}

static void xml_stop(xml_reader_t *reader, esp_err_t error)
{
    reader->error = error;
    XML_StopParser(reader->parser, XML_FALSE);
}

static void xml_start(void *ctx, const char *name, const char **attributes)
{
    xml_reader_t *reader = ctx;
    reader->depth++;
    bool server = false;
    if (reader->depth == 1) {
        if (!strcmp(name, "echolink-servers")) reader->root = ROOT_LIST;
        else if (!strcmp(name, "echolink-backup")) reader->root = ROOT_BACKUP;
        else if (!strcmp(name, "server")) reader->root = ROOT_SERVER, server = true;
        else {
            xml_stop(reader, ESP_ERR_INVALID_ARG);
            return;
        }
        const char *version = xml_attribute(attributes, "version");
        if (reader->root != ROOT_SERVER &&
            ((reader->root == ROOT_BACKUP && !version) || (version && strcmp(version, "1")))) {
            xml_stop(reader, ESP_ERR_INVALID_VERSION);
            return;
        }
        reader->document.backup = reader->root == ROOT_BACKUP;
    } else if (reader->root == ROOT_LIST && reader->depth == 2 && !strcmp(name, "server")) {
        server = true;
    } else if (reader->root == ROOT_BACKUP && reader->depth == 2 &&
               !strcmp(name, "settings") && !reader->settings_seen) {
        reader->settings_seen = true;
        esp_err_t err = xml_settings(attributes, &reader->document.settings);
        if (err != ESP_OK) xml_stop(reader, err);
    } else if (reader->root == ROOT_BACKUP && reader->depth == 2 &&
               !strcmp(name, "servers") && !reader->servers_seen) {
        reader->servers_seen = reader->in_servers = true;
    } else if (reader->root == ROOT_BACKUP && reader->in_servers &&
               reader->depth == 3 && !strcmp(name, "server")) {
        server = true;
    } else {
        xml_stop(reader, ESP_ERR_INVALID_ARG);
        return;
    }
    if (server && reader->server) {
        esp_err_t err = reader->server(reader->ctx, attributes);
        if (err != ESP_OK) xml_stop(reader, err);
    }
}

static void xml_end(void *ctx, const char *name)
{
    xml_reader_t *reader = ctx;
    if (reader->depth == 2 && !strcmp(name, "servers")) reader->in_servers = false;
    reader->depth--;
}

static void xml_text(void *ctx, const char *text, int len)
{
    for (int i = 0; i < len; i++) {
        if (!isspace((unsigned char)text[i])) {
            xml_stop(ctx, ESP_ERR_INVALID_ARG);
            return;
        }
    }
}

static void xml_doctype(void *ctx, const char *name, const char *system_id,
                        const char *public_id, int internal_subset)
{
    xml_stop(ctx, ESP_ERR_INVALID_ARG);
}

esp_err_t xml_read(const char *text, xml_server_cb_t server, void *ctx, xml_document_t *document)
{
    ESP_RETURN_ON_FALSE(text, ESP_ERR_INVALID_ARG, TAG, "missing document");
    size_t len = strlen(text);
    ESP_RETURN_ON_FALSE(len && len <= INT_MAX, ESP_ERR_INVALID_SIZE, TAG, "invalid document size");
    xml_reader_t reader = {.parser = XML_ParserCreate("UTF-8"), .server = server, .ctx = ctx};
    ESP_RETURN_ON_FALSE(reader.parser, ESP_ERR_NO_MEM, TAG, "allocate parser");
    XML_SetUserData(reader.parser, &reader);
    XML_SetElementHandler(reader.parser, xml_start, xml_end);
    XML_SetCharacterDataHandler(reader.parser, xml_text);
    XML_SetStartDoctypeDeclHandler(reader.parser, xml_doctype);
    XML_SetParamEntityParsing(reader.parser, XML_PARAM_ENTITY_PARSING_NEVER);
    if (XML_Parse(reader.parser, text, (int)len, XML_TRUE) != XML_STATUS_OK) {
        if (reader.error == ESP_OK) {
            reader.error = XML_GetErrorCode(reader.parser) == XML_ERROR_NO_MEMORY
                               ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
        }
        ESP_LOGW(TAG, "invalid document at line %lu: %s (%s)",
                 (unsigned long)XML_GetCurrentLineNumber(reader.parser),
                 XML_ErrorString(XML_GetErrorCode(reader.parser)), esp_err_to_name(reader.error));
    }
    XML_ParserFree(reader.parser);
    if (reader.error == ESP_OK && reader.root == ROOT_BACKUP &&
        (!reader.settings_seen || !reader.servers_seen)) {
        ESP_LOGW(TAG, "backup is missing settings or servers");
        reader.error = ESP_ERR_INVALID_ARG;
    }
    if (reader.error == ESP_OK && document) *document = reader.document;
    return reader.error;
}
