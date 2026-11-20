// CardputerZero audio through PulseAudio (PipeWire's pulse server): 8 kHz
// mono 16-bit both ways, resampled by the server. A USB headset is used when
// one is plugged in, else the built-in speaker and mic. The device is chosen
// again whenever a stream opens, so plugging a headset in takes effect at the
// next received transmission or the next press of Space.
//
// The volume setting drives the chosen output device's own volume (and
// unmutes it): the launcher's system volume is often low or muted, and
// software gain alone can't get past that.

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#include "audio.h"
#include "el_qso.h"
#include "esp_log.h"

static const char *TAG = "audio";

#define PLAY_BUF_SAMPLES   (AUDIO_RATE)                   // 1 s
#define PLAY_START_SAMPLES (3 * 4 * AUDIO_FRAME)          // 240 ms: three packets
#define PLAY_IDLE_CLOSE_MS 3000

static audio_mic_cb_t s_mic_cb;
static volatile bool s_loopback;
static volatile bool s_playing;
static volatile float s_volume = 0.7f;
static volatile float s_mic_gain = 1.0f;

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cv = PTHREAD_COND_INITIALIZER;
static int16_t s_ring[PLAY_BUF_SAMPLES];
static size_t s_head, s_fill;

// ---- device choice ---------------------------------------------------------

typedef struct {
    bool sink;
    char usb[256];
    char builtin[256];
    uint8_t usb_ch, builtin_ch;
    bool done;
} pick_t;

static void consider(pick_t *p, const char *name, bool monitor, uint8_t channels)
{
    if (!name || monitor) return;
    if (!p->usb[0] && strstr(name, ".usb-")) {
        snprintf(p->usb, sizeof(p->usb), "%s", name);
        p->usb_ch = channels;
    }
    if (!p->builtin[0] && strstr(name, "platform-sound")) {
        snprintf(p->builtin, sizeof(p->builtin), "%s", name);
        p->builtin_ch = channels;
    }
}

static void sink_cb(pa_context *c, const pa_sink_info *i, int eol, void *u)
{
    pick_t *p = u;
    if (eol) {
        p->done = true;
        return;
    }
    consider(p, i->name, false, i->volume.channels);
}

static void source_cb(pa_context *c, const pa_source_info *i, int eol, void *u)
{
    pick_t *p = u;
    if (eol) {
        p->done = true;
        return;
    }
    consider(p, i->name, i->monitor_of_sink != PA_INVALID_INDEX, i->volume.channels);
}

static bool run_op(pa_mainloop *ml, pa_operation *op)
{
    if (!op) return false;
    for (int spins = 0; spins < 400 && pa_operation_get_state(op) == PA_OPERATION_RUNNING; spins++) {
        if (pa_mainloop_iterate(ml, 1, NULL) < 0) break;
    }
    pa_operation_unref(op);
    return true;
}

// The USB headset's sink/source if one is there, else the built-in one, else
// "" (the server's default). For a sink, also sets its volume to the volume
// setting and unmutes it.
static void pick_device(bool sink, char *out, size_t size)
{
    out[0] = 0;
    pick_t p = {.sink = sink};
    pa_mainloop *ml = pa_mainloop_new();
    if (!ml) return;
    pa_context *ctx = pa_context_new(pa_mainloop_get_api(ml), "scheff-echolink");
    if (ctx && pa_context_connect(ctx, NULL, PA_CONTEXT_NOFLAGS, NULL) >= 0) {
        bool ready = false;
        for (int spins = 0; spins < 400; spins++) {
            pa_context_state_t st = pa_context_get_state(ctx);
            if (st == PA_CONTEXT_FAILED || st == PA_CONTEXT_TERMINATED) break;
            if (st == PA_CONTEXT_READY) {
                ready = true;
                break;
            }
            if (pa_mainloop_iterate(ml, 1, NULL) < 0) break;
        }
        if (ready && run_op(ml, sink ? pa_context_get_sink_info_list(ctx, sink_cb, &p)
                                     : pa_context_get_source_info_list(ctx, source_cb, &p))) {
            const char *name = p.usb[0] ? p.usb : p.builtin;
            uint8_t ch = p.usb[0] ? p.usb_ch : p.builtin_ch;
            if (sink && name[0]) {
                pa_cvolume cv;
                pa_cvolume_set(&cv, ch ? ch : 2, (pa_volume_t)(PA_VOLUME_NORM * s_volume));
                run_op(ml, pa_context_set_sink_volume_by_name(ctx, name, &cv, NULL, NULL));
                run_op(ml, pa_context_set_sink_mute_by_name(ctx, name, 0, NULL, NULL));
            }
        }
        pa_context_disconnect(ctx);
    }
    if (ctx) pa_context_unref(ctx);
    pa_mainloop_free(ml);
    snprintf(out, size, "%s", p.usb[0] ? p.usb : p.builtin);
}

