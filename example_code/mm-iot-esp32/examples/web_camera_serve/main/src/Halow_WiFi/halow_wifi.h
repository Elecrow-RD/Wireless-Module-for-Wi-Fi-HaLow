#ifndef _HALOW_WIFI_
#define _HALOW_WIFI_

#include <esp_system.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "esp_timer.h"
#include "esp_http_server.h"

#include "ff.h"

typedef struct {
    char ssid[32];
    int ssid_len;
    char passphrase[64];
    int passphrase_len;
    int security_type;
} sta_config_t;
extern sta_config_t sta_config_;

void HalowWiFi(void);
void halow_init(void);
#endif