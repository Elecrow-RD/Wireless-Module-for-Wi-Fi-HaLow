#include "wifi.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"

#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_mac.h"

#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#define TAG "WiFi"

EM_WIFI_MODE_STATE wifi_run_state_ = WIFI_RUN_MODE_IDLE;
EM_WIFI_STATE wifi_sta_state_ = WIFI_IDLE;
esp_netif_t *netif_ = NULL;  

void wifi_event_handler(void* arg, esp_event_base_t event_base,int32_t event_id, void* event_data)
{   
    if(event_base == WIFI_EVENT) {
        switch (event_id)
        {
        case WIFI_EVENT_STA_START:      //WIFI以STA模式启动后触发此事件
            // esp_wifi_connect();         //启动WIFI连接
            // instance->wifi_sta_state_ = WIFI_CONNECTING;
            break;
        case WIFI_EVENT_STA_CONNECTED:  //WIFI连上路由器后，触发此事件
            // ESP_LOGI(TAG, "connected to AP successfully");
            wifi_sta_state_ = WIFI_CONNECTED;
            wifi_ap_record_t ap_info;
            esp_wifi_sta_get_ap_info(&ap_info); // 获取当前AP信息
            printf("connected to AP successfully\n");
            printf("Connected RSSI: %d dBm\n", ap_info.rssi);
            break;
        case WIFI_EVENT_STA_DISCONNECTED:   //WIFI从路由器断开连接后触发此事件
            if (WIFI_IDLE != wifi_sta_state_) {
                const wifi_event_sta_disconnected_t* disconn = (const wifi_event_sta_disconnected_t*) event_data;
                ESP_LOGW(TAG, "Disconnected, reason: %d", disconn->reason);
                esp_wifi_connect();             //继续重连
                wifi_sta_state_ = WIFI_DISCONNECTED;
                // ESP_LOGI(TAG,"connect to the AP fail,retry now");
                printf("connect to the AP fail,retry now\n");
            }
            break;


        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t* event1=(wifi_event_ap_staconnected_t*) event_data;
            printf("station " MACSTR " join,AID=%d\n", MAC2STR(event1->mac), event1->aid);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t* event2=(wifi_event_ap_stadisconnected_t*) event_data;
            printf( "station " MACSTR " leave,AID=%d,reason=%d\n", MAC2STR(event2->mac),event2->aid,event2->reason);
            break;
        }
        default:
            break;
        }
    }

    else if(event_base == IP_EVENT)                  //IP相关事件
    {
        switch(event_id)
        {
            case IP_EVENT_STA_GOT_IP:           //只有获取到路由器分配的IP，才认为是连上了路由器
                // ESP_LOGI(TAG,"get ip address");
                // printf("get ip address\n");
                ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
                printf("WiFi connected with IP:" IPSTR "\n", IP2STR(&event->ip_info.ip));
                break;
        }
    }
}

void bsp_wifi_init(void)
{
    ESP_LOGI(TAG, "bsp_wifi_init");
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 初始化基础网络栈
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    //初始化WIFI
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
}

void bsp_wifi_sta_init(void)
{
    ESP_LOGI(TAG, "bsp_wifi_sta_init");
    if (NULL != netif_) {
        return;
    }

    netif_ = esp_netif_create_default_wifi_sta(); //使用默认配置创建STA对象

    //注册事件
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    // 初始化STA模式的默认配置
    wifi_config_t wifi_config = {0};

    // WIFI_PS_NONE        // 无节能
    // WIFI_PS_MIN_MODEM   // 轻度休眠（默认）
    // WIFI_PS_MAX_MODEM   // 深度休眠
    // ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));     //设置wifi功耗为不需要节能

    //启动WIFI
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );         //设置工作模式为STA
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );   //设置wifi配置
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));    //设置wifi配置存储在RAM中
    ESP_ERROR_CHECK(esp_wifi_start());                         //启动WIFI

    wifi_run_state_ = WIFI_RUN_MODE_STA;

    // xTaskCreatePinnedToCore(&PandaTest::WifiTask, "WifiTask", 2048, this, 8, &wifi_task_handle_, 1);
}

void bsp_wifi_connect(char* ssid, char* password)
{
    ESP_ERROR_CHECK(esp_wifi_stop());

    wifi_config_t wifi_config = {0};
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK; // 强制 WPA2

    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid) - 1] = '\0';
    
    strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.password[sizeof(wifi_config.sta.password) - 1] = '\0';

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE)); // 关闭省电模式

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    wifi_sta_state_ = WIFI_CONNECTING;
}

void bsp_wifi_disconnect(void)
{
    esp_wifi_disconnect();
    esp_wifi_stop();
}

EM_WIFI_STATE bsp_wifi_get_state(void)
{
    return wifi_sta_state_;
}
