idf_component_register(
    SRCS "app_main.c" "board.c" "audio.c" "net_wifi.c" "settings.c"
         "el_directory.c" "el_qso.c" "ptt.c" "console_cmds.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES gsm esp_driver_i2c esp_driver_i2s esp_driver_gpio
                  esp_netif esp_event esp_wifi nvs_flash console lwip esp_timer)
