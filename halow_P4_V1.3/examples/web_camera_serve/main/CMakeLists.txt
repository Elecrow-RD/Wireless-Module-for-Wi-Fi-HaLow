idf_component_register(SRCS "p4_main.c" "p4_camera.c" "p4_web.c" "p4_storage.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_event esp_netif nvs_flash esp_http_server
                                     esp_driver_jpeg esp_psram esp_timer esp_hw_support
                                     fatfs sdmmc esp_driver_sdmmc)

