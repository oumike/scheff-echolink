#pragma once

// Known EchoLink nodes (conferences, links, repeaters), kept as XML on the
// internal FAT partition:
//
//   /data/echolink-default-servers.xml  snapshot of the first directory list,
//                                       written once and never edited
//   /data/echolink-servers.xml          working copy the UI reads and edits,
//                                       created from the default
//
// File format:
//   <?xml version="1.0" encoding="UTF-8"?>
//   <echolink-servers version="1" generated="2026-09-26T18:00:00Z">
//     <server call="*ECHOTEST*" node="9999" type="conference"
//             desc="EchoLink Test Server" ip="1.2.3.4" host=""
//             favorite="true"/>
//   </echolink-servers>
//
// `ip` is the address last seen in the directory (EchoLink addresses change,
// so connects still resolve through the directory). `host` is an optional
// fixed hostname or IP entered by hand for a manual entry.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "el_directory.h"

#define SERVER_LIST_DEFAULT_PATH "/data/echolink-default-servers.xml"
#define SERVER_LIST_PATH         "/data/echolink-servers.xml"

typedef enum {
    SERVER_CONFERENCE,
    SERVER_LINK,
    SERVER_REPEATER,
    SERVER_OTHER,
} server_type_t;

typedef struct {
    char call[16];
    char desc[48];
    char host[64];
    uint32_t node;
    uint32_t ip;  // network byte order, 0 if unknown
    server_type_t type;
    bool manual;
    bool favorite;
} server_entry_t;

// Mounts /data and loads the working list, creating it from the default if
// only the default exists. A missing list is not an error: the snapshot is
// taken on the first directory download.
esp_err_t server_list_init(void);

// Called with each fresh directory list. Writes the default snapshot (and
// working copy) the first time; later calls do nothing.
void server_list_on_directory(const el_station_t *stations, size_t count);

size_t server_list_count(void);

// Bucket key for an entry or filter: 'A'..'Z', or '#' for callsigns that
// start with a digit. The leading "*" of a conference name is ignored.
char server_list_bucket(const char *call);

// Pseudo-bucket for server_list_get_bucket(): favorites only.
#define SERVER_BUCKET_FAVORITES '*'

// Copies up to `max` entries in bucket `bucket` (or all favorites, for
// SERVER_BUCKET_FAVORITES) into `out`, sorted. Returns the number copied.
size_t server_list_get_bucket(char bucket, server_entry_t *out, size_t max);

// Copies up to `max` entries whose callsign or description contains `query`
// (case-insensitive), or whose node number starts with it, into `out`.
size_t server_list_search(const char *query, server_entry_t *out, size_t max);

size_t server_list_favorite_count(void);

// Marks or unmarks `call` as a favorite and saves the working list.
esp_err_t server_list_set_favorite(const char *call, bool favorite);

// Saves `entry` to the working list and rewrites echolink-servers.xml. With
// `orig_call` set, the entry with that callsign is replaced (an edit, which
// may rename it); otherwise it is added, replacing any with the same callsign.
// The default file is never touched. New entries are marked manual.
esp_err_t server_list_put(const char *orig_call, const server_entry_t *entry);

// Removes the entry with callsign `call` from the working list.
esp_err_t server_list_delete(const char *call);

// Replaces the working list with the <server> entries in `xml` (a file in the
// format above, or a complete backup) and saves it. Empty legacy lists return
// ESP_ERR_NOT_FOUND; complete backups can intentionally restore an empty list.
// Validation or write failures leave the active list unchanged. The default
// file is never touched.
esp_err_t server_list_import_buffer(const char *xml, size_t *count);
esp_err_t server_list_import_file(const char *path, size_t *count);

// Writes the working list to `path` (e.g. on the SD card).
esp_err_t server_list_export_file(const char *path);

typedef esp_err_t (*server_list_write_cb_t)(void *ctx, const char *data, size_t len);
// Streams <server> elements from a consistent RAM snapshot, without holding
// the list lock or a FAT file open during a potentially slow network transfer.
esp_err_t server_list_export_entries(server_list_write_cb_t write, void *ctx);

// Registers a callback run (from the caller's task) whenever the list changes.
void server_list_set_changed_cb(void (*cb)(void));