// Applies volume changes off the UI thread, coalescing a dragged slider.
static pthread_mutex_t s_vol_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_vol_cv = PTHREAD_COND_INITIALIZER;
static bool s_vol_dirty;

static void *volume_thread(void *arg)
{
    for (;;) {
        pthread_mutex_lock(&s_vol_mu);
        while (!s_vol_dirty) pthread_cond_wait(&s_vol_cv, &s_vol_mu);
        s_vol_dirty = false;
        pthread_mutex_unlock(&s_vol_mu);
        char dev[256];
        pick_device(true, dev, sizeof(dev));
        usleep(100 * 1000);
    }
    return NULL;
}

static pa_simple *open_stream(bool playback)
{
    static const pa_sample_spec spec = {.format = PA_SAMPLE_S16LE, .rate = AUDIO_RATE, .channels = 1};
    pa_buffer_attr attr = {
        .maxlength = (uint32_t)-1,
        .tlength = playback ? AUDIO_RATE / 10 * 2 : (uint32_t)-1,  // 100 ms
        .prebuf = (uint32_t)-1,
        .minreq = (uint32_t)-1,
        .fragsize = playback ? (uint32_t)-1 : AUDIO_FRAME * 2 * 2,  // 40 ms
    };
    char dev[256];
    pick_device(playback, dev, sizeof(dev));
    int err = 0;
    pa_simple *s = pa_simple_new(NULL, "Scheff EchoLink", playback ? PA_STREAM_PLAYBACK : PA_STREAM_RECORD,
                                 dev[0] ? dev : NULL, playback ? "EchoLink audio" : "EchoLink mic", &spec, NULL,
                                 &attr, &err);
    if (!s) {
        ESP_LOGE(TAG, "%s %s: %s", playback ? "playback" : "capture", dev[0] ? dev : "(default)", pa_strerror(err));
    } else {
        ESP_LOGI(TAG, "%s on %s", playback ? "playing" : "recording", dev[0] ? dev : "the default device");
    }
    return s;
}

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

// ---- playback --------------------------------------------------------------

static void *play_thread(void *arg)
{
    pa_simple *s = NULL;
    int16_t frame[AUDIO_FRAME];
    uint32_t idle_since = now_ms();
    for (;;) {
        pthread_mutex_lock(&s_mu);
        if (!s_playing) {
            if (s) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 100 * 1000000L;
                if (ts.tv_nsec >= 1000000000L) {
                    ts.tv_sec++;
                    ts.tv_nsec -= 1000000000L;
                }
                pthread_cond_timedwait(&s_cv, &s_mu, &ts);
            } else {
                while (!s_playing) pthread_cond_wait(&s_cv, &s_mu);
            }
        }
        bool playing = s_playing;
        size_t got = 0;
        if (playing) {
            got = s_fill < AUDIO_FRAME ? s_fill : AUDIO_FRAME;
            size_t tail = (s_head + PLAY_BUF_SAMPLES - s_fill) % PLAY_BUF_SAMPLES;
            for (size_t i = 0; i < got; i++) frame[i] = s_ring[(tail + i) % PLAY_BUF_SAMPLES];
            s_fill -= got;
            if (got < AUDIO_FRAME) s_playing = false;  // drained: prebuffer again next time
        }
        pthread_mutex_unlock(&s_mu);

        if (!playing) {
            if (s && now_ms() - idle_since > PLAY_IDLE_CLOSE_MS) {
                pa_simple_drain(s, NULL);
                pa_simple_free(s);
                s = NULL;
            }
            continue;
        }
        idle_since = now_ms();
        if (!s && !(s = open_stream(true))) {
            usleep(500 * 1000);
            continue;
        }
        memset(frame + got, 0, (AUDIO_FRAME - got) * sizeof(int16_t));
        int err;
        if (pa_simple_write(s, frame, sizeof(frame), &err) < 0) {
            ESP_LOGW(TAG, "playback: %s", pa_strerror(err));
            pa_simple_free(s);
            s = NULL;
        }
    }
    return NULL;
}

