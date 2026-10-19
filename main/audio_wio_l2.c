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
#include "board.h"

static const char *TAG = "audio";

#define PLAY_BUF_BYTES   (AUDIO_RATE * sizeof(int16_t))
#define PLAY_START_BYTES (3 * 4 * AUDIO_FRAME * sizeof(int16_t))

// The ES8311 DAC and the ES7243E microphone ADC share one full-duplex I2S
// port: the S3 drives MCLK/BCLK/WS, the ES8311 takes DOUT and the ES7243E
// drives DIN. Each chip gets its own codec device on the shared data path.
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

static esp_err_t speaker_open(const audio_codec_data_if_t *data_if)
{
    const audio_codec_ctrl_if_t *ctrl_if = codec_ctrl(BOARD_ES8311_ADDR);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(ctrl_if && gpio_if, ESP_FAIL, TAG, "ES8311 interfaces");
    es8311_codec_cfg_t codec_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,  // the amplifier is switched on the I/O expander
        .use_mclk = true,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&codec_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "ES8311 codec");
    esp_codec_dev_cfg_t device_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_speaker = esp_codec_dev_new(&device_cfg);
    ESP_RETURN_ON_FALSE(s_speaker, ESP_FAIL, TAG, "ES8311 device");
    esp_codec_dev_sample_info_t fs = stream_format();
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_speaker, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "ES8311 open");
    return ESP_OK;
}

static int find_mic_adc(void)
{
    i2c_master_bus_handle_t bus = board_codec_i2c_bus();
    if (i2c_master_probe(bus, BOARD_ES7243E_ADDR, 50) == ESP_OK) return BOARD_ES7243E_ADDR;
    // The ES7243E's address straps select 0x10-0x17.
    for (uint16_t addr = 0x10; addr <= 0x17; addr++) {
        if (addr != BOARD_ES7243E_ADDR && i2c_master_probe(bus, addr, 50) == ESP_OK) return addr;
    }
    return -1;
}

// Called with the speaker open, so MCLK is already running for the ADC.
static esp_err_t microphone_open(const audio_codec_data_if_t *data_if)
{
    int addr = find_mic_adc();
    ESP_RETURN_ON_FALSE(addr >= 0, ESP_ERR_NOT_FOUND, TAG, "no ES7243E on I2C");
    if (addr != BOARD_ES7243E_ADDR) ESP_LOGW(TAG, "ES7243E answered at 0x%02x", addr);
    const audio_codec_ctrl_if_t *ctrl_if = codec_ctrl(addr);
    ESP_RETURN_ON_FALSE(ctrl_if, ESP_FAIL, TAG, "ES7243E interface");
    es7243e_codec_cfg_t codec_cfg = {.ctrl_if = ctrl_if};
    const audio_codec_if_t *codec_if = es7243e_codec_new(&codec_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "ES7243E codec");
    esp_codec_dev_cfg_t device_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_mic = esp_codec_dev_new(&device_cfg);
    ESP_RETURN_ON_FALSE(s_mic, ESP_FAIL, TAG, "ES7243E device");
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
        for (size_t i = 0; i < AUDIO_FRAME; i++) {
            stereo[i * 2] = mono[i];
            stereo[i * 2 + 1] = mono[i];
        }
        // Blocks for ~20 ms; the I2S clock paces this loop.
        esp_codec_dev_write(s_speaker, stereo, sizeof(stereo));
    }
}

static void mic_task(void *arg)
{
    int16_t stereo[AUDIO_FRAME * 2];
    int16_t mono[AUDIO_FRAME];
    for (;;) {
        if (esp_codec_dev_read(s_mic, stereo, sizeof(stereo)) != ESP_CODEC_DEV_OK) {
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

    const audio_codec_data_if_t *data_if;
    ESP_RETURN_ON_ERROR(i2s_open(&data_if), TAG, "I2S");
    ESP_RETURN_ON_ERROR(speaker_open(data_if), TAG, "speaker");
    // Without a microphone the board still receives; transmit is silent.
    esp_err_t err = microphone_open(data_if);
    if (err != ESP_OK) ESP_LOGW(TAG, "microphone unavailable: %s", esp_err_to_name(err));
    // Amplifier on last, once the DAC is clocked and outputting silence.
    ESP_RETURN_ON_ERROR(board_audio_power(true), TAG, "amplifier");

    xTaskCreate(play_task, "audio_play", 4096, NULL, 20, NULL);
    if (s_mic) xTaskCreate(mic_task, "audio_mic", 4096, NULL, 19, NULL);
    ESP_LOGI(TAG, "Wio Tracker L2 audio running at %d Hz mono%s", AUDIO_RATE, s_mic ? "" : " (no mic)");
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
