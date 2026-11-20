#pragma once

// Board support for the LilyGO T-Display-P4, M5Stack Tab5, LilyGO T-Deck Plus,
// Seeed Wio Tracker L2 and M5Stack CardputerZero (Linux) targets. CONFIG_EL_BOARD_* selects the board.

#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_err.h"
#if !CONFIG_EL_LINUX
#include "driver/i2c_master.h"
#endif
#if CONFIG_IDF_TARGET_ESP32S3
#include "driver/spi_master.h"
#endif

#if CONFIG_EL_BOARD_CARDPUTER_ZERO
// M5Stack CardputerZero: a Raspberry Pi CM0 running Debian. The hardware is
// the OS's (framebuffer, evdev keyboard, PulseAudio, NetworkManager); see
// linux/src/.
#define BOARD_NAME           "M5Stack CardputerZero"
#define BOARD_HOSTNAME       "scheff-echolink-cp0"
#define BOARD_AP_SSID_PREFIX "Scheff-EchoLink-CP0"
#define BOARD_PTT_HINT       "the Space bar"
#elif CONFIG_EL_BOARD_TDISPLAY_P4
#define BOARD_NAME           "LilyGO T-Display-P4"
#define BOARD_HOSTNAME       "scheff-echolink-p4"
#define BOARD_AP_SSID_PREFIX "Scheff-EchoLink-P4"
#define BOARD_PTT_HINT       "the BOOT button"

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
#elif CONFIG_EL_BOARD_TAB5
// M5Stack Tab5. Pins follow M5Stack's Tab5 BSP, M5Unified, scheff-zello and
// monster-cyberspace.
#define BOARD_NAME           "M5Stack Tab5"
#define BOARD_HOSTNAME       "scheff-echolink-tab5"
#define BOARD_AP_SSID_PREFIX "Scheff-EchoLink-Tab5"
#define BOARD_PTT_HINT       "the on-screen Talk button"

// One I2C bus: PI4IOE5V6408 expanders (0x43, 0x44), INA226 battery monitor
// (0x41), ES8388 DAC (0x10), ES7210 mic ADC (0x40), and the panel's touch
// controller (0x55, or a GT911 at 0x14/0x5D beside the ILI9881C panel).
#define BOARD_I2C_SDA 31
#define BOARD_I2C_SCL 32

// Touch interrupt, pulled up while the touch controller comes out of reset.
#define BOARD_TOUCH_INT 23

// LCD backlight, PWM.
#define BOARD_LCD_BL 22

// ES8388 DAC (into the NS4150B speaker amp) and ES7210 mic ADC share I2S0.
#define BOARD_I2S_MCLK 30
#define BOARD_I2S_BCLK 27
#define BOARD_I2S_WS   29
#define BOARD_I2S_DOUT 26  // P4 -> ES8388
#define BOARD_I2S_DIN  28  // ES7210 -> P4
#define BOARD_ES8388_ADDR 0x10
#define BOARD_ES7210_ADDR 0x40

// microSD on SDMMC slot 0, 4-bit (IO MUX pins CLK 43, CMD 44, D0-D3 39-42),
// as on the T-Display-P4. There is no talk button: BOARD_BOOT_BUTTON stays
// undefined and talking is on the screen (or a USB headset's mic).
#elif CONFIG_EL_BOARD_TDECK_PLUS
// LilyGO T-Deck Plus. The Plus uses the original T-Deck peripheral map.
#define BOARD_NAME           "LilyGO T-Deck Plus"
#define BOARD_HOSTNAME       "scheff-echolink-tdeck"
#define BOARD_AP_SSID_PREFIX "Scheff-EchoLink-TDeck"
#define BOARD_PTT_HINT       "the Space bar"

