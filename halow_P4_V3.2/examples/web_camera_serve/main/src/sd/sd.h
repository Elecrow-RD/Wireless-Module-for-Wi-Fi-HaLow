#ifndef _BSP_SD_H_
#define _BSP_SD_H_

/*————————————————————————————————————————Header file declaration————————————————————————————————————————*/
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
/*——————————————————————————————————————Header file declaration end——————————————————————————————————————*/

/*——————————————————————————————————————————Variable declaration—————————————————————————————————————————*/
#define SD_TAG "SD_CARD"
#define SD_INFO(fmt, ...) ESP_LOGI(SD_TAG, fmt, ##__VA_ARGS__)
#define SD_DEBUG(fmt, ...) ESP_LOGD(SD_TAG, fmt, ##__VA_ARGS__)
#define SD_ERROR(fmt, ...) ESP_LOGE(SD_TAG, fmt, ##__VA_ARGS__)



#define EXAMPLE_MAX_CHAR_SIZE 64
#define SD_MOUNT_POINT "/sdcard"
#define IMAGE_PATH SD_MOUNT_POINT "/image"
#define SD_GPIO_MISO CONFIG_SD_GPIO_MISO
#define SD_GPIO_MOSI CONFIG_SD_GPIO_MOSI
#define SD_GPIO_CLK CONFIG_SD_GPIO_CLK

esp_err_t create_file(const char *filename);
esp_err_t create_dir(const char * path);
esp_err_t write_string_file(const char *filename, char *data);
esp_err_t read_string_file(const char *filename);
esp_err_t write_file(const char *filename, char *data, size_t size);
esp_err_t write_file_seek(const char *filename, void *data, size_t size, int32_t seek);
esp_err_t read_file(const char *filename, char *data, size_t size);
esp_err_t read_file_size(const char *filename);
int read_file_num(const char *dir_path);
void get_sd_card_info(void);
esp_err_t format_sd_card();
esp_err_t sd_init();


/*———————————————————————————————————————Variable declaration end——————————————-—————————————————————————*/
#endif
