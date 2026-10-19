set(COMMON_SRCS
     "app_main.c" "net_wifi.c" "settings.c" "el_directory.c" "el_qso.c"
     "console_cmds.c" "lv_mem_psram.c" "server_list.c"
     "ui.c" "ui_wifi.c" "ui_account.c" "ui_home.c" "ui_chat.c"
     "onboard.c" "web_config.c")

if(IDF_TARGET STREQUAL "esp32s3")
     set(PLATFORM_SRCS "board_tdeck.c" "display_tdeck.c" "audio_tdeck.c"
                           "ptt_tdeck.c" "sdcard_tdeck.c")
     set(PLATFORM_REQUIRES esp_adc esp_driver_spi esp_driver_sdspi esp_rom)
elseif(IDF_TARGET STREQUAL "esp32p4")
     set(PLATFORM_SRCS "board.c" "display.c" "audio.c" "ptt.c" "sdcard.c")
     set(PLATFORM_REQUIRES esp_driver_sdmmc)
else()
     message(FATAL_ERROR "EchoLink supports only ESP32-P4 and ESP32-S3 targets")
endif()

idf_component_register(
     SRCS ${COMMON_SRCS} ${PLATFORM_SRCS}
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web/index.html"
    PRIV_REQUIRES gsm esp_driver_i2c esp_driver_i2s esp_driver_gpio
                  esp_netif esp_event esp_wifi nvs_flash console lwip esp_timer
                  esp_lcd esp_lvgl_port lvgl fatfs wear_levelling
                      esp_http_server json sdmmc ${PLATFORM_REQUIRES})
