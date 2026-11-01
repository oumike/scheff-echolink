#include "el_qso.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "gsm.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "audio.h"
#include "board.h"
#include "el_directory.h"
#include "settings.h"

static const char *TAG = "el_qso";

#define PORT_AUDIO        5198
#define PORT_CTRL         5199

#define RTP_VERSION       3
#define RTCP_RR           201
#define RTCP_SDES         202
#define RTCP_BYE          203
#define SDES_END          0
#define SDES_CNAME        1
#define SDES_NAME         2
#define SDES_EMAIL        3
#define SDES_PHONE        4

#define GSM_FRAME_BYTES   33
#define FRAMES_PER_PACKET 4
#define RTP_HDR_BYTES     12
#define AUDIO_PKT_BYTES   (RTP_HDR_BYTES + FRAMES_PER_PACKET * GSM_FRAME_BYTES)

#define KEEPALIVE_US      (10 * 1000 * 1000LL)
#define CONNECT_RETRIES   5
#define LINK_TIMEOUT_US   (50 * 1000 * 1000LL)
#define RX_HOLD_US        (300 * 1000LL)

static struct {
    SemaphoreHandle_t lock;
    int audio_fd, ctrl_fd;

    volatile qso_state_t state;
    uint32_t remote_ip;
    char remote_call[16];
    char remote_name[32];
    char end_reason[32];
    char info[512];
    int retries;
    int64_t last_rx_us, next_keepalive_us, last_audio_us;
    uint32_t rx_packets, tx_packets;

    volatile bool ptt;
    uint16_t tx_seq;
    int16_t tx_pcm[FRAMES_PER_PACKET * AUDIO_FRAME];
    size_t tx_fill;

    gsm enc, dec;

    uint8_t sdes[256];
    size_t sdes_len;
} q;

/* ---- RTCP packet builders (after SvxLink rtpacket.cpp) ---- */

static uint8_t *put_item(uint8_t *p, uint8_t type, const char *text)
{
    size_t len = strlen(text);
    if (len > 255) len = 255;
    *p++ = type;
    *p++ = len;
    memcpy(p, text, len);
    return p + len;
}

// Empty receiver report that prefixes every RTCP packet we send.
static uint8_t *put_null_rr(uint8_t *p)
{
    const uint8_t rr[8] = {RTP_VERSION << 6, RTCP_RR, 0, 1, 0, 0, 0, 0};
    memcpy(p, rr, sizeof(rr));
    return p + sizeof(rr);
}

static size_t build_sdes(uint8_t *out, const char *call, const char *name)
{
    uint8_t *p = put_null_rr(out);
    uint8_t *sdes = p;
    sdes[0] = (RTP_VERSION << 6) | 1;  // one source chunk
    sdes[1] = RTCP_SDES;
    memset(sdes + 4, 0, 4);            // SSRC 0
    p = sdes + 8;

    char line[64];
    snprintf(line, sizeof(line), "%-15s%s", call, name);
    p = put_item(p, SDES_CNAME, "CALLSIGN");
    p = put_item(p, SDES_NAME, line);
    p = put_item(p, SDES_EMAIL, "CALLSIGN");
    p = put_item(p, SDES_PHONE, "08:30");
    *p++ = SDES_END;
    *p++ = 0;
    while ((p - sdes) & 3) *p++ = 0;

    size_t words = (p - sdes) / 4 - 1;
    sdes[2] = words >> 8;
    sdes[3] = words;
    return p - out;
}

static size_t build_bye(uint8_t *out)
{
    uint8_t *p = put_null_rr(out);
    uint8_t *bye = p;
    bye[0] = (RTP_VERSION << 6) | 1;
    bye[1] = RTCP_BYE;
    memset(bye + 4, 0, 4);
    p = bye + 8;
    const char *reason = "jan2002";
    *p++ = strlen(reason);
    memcpy(p, reason, strlen(reason));
    p += strlen(reason);
    while ((p - bye) & 3) *p++ = 0;

    size_t words = (p - bye) / 4 - 1;
    bye[2] = words >> 8;
    bye[3] = words;
    return p - out;
}

