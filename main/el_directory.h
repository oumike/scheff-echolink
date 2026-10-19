#pragma once

// EchoLink directory server client (TCP 5200): station login/registration and
// the station list used to resolve callsigns/node numbers to IP addresses.
// Protocol as implemented by SvxLink's echolib (EchoLinkDirectory.cpp).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    EL_DIR_OFFLINE,
    EL_DIR_ONLINE,
    EL_DIR_BUSY,
} el_dir_status_t;

typedef struct {
    char call[16];
    char desc[48];
    char time[6];   // "HH:MM" from the directory, if present
    bool busy;
    uint32_t node;
    uint32_t ip;    // network byte order
} el_station_t;

// Starts the background task that keeps us registered (every 5 min) and
// fetches the station list after the first successful login.
void el_dir_start(void);

// Request a status change; applied immediately by the background task.
void el_dir_set_status(el_dir_status_t status);
el_dir_status_t el_dir_status(void);

// Log in again now, e.g. after the callsign or password changed.
void el_dir_relogin(void);
bool el_dir_logged_in(void);

// Synchronous; can take several seconds. Safe to call from the console.
esp_err_t el_dir_login(el_dir_status_t status);
esp_err_t el_dir_fetch_list(void);

size_t el_dir_count(void);
// Look up by callsign (case-insensitive, "*" optional for conferences) or by
// node number when `query` is all digits. Returns true and fills `out`.
bool el_dir_find(const char *query, el_station_t *out);
bool el_dir_find_ip(uint32_t ip, el_station_t *out);
// Print up to `max` stations whose callsign or description contains `filter`.
void el_dir_print(const char *filter, int max);
