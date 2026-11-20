#include "audio.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "board.h"
#if CONFIG_EL_USB_AUDIO
#include "usb_audio.h"
#endif

static const char *TAG = "audio";

#define PLAY_BUF_BYTES   (AUDIO_RATE * sizeof(int16_t))
#define PLAY_START_BYTES (3 * 4 * AUDIO_FRAME * sizeof(int16_t))

// The ES8388 DAC and the ES7210 microphone ADC share one full-duplex I2S
// port, both stereo: the P4 drives MCLK/BCLK/WS, the ES8388 takes DOUT and
// the ES7210 (MIC1 and MIC2, plain I2S as M5Unified runs it) drives DIN.
// The NS4150B amplifier is switched on the I/O expander.
//
// With CONFIG_EL_USB_AUDIO a USB headset on the USB-A port takes over: while
// it is plugged in, its microphone feeds transmit and its earphones get
// receive audio instead of the built-in microphones and speaker.
static esp_codec_dev_handle_t s_speaker;
static esp_codec_dev_handle_t s_mic;
static StreamBufferHandle_t s_play;
static audio_mic_cb_t s_mic_cb;
static volatile bool s_playing;
static volatile bool s_loopback;

static esp_codec_dev_sample_info_t stream_format(void)
{
    return (esp_codec_dev_sample_info_t){
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = AUDIO_RATE,
        .mclk_multiple = 256,
    };
}

static const audio_codec_ctrl_if_t *codec_ctrl(uint8_t addr)
{
    audio_codec_i2c_cfg_t cfg = {
        .port = I2C_NUM_0,
        .addr = addr << 1,  // esp_codec_dev takes the 8-bit address
        .bus_handle = board_codec_i2c_bus(),
    };
    return audio_codec_new_i2c_ctrl(&cfg);
}

static esp_err_t speaker_open(const audio_codec_data_if_t *data_if, const audio_codec_gpio_if_t *gpio_if)
{
    const audio_codec_ctrl_if_t *ctrl_if = codec_ctrl(BOARD_ES8388_ADDR);
    ESP_RETURN_ON_FALSE(ctrl_if, ESP_FAIL, TAG, "ES8388 interface");
    es8388_codec_cfg_t codec_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,  // the amplifier is switched on the I/O expander
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    const audio_codec_if_t *codec_if = es8388_codec_new(&codec_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "ES8388 codec");
    esp_codec_dev_cfg_t device_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_speaker = esp_codec_dev_new(&device_cfg);
    ESP_RETURN_ON_FALSE(s_speaker, ESP_FAIL, TAG, "ES8388 device");
    esp_codec_dev_sample_info_t fs = stream_format();
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_speaker, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "ES8388 open");
    return ESP_OK;
}

// Called with the speaker open, so MCLK is already running for the ADC.
static esp_err_t microphone_open(const audio_codec_data_if_t *data_if)
{
    const audio_codec_ctrl_if_t *ctrl_if = codec_ctrl(BOARD_ES7210_ADDR);
    ESP_RETURN_ON_FALSE(ctrl_if, ESP_FAIL, TAG, "ES7210 interface");
    es7210_codec_cfg_t codec_cfg = {
        .ctrl_if = ctrl_if,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    const audio_codec_if_t *codec_if = es7210_codec_new(&codec_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "ES7210 codec");
    esp_codec_dev_cfg_t device_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_mic = esp_codec_dev_new(&device_cfg);
    ESP_RETURN_ON_FALSE(s_mic, ESP_FAIL, TAG, "ES7210 device");
    esp_codec_dev_sample_info_t fs = stream_format();
    if (esp_codec_dev_open(s_mic, &fs) != ESP_CODEC_DEV_OK) {
        esp_codec_dev_delete(s_mic);
        s_mic = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t i2s_open(const audio_codec_data_if_t **data_if)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t channel_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_cfg, &tx, &rx), TAG, "I2S channel");
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK,
            .bclk = BOARD_I2S_BCLK,
            .ws = BOARD_I2S_WS,
            .dout = BOARD_I2S_DOUT,
            .din = BOARD_I2S_DIN,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "I2S tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "I2S rx");
    audio_codec_i2s_cfg_t data_cfg = {.port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx};
    *data_if = audio_codec_new_i2s_data(&data_cfg);
    ESP_RETURN_ON_FALSE(*data_if, ESP_FAIL, TAG, "I2S data interface");
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
#if CONFIG_EL_USB_AUDIO
        if (usb_audio_speaker_ready()) {
            // The headset's buffer paces this loop; the built-in speaker
            // falls silent (the I2S channel clears itself).
            if (!usb_audio_write(mono)) vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
#endif
        for (size_t i = 0; i < AUDIO_FRAME; i++) {
            stereo[i * 2] = mono[i];
            stereo[i * 2 + 1] = mono[i];
        }
        // Blocks for ~20 ms; the I2S clock paces this loop.
        esp_codec_dev_write(s_speaker, stereo, sizeof(stereo));
    }
}

