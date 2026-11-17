#ifndef _WIFI_H_
#define _WIFI_H_

typedef enum {
    WIFI_RUN_MODE_IDLE,
    WIFI_RUN_MODE_STA,
    WIFI_RUN_MODE_AP,
    WIFI_RUN_MODE_IS_DEINIT,
} EM_WIFI_MODE_STATE;

typedef enum {
    WIFI_IDLE,
    WIFI_CONNECTING,
    WIFI_CONNECTED,
    WIFI_DISCONNECTED,
} EM_WIFI_STATE;

void bsp_wifi_init(void);

void bsp_wifi_sta_init(void);
void bsp_wifi_connect(char* ssid, char* password);
void bsp_wifi_disconnect(void);
EM_WIFI_STATE bsp_wifi_get_state(void);
#endif