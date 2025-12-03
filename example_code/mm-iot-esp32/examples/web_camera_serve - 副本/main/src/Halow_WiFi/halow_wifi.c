

#include <endian.h>
#include <string.h>
#include "mmosal.h"
#include "mmwlan.h"
#include "mmipal.h"

#include "mm_app_common.h"

#include "halow_wifi.h"


httpd_handle_t stream_server;
TaskHandle_t heart_beat_handle;
static const char *TAG = "esp32-cam Webserver";

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

void heart_beat_task(void *param)
{
    esp_err_t err = ESP_OK;
    while (1)
    {
        app_wlan_arp_send();
        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}



esp_err_t jpg_stream_httpd_handler(httpd_req_t *req){
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len;
    uint8_t * _jpg_buf;
    char * part_buf[64];
    static int64_t last_frame = 0;
    if(!last_frame) {
        last_frame = esp_timer_get_time();
    }

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if(res != ESP_OK){
        return res;
    }

    while(true){
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
            break;
        }
        if(fb->format != PIXFORMAT_JPEG){
            bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
            if(!jpeg_converted){
                ESP_LOGE(TAG, "JPEG compression failed");
                esp_camera_fb_return(fb);
                res = ESP_FAIL;
            }
        } else {
            _jpg_buf_len = fb->len;
            _jpg_buf = fb->buf;
        }

        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if(res == ESP_OK){
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);

            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }
        if(fb->format != PIXFORMAT_JPEG){
            free(_jpg_buf);
        }
        esp_camera_fb_return(fb);
        if(res != ESP_OK){
            break;
        }
        int64_t fr_end = esp_timer_get_time();
        int64_t frame_time = fr_end - last_frame;
        last_frame = fr_end;
        frame_time /= 1000;
        ESP_LOGI(TAG, "MJPG: %uKB %ums (%.1ffps)",
            (uint32_t)(_jpg_buf_len/1024),
            (uint32_t)frame_time, 1000.0 / (uint32_t)frame_time);
    }

    last_frame = 0;
    return res;
}

httpd_uri_t uri_get = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = jpg_stream_httpd_handler,
    .user_ctx = NULL};
httpd_handle_t setup_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t stream_httpd  = NULL;

    if (httpd_start(&stream_httpd , &config) == ESP_OK)
    {
        httpd_register_uri_handler(stream_httpd , &uri_get);
    }

    return stream_httpd;
}

void stop_stream_server(httpd_handle_t stream_httpd)
{
    if (stream_httpd != NULL) {
        // 停止HTTP服务器
        httpd_stop(stream_httpd);
        printf("Stream server stopped successfully\n");
    } else {
        printf("Stream server handle is NULL\n");
    }
}

void HalowWiFi()
{
    esp_err_t err;
    printf("\n\nWeb Camera Serve Demo (Built " __DATE__ " " __TIME__ ")\n\n");
    /* Initialize and connect to WiFi, blocks till connected */
    

    app_wlan_start();
    stream_server = setup_server();
    ESP_LOGI(TAG, "ESP32 CAM Web Server is up and running\n");
    xTaskCreate(heart_beat_task, "heart_beat_task", 4096, NULL, configMAX_PRIORITIES - 5, &heart_beat_handle);
    // while(1) {
    //     app_wlan_arp_send();
    //     vTaskDelay(5000/portTICK_PERIOD_MS); //Tell the router that the device is online.
    // }
}

void stop_webcam()
{
    
    if (heart_beat_handle) {
        vTaskDelete(heart_beat_handle);
        heart_beat_handle = NULL;
    }

    if (stream_server) {
        stop_stream_server(stream_server);
        stream_server = NULL;
    }

    // 停止摄像头
    esp_camera_deinit();
    
    app_wlan_stop();
    //mmwlan_deinit();
    ESP_LOGI(TAG, "Web Camera Serve stopped");
}