

#include <endian.h>
#include <string.h>
#include "mmosal.h"
#include "mmwlan.h"
#include "mmipal.h"

#include "mm_app_common.h"

#include "halow_wifi.h"
#include "sd.h"

httpd_handle_t stream_httpd_1  = NULL;
httpd_handle_t stream_httpd_2  = NULL;

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

static int button_state = 1;

typedef struct
{
    size_t size;  // number of values used for filtering
    size_t index; // current value index
    size_t count; // value count
    int sum;
    int *values; // array to be filled with values
} ra_filter_t;

char* get_time_str() {
    time_t now;
    struct tm timeinfo;
    static char time_str[20];
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", &timeinfo);
    return time_str;
}

// 按照给定的路径保存图像数据
esp_err_t save_image_to_sd(camera_fb_t *fb) {
    esp_err_t err;
    const char *FILENAME = "123Halow.jpg";
    char path[50];

    // 生成唯一的文件路径，文件名为 jpgCount.jpg
    snprintf(path, sizeof(path), SD_MOUNT_POINT "/%s", FILENAME);

    // 创建文件
    err = create_file(path);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create file %s", path);
        return ESP_FAIL;
    }

    // 写入图像数据到文件
    err = write_file(path, (char*)fb->buf, fb->len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write to file %s", path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saved photo to %s", path);
    return ESP_OK;
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

const char index_web[] = 
  "<html>"
  "  <head>"
  "    <title>Video Streaming Demonstration</title>"
  "  </head>"
  "  <body>"
  "    <p><h1>Video Streaming Demonstration</h1></p>"
  "    <p><img id=\"stream\" src=\"\" style=\"transform:rotate(180deg)\"/></p>"
  "    <iframe width=0 height=0 frameborder=0 id=\"myiframe\" name=\"myiframe\"></iframe>"
  "    <p><form action=\"/button\" method=\"POST\" target=\"myiframe\"><input type=\"submit\" value=\"Save it to SDcard\"></form></p>"
  "  </body>"
  "  <script>"
  "  document.addEventListener('DOMContentLoaded', function (event) {"
  "    var baseHost = document.location.origin;"
  "    var streamUrl = baseHost + ':81';"
  "    const view = document.getElementById('stream');"
  "    view.src = `${streamUrl}/stream`;"
  "  });"
  "  </script>"
  "</html>";


static esp_err_t index_handler(httpd_req_t *req)
{
  esp_err_t err;
  err = httpd_resp_set_type(req, "text/html");
  sensor_t *s = esp_camera_sensor_get();
  if (s != NULL)
  {
      err = httpd_resp_send(req, (const char *)index_web, sizeof(index_web));
  }
  else
  {
      ESP_LOGE(TAG, "Camera sensor not found");
      err = httpd_resp_send_500(req);
  }
  return err;
}

static esp_err_t button_handler(httpd_req_t *req)
{
  esp_err_t err;
  camera_fb_t * fb = NULL;
  fb = esp_camera_fb_get();
  if (!fb)
  {
      ESP_LOGE(TAG, "Camera capture failed");
      err = ESP_FAIL;
  }
  else
  {
    //String video = "/video";
    //int jpgCount=readFileNum(SD, video.c_str());
    //String path = video + "/" + String(jpgCount) +".jpg";
    //writejpg(SD, path.c_str(), fb->buf, fb->len);
    save_image_to_sd(fb);  // 保存图像到SD卡
    esp_camera_fb_return(fb);
    fb = NULL;
    err=ESP_OK;
  }
  return err;
}

httpd_uri_t uri_get = {
    .uri = "/stream",
    .method = HTTP_GET,
    .handler = jpg_stream_httpd_handler,
    .user_ctx = NULL};

httpd_uri_t button_uri = {
    .uri = "/button",
    .method = HTTP_POST,
    .handler = button_handler,
    .user_ctx = NULL}; 

httpd_uri_t index_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = index_handler,
    .user_ctx = NULL};

void setup_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    if (httpd_start(&stream_httpd_1, &config) == ESP_OK)
    {
        httpd_register_uri_handler(stream_httpd_1, &index_uri);        
        httpd_register_uri_handler(stream_httpd_1, &button_uri);
    }
    config.server_port += 1;
    config.ctrl_port += 1;
    if (httpd_start(&stream_httpd_2 , &config) == ESP_OK)
    {
        httpd_register_uri_handler(stream_httpd_2 , &uri_get);
    }

}

void halow_init()
{
    app_wlan_init();
}


void HalowWiFi()
{
    esp_err_t err;
    printf("\n\nWeb Camera Serve Demo (Built " __DATE__ " " __TIME__ ")\n\n");

    /* Initialize and connect to WiFi, blocks till connected */
    app_wlan_start();

    if (stream_httpd_1) {
        httpd_stop(stream_httpd_1);  
        stream_httpd_1 = NULL;   
    }
    if (stream_httpd_2) {
        httpd_stop(stream_httpd_2);  
        stream_httpd_2 = NULL;   
    }  

    setup_server();
    ESP_LOGI(TAG, "ESP32 CAM Web Server is up and running\n");
    xTaskCreate(heart_beat_task, "heart_beat_task", 4096, NULL, configMAX_PRIORITIES - 5, &heart_beat_handle);
    // while(1) {
    //     app_wlan_arp_send();
    //     vTaskDelay(5000/portTICK_PERIOD_MS); //Tell the router that the device is online.
    // }
}

void stop_stream_server()
{
    if (stream_httpd_1 != NULL) {
        httpd_stop(stream_httpd_1);
        stream_httpd_1 = NULL;
        printf("Stream server 1 stopped successfully\n");
    } else {
        printf("Stream server 1 handle is NULL\n");
    }

    if (stream_httpd_2 != NULL) {
        httpd_stop(stream_httpd_2);
        stream_httpd_2 = NULL;
        printf("Stream server 2 stopped successfully\n");
    } else {
        printf("Stream server 2 handle is NULL\n");
    }
}

void stop_webcam()
{
    
    if (heart_beat_handle) {
        vTaskDelete(heart_beat_handle);
        heart_beat_handle = NULL;
    }


    stop_stream_server();


    // 停止摄像头
    esp_camera_deinit();
    
    app_wlan_stop();
    //mmwlan_deinit();
    ESP_LOGI(TAG, "Web Camera Serve stopped");
}