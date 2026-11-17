#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "esp_ldo_regulator.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "linux/videodev2.h"
#include "driver/jpeg_encode.h"
#include "p4_camera.h"

#define BUFFER_COUNT 2
static const char *TAG = "p4_camera";
static int camera_fd = -1;
static uint8_t *buffers[BUFFER_COUNT];
static size_t buffer_sizes[BUFFER_COUNT];
static uint32_t source_width, source_height, source_stride;
static uint32_t output_width, output_height;
static uint8_t *scaled, *encoded, *latest;
static size_t scaled_size, encoded_size, latest_size;
static int64_t latest_time;
static SemaphoreHandle_t frame_lock;
static jpeg_encoder_handle_t encoder;
static bool streaming, video_initialized, running;
/* Match the board's working Lesson13 camera example: CSI/camera rails must
 * be enabled before SCCB probes the sensor. Keep references for app lifetime. */
static esp_ldo_channel_handle_t camera_ldo3;
static esp_ldo_channel_handle_t camera_ldo4;

static void cleanup_capture(void)
{
    if (camera_fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (streaming) ioctl(camera_fd, VIDIOC_STREAMOFF, &type);
        /* MMAP buffers belong to esp_video and are released by close. */
        close(camera_fd);
        camera_fd = -1;
    }
    if (encoder) { jpeg_del_encoder_engine(encoder); encoder = NULL; }
    free(scaled); scaled = NULL;
    free(encoded); encoded = NULL;
    if (video_initialized) { esp_video_deinit(); video_initialized = false; }
    streaming = false;
}

bool p4_camera_ready(void)
{
    if (!frame_lock) return false;
    xSemaphoreTake(frame_lock, portMAX_DELAY);
    bool ready = running && latest_size && esp_timer_get_time() - latest_time < 5000000;
    xSemaphoreGive(frame_lock);
    return ready;
}