#define BOARD_POWER_ON       10
#define BOARD_I2C_SDA        18
#define BOARD_I2C_SCL         8
#define BOARD_BAT_ADC         4
#define BOARD_TOUCH_INT      16
#define BOARD_KEYBOARD_INT   46
#define BOARD_SDCARD_CS      39
#define BOARD_TFT_CS         12
#define BOARD_RADIO_CS        9
#define BOARD_TFT_DC         11
#define BOARD_TFT_BACKLIGHT  42
#define BOARD_SPI_MOSI       41
#define BOARD_SPI_MISO       38
#define BOARD_SPI_SCLK       40
#define BOARD_SPEAKER_WS      5
#define BOARD_SPEAKER_BCLK    7
#define BOARD_SPEAKER_DOUT    6
#define BOARD_MIC_MCLK       48
#define BOARD_MIC_WS         21
#define BOARD_MIC_BCLK       47
#define BOARD_MIC_DIN        14
#define BOARD_BOOT_BUTTON     0
#define BOARD_SPI_HOST SPI2_HOST
#elif CONFIG_EL_BOARD_WIO_TRACKER_L2
// Seeed Wio Tracker L2. Pins follow Seeed's Meshtastic variant
// (variants/esp32s3/seeed_wio_tracker_L2).
#define BOARD_NAME           "Seeed Wio Tracker L2"
#define BOARD_HOSTNAME       "scheff-echolink-wio-l2"
#define BOARD_AP_SSID_PREFIX "Scheff-EchoLink-WioL2"
#define BOARD_PTT_HINT       "the user button"

// One I2C bus: PCA9555 expander, GT911 touch, LP5814 backlight, ADS1115
// battery ADC, ES8311 DAC and ES7243E microphone ADC. Seeed runs it at
// 100 kHz.
#define BOARD_I2C_SDA        47
#define BOARD_I2C_SCL        48
#define BOARD_I2C_HZ         100000
#define BOARD_EXPANDER_ADDR  0x21
#define BOARD_BACKLIGHT_ADDR 0x2c
#define BOARD_BAT_ADC_ADDR   0x48
#define BOARD_ES8311_ADDR    0x18
#define BOARD_ES7243E_ADDR   0x14

// NV3031B LCD over QSPI. Its reset is on the expander.
#define BOARD_LCD_SPI_HOST   SPI3_HOST
#define BOARD_LCD_SCLK       42
#define BOARD_LCD_IO0        41
#define BOARD_LCD_IO1        40
#define BOARD_LCD_IO2        39
#define BOARD_LCD_IO3        38
#define BOARD_LCD_CS         46

// One I2S bus: ES8311 DAC on DOUT, ES7243E ADC on DIN.
#define BOARD_I2S_MCLK       10
#define BOARD_I2S_BCLK       11
#define BOARD_I2S_WS         12
#define BOARD_I2S_DOUT       16
#define BOARD_I2S_DIN        15

// microSD, 1-bit SDMMC through the GPIO matrix.
#define BOARD_SD_CLK          2
#define BOARD_SD_CMD          3
#define BOARD_SD_D0           1

// SX1262 LoRa chip select, held high so the radio stays off its bus.
#define BOARD_RADIO_CS       21

// User button, active low.
#define BOARD_BOOT_BUTTON     0
#else
#error "No supported board selected (EL_BOARD in menuconfig)"
#endif

esp_err_t board_init(void);

// Speaker power: on the T-Display-P4 the shared OUT_5V audio domain (NS4150B
// speaker amp and the ES8311 analog supply), on the Tab5 the NS4150B amp.
esp_err_t board_audio_power(bool on);

// ESP32-C6 coprocessor: the EN line (active high) on the T-Display-P4, its
// power switch on the Tab5.
esp_err_t board_c6_enable(bool on);

// microSD card supply (XL9535 IO15, active low). A no-op on the Tab5, whose
// slot is always powered.
esp_err_t board_sd_power(bool on);

// 5 V to the Tab5's USB-A host port.
esp_err_t board_usb_host_power(bool on);

// RM69A10 AMOLED and GT9895 touch reset lines (XL9535). false holds the part
// in reset, true releases it.
esp_err_t board_screen_reset_release(bool released);
esp_err_t board_touch_reset_release(bool released);

// Reads the state of charge (0-100%): the BQ27220 gauge on the T-Display-P4,
// an estimate from the pack voltage on the Tab5.
esp_err_t board_battery_percent(uint8_t *percent);

#if !CONFIG_EL_LINUX
// I2C bus 1 (expander, touch) and bus 2 (codec). The Tab5 returns its one
// shared bus for both.
i2c_master_bus_handle_t board_main_i2c_bus(void);
i2c_master_bus_handle_t board_codec_i2c_bus(void);
#endif
