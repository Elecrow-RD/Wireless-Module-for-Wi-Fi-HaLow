/*
 * Copyright 2022-2023 Morse Micro
 *
 * This file is licensed under terms that can be found in the LICENSE.md file in the root
 * directory of the Morse Micro IoT SDK software package.
 */

/**
 * @file
 * @brief Throughput measurement using iperf.
 *
 * The Iperf parameters are specified using the defines in the file. Additional defines in
 * @c loadconfig.c and @c mm_app_common.c are used to configure the network stack and WLAN
 * interface.
 *
 * @note It is assumed that you have followed the steps in the @ref GETTING_STARTED guide and are
 * therefore familiar with how to build, flash, and monitor an application using the MM-IoT-SDK
 * framework.
 *
 * This file demonstrates how to run iperf using the Morse Micro WLAN API.
 */


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "halow_wifi.h"
#include "esp_wifi.h"
#include "audio.h"
#include "wifi.h"
#include "ble.h"
#include "uart.h"
#include "sd.h"
#include "esp_heap_caps.h"

#define MAIN_TAG "MAIN"
#define MAIN_INFO(fmt, ...) ESP_LOGI(MAIN_TAG, fmt, ##__VA_ARGS__)

#define Halow_WiFi

static const char *TAG = "DMA_CHECK";
TaskHandle_t uart;
TaskHandle_t audio;
TaskHandle_t mic;
TaskHandle_t auto_test;
TaskHandle_t sd;
TaskHandle_t wifi;
TaskHandle_t halow_wifi_handler;

bool cyc = false;
char test_mode = 0;
static char wifi_ssid[32];
static char wifi_password[64];
int rx_num;
int cur_pos = 0;
static char temp_data[64];

#define MAX_LEN 128
typedef struct
{
    bool work_flag;
    bool audio_flag;
    bool wifi_flag;
    bool bluetooth_flag;
} work_message;
work_message work_status;

bool AT_CWJAP = false;
char data[MAX_LEN];
int status = UART_SCAN;
int Rx_Bytes = 0;


sta_config_t sta_config_;

#define CONFIG_XCLK_FREQ 20000000 


static esp_err_t init_camera(void)
{
    camera_config_t camera_config = {
        .pin_pwdn  = -1,
        .pin_reset = -1,
        .pin_xclk = 10,
        .pin_sccb_sda = 40,
        .pin_sccb_scl = 39,

        .pin_d7 = 48,
        .pin_d6 = 11,
        .pin_d5 = 12,
        .pin_d4 = 14,
        .pin_d3 = 16,
        .pin_d2 = 18,
        .pin_d1 = 17,
        .pin_d0 = 15,
        .pin_vsync = 38,
        .pin_href = 47,
        .pin_pclk = 13,

        .xclk_freq_hz = CONFIG_XCLK_FREQ,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_QVGA,

        .jpeg_quality = 10,
        .fb_count = 2,
        .fb_location = 0,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY};//CAMERA_GRAB_LATEST. Sets when buffers should be filled
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK)
    {
        return err;
    }
    return ESP_OK;
}

void wifi_task(void *param)
{
    wifi_ap_record_t ap_info;

    while (1) {
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            ESP_LOGI(TAG, "Connected SSID: %s, RSSI: %d dBm", ap_info.ssid, ap_info.rssi);
        } else {
            ESP_LOGW(TAG, "Not connected to any AP");
        }

        vTaskDelay(pdMS_TO_TICKS(5000));  // 每 5 秒打印一次
    }
}

