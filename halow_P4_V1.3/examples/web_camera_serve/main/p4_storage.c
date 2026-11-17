#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/stat.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "p4_camera.h"

static const char *TAG = "p4_storage";
static bool mounted;
#if CONFIG_P4_CAMERA_SD
static sdmmc_card_t *card;
#endif

bool p4_storage_ready(void) { return mounted; }

esp_err_t p4_storage_init(void)
{
#if CONFIG_P4_CAMERA_SD
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    /* The board schematic routes J5 VDD directly to 3.3 V; it is not powered by P4 LDO4. */
    esp_err_t ret = ESP_OK;
    /* J5 exposes CMD, CLK, D0 only. DATA1/2/3 have pullups but no P4 routes. */
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = GPIO_NUM_43;
    slot.cmd = GPIO_NUM_44;
    slot.d0 = GPIO_NUM_39;
    slot.d1 = slot.d2 = slot.d3 = slot.d4 = slot.d5 = slot.d6 = slot.d7 = GPIO_NUM_NC;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    const esp_vfs_fat_sdmmc_mount_config_t config = {
        .format_if_mount_failed = false, .max_files = 3, .allocation_unit_size = 16 * 1024,
    };
    ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &config, &card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD unavailable (%s); preview and browser download still work", esp_err_to_name(ret));
        return ret;
    }
    if (mkdir("/sdcard/photos", 0775) != 0 && errno != EEXIST) {
        esp_vfs_fat_sdcard_unmount("/sdcard", card);
        return ESP_FAIL;
    }
    mounted = true;
    ESP_LOGI(TAG, "SD mounted, 1-bit: CLK=43 CMD=44 D0=39; photos in /photos");
    return ESP_OK;
#else
    ESP_LOGI(TAG, "SD saving disabled; enable it in Elecrow P4 HaLow camera menu if needed");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t p4_storage_save(const uint8_t *data, size_t length, char *path, size_t capacity)
{
    if (!mounted) return ESP_ERR_INVALID_STATE;
    if (!data || !length || !path || capacity < 80) return ESP_ERR_INVALID_ARG;
    /* 8.3 filenames also work with the default FatFS configuration.
     * O_EXCL preserves existing photos across reboots. One HTTP task calls this. */
    static unsigned photo_number;
    FILE *file = NULL;
    while (photo_number < 9999999) {
        snprintf(path, capacity, "/sdcard/photos/P%07u.JPG", ++photo_number);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            break;
        }
        file = fdopen(fd, "wb");
        if (!file) { close(fd); remove(path); }
        break;
    }
    if (!file) return ESP_FAIL;
    bool ok = fwrite(data, 1, length, file) == length;
    if (fclose(file) != 0) ok = false;
    if (!ok) { remove(path); return ESP_FAIL; }
    ESP_LOGI(TAG, "Saved %s (%zu bytes)", path, length);
    return ESP_OK;
}