static void mic_deliver(const int16_t *mono)
{
    if (s_loopback) {
        xStreamBufferSend(s_play, mono, AUDIO_FRAME * sizeof(int16_t), 0);
    } else if (s_mic_cb) {
        s_mic_cb(mono, AUDIO_FRAME);
    }
}

static void mic_task(void *arg)
{
    int16_t stereo[AUDIO_FRAME * 2];
    int16_t mono[AUDIO_FRAME];
    for (;;) {
#if CONFIG_EL_USB_AUDIO
        if (usb_audio_mic_ready()) {
            if (usb_audio_read(mono)) mic_deliver(mono);
            continue;
        }
#endif
        if (!s_mic || esp_codec_dev_read(s_mic, stereo, sizeof(stereo)) != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        for (size_t i = 0; i < AUDIO_FRAME; i++) {
            mono[i] = ((int32_t)stereo[i * 2] + stereo[i * 2 + 1]) / 2;
        }
        mic_deliver(mono);
    }
}

esp_err_t audio_init(audio_mic_cb_t mic_cb)
{
    s_mic_cb = mic_cb;
    s_play = xStreamBufferCreate(PLAY_BUF_BYTES, 1);
    ESP_RETURN_ON_FALSE(s_play, ESP_ERR_NO_MEM, TAG, "play buffer");

    const audio_codec_data_if_t *data_if;
    ESP_RETURN_ON_ERROR(i2s_open(&data_if), TAG, "I2S");
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if, ESP_FAIL, TAG, "codec gpio");
    ESP_RETURN_ON_ERROR(speaker_open(data_if, gpio_if), TAG, "speaker");
    // Without a microphone the board still receives; transmit is silent
    // unless a USB headset brings one.
    esp_err_t err = microphone_open(data_if);
    if (err != ESP_OK) ESP_LOGW(TAG, "built-in microphone unavailable: %s", esp_err_to_name(err));
    // Amplifier on last, once the DAC is clocked and outputting silence.
    ESP_RETURN_ON_ERROR(board_audio_power(true), TAG, "amplifier");

#if CONFIG_EL_USB_AUDIO
    // Optional: the built-in audio works without it.
    err = usb_audio_init();
    if (err != ESP_OK) ESP_LOGW(TAG, "USB headset support unavailable: %s", esp_err_to_name(err));
#endif

    xTaskCreate(play_task, "audio_play", 4096, NULL, 20, NULL);
    xTaskCreate(mic_task, "audio_mic", 4096, NULL, 19, NULL);
    ESP_LOGI(TAG, "Tab5 audio running at %d Hz mono%s", AUDIO_RATE, s_mic ? "" : " (no built-in mic)");
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
    if (s_speaker) esp_codec_dev_set_out_vol(s_speaker, percent);
#if CONFIG_EL_USB_AUDIO
    usb_audio_set_volume(percent);
#endif
}

void audio_set_mic_gain(int db)
{
    if (s_mic) esp_codec_dev_set_in_gain(s_mic, db);
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
