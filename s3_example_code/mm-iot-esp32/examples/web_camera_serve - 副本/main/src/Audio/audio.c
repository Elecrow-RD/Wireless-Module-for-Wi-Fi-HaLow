



#include "audio.h"
#include "elecrow_audio.h"

static const char *NS4168_TAG = "NS4168";
i2s_chan_handle_t tx_chan;
i2s_chan_handle_t rx_chan;
extern const int16_t audio_data[];

#define I2S_NUM         I2S_NUM_0
#define SAMPLE_RATE     16000
#define I2S_BCK_IO      42      // CLK
#define I2S_WS_IO       -1      // 不需要 LRCK，可以设置 -1
#define I2S_DATA_IO     41      // DATA

#define RECORD_TIME     5       // 秒
#define BUF_SIZE        (SAMPLE_RATE * RECORD_TIME)

static int16_t i2s_buf[BUF_SIZE];

i2s_chan_handle_t get_audio_handle()
{
    return tx_chan;
}

esp_err_t audio_ctrl_init()
{
    esp_err_t err = ESP_OK;
    const gpio_config_t audio_gpio_cofig = {
        .pin_bit_mask = 1ULL << GPIO_NUM_6,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = true,
        .pull_down_en = false,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&audio_gpio_cofig);
    if (err != ESP_OK)
        return err;
    return err;
}

esp_err_t set_Audio_ctrl(bool state)
{
    esp_err_t err = ESP_OK;
    bool status = !state;
    err = gpio_set_level(GPIO_NUM_6, status);
    return err;
}   


esp_err_t Audio_play_data(uint8_t *buf, size_t size)
{
    esp_err_t err = ESP_OK;
    size_t bytes_written = 0;
    set_Audio_ctrl(true);
    err = i2s_channel_write(tx_chan, buf, size, &bytes_written, portMAX_DELAY);
    if (err != ESP_OK)
    {
        return err;
    }
    set_Audio_ctrl(false);
    return err;
}

esp_err_t play_example()
{

    esp_err_t err = ESP_OK;
    uint8_t *write_buf = heap_caps_malloc(sizeof(ELECROW_Audio) * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    for (size_t i = 0, j = 0; i < sizeof(ELECROW_Audio); i += 2, j += 4)
    {
        write_buf[j] = 0;
        write_buf[j + 1] = 0;
        write_buf[j + 2] = ELECROW_Audio[i];
        write_buf[j + 3] = ELECROW_Audio[i + 1];
    }

    err = Audio_play_data(write_buf, sizeof(ELECROW_Audio)*sizeof(int16_t));
    if (err != ESP_OK)
    {
        free(write_buf);
        return err;
    }
    free(write_buf);
    return err;
}

esp_err_t Audio_play_wav_data(uint8_t *buf, size_t size)
{
    esp_err_t err = ESP_OK;
    size_t bytes_written = 0;
    size_t residue_write_size = size - 44;
    set_Audio_ctrl(true);
    vTaskDelay(pdMS_TO_TICKS(500));
    err = i2s_channel_write(tx_chan, (buf + 44), residue_write_size, &bytes_written, portMAX_DELAY);
    if (err != ESP_OK)
    {
        printf("Audio play error,Bytes read: %d", bytes_written);
        return err;
    }
    set_Audio_ctrl(false);
    return err;
}

void Audio_Channl_Del()
{

    esp_err_t err;

    if (tx_chan != NULL) {
        i2s_channel_disable(tx_chan);
    }

    if (tx_chan != NULL) {
        err = i2s_del_channel(tx_chan);
        if (err != ESP_OK) {
            ESP_LOGE("AUDIO", "Failed to delete I2S channel: 0x%x", err);
        } else {
            tx_chan = NULL;
        }
    }

}

void Audio_Init()
{
    esp_err_t err;
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_1,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 256,
        .auto_clear = true,
    };
    err = i2s_new_channel(&chan_cfg, &tx_chan, NULL);
    if (err != ESP_OK)
        return err;
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = 16000,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = 19,
            .ws = 45,
            .dout = 20,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    i2s_channel_init_std_mode(tx_chan, &std_cfg);

    i2s_channel_enable(tx_chan);
}

void mic_init()
{
    esp_err_t err;
    i2s_chan_config_t rx_chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 256,
        .auto_clear = true,
    };
    err = i2s_new_channel(&rx_chan_cfg, NULL, &rx_chan);
    if (err != ESP_OK)
        return err;
    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = {
            .sample_rate_hz = SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .dn_sample_mode = I2S_PDM_DSR_8S,
        },
        /* The data bit-width of PDM mode is fixed to 16 */
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_PDM_SLOT_LEFT,
        },
        .gpio_cfg = {
            .clk = 42,
            .din = 41,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };
    err = i2s_channel_init_pdm_rx_mode(rx_chan, &pdm_rx_cfg);
    if (err != ESP_OK)
        return err;
    err = i2s_channel_enable(rx_chan);
    if (err != ESP_OK)
        return err;
    return err;
}

esp_err_t mic_read_to_audio(size_t rec_seconds)
{
    esp_err_t err = ESP_OK;
    size_t bytes_read = 0;
    size_t bytes_write = 0;
    if (rec_seconds > 60)
    {
        MIC_INFO("Exceeding the maximum recording duration");
        return ESP_FAIL;
    }
    size_t rec_size = rec_seconds * SAMPLE_RATE;
    i2s_chan_handle_t write_handle = get_audio_handle();
    int16_t *read_buf = heap_caps_malloc(rec_size, MALLOC_CAP_SPIRAM);
    if (NULL == read_buf) {
        MIC_INFO("mic read_buf fail to apply");
        return ESP_FAIL;
    }
    memset(read_buf, 0, rec_size);
    int16_t *write_buf = heap_caps_malloc(rec_size * 2, MALLOC_CAP_SPIRAM);
    if (NULL == write_buf) {
        MIC_INFO("mic write_buf fail to apply");
        return ESP_FAIL;
    }
    memset(write_buf, 0, rec_size * 2);
    MIC_INFO("Start Recording %d of audio data", rec_seconds);
    err = i2s_channel_read(rx_chan, read_buf, rec_size, &bytes_read, portMAX_DELAY);
    if (err != ESP_OK)
    {
        MIC_INFO("read mic data fail");
        return err;
    }
    if (bytes_read != rec_size)
    {
        MIC_INFO("read mic data num error");
        return err;
    }

    int32_t data;
    int32_t array_size = rec_size / 2;
    for (size_t i = 0, j = 0; i < array_size; i++, j += 2)
    {
        data = read_buf[i] * 10;
        if (data > 32767)
            data = 32767;
        else if (data < -32768)
            data = -32768;
        write_buf[j] = data;
        write_buf[j + 1] = data;

        // write_buf[j] = read_buf[i];
        // write_buf[j + 1] = read_buf[i];


        // if (0 != read_buf[i]) {
        //     printf("[%d]=%d\r\n", i, read_buf[i]);
        // }
    }
    MIC_INFO("Start play audio data");
    set_Audio_ctrl(true);
    err = i2s_channel_write(write_handle, write_buf, rec_size * 2, &bytes_write, portMAX_DELAY);
    if (err != ESP_OK)
    {
        set_Audio_ctrl(false);
        heap_caps_free(read_buf);
        heap_caps_free(write_buf);
        return err;
    }
    set_Audio_ctrl(false);
    heap_caps_free(read_buf);
    heap_caps_free(write_buf);
    return err;
}

