set(src
    "src/Halow_WiFi/mm_app_loadconfig.c"
    "src/Halow_WiFi/mm_app_common.c"
    "src/Halow_WiFi/halow_wifi.c"
    "src/Bluetooth/ble.c"
    "src/Audio/audio.c"
    "src/WiFi/wifi.c"
    src/Uart/uart.c
    src/sd/sd.c
    "src/main.c")

set(inc
    "src")

idf_component_register(SRCS ${src}
                       INCLUDE_DIRS ${inc} src/Audio src/Halow_WiFi src/WiFi src/Bluetooth src/Uart src/sd
                       PRIV_REQUIRES driver esp_wifi nvs_flash bt morselib mm_shims mmipal lwip esp_timer esp_http_server fatfs)