// Walk a compound RTCP packet looking for `type`. Returns the sub-packet.
static const uint8_t *find_rtcp(const uint8_t *buf, size_t len, uint8_t type, size_t *sub_len)
{
    const uint8_t *p = buf, *end = buf + len;
    while (p + 4 <= end) {
        int ver = p[0] >> 6;
        if (ver != RTP_VERSION && ver != 1) break;
        size_t l = (((size_t)p[2] << 8 | p[3]) + 1) * 4;
        if (p + l > end) break;
        if (p[1] == type) {
            *sub_len = l;
            return p;
        }
        p += l;
    }
    return NULL;
}

static bool sdes_get(const uint8_t *buf, size_t len, uint8_t item, char *out, size_t out_len)
{
    size_t l;
    const uint8_t *p = find_rtcp(buf, len, RTCP_SDES, &l);
    if (!p || (p[0] & 0x1f) == 0) return false;
    const uint8_t *cp = p + 8, *end = p + l;
    while (cp + 2 <= end && cp[0] != SDES_END) {
        uint8_t ilen = cp[1];
        if (cp + 2 + ilen > end) break;
        if (cp[0] == item) {
            size_t n = ilen < out_len - 1 ? ilen : out_len - 1;
            memcpy(out, cp + 2, n);
            out[n] = 0;
            return true;
        }
        cp += 2 + ilen;
    }
    return false;
}

/* ---- sending ---- */

static void send_to(int fd, uint32_t ip, uint16_t port, const void *buf, size_t len)
{
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = ip};
    if (sendto(fd, buf, len, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
        ESP_LOGW(TAG, "sendto port %u failed: errno %d", port, errno);
    }
}

static void send_sdes(uint32_t ip)
{
    send_to(q.ctrl_fd, ip, PORT_CTRL, q.sdes, q.sdes_len);
}

static void send_bye(uint32_t ip)
{
    uint8_t pkt[32];
    send_to(q.ctrl_fd, ip, PORT_CTRL, pkt, build_bye(pkt));
}

// Info text the remote shows when we connect ("oNDATA\r" + text + NUL).
static void send_info(uint32_t ip)
{
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "oNDATA\r%s %s\rEchoLink on " BOARD_NAME "\r", g_settings.callsign,
                     g_settings.name);
    if (n > 0 && n < sizeof(buf)) send_to(q.audio_fd, ip, PORT_AUDIO, buf, n + 1);
}

/* ---- state changes (call with q.lock held) ---- */

static void set_idle(const char *why)
{
    if (q.state == QSO_IDLE) return;
    printf("\n*** Disconnected from %s (%s)\n", q.remote_call[0] ? q.remote_call : "?", why);
    strlcpy(q.end_reason, why, sizeof(q.end_reason));
    q.state = QSO_IDLE;
    q.ptt = false;
    q.remote_ip = 0;
    audio_play_flush();
    el_dir_set_status(EL_DIR_ONLINE);
}

static void set_connected(void)
{
    q.state = QSO_CONNECTED;
    q.last_rx_us = esp_timer_get_time();
    q.rx_packets = q.tx_packets = 0;
    send_info(q.remote_ip);
    el_dir_set_status(EL_DIR_BUSY);
    printf("\n*** Connected to %s %s\n", q.remote_call, q.remote_name);
}

/* ---- receiving ---- */

static void parse_remote_name(const uint8_t *buf, size_t len)
{
    char id[128];
    if (!sdes_get(buf, len, SDES_NAME, id, sizeof(id))) return;
    char *sp = strpbrk(id, " \t\r\n");
    if (sp) {
        *sp++ = 0;
        while (*sp && isspace((unsigned char)*sp)) sp++;
        strlcpy(q.remote_name, sp, sizeof(q.remote_name));
    }
    if (id[0]) strlcpy(q.remote_call, id, sizeof(q.remote_call));
}

