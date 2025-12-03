#ifndef _AUDIO_H_
#define _AUDIO_H_

#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include <esp_system.h>
#include "esp_log.h"
#include <esp_system.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MIC_TAG "MIC"
#define MIC_INFO(fmt, ...) ESP_LOGI(MIC_TAG, fmt, ##__VA_ARGS__)

void Audio_Init(void);
void Audio_Channl_Del(void);
esp_err_t audio_ctrl_init(void);
esp_err_t set_Audio_ctrl(bool state);
esp_err_t Audio_play_data(uint8_t *buf, size_t size);
esp_err_t play_example(void);

void mic_init(void);
esp_err_t mic_read_to_audio(size_t rec_seconds);
#endif