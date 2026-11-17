#include <stdlib.h>
#include <stdio.h>
#include "sdkconfig.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "p4_camera.h"

static const char *TAG = "p4_web";
static const char page[] =
"<!doctype html><html lang='en'><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32-P4 HaLow Camera</title><style>body{font:16px sans-serif;background:#172331;color:#eef;"
"max-width:900px;margin:32px auto;padding:16px}img{width:100%;background:#000;min-height:120px}"
"button,a{display:inline-block;margin:8px;padding:12px;background:#dceaff;color:#123;border:0;"
"border-radius:6px;text-decoration:none}#status{padding:10px}</style>"
"<h1>ESP32-P4 · SC2336 · HaLow Camera</h1><p id='status'>Checking camera...</p>"
"<img id='video' alt='Camera preview'><div><button id='start'>Start preview</button>"
"<button id='stop'>Stop preview</button><a href='/capture' download='capture.jpg'>Download photo</a><button id='save'>Save to SD card</button></div><p id='saved'></p>"
"<p>Preview speed depends on the HaLow link. Up to 5 fps by default; one preview client at a time.</p>"
"<script>const v=document.querySelector('#video');"
"document.querySelector('#start').onclick=()=>{v.src='http://'+location.hostname+':81/stream?t='+Date.now()};"
"document.querySelector('#stop').onclick=()=>{v.removeAttribute('src')};"
"document.querySelector('#save').onclick=async()=>{try{let r=await fetch('/button',{method:'POST'});"
"document.querySelector('#saved').textContent=await r.text();}catch(e){document.querySelector('#saved').textContent='Save failed';}};"
"async function status(){try{let r=await fetch('/status');let s=await r.json();"
"document.querySelector('#save').disabled=!s.sd_ready;document.querySelector('#status').textContent=s.camera_ready?'Camera ready. Select Start preview.':'No camera image available. Check the device log.';}"
"catch(e){document.querySelector('#status').textContent='Connection to device lost';}}status();setInterval(status,5000);</script></html>";

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char status[128];
    snprintf(status, sizeof(status), "{\"camera_ready\":%s,\"sd_ready\":%s,\"max_fps\":%d}",
             p4_camera_ready() ? "true" : "false", p4_storage_ready() ? "true" : "false", CONFIG_P4_CAMERA_FPS);
    return httpd_resp_sendstr(req, status);
}

static esp_err_t unavailable(httpd_req_t *req)
{
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "No camera frame is available. Check the SC2336 CSI startup log.");
}

static esp_err_t capture_handler(httpd_req_t *req)
{
    uint8_t *jpeg = NULL;
    size_t length = 0;
    if (p4_camera_capture(&jpeg, &length) != ESP_OK) return unavailable(req);
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t ret = httpd_resp_send(req, (const char *)jpeg, length);
    free(jpeg);
    return ret;
}

static esp_err_t save_handler(httpd_req_t *req)
{
    if (!p4_storage_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "SD card unavailable. Insert a FAT-formatted card and restart, or use Download photo.");
    }
    uint8_t *jpeg = NULL;
    size_t length = 0;
    if (p4_camera_capture(&jpeg, &length) != ESP_OK) return unavailable(req);
    char path[96];
    esp_err_t ret = p4_storage_save(jpeg, length, path, sizeof(path));
    free(jpeg);
    if (ret != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save photo");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, path);
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    uint8_t *jpeg = NULL;
    size_t length = 0;
    if (p4_camera_capture(&jpeg, &length) != ESP_OK) return unavailable(req);
    esp_err_t ret = httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=p4frame");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    while (ret == ESP_OK) {
        char header[128];
        int n = snprintf(header, sizeof(header), "\r\n--p4frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n", length);
        ret = httpd_resp_send_chunk(req, header, n);
        if (ret == ESP_OK) ret = httpd_resp_send_chunk(req, (const char *)jpeg, length);
        free(jpeg); jpeg = NULL;
        if (ret != ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(1000 / CONFIG_P4_CAMERA_FPS));
        ret = p4_camera_capture(&jpeg, &length);
    }
    free(jpeg);
    /* A failed stream must close the socket: never send an HTML error mid-MJPEG. */
    return ESP_FAIL;
}

esp_err_t p4_web_start(void)
{
    httpd_handle_t page_server = NULL, stream_server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;
    cfg.send_wait_timeout = 10;
    esp_err_t ret = httpd_start(&page_server, &cfg);
    if (ret != ESP_OK) return ret;
    const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_handler },
        { .uri = "/status", .method = HTTP_GET, .handler = status_handler },
        { .uri = "/capture", .method = HTTP_GET, .handler = capture_handler },
        { .uri = "/button", .method = HTTP_POST, .handler = save_handler },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        ret = httpd_register_uri_handler(page_server, &routes[i]);
        if (ret != ESP_OK) goto failed;
    }
    cfg.server_port = 81;
    cfg.ctrl_port += 1;
    cfg.max_open_sockets = 1;
    ret = httpd_start(&stream_server, &cfg);
    if (ret != ESP_OK) goto failed;
    const httpd_uri_t stream = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler };
    ret = httpd_register_uri_handler(stream_server, &stream);
    if (ret != ESP_OK) goto failed;
    ESP_LOGI(TAG, "HTTP ready: port 80 page/status/snapshot, port 81 MJPEG");
    return ESP_OK;
failed:
    if (stream_server) httpd_stop(stream_server);
    httpd_stop(page_server);
    return ret;
}