static void handle_ctrl(const uint8_t *buf, size_t len, uint32_t from)
{
    size_t l;
    bool is_bye = find_rtcp(buf, len, RTCP_BYE, &l) != NULL;
    bool is_sdes = !is_bye && find_rtcp(buf, len, RTCP_SDES, &l) != NULL;

    xSemaphoreTake(q.lock, portMAX_DELAY);
    if (q.state != QSO_IDLE && from == q.remote_ip) {
        q.last_rx_us = esp_timer_get_time();
        if (is_bye) {
            set_idle("remote hung up");
        } else if (is_sdes) {
            parse_remote_name(buf, len);
            if (q.state == QSO_CONNECTING) set_connected();
        }
    } else if (is_sdes) {
        // Someone is calling us. Accept only when idle and when the caller is
        // a registered station (same check the official client makes).
        el_station_t st;
        bool known = el_dir_find_ip(from, &st);
        if (q.state == QSO_IDLE && g_settings.accept_incoming && known) {
            q.remote_ip = from;
            strlcpy(q.remote_call, st.call, sizeof(q.remote_call));
            q.remote_name[0] = 0;
            parse_remote_name(buf, len);
            send_sdes(from);
            printf("\n*** Incoming connection from %s\n", q.remote_call);
            set_connected();
            q.next_keepalive_us = esp_timer_get_time() + KEEPALIVE_US;
        } else {
            struct in_addr a = {.s_addr = from};
            ESP_LOGI(TAG, "rejecting connection from %s (%s)", inet_ntoa(a),
                     q.state != QSO_IDLE ? "busy" : !known ? "not in directory" : "incoming disabled");
            send_bye(from);
        }
    }
    xSemaphoreGive(q.lock);
}

static void handle_text(const uint8_t *buf, size_t len)
{
    char text[512];
    size_t n = len < sizeof(text) - 1 ? len : sizeof(text) - 1;
    memcpy(text, buf, n);
    text[n] = 0;  // packets are NUL-terminated, but don't trust that
    for (char *c = text; *c; c++) if (*c == '\r') *c = '\n';
    if (len > 6 && buf[6] == '\r') {
        xSemaphoreTake(q.lock, portMAX_DELAY);
        strlcpy(q.info, text + 7, sizeof(q.info));
        xSemaphoreGive(q.lock);
        printf("\n--- info from %s ---\n%s\n---\n", q.remote_call, text + 7);
    } else {
        printf("\n[chat] %s\n", text + 6);
    }
}

static void handle_audio(const uint8_t *buf, size_t len, uint32_t from)
{
    if (!len || q.state == QSO_IDLE || from != q.remote_ip) return;
    q.last_rx_us = esp_timer_get_time();

    if (buf[0] != 0xc0) {
        if (len > 6 && !memcmp(buf + 1, "NDATA", 5)) handle_text(buf, len);
        return;
    }
    if (len < AUDIO_PKT_BYTES || buf[1] != 3) {
        ESP_LOGD(TAG, "ignoring audio packet pt=%d len=%u", len > 1 ? buf[1] : -1, (unsigned)len);
        return;
    }
    q.rx_packets++;
    q.last_audio_us = q.last_rx_us;
    if (q.ptt) return;  // half duplex: don't play while we're transmitting

    int16_t pcm[FRAMES_PER_PACKET * AUDIO_FRAME];
    for (int i = 0; i < FRAMES_PER_PACKET; i++) {
        if (gsm_decode(q.dec, (gsm_byte *)buf + RTP_HDR_BYTES + i * GSM_FRAME_BYTES,
                       pcm + i * AUDIO_FRAME) != 0) {
            ESP_LOGW(TAG, "discarding invalid GSM audio packet");
            return;
        }
    }
    audio_play_push(pcm, FRAMES_PER_PACKET * AUDIO_FRAME);
}