void sd_task(void *param)
{
    esp_err_t err = ESP_OK;
    const char *file_hello = SD_MOUNT_POINT "/hello.txt";
    char *data = "hello world!";
    get_sd_card_info();
    while (1)
    {
        err = write_string_file(file_hello, data);
        if (err != ESP_OK)
        {
            printf("write error");
            continue;
        }
        err = read_string_file(file_hello);
        if (err != ESP_OK)
        {
            printf("read error");
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        //SendData("Exit SD test");
        vTaskDelete(NULL);
    }
}

void mic_task(void *param)
{
    esp_err_t err = ESP_OK;
    while (1)
    {
        work_status.audio_flag = false;
        err = mic_read_to_audio(5);
        // if (err != ESP_OK)
        //     MAIN_ERROR("play audio for mic read: [ %s ]", esp_err_to_name(err));
        work_status.audio_flag = true;
        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}

void audio_task(void *param)
{
    esp_err_t err = ESP_OK;
    while (1)
    {
        work_status.audio_flag = false;

        err = play_example();
        // if (err != ESP_OK)
        //     MAIN_ERROR("play audio: [ %s ]", esp_err_to_name(err));
        work_status.audio_flag = true;
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
}

void auto_test_task(void *param)
{
    esp_err_t err = ESP_OK;
    while (1)
    {
        work_status.audio_flag = false;
        if(test_mode == 1)
        {
            Audio_Init();
            vTaskDelay(500 / portTICK_PERIOD_MS);
            printf("start auto test\n");
            play_example();
            set_Audio_ctrl(false);
            vTaskDelay(2000/portTICK_PERIOD_MS);
            printf("Exit Audio test\n");
            mic_read_to_audio(5);
            vTaskDelay(3000/portTICK_PERIOD_MS);
            printf("Exit Mic Test\n");
            Audio_Channl_Del();
            vTaskDelay(3000/portTICK_PERIOD_MS);
            //bt_init();
            printf("Start Bluetooth Test\n");
            start_Adv();
            vTaskDelay(5000/portTICK_PERIOD_MS);
            ble_disconnect();
            
            vTaskDelay(500 / portTICK_PERIOD_MS);
            stop_Adv();
            printf("Exit Bluetooth Test\n");
            vTaskDelay(3000/portTICK_PERIOD_MS);
            HalowWiFi();
            vTaskDelay(10000/portTICK_PERIOD_MS);
            stop_webcam();
        }

    //     else if(test_mode == 2)
    //     {    

    //         while (1) {
    //             rx_num = uart_read_bytes(UART_NUM_0, &temp_data[cur_pos], 1, portMAX_DELAY);
    //             printf("rec data:%c\n", temp_data[cur_pos]);
    //             if ('\n' == temp_data[cur_pos]) {

    //                 char *pos = strstr(temp_data, "ifi_ssid:");

    //                 temp_data[cur_pos] = '\0';

    //                 strncpy(wifi_ssid, pos + strlen("wifi_ssid:"), sizeof(wifi_ssid));

    //                 break;
    //             }

    //             cur_pos += rx_num;

    //         }
            
    //         cur_pos = 0;
    //         while (1) {
    //             rx_num = uart_read_bytes(UART_NUM_0, &temp_data[cur_pos], 1, portMAX_DELAY);
    //             printf("rec data:%c\n", temp_data[cur_pos]);
    //             if ('\n' == temp_data[cur_pos]) {
    //                 char *pos = strstr(temp_data, "ifi_password:");
    //                 temp_data[cur_pos] = '\0';
    //                 strncpy(wifi_password, pos + strlen("ifi_password:"), sizeof(wifi_password));
    //                 break;
    //             }
    //             cur_pos += rx_num;
    //         }

    //         printf("wifi_ssid:%s\n", wifi_ssid);
    //         printf("wifi_password:%s\n", wifi_password);
    //         // bsp_wifi_ap_sta_deinit();
    //         bsp_wifi_connect(wifi_ssid, wifi_password);
    //         while (WIFI_CONNECTING == bsp_wifi_get_state()) {
    //             vTaskDelay(100 / portTICK_PERIOD_MS);
    //         }

    //         printf("Entry wifi test");
    //         xTaskCreate(wifi_task, "sd", 4096, NULL, configMAX_PRIORITIES - 5, &wifi);
    //         vTaskDelay(12000/ portTICK_PERIOD_MS);
    //         vTaskDelete(wifi);
    //         bsp_wifi_disconnect();
    //         printf("Exit wifi test\n");
    //         vTaskDelay(1000 / portTICK_PERIOD_MS);
    //         printf("Entry sd test\n");
    //         xTaskCreate(sd_task, "sd", 4096, NULL, configMAX_PRIORITIES - 5, &sd);

    //     }
        work_status.audio_flag = true;
    //     vTaskDelay(5000 / portTICK_PERIOD_MS);
        
        if(cyc == false)
        {
            printf("auto test end\n");
            break;
        }
    }
    vTaskDelete(NULL);
}

void halow_task(void *pvParameters)
{
    while(1)
    {
        if(AT_CWJAP)
        {
            AT_CWJAP = false;
            printf("halow wifi start\n");
            HalowWiFi();  
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}


void parse_at_cwjap(const char *cmd)
{
    // 格式: AT+CWJAP="SSID","PASSWORD"
    const char *p1 = strchr(cmd, '"');
    if (!p1) return;
    const char *p2 = strchr(p1 + 1, '"');
    if (!p2) return;
    const char *p3 = strchr(p2 + 1, '"');
    if (!p3) return;
    const char *p4 = strchr(p3 + 1, '"');
    if (!p4) return;

    int ssid_len = p2 - p1 - 1;
    int pass_len = p4 - p3 - 1;

    if (ssid_len <= 0 || pass_len <= 0) return;
    if (ssid_len >= sizeof(sta_config_.ssid)) ssid_len = sizeof(sta_config_.ssid) - 1;
    if (pass_len >= sizeof(sta_config_.passphrase)) pass_len = sizeof(sta_config_.passphrase) - 1;

    strncpy(sta_config_.ssid, p1 + 1, ssid_len);
    sta_config_.ssid[ssid_len] = '\0';
    sta_config_.ssid_len = ssid_len;

    strncpy(sta_config_.passphrase, p3 + 1, pass_len);
    sta_config_.passphrase[pass_len] = '\0';
    sta_config_.passphrase_len = pass_len;

    printf("Parsed SSID: %s\n", sta_config_.ssid);
    printf("Parsed PASS: %s\n", sta_config_.passphrase);

    if (halow_wifi_handler) {
        vTaskDelete(halow_wifi_handler);
        halow_wifi_handler = NULL;
    }
    xTaskCreate(halow_task, "halow", 4096, NULL, configMAX_PRIORITIES - 5, &halow_wifi_handler);
    AT_CWJAP = true;
}

#define UART_BUF_SIZE 128
char uart_buf[UART_BUF_SIZE];
int uart_idx = 0;
void uart_task(void *param)
{
    int Rx_Bytes = 0;
    char *data = (char *)malloc(512+1);
    uart_state status = UART_SCAN;
    esp_err_t err;
    while(1)
    {
        switch(status)
        {
        case UART_SCAN:
            // Rx_Bytes = uart_read_bytes(UART_NUM_0, data, 1, portMAX_DELAY);
            // printf("UART SCAN:%s\n",data);
            // if (Rx_Bytes >= 1)
            // {
            //     data[Rx_Bytes] = 0;
            //     status = UART_DECODE;
            // }
            // else if (Rx_Bytes == -1)
            //     status = UART_ERR;
                uint8_t byte;
                Rx_Bytes = uart_read_bytes(UART_NUM_0, &byte, 1, 10 / portTICK_PERIOD_MS);
                if (Rx_Bytes > 0) {
                    if (byte == ';') { 
                        uart_buf[uart_idx] = '\0';
                        status = UART_DECODE;
                        uart_idx = 0; 
                    } else {
                        if (uart_idx < UART_BUF_SIZE - 1) {
                            uart_buf[uart_idx++] = byte;
                        }
                    }
                } 
            break;
        case UART_DECODE:

            if (strncmp(uart_buf, "AT+CWJAP=", 9) == 0)
            {
               
                parse_at_cwjap(uart_buf);
            }
            status = UART_SCAN;
            #if 0
            switch (data[0])
            {

            case 'T':
            {
                work_status.work_flag = true;
                MAIN_INFO("Entry test");
                #ifdef Halow_WiFi
                printf("Mode1\n");
                test_mode = 1;
                #endif

                #ifdef WiFi
                printf("Mode2\n");
                test_mode = 2;
                #endif
                status = UART_SCAN;
                break;
            }
            case 't':
            {
                if (work_status.work_flag)
                {
                    work_status.work_flag = false;
                    test_mode = 0;
                    MAIN_INFO("Exit test");
                }
                status = UART_SCAN;
                break;
            }
            case 'A':
            {
                if (work_status.work_flag)
                {
                    MAIN_INFO("Entry audio test");
                    Audio_Init();
                    vTaskDelay(500 / portTICK_PERIOD_MS);
                    xTaskCreate(audio_task, "audio", 4096, NULL, configMAX_PRIORITIES - 5, &audio);
                }
                status = UART_SCAN;
                break;
            }
            case 'a':
            {
                if (work_status.work_flag)
                {
                    set_Audio_ctrl(false);
                    while (!(work_status.audio_flag))
                    {
                        vTaskDelay(10 / portTICK_PERIOD_MS);
                    }
                    
                    
                    Audio_Channl_Del();
                    vTaskDelete(audio);
                    MAIN_INFO("Exit audio test");
                }
                status = UART_SCAN;
                break;
            }
            case 'I':
            {
                if (work_status.work_flag)
                {
                    MAIN_INFO("Entry mic test");
                    Audio_Init();
                    vTaskDelay(500 / portTICK_PERIOD_MS);
                    xTaskCreate(mic_task, "mic", 4096, NULL, configMAX_PRIORITIES - 5, &mic);
                }
                status = UART_SCAN;
                break;
            }
            case 'i':
            {
                if (work_status.work_flag)
                {
                    while (!(work_status.audio_flag))
                    {
                        vTaskDelay(10 / portTICK_PERIOD_MS);
                    }
                    set_Audio_ctrl(false);
                    Audio_Channl_Del();
                    vTaskDelete(mic);
                    SendData("Exit mic test");
                }
                status = UART_SCAN;
                break;
            }
            case 'B':
            {
                if (work_status.work_flag)
                {
                    MAIN_INFO("Entry bluetooth test");
                    err = start_Adv();
                    
                    //bt_init();
                    // xTaskCreate(lora_task, "sx1262", 4096, NULL, configMAX_PRIORITIES - 5, &lora);
                }
                status = UART_SCAN;
                break;
            }
            case 'b':
            {
                if (work_status.work_flag)
                {
                    // while (!(work_status.lora_flag))
                    // {
                    //     vTaskDelay(10 / portTICK_PERIOD_MS);
                    // }
                    // vTaskDelete(lora);
                    ble_disconnect();
                    vTaskDelay(500 / portTICK_PERIOD_MS);
                    stop_Adv();
                    
                    MAIN_INFO("Exit bluetooth test");
                }
                status = UART_SCAN;
                break;
            }
            case 'W':
            {
                if (work_status.work_flag)
                {


                    
                    while (1) {
                        rx_num = uart_read_bytes(UART_NUM_0, &temp_data[cur_pos], 1, portMAX_DELAY);

                        if ('\n' == temp_data[cur_pos]) {

                            char *pos = strstr(temp_data, "wifi_ssid:");

                            temp_data[cur_pos] = '\0';

                            strncpy(wifi_ssid, pos + strlen("wifi_ssid:"), sizeof(wifi_ssid));
                            MAIN_INFO("Entry wifi test25");
                            break;
                        }

                        cur_pos += rx_num;

                    }
                    
                    cur_pos = 0;
                    while (1) {
                        rx_num = uart_read_bytes(UART_NUM_0, &temp_data[cur_pos], 1, portMAX_DELAY);
                        if ('\n' == temp_data[cur_pos]) {
                            char *pos = strstr(temp_data, "wifi_password:");
                            temp_data[cur_pos] = '\0';
                            strncpy(wifi_password, pos + strlen("wifi_password:"), sizeof(wifi_password));
                            break;
                        }
                        cur_pos += rx_num;
                    }

                    MAIN_INFO("wifi_ssid:%s", wifi_ssid);
                    MAIN_INFO("wifi_password:%s", wifi_password);
                    // bsp_wifi_ap_sta_deinit();
                    bsp_wifi_connect(wifi_ssid, wifi_password);
                    while (WIFI_CONNECTING == bsp_wifi_get_state()) {
                        vTaskDelay(100 / portTICK_PERIOD_MS);
                    }

                    MAIN_INFO("Entry wifi test");
                    xTaskCreate(wifi_task, "sd", 4096, NULL, configMAX_PRIORITIES - 5, &wifi);
                }
                status = UART_SCAN;
                break;
            }
            case 'w':
            {
                if (work_status.work_flag)
                {
                    // while (!(work_status.lora_flag))
                    // {
                    //     vTaskDelay(10 / portTICK_PERIOD_MS);
                    // }
                    // vTaskDelete(lora);
                    vTaskDelete(wifi);
                    bsp_wifi_disconnect();
                    while (WIFI_DISCONNECTED != bsp_wifi_get_state()) {
                        vTaskDelay(100 / portTICK_PERIOD_MS);
                    }
                    MAIN_INFO("Exit wifi test");
                }
                status = UART_SCAN;
                break;
            }
            case 'H':
            {
                esp_event_loop_create_default(); //httpd use event loop
                err = init_camera();
                if (err != ESP_OK)
                {
                    printf("err: %s\n", esp_err_to_name(err));
                    return;
                }
                HalowWiFi();
                MAIN_INFO("Entry Halow wifi test");
                status = UART_SCAN;
                break;
            }
            case 'h':
            {
                stop_webcam();
                MAIN_INFO("Stop Halow wifi test");
                status = UART_SCAN;
                break;
            }
            case 'S':
            {
                if (work_status.work_flag)
                {

                    MAIN_INFO("Entry SD test");
                    MAIN_INFO("SD CLK = GPIO_NUM_7");
                    MAIN_INFO("SD CMD = GPIO_NUM_9");
                    MAIN_INFO("SD D0 = GPIO_NUM_8");
                    xTaskCreate(sd_task, "sd", 4096, NULL, configMAX_PRIORITIES - 5, &sd);
                    status = UART_SCAN;
                    break;
                }
            }
            case 's':
            {
                vTaskDelete(sd);
                printf("Exit sd test\n");
                status = UART_SCAN;
                break;
            }
            case 'N':
            {
                printf("Entry auto test\n");
                xTaskCreate(auto_test_task, "auto", 4096, NULL, configMAX_PRIORITIES - 5, &auto_test);
                status = UART_SCAN;
                break;
            }
            case 'n':
            {
                //vTaskDelete(auto_test);
                set_Audio_ctrl(false);
                Audio_Channl_Del();
                ble_disconnect();
                
                vTaskDelay(500 / portTICK_PERIOD_MS);
                stop_Adv();
                stop_webcam();
                status = UART_SCAN;
                break;
                
            }
            case 'G':
            {
                printf("press test\n");
                if (work_status.work_flag)
                {
                    printf("Entry press test\n");
                    cyc = true;
                    xTaskCreate(auto_test_task, "auto", 4096, NULL, configMAX_PRIORITIES - 5, &auto_test);
                    status = UART_SCAN;
                    break;
                }
            }
            case 'g':
            {
                //vTaskDelete(wifi);
                vTaskDelete(auto_test);
                set_Audio_ctrl(false);
                Audio_Channl_Del();
                ble_disconnect();
                
                vTaskDelay(500 / portTICK_PERIOD_MS);
                stop_Adv();
                stop_webcam();
                printf("Exit press test\n");
                cyc = false;
            }
            default:
                MAIN_INFO("read data is invalid arg: %c", data[0]);
                status = UART_SCAN;
                break;
            }
        #endif
            break;
        
        case UART_ERR:
            MAIN_INFO("read data error");
            status = UART_SCAN;
            break;
        }
    }
    
    free(data);
}


/**
 * Main entry point to the application. This will be invoked in a thread once operating system
 * and hardware initialization has completed. It may return, but it does not have to.
 */
void app_main(void)
{
    esp_err_t err;
    
    audio_ctrl_init();  
    set_Audio_ctrl(false);
#ifdef Halow_WiFi
    app_wlan_init();
#endif


#ifdef WiFi
    sd_init();
    bsp_wifi_init();
    bsp_wifi_sta_init();
#endif
    //bt_init();  
    


    //mic_init();
    vTaskDelay(500 / portTICK_PERIOD_MS);
    

    vTaskDelay(10/portTICK_PERIOD_MS);
    esp_event_loop_create_default(); //httpd use event loop
    err = init_camera();
    if (err != ESP_OK)
    {
        printf("err: %s\n", esp_err_to_name(err));
        return;
    }
    //HalowWiFi();
#if 0
#ifdef HALOW_WIFI
    
#endif  

#ifdef AUDIO_EXAMPLE
    Audio_Init();
    vTaskDelay(500 / portTICK_PERIOD_MS);
    set_Audio_ctrl(false);
    play_example();
#endif

#ifdef MIC
    Audio_Init();
    mic_init();

    mic_read_to_audio(5);
#endif

#ifdef WIFI
    char * ssid = "yanfa_software";
    char * password = "yanfa-123456";

    bsp_wifi_init();
    bsp_wifi_sta_init();

    bsp_wifi_connect(ssid, password);
    while (WIFI_CONNECTED != bsp_wifi_get_state()) {
        printf(".");
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    printf("WiFi Connected\n");

#endif

#ifdef BLE
    
    bt_init();
#endif
#endif

    uart_init();
    xTaskCreate(uart_task, "uart", 4096, NULL, configMAX_PRIORITIES - 4, &uart);
    // xTaskCreate(halow_task, "halow", 4096, NULL, configMAX_PRIORITIES - 5, &halow_wifi_handler);
    // while(1)
    // {
    //     if(AT_CWJAP)
    //     {
    //         AT_CWJAP = false;
    //         printf("halow wifi start\n");
    //         HalowWiFi();
    //     }
    // }

    //

}
