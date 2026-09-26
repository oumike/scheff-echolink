#pragma once

// LilyGO T-Display-P4 (V1.0) board support: the bits this app needs.
// Pin map from lilygo_device_driver/src/device/t_display_p4/t_display_p4_config.h

#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

// I2C bus 1: XL9535 I/O expander, touch, RTC, fuel gauge.
#define BOARD_I2C1_SDA 7
#define BOARD_I2C1_SCL 8
// I2C bus 2: ES8311 codec, IMU, haptics.
#define BOARD_I2C2_SDA 20
#define BOARD_I2C2_SCL 21

// ES8311 I2S (port 0)
#define BOARD_I2S_MCLK 13
#define BOARD_I2S_BCLK 12
#define BOARD_I2S_WS   9
#define BOARD_I2S_DOUT 10  // P4 -> codec DAC
#define BOARD_I2S_DIN  11  // codec ADC -> P4
#define BOARD_ES8311_ADDR 0x18

// BOOT button, active low. Shared with the Ethernet RMII TXD1, so don't use
// it as PTT if Ethernet is ever enabled.
#define BOARD_BOOT_BUTTON 35

esp_err_t board_init(void);

// Shared OUT_5V audio domain: NS4150B speaker amp and the ES8311 analog supply.
esp_err_t board_audio_power(bool on);

// ESP32-C6 coprocessor EN line (active high).
esp_err_t board_c6_enable(bool on);

i2c_master_bus_handle_t board_codec_i2c_bus(void);