static void timers(void)
{
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(q.lock, portMAX_DELAY);
    if (q.state != QSO_IDLE && now >= q.next_keepalive_us) {
        q.next_keepalive_us = now + KEEPALIVE_US;
        if (q.state == QSO_CONNECTING && ++q.retries >= CONNECT_RETRIES) {
            set_idle("no answer");
        } else {
            send_sdes(q.remote_ip);
        }
    }
    if (q.state == QSO_CONNECTED && now - q.last_rx_us > LINK_TIMEOUT_US) {
        set_idle("link timeout");
    }
    xSemaphoreGive(q.lock);
}

static void net_task(void *arg)
{
    static uint8_t buf[1500];
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(q.audio_fd, &rfds);
        FD_SET(q.ctrl_fd, &rfds);
        struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
        int maxfd = q.audio_fd > q.ctrl_fd ? q.audio_fd : q.ctrl_fd;
        if (select(maxfd + 1, &rfds, NULL, NULL, &tv) > 0) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            if (FD_ISSET(q.ctrl_fd, &rfds)) {
                int n = recvfrom(q.ctrl_fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
                if (n > 0) handle_ctrl(buf, n, from.sin_addr.s_addr);
            }
            fl = sizeof(from);
            if (FD_ISSET(q.audio_fd, &rfds)) {
                int n = recvfrom(q.audio_fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
                if (n > 0) handle_audio(buf, n, from.sin_addr.s_addr);
            }
        }
        timers();
    }
}

