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
#include "nvs_flash.h"
#include "esp_err.h"

#include "halow_wifi.h"
#include "esp_wifi.h"
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

bool AT_CWJAP = true;
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
        .pin_xclk = 47,
        .pin_sccb_sda = 45,
        .pin_sccb_scl = 42,

        .pin_d7 = 38,
        .pin_d6 = 48,
        .pin_d5 = 46,
        .pin_d4 = 18,
        .pin_d3 = 14,
        .pin_d2 = 12,
        .pin_d1 = 13,
        .pin_d0 = 17,
        .pin_vsync = 40,
        .pin_href = 39,
        .pin_pclk = 21,

        .xclk_freq_hz = CONFIG_XCLK_FREQ,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_VGA,

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

void sd_task()
{
    esp_err_t err = ESP_OK;
    const char *file_hello = SD_MOUNT_POINT "/hello.txt";
    char *data = "hello world!";
    //get_sd_card_info();
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


void halow_task(void *pvParameters)
{
    while(1)
    {
        if(AT_CWJAP == false)
        {
            AT_CWJAP = true;
            printf("halow wifi start\n");
            HalowWiFi();  
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

void save_config_to_flash(const wifi_config_t* config) {
    nvs_handle_t nvs_handle;
    nvs_open("wifi_data", NVS_READWRITE, &nvs_handle);

    nvs_set_blob(nvs_handle, "sta_cfg", config, sizeof(wifi_config_t));

    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    printf("WiFi Configuration saved to Flash.\n");
}

bool load_config_from_flash(sta_config_t* config) {
    nvs_handle_t nvs_handle;
    if (nvs_open("wifi_data", NVS_READONLY, &nvs_handle) != ESP_OK) {
        printf("NVS open failed or data not found.\n");
        return false;
    }

    size_t required_size = sizeof(wifi_config_t);
    esp_err_t err = nvs_get_blob(nvs_handle, "sta_cfg", config, &required_size);

    if (err == ESP_OK && required_size == sizeof(wifi_config_t)) {
        printf("WiFi Configuration loaded from Flash.\n");
        printf("Loaded SSID: %s\n", config->ssid);

        nvs_close(nvs_handle);
        return true;
    } else {
        printf("Failed to load configuration (Error: %d).\n", err);
        nvs_close(nvs_handle);
        return false;
    }
}

void parse_at_cwjap(const char *cmd)
{

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
    save_config_to_flash(&sta_config_);

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
uint8_t byte;
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
                
                Rx_Bytes = uart_read_bytes(UART_NUM_0, &byte, 1, 10 / portTICK_PERIOD_MS);
                if (Rx_Bytes > 0) {
                    if (byte == ';') { 
                        uart_buf[uart_idx] = '\0';
                        if (strncmp(uart_buf, "AT+CWJAP=", 9) == 0)
                        {
                            uart_idx = 0; 
                            parse_at_cwjap(uart_buf);
                        }
                        else
                        {
                            printf("Received non-AT data: %s\n", uart_buf);
                            status = UART_DECODE;
                            uart_idx = 0; 
                        }

                    } else {
                        if (uart_idx < UART_BUF_SIZE - 1) {
                            uart_buf[uart_idx++] = byte;
                        }
                    }
                } 
            break;
        case UART_DECODE:       
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
    halow_init();
    bt_init();  
    err = sd_init();
    if(err == ESP_OK)
    {
        create_dir(IMAGE_PATH);
    }

    if (load_config_from_flash(&sta_config_)) {
        printf("Configuration in sta_config_ ready for use.\n");
    } else {
        printf("Using default/fallback Wi-Fi configuration.\n");
    }

    esp_event_loop_create_default(); //httpd use event loop
    err = init_camera();
    if (err != ESP_OK)
    {
        printf("err: %s\n", esp_err_to_name(err));
        return;
    }
    start_Adv();
    uart_init();
    xTaskCreate(uart_task, "uart", 4096, NULL, configMAX_PRIORITIES - 4, &uart);
    xTaskCreate(halow_task, "halow", 4096, NULL, configMAX_PRIORITIES - 5, &halow_wifi_handler);
}
