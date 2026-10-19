#pragma once

// A single EchoLink QSO (point-to-point connection), as used by the PC client
// in single-user mode. Wire format follows SvxLink's echolib:
//   UDP 5199: RTCP (version bits = 3) RR+SDES keepalives every 10 s, RR+BYE.
//   UDP 5198: RTP-ish audio: 12-byte header {0xC0, PT 3, seq, 0, 0} + 4 x 33
//             byte GSM 06.10 frames (80 ms), plus "oNDATA" info/chat text.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    QSO_IDLE,
    QSO_CONNECTING,
    QSO_CONNECTED,
} qso_state_t;

typedef struct {
    qso_state_t state;
    char remote_call[16];
    char remote_name[32];
    uint32_t remote_ip;  // network byte order
    bool ptt;
    bool receiving;
    uint32_t rx_packets, tx_packets;
    char end_reason[32];  // why the last connection ended ("no answer", ...)
} qso_status_t;

esp_err_t el_qso_init(void);

// Connect to a station by callsign, node number, or dotted IPv4 address.
esp_err_t el_qso_connect(const char *target);
void el_qso_disconnect(void);

void el_qso_set_ptt(bool on);
bool el_qso_ptt(void);
esp_err_t el_qso_send_chat(const char *msg);

// Rebuild the cached SDES packet after the callsign/name setting changes.
void el_qso_update_identity(void);

void el_qso_get_status(qso_status_t *out);

// The station info text the remote sent on connect (empty until it arrives).
void el_qso_get_info(char *out, size_t size);

// Mic frames from the audio layer (160 samples @ 8 kHz).
void el_qso_mic_frame(const int16_t *pcm, size_t samples);
