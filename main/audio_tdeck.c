#include "audio.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "board.h"

static const char *TAG = "audio";

#define PLAY_BUF_BYTES   (AUDIO_RATE * sizeof(int16_t))
#define PLAY_START_BYTES (3 * 4 * AUDIO_FRAME * sizeof(int16_t))

static i2s_chan_handle_t s_speaker;
static esp_codec_dev_handle_t s_mic_codec;
static StreamBufferHandle_t s_play;
static audio_mic_cb_t s_mic_cb;
static volatile bool s_playing;
static volatile bool s_loopback;
static volatile int s_volume = 70;

static esp_err_t speaker_open(void)
{
    i2s_chan_config_t channel_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_cfg, &s_speaker, NULL), TAG, "speaker channel");
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_SPEAKER_BCLK,
            .ws = BOARD_SPEAKER_WS,
            .dout = BOARD_SPEAKER_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_speaker, &std_cfg), TAG, "speaker I2S");
    return i2s_channel_enable(s_speaker);
}

static esp_err_t microphone_open(void)
{
    i2s_chan_handle_t mic_rx;
    i2s_chan_config_t channel_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_cfg, NULL, &mic_rx), TAG, "microphone channel");
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_MIC_MCLK,
            .bclk = BOARD_MIC_BCLK,
            .ws = BOARD_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = BOARD_MIC_DIN,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(mic_rx, &std_cfg), TAG, "microphone I2S");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(mic_rx), TAG, "microphone enable");

    audio_codec_i2s_cfg_t data_cfg = {.port = I2S_NUM_1, .rx_handle = mic_rx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&data_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = board_codec_i2c_bus(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(data_if && ctrl_if, ESP_FAIL, TAG, "ES7210 interfaces");
    es7210_codec_cfg_t codec_cfg = {
        .ctrl_if = ctrl_if,
        .master_mode = false,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    const audio_codec_if_t *codec_if = es7210_codec_new(&codec_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "ES7210 codec");
    esp_codec_dev_cfg_t device_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_mic_codec = esp_codec_dev_new(&device_cfg);
    ESP_RETURN_ON_FALSE(s_mic_codec, ESP_FAIL, TAG, "ES7210 device");
    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = AUDIO_RATE,
        .mclk_multiple = 256,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_mic_codec, &sample_cfg) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "ES7210 open");
    return ESP_OK;
}

static void play_task(void *arg)
{
    int16_t mono[AUDIO_FRAME];
    int16_t stereo[AUDIO_FRAME * 2];
    for (;;) {
        size_t got = 0;
        if (!s_playing && xStreamBufferBytesAvailable(s_play) >= PLAY_START_BYTES) s_playing = true;
        if (s_playing) {
            got = xStreamBufferReceive(s_play, mono, sizeof(mono), 0);
            if (got < sizeof(mono)) s_playing = false;
        }
        memset((uint8_t *)mono + got, 0, sizeof(mono) - got);
        int volume = s_volume;
        for (size_t i = 0; i < AUDIO_FRAME; i++) {
            int16_t sample = (int32_t)mono[i] * volume / 100;
            stereo[i * 2] = sample;
            stereo[i * 2 + 1] = sample;
        }
        size_t written;
        i2s_channel_write(s_speaker, stereo, sizeof(stereo), &written, portMAX_DELAY);
    }
}

static void mic_task(void *arg)
{
    int16_t stereo[AUDIO_FRAME * 2];
    int16_t mono[AUDIO_FRAME];
    for (;;) {
        if (esp_codec_dev_read(s_mic_codec, stereo, sizeof(stereo)) != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        for (size_t i = 0; i < AUDIO_FRAME; i++) {
            mono[i] = ((int32_t)stereo[i * 2] + stereo[i * 2 + 1]) / 2;
        }
        if (s_loopback) {
            xStreamBufferSend(s_play, mono, sizeof(mono), 0);
        } else if (s_mic_cb) {
            s_mic_cb(mono, AUDIO_FRAME);
        }
    }
}

esp_err_t audio_init(audio_mic_cb_t mic_cb)
{
    s_mic_cb = mic_cb;
    s_play = xStreamBufferCreate(PLAY_BUF_BYTES, 1);
    ESP_RETURN_ON_FALSE(s_play, ESP_ERR_NO_MEM, TAG, "play buffer");
    ESP_RETURN_ON_ERROR(board_audio_power(true), TAG, "audio power");
    ESP_RETURN_ON_ERROR(speaker_open(), TAG, "speaker");
    ESP_RETURN_ON_ERROR(microphone_open(), TAG, "microphone");
    xTaskCreate(play_task, "audio_play", 4096, NULL, 20, NULL);
    xTaskCreate(mic_task, "audio_mic", 4096, NULL, 19, NULL);
    ESP_LOGI(TAG, "T-Deck audio running at %d Hz mono", AUDIO_RATE);
    return ESP_OK;
}

void audio_play_push(const int16_t *pcm, size_t samples)
{
    xStreamBufferSend(s_play, pcm, samples * sizeof(int16_t), 0);
}

void audio_play_flush(void)
{
    xStreamBufferReset(s_play);
}

bool audio_is_playing(void)
{
    return s_playing;
}

void audio_set_volume(int percent)
{
    s_volume = percent < 0 ? 0 : percent > 100 ? 100 : percent;
}

void audio_set_mic_gain(int db)
{
    if (s_mic_codec) esp_codec_dev_set_in_gain(s_mic_codec, db);
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