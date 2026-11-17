#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
esp_err_t p4_camera_init(void);
bool p4_camera_ready(void);
/* Caller owns the returned JPEG copy and must free it. Safe across HTTP servers. */
esp_err_t p4_camera_capture(uint8_t **data, size_t *length);
esp_err_t p4_web_start(void);
esp_err_t p4_storage_init(void);
bool p4_storage_ready(void);
esp_err_t p4_storage_save(const uint8_t *data, size_t length, char *path, size_t capacity);