static int bind_udp(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ---- public API ---- */

void el_qso_update_identity(void)
{
    xSemaphoreTake(q.lock, portMAX_DELAY);
    q.sdes_len = build_sdes(q.sdes, g_settings.callsign, g_settings.name);
    xSemaphoreGive(q.lock);
}

esp_err_t el_qso_init(void)
{
    q.lock = xSemaphoreCreateMutex();
    q.enc = gsm_create();
    q.dec = gsm_create();
    if (!q.lock || !q.enc || !q.dec) return ESP_ERR_NO_MEM;

    // Source ports must be 5198/5199 too: the remote replies to these, and
    // it's what lets replies through a NAT without port forwarding.
    q.audio_fd = bind_udp(PORT_AUDIO);
    q.ctrl_fd = bind_udp(PORT_CTRL);
    if (q.audio_fd < 0 || q.ctrl_fd < 0) {
        ESP_LOGE(TAG, "could not bind UDP %d/%d", PORT_AUDIO, PORT_CTRL);
        return ESP_FAIL;
    }
    el_qso_update_identity();
    xTaskCreate(net_task, "el_qso", 8192, NULL, 18, NULL);
    return ESP_OK;
}

esp_err_t el_qso_connect(const char *target)
{
    el_station_t st = {0};
    struct in_addr addr;
    if (inet_aton(target, &addr)) {
        st.ip = addr.s_addr;
        if (!el_dir_find_ip(st.ip, &st)) strlcpy(st.call, target, sizeof(st.call));
    } else if (!el_dir_find(target, &st)) {
        printf("%s not found in directory (%u stations loaded)\n", target, (unsigned)el_dir_count());
        return ESP_ERR_NOT_FOUND;
    }
    if (st.busy) printf("Note: %s is marked BUSY in the directory\n", st.call);
    if (!el_dir_logged_in()) printf("Warning: not logged in to the directory; the remote will likely refuse us\n");

    xSemaphoreTake(q.lock, portMAX_DELAY);
    if (q.state != QSO_IDLE) {
        xSemaphoreGive(q.lock);
        printf("Already connected; disconnect first\n");
        return ESP_ERR_INVALID_STATE;
    }
    q.remote_ip = st.ip;
    strlcpy(q.remote_call, st.call, sizeof(q.remote_call));
    q.remote_name[0] = 0;
    q.end_reason[0] = 0;
    q.info[0] = 0;
    q.state = QSO_CONNECTING;
    q.retries = 0;
    q.next_keepalive_us = esp_timer_get_time() + KEEPALIVE_US;
    send_sdes(q.remote_ip);
    xSemaphoreGive(q.lock);

    addr.s_addr = st.ip;
    printf("Connecting to %s (node %lu, %s)...\n", st.call, (unsigned long)st.node, inet_ntoa(addr));
    return ESP_OK;
}

void el_qso_disconnect(void)
{
    xSemaphoreTake(q.lock, portMAX_DELAY);
    if (q.state != QSO_IDLE) {
        send_bye(q.remote_ip);
        set_idle("local");
    }
    xSemaphoreGive(q.lock);
}

void el_qso_set_ptt(bool on)
{
    if (on && q.state != QSO_CONNECTED) return;
    if (on == q.ptt) return;
    q.ptt = on;
    if (on) audio_play_flush();
    printf("\n*** %s\n", on ? "TX" : "RX");
}

bool el_qso_ptt(void)
{
    return q.ptt;
}

esp_err_t el_qso_send_chat(const char *msg)
{
    if (q.state != QSO_CONNECTED) return ESP_ERR_INVALID_STATE;
    char buf[300];
    int n = snprintf(buf, sizeof(buf), "oNDATA%s>%s\r\n", g_settings.callsign, msg);
    if (n <= 0 || n >= sizeof(buf)) return ESP_ERR_INVALID_SIZE;
    send_to(q.audio_fd, q.remote_ip, PORT_AUDIO, buf, n + 1);
    return ESP_OK;
}

void el_qso_get_status(qso_status_t *out)
{
    xSemaphoreTake(q.lock, portMAX_DELAY);
    out->state = q.state;
    strlcpy(out->remote_call, q.remote_call, sizeof(out->remote_call));
    strlcpy(out->remote_name, q.remote_name, sizeof(out->remote_name));
    out->remote_ip = q.remote_ip;
    out->ptt = q.ptt;
    out->receiving = q.state == QSO_CONNECTED && esp_timer_get_time() - q.last_audio_us < RX_HOLD_US;
    out->rx_packets = q.rx_packets;
    out->tx_packets = q.tx_packets;
    strlcpy(out->end_reason, q.end_reason, sizeof(out->end_reason));
    xSemaphoreGive(q.lock);
}

// Runs in the audio capture task. Only this task touches the encoder and
// tx buffer, so no lock is needed beyond reading the connection state.
void el_qso_mic_frame(const int16_t *pcm, size_t samples)
{
    if (!q.ptt || q.state != QSO_CONNECTED || samples != AUDIO_FRAME) {
        q.tx_fill = 0;
        return;
    }
    memcpy(q.tx_pcm + q.tx_fill, pcm, samples * sizeof(int16_t));
    q.tx_fill += samples;
    if (q.tx_fill < FRAMES_PER_PACKET * AUDIO_FRAME) return;
    q.tx_fill = 0;

    uint8_t pkt[AUDIO_PKT_BYTES];
    uint16_t seq = q.tx_seq++;
    pkt[0] = 0xc0;
    pkt[1] = 3;  // GSM
    pkt[2] = seq >> 8;
    pkt[3] = seq;
    memset(pkt + 4, 0, 8);  // timestamp, SSRC
    for (int i = 0; i < FRAMES_PER_PACKET; i++) {
        gsm_encode(q.enc, q.tx_pcm + i * AUDIO_FRAME, pkt + RTP_HDR_BYTES + i * GSM_FRAME_BYTES);
    }
    send_to(q.audio_fd, q.remote_ip, PORT_AUDIO, pkt, sizeof(pkt));
    q.tx_packets++;
}

void el_qso_get_info(char *out, size_t size)
{
    xSemaphoreTake(q.lock, portMAX_DELAY);
    strlcpy(out, q.info, size);
    xSemaphoreGive(q.lock);
}