// ---- capture ---------------------------------------------------------------

static void *mic_thread(void *arg)
{
    pa_simple *s = NULL;
    int16_t frame[AUDIO_FRAME];
    for (;;) {
        bool want = el_qso_ptt() || s_loopback;
        if (!want) {
            if (s) {
                pa_simple_free(s);
                s = NULL;
            }
            usleep(10 * 1000);
            continue;
        }
        if (!s && !(s = open_stream(false))) {
            usleep(500 * 1000);
            continue;
        }
        int err;
        if (pa_simple_read(s, frame, sizeof(frame), &err) < 0) {
            ESP_LOGW(TAG, "capture: %s", pa_strerror(err));
            pa_simple_free(s);
            s = NULL;
            continue;
        }
        float g = s_mic_gain;
        if (g != 1.0f) {
            for (size_t i = 0; i < AUDIO_FRAME; i++) {
                float x = frame[i] * g;
                frame[i] = x > 32767 ? 32767 : x < -32768 ? -32768 : (int16_t)x;
            }
        }
        if (s_loopback) {
            audio_play_push(frame, AUDIO_FRAME);
        } else if (s_mic_cb) {
            s_mic_cb(frame, AUDIO_FRAME);
        }
    }
    return NULL;
}

// ---- API -------------------------------------------------------------------

esp_err_t audio_init(audio_mic_cb_t mic_cb)
{
    s_mic_cb = mic_cb;
    pthread_t t;
    if (pthread_create(&t, NULL, play_thread, NULL) != 0) return ESP_FAIL;
    pthread_detach(t);
    if (pthread_create(&t, NULL, mic_thread, NULL) != 0) return ESP_FAIL;
    pthread_detach(t);
    if (pthread_create(&t, NULL, volume_thread, NULL) != 0) return ESP_FAIL;
    pthread_detach(t);
    ESP_LOGI(TAG, "PulseAudio at %d Hz mono, USB headset preferred", AUDIO_RATE);
    return ESP_OK;
}

void audio_play_push(const int16_t *pcm, size_t samples)
{
    pthread_mutex_lock(&s_mu);
    for (size_t i = 0; i < samples && s_fill < PLAY_BUF_SAMPLES; i++) {
        s_ring[s_head] = pcm[i];
        s_head = (s_head + 1) % PLAY_BUF_SAMPLES;
        s_fill++;
    }
    if (!s_playing && s_fill >= PLAY_START_SAMPLES) {
        s_playing = true;
        pthread_cond_signal(&s_cv);
    }
    pthread_mutex_unlock(&s_mu);
}

void audio_play_flush(void)
{
    pthread_mutex_lock(&s_mu);
    s_fill = 0;
    s_playing = false;
    pthread_mutex_unlock(&s_mu);
}

bool audio_is_playing(void)
{
    return s_playing;
}

void audio_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent / 100.0f;
    pthread_mutex_lock(&s_vol_mu);
    s_vol_dirty = true;
    pthread_cond_signal(&s_vol_cv);
    pthread_mutex_unlock(&s_vol_mu);
}

// The ESP32 boards set this as the codec's analog gain, 30 dB by default; the
// server already levels its sources, so here it is relative to that default.
void audio_set_mic_gain(int db)
{
    s_mic_gain = powf(10.0f, (db - 30) / 20.0f);
}

void audio_set_loopback(bool on)
{
    s_loopback = on;
    if (!on) audio_play_flush();
}

bool audio_loopback(void)
{
    return s_loopback;
}