esp_err_t p4_camera_capture(uint8_t **data, size_t *length)
{
    if (!data || !length) return ESP_ERR_INVALID_ARG;
    *data = NULL;
    *length = 0;
    if (!frame_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(frame_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (running && latest_size && esp_timer_get_time() - latest_time < 5000000) {
        *data = heap_caps_malloc(latest_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (*data) {
            memcpy(*data, latest, latest_size);
            *length = latest_size;
            ret = ESP_OK;
        } else ret = ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(frame_lock);
    return ret;
}

static void capture_task(void *arg)
{
    const jpeg_encode_cfg_t cfg = {
        .width = output_width, .height = output_height,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = CONFIG_P4_CAMERA_JPEG_QUALITY,
    };
    int64_t next_frame = 0;
    bool first_frame = true;
    unsigned encode_failures = 0;
    while (true) {
        struct v4l2_buffer buf = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
        if (ioctl(camera_fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGE(TAG, "Camera dequeue failed: errno=%d", errno);
            break;
        }
        bool valid = buf.index < BUFFER_COUNT && !(buf.flags & V4L2_BUF_FLAG_ERROR);
        if (valid) valid = buffer_sizes[buf.index] >= source_stride * source_height &&
                           buf.bytesused >= source_stride * source_height;
        const int64_t now = esp_timer_get_time();
        esp_err_t ret = ESP_OK;
        uint32_t jpeg_length = 0;
        const bool encode_due = valid && now >= next_frame;
        if (encode_due) {
            /* Scale before encoding: the proven HaLow fallback currently runs at 1 MHz.
             * Copy each RGB565 pair unchanged, preserving the ISP/encoder byte order. */
            const uint8_t *src = buffers[buf.index];
            for (uint32_t y = 0; y < output_height; ++y) {
                uint32_t sy = y * source_height / output_height;
                for (uint32_t x = 0; x < output_width; ++x) {
                    uint32_t sx = x * source_width / output_width;
                    size_t to = (y * output_width + x) * 2;
                    size_t from = sy * source_stride + sx * 2;
                    scaled[to] = src[from]; scaled[to + 1] = src[from + 1];
                }
            }
            next_frame = now + 1000000 / CONFIG_P4_CAMERA_FPS;
        }
        /* Return the DMA buffer on every path, including encoder failures. */
        if (ioctl(camera_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "Camera requeue failed: errno=%d", errno);
            break;
        }
        if (encode_due) {
            ret = jpeg_encoder_process(encoder, &cfg, scaled, scaled_size,
                                       encoded, encoded_size, &jpeg_length);
        }
        if (ret != ESP_OK) {
            if (++encode_failures <= 3) ESP_LOGE(TAG, "JPEG encode: %s", esp_err_to_name(ret));
            continue;
        }
        if (jpeg_length && jpeg_length <= encoded_size) {
            xSemaphoreTake(frame_lock, portMAX_DELAY);
            memcpy(latest, encoded, jpeg_length);
            latest_size = jpeg_length;
            latest_time = esp_timer_get_time();
            xSemaphoreGive(frame_lock);
            if (first_frame) {
                ESP_LOGI(TAG, "First JPEG ready: %" PRIu32 "x%" PRIu32 ", %" PRIu32 " bytes",
                         output_width, output_height, jpeg_length);
                first_frame = false;
            }
        }
        vTaskDelay(1);
    }
    xSemaphoreTake(frame_lock, portMAX_DELAY);
    running = false;
    latest_size = 0;
    free(latest); latest = NULL;
    xSemaphoreGive(frame_lock);
    cleanup_capture();
    ESP_LOGE(TAG, "Camera stopped; HTTP status stays available. Check camera/CSI configuration.");
    vTaskDelete(NULL);
}

esp_err_t p4_camera_init(void)
{
    ESP_LOGI(TAG, "Camera compatibility v9: esp_video-managed SCCB I2C1; P4 rev1.x; preview max=%d fps, width=%d, JPEG quality=%d",
             CONFIG_P4_CAMERA_FPS, CONFIG_P4_CAMERA_MAX_WIDTH, CONFIG_P4_CAMERA_JPEG_QUALITY);
    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM is required for CSI frame buffers");
        return ESP_ERR_INVALID_STATE;
    }
    frame_lock = xSemaphoreCreateMutex();
    if (!frame_lock) return ESP_ERR_NO_MEM;
    /* Follow the known-working Lesson13-Camera_Real-Time board sequence:
     * acquire the P4 camera/CSI LDO rails before touching SCCB, use I2C1,
     * and leave sensor reset under board hardware control. */
    const esp_ldo_channel_config_t ldo3_cfg = { .chan_id = 3, .voltage_mv = 2500 };
    const esp_ldo_channel_config_t ldo4_cfg = { .chan_id = 4, .voltage_mv = 3300 };
    esp_err_t ret = esp_ldo_acquire_channel(&ldo3_cfg, &camera_ldo3);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera LDO3 (2.5 V) enable failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_ldo_acquire_channel(&ldo4_cfg, &camera_ldo4);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera LDO4 (3.3 V) enable failed: %s", esp_err_to_name(ret));
        esp_ldo_release_channel(camera_ldo3);
        camera_ldo3 = NULL;
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Match the known-working P4 camera implementation: esp_video owns SCCB. */
    const esp_video_init_csi_config_t csi = {
        .sccb_config = { .init_sccb = true,
            .i2c_config = { .port = 1, .sda_pin = CONFIG_P4_CAMERA_SDA, .scl_pin = CONFIG_P4_CAMERA_SCL },
            .freq = 100000 },
        .reset_pin = -1,
        .pwdn_pin = -1,
    };
    const esp_video_init_config_t video = { .csi = &csi };
    ESP_LOGI(TAG, "SC2336 MIPI: SCCB I2C1 SDA=%d SCL=%d, esp_video-managed bus, board RESET control, LDO3=2.5 V LDO4=3.3 V, XCLK=24 MHz",
             CONFIG_P4_CAMERA_SDA, CONFIG_P4_CAMERA_SCL);
    ret = esp_video_init(&video);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init: %s; check SC2336 ribbon orientation and connector", esp_err_to_name(ret));
        return ret;
    }
    video_initialized = true;
    const char *stage = "open CSI device";
    camera_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (camera_fd < 0) goto failed;
    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    stage = "VIDIOC_G_FMT (sensor format)";
    if (ioctl(camera_fd, VIDIOC_G_FMT, &fmt)) goto failed;
    ESP_LOGI(TAG, "Sensor format: %" PRIu32 "x%" PRIu32 ", fourcc=0x%08" PRIx32 ", returned type=%" PRIu32,
             fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.pixelformat, fmt.type);
    /* esp_video 1.4.1 initially returns the stored stream format with type=0.
     * Reusing it directly for S_FMT selects no stream and fails with EINVAL.
     * Match Lesson13: build a fresh capture-format request with the geometry. */
    struct v4l2_format rgb = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix = {
            .width = fmt.fmt.pix.width,
            .height = fmt.fmt.pix.height,
            .pixelformat = V4L2_PIX_FMT_RGB565,
        },
    };
    fmt = rgb;
    stage = "VIDIOC_S_FMT (RGB565)";
    if (ioctl(camera_fd, VIDIOC_S_FMT, &fmt)) goto failed;
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    stage = "VIDIOC_G_FMT (RGB565)";
    if (ioctl(camera_fd, VIDIOC_G_FMT, &fmt)) goto failed;
    stage = "validate RGB565 geometry";
    ret = ESP_ERR_INVALID_SIZE;
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565 || !fmt.fmt.pix.width || !fmt.fmt.pix.height) goto failed;
    source_width = fmt.fmt.pix.width;
    source_height = fmt.fmt.pix.height;
    source_stride = fmt.fmt.pix.bytesperline ? fmt.fmt.pix.bytesperline : source_width * 2;
    if (source_stride < source_width * 2) goto failed;
    output_width = source_width < CONFIG_P4_CAMERA_MAX_WIDTH ? source_width : CONFIG_P4_CAMERA_MAX_WIDTH;
    output_width &= ~15U;
    output_height = (output_width * source_height / source_width) & ~15U;
    if (!output_width || !output_height) goto failed;
    ESP_LOGI(TAG, "CSI capture %" PRIu32 "x%" PRIu32 ", JPEG %" PRIu32 "x%" PRIu32 " <=%d fps",
             source_width, source_height, output_width, output_height, CONFIG_P4_CAMERA_FPS);
    stage = "create JPEG encoder";
    jpeg_encode_engine_cfg_t engine = { .timeout_ms = 1000 };
    ret = jpeg_new_encoder_engine(&engine, &encoder);
    if (ret != ESP_OK) goto failed;
    jpeg_encode_memory_alloc_cfg_t input = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
    jpeg_encode_memory_alloc_cfg_t output = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    scaled = jpeg_alloc_encoder_mem(output_width * output_height * 2, &input, &scaled_size);
    encoded = jpeg_alloc_encoder_mem(output_width * output_height * 3 + 1024, &output, &encoded_size);
    latest = heap_caps_malloc(encoded_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    stage = "allocate JPEG buffers";
    if (!scaled || !encoded || !latest) { ret = ESP_ERR_NO_MEM; goto failed; }
    struct v4l2_requestbuffers req = {
        .count = BUFFER_COUNT, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
    };
    stage = "VIDIOC_REQBUFS";
    if (ioctl(camera_fd, VIDIOC_REQBUFS, &req)) goto failed;
    if (req.count != BUFFER_COUNT) { ret = ESP_ERR_INVALID_SIZE; goto failed; }
    for (int i = 0; i < BUFFER_COUNT; ++i) {
        struct v4l2_buffer buf = { .index = i, .type = req.type, .memory = req.memory };
        stage = "VIDIOC_QUERYBUF";
        if (ioctl(camera_fd, VIDIOC_QUERYBUF, &buf)) goto failed;
        stage = "mmap CSI buffer";
        buffers[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, camera_fd, buf.m.offset);
        if (!buffers[i] || buffers[i] == MAP_FAILED) goto failed;
        buffer_sizes[i] = buf.length;
        stage = "VIDIOC_QBUF";
        if (ioctl(camera_fd, VIDIOC_QBUF, &buf)) goto failed;
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    stage = "VIDIOC_STREAMON";
    if (ioctl(camera_fd, VIDIOC_STREAMON, &type)) goto failed;
    streaming = true;
    ESP_LOGI(TAG, "CSI STREAMON succeeded; waiting for first JPEG");
    running = true;
    stage = "create capture task";
    if (xTaskCreate(capture_task, "camera_capture", 6144, NULL, 5, NULL) != pdPASS) {
        running = false; ret = ESP_ERR_NO_MEM; goto failed;
    }
    /* Catch a detected sensor that never produces CSI frames. Do not reset the
     * board or kill a blocked DQBUF task; networking/status can still diagnose it. */
    for (int i = 0; i < 50 && !p4_camera_ready(); ++i) vTaskDelay(pdMS_TO_TICKS(100));
    if (!p4_camera_ready()) {
        ESP_LOGW(TAG, "No JPEG received within 5 seconds; check MIPI lanes, ribbon and 24 MHz clock");
    }
    return ESP_OK;
failed:
    ESP_LOGE(TAG, "Camera setup failed at %s: %s, errno=%d", stage,
             esp_err_to_name(ret == ESP_OK ? ESP_FAIL : ret), errno);
    cleanup_capture();
    free(latest); latest = NULL;
    return ret == ESP_OK ? ESP_FAIL : ret;
}

