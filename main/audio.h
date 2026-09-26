#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// EchoLink audio is 8 kHz mono 16-bit, carried as GSM 06.10 in 20 ms frames.
#define AUDIO_RATE        8000
#define AUDIO_FRAME       160  // samples per 20 ms GSM frame

// Called from the capture task with each 20 ms mic frame.
typedef void (*audio_mic_cb_t)(const int16_t *pcm, size_t samples);

esp_err_t audio_init(audio_mic_cb_t mic_cb);

// Queue decoded received audio for playback. Playback starts once a few
// packets are buffered (jitter cushion) and stops when the buffer drains.
void audio_play_push(const int16_t *pcm, size_t samples);
void audio_play_flush(void);
bool audio_is_playing(void);

void audio_set_volume(int percent);
void audio_set_mic_gain(int db);

// Local mic -> speaker loopback for bring-up testing.
void audio_set_loopback(bool on);
bool audio_loopback(void);
