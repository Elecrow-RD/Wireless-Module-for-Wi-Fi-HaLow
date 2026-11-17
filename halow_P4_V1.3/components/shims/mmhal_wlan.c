/*
 * Copyright 2021-2023 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "mmhal_wlan.h"
#include "mmosal.h"
#include "mmutils.h"

#include "esp_system.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_mac.h"
#include "driver/spi_master.h"
#include "driver/spi_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef CONFIG_MM_SPI_GPIO_PROBE
#include "esp_rom_sys.h"
#endif

static const char *TAG = "MMHaLow";

/**
 * The number of bytes to send as part of @ref mmhal_wlan_send_training_seq for SD over SPI.
 * This must be at least 74 bits (>= 10 bytes), see section 6.4.1.1 of the SD spec
 * "Physical Layer Simplified Specification Version 9.10" for more detail.
 */
#define BYTE_TRAIN 16

MM_STATIC_ASSERT((BYTE_TRAIN >= 10), "BYTE_TRAIN must be at least 10.");

/** SPI hw interrupt handler. Must be set before enabling irq */
static mmhal_irq_handler_t spi_irq_handler = NULL;

#ifdef CONFIG_HALOW_PS_MODE
/** busy interrupt handler. Must be set before enabling irq */
static mmhal_irq_handler_t busy_irq_handler = NULL;
#endif /* CONFIG_HALOW_PS_MODE */

static spi_device_handle_t spi_handle;

#ifdef CONFIG_MM_SPI_STARTUP_PROBE
static uint32_t probe_clock_hz = CONFIG_MM_SPI_CLOCK_MHZ * 1000000;
static uint32_t probe_settle_ms = CONFIG_MM_RESET_SETTLE_MS;
static unsigned probe_spi_errors;
static bool probe_capture;
static uint8_t probe_rx[16];
static unsigned probe_rx_count;
#endif

#ifdef CONFIG_MM_SPI_RX_ALIGNMENT_TRIAL
static bool probe_rx_alignment;
static uint8_t probe_rx_previous = 0xff;
static uint8_t probe_alignment_raw[24];
static unsigned probe_alignment_raw_count;

/* Receive-only stream realignment. This is independently implemented here;
 * the vendor Linux SPI driver is the evidence for testing a one-bit offset,
 * not evidence that its controller-specific workaround applies to this P4.
 * Keep the last raw bit across API calls until chip select is released. */
static uint8_t wlan_hal_align_rx_byte(uint8_t raw, uint8_t *previous)
{
    uint8_t aligned = (uint8_t)(((*previous & 1U) << 7) | (raw >> 1));
    *previous = raw;
    return aligned;
}
#endif

#ifdef CONFIG_MM_SPI_GPIO_PROBE
static bool probe_gpio_active;

/* Slow mode-0 comparison: change MOSI while SCK is low, then sample MISO
 * while SCK is high. No DMA, SPI peripheral, response shifting or bypasses.
 * This is only used for bounded startup and chip-ID reads, never RF traffic. */
static uint8_t wlan_hal_gpio_rw(uint8_t tx)
{
    uint8_t rx = 0;
    for (unsigned bit = 0; bit < 8; bit++)
    {
        gpio_set_level(CONFIG_MM_SPI_MOSI, (tx & 0x80) != 0);
        esp_rom_delay_us(5);
        gpio_set_level(CONFIG_MM_SPI_SCK, 1);
        esp_rom_delay_us(5);
        rx = (uint8_t)((rx << 1) | gpio_get_level(CONFIG_MM_SPI_MISO));
        gpio_set_level(CONFIG_MM_SPI_SCK, 0);
        tx <<= 1;
    }
    return rx;
}

static esp_err_t wlan_hal_gpio_probe_begin(void)
{
    gpio_set_level(CONFIG_MM_SPI_CS, 1);
    gpio_set_level(CONFIG_MM_RESET_N, 0);
    esp_err_t ret = spi_bus_remove_device(spi_handle);
    if (ret != ESP_OK)
    {
        return ret;
    }
    spi_handle = NULL;
    ret = spi_bus_free(SPI2_HOST);
    if (ret != ESP_OK)
    {
        return ret;
    }
    /* Select GPIO rather than the peripheral output signal on these pins. */
    const gpio_num_t pins[] = {CONFIG_MM_SPI_SCK, CONFIG_MM_SPI_MOSI, CONFIG_MM_SPI_MISO};
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++)
    {
        ret = gpio_reset_pin(pins[i]);
        if (ret != ESP_OK)
        {
            return ret;
        }
    }
    gpio_set_level(CONFIG_MM_SPI_SCK, 0);
    gpio_set_level(CONFIG_MM_SPI_MOSI, 1);
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << CONFIG_MM_SPI_SCK) | (1ULL << CONFIG_MM_SPI_MOSI),
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&out);
    if (ret != ESP_OK)
    {
        return ret;
    }
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << CONFIG_MM_SPI_MISO,
        .mode = GPIO_MODE_INPUT,
#ifdef CONFIG_MM_SPI_MISO_PULLUP
        .pull_up_en = GPIO_PULLUP_ENABLE,
#endif
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&in);
    if (ret == ESP_OK)
    {
        probe_gpio_active = true;
    }
    return ret;
}
#endif

static void wlan_hal_gpio_init(void)
{
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = 1ull << CONFIG_MM_SPI_CS;
#ifdef CONFIG_HALOW_PS_MODE
    io_conf.pin_bit_mask |= 1ull << CONFIG_MM_WAKE;
#endif /* CONFIG_HALOW_PS_MODE */
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);

#ifdef CONFIG_HALOW_PS_MODE
    gpio_set_level(CONFIG_MM_WAKE, 0);
#endif /* CONFIG_HALOW_PS_MODE */
    gpio_set_level(CONFIG_MM_SPI_CS, 0);

#ifdef CONFIG_HALOW_PS_MODE
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ull << CONFIG_MM_BUSY);
    io_conf.pull_down_en = 1;
    gpio_config(&io_conf);
#endif /* CONFIG_HALOW_PS_MODE */

    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ull << CONFIG_MM_SPI_IRQ);
    io_conf.pull_down_en = 0;
    gpio_config(&io_conf);
}

static void wlan_hal_spi_init(void)
{
    esp_err_t ret;

    spi_bus_config_t buscfg = {
        .miso_io_num = CONFIG_MM_SPI_MISO,
        .mosi_io_num = CONFIG_MM_SPI_MOSI,
        .sclk_io_num = CONFIG_MM_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        /* max_transfer_sz defaults to 4092 if 0 when DMA enabled, or to SOC_SPI_MAXIMUM_BUFFER_SIZE
         * if DMA is disabled. */
        .max_transfer_sz = 0,
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };
    ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "spi_bus_initialize failed\n");
    }

#ifdef CONFIG_MM_SPI_MISO_PULLUP
    ESP_ERROR_CHECK(gpio_set_pull_mode(CONFIG_MM_SPI_MISO, GPIO_PULLUP_ONLY));
    ESP_LOGI(TAG, "MISO weak pull-up enabled: comparing with v6; ACK/CRC checks unchanged");
#endif

    /* Keep the upstream 40 MHz default, but allow board-specific timing diagnostics. */
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = CONFIG_MM_SPI_CLOCK_MHZ * 1000000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    ret = spi_bus_add_device(SPI2_HOST, &dev_cfg, &spi_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "spi_bus_add_device failed\n");
    }

    /* The actual clock frequency may not be the one that was set as it is re-calculated by the
     * driver to the nearest hardware-compatible number. Importantly it is the "nearest", so it
     * could be above the value set. */
    int actual_freq_khz = 0;
    spi_device_get_actual_freq(spi_handle, &actual_freq_khz);
    ESP_LOGI(TAG, "HaLow SPI requested %d MHz, actual %d kHz; SCK=%d MOSI=%d MISO=%d CS=%d IRQ=%d",
             CONFIG_MM_SPI_CLOCK_MHZ, actual_freq_khz, CONFIG_MM_SPI_SCK,
             CONFIG_MM_SPI_MOSI, CONFIG_MM_SPI_MISO, CONFIG_MM_SPI_CS, CONFIG_MM_SPI_IRQ);
}

static void wlan_hal_spi_deinit(void)
{
#ifdef CONFIG_MM_SPI_GPIO_PROBE
    if (spi_handle == NULL)
    {
        return;
    }
#endif
    esp_err_t ret = spi_bus_remove_device(spi_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "spi_bus_remove_device failed\n");
    }

    ret = spi_bus_free(SPI2_HOST);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "spi_bus_initialize failed\n");
    }
}

/**
 * Minium transfer length in bytes before interrupt based transactions are used. This is because
 * there is some setup time associated with using the interrupt based method when compared to the
 * polling method. In the cases where the difference in setup time exceeds the transaction duration
 * it is more efficient to uses the polling method instead of the interrupt based one. The below
 * equation was used to calculate this.
 *
 * (DMA_TRANSACTION_DURATION - POLL_TRANSACTION_DURATION) / (8/SPI_FREQ)
 *
 * The typical duration for the ESP32 can be found in the [transaction
 * duration](https://docs.espressif.com/projects/esp-idf/en/v5.1.1/esp32s3/api-reference/peripherals/spi_master.html#transaction-duration)
 * section of the docs.
 */
#define INTERRUPT_TRANSFER_MIN_LENGTH 75

static void spi_master_rw_raw(const uint8_t *w_data, uint8_t *r_data, size_t len)
{
#ifdef CONFIG_MM_SPI_GPIO_PROBE
    if (probe_gpio_active)
    {
        for (size_t i = 0; i < len; i++)
        {
            uint8_t rx = wlan_hal_gpio_rw(w_data != NULL ? w_data[i] : 0xff);
            if (r_data != NULL)
            {
                r_data[i] = rx;
            }
        }
        return;
    }
#endif
    spi_transaction_t trans_desc = {
        .rx_buffer = r_data,
        .tx_buffer = w_data,
        .length = (len * 8),
        .flags = 0,
#ifdef CONFIG_MM_SPI_STARTUP_PROBE
        .override_freq_hz = probe_clock_hz,
#endif
    };

    esp_err_t err;
    if (len < INTERRUPT_TRANSFER_MIN_LENGTH)
    {
        err = spi_device_polling_transmit(spi_handle, &trans_desc);
    }
    else
    {
        err = spi_device_transmit(spi_handle, &trans_desc);
    }

    if (err != ESP_OK)
    {
        printf("SPI rw error = %x\n", err);
#ifdef CONFIG_MM_SPI_STARTUP_PROBE
        probe_spi_errors++;
        if (r_data != NULL)
        {
            memset(r_data, 0xff, len);
        }
#endif
    }
}

static void spi_master_rw(const uint8_t *w_data, uint8_t *r_data, size_t len)
{
#ifdef CONFIG_MM_SPI_RX_ALIGNMENT_TRIAL
    if (probe_rx_alignment)
    {
        /* Capture every raw bit, including TX-only calls. SDK command, data
         * token, payload and CRC reads may use separate transfer calls.
         * Word-aligned scratch buffers also avoid short DMA RX overruns. */
        uint32_t tx_words[32];
        uint32_t rx_words[32];
        uint8_t *tx = (uint8_t *)tx_words;
        uint8_t *rx = (uint8_t *)rx_words;
        while (len > 0)
        {
            size_t count = len < sizeof(tx_words) ? len : sizeof(tx_words);
            if (w_data != NULL)
            {
                memcpy(tx, w_data, count);
            }
            else
            {
                memset(tx, 0xff, count);
            }
            spi_master_rw_raw(tx, rx, count);
            for (size_t i = 0; i < count; i++)
            {
                if (probe_capture && probe_alignment_raw_count < sizeof(probe_alignment_raw))
                {
                    probe_alignment_raw[probe_alignment_raw_count++] = rx[i];
                }
                uint8_t aligned = wlan_hal_align_rx_byte(rx[i], &probe_rx_previous);
                if (r_data != NULL)
                {
                    r_data[i] = aligned;
                }
            }
            if (w_data != NULL)
            {
                w_data += count;
            }
            if (r_data != NULL)
            {
                r_data += count;
            }
            len -= count;
        }
        return;
    }
#endif
    spi_master_rw_raw(w_data, r_data, len);
}

/* Reset timing is a minimum wall-clock duration, not just a scheduler yield.
 * mmosal_task_sleep(5) truncates to zero ticks at the default 100 Hz tick rate.
 * Round up and add one tick to cover entering just before a tick boundary.
 * Keep this local so general driver task scheduling is unchanged.
 */
static void wlan_hal_reset_delay_ms(uint32_t duration_ms)
{
    TickType_t ticks = (duration_ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS;
    vTaskDelay(ticks + 1);
}

void mmhal_wlan_hard_reset(void)
{
#ifdef CONFIG_MM_SPI_STARTUP_PROBE
    /* Keep the device deselected throughout reset and boot. */
    gpio_set_level(CONFIG_MM_SPI_CS, 1);
#endif
    gpio_set_level(CONFIG_MM_RESET_N, 0);
    wlan_hal_reset_delay_ms(5);
    gpio_set_level(CONFIG_MM_RESET_N, 1);
#ifdef CONFIG_MM_SPI_STARTUP_PROBE
    wlan_hal_reset_delay_ms(probe_settle_ms);
#else
    wlan_hal_reset_delay_ms(CONFIG_MM_RESET_SETTLE_MS);
#endif
    ESP_LOGI(TAG, "HaLow reset: minimum low=5 ms, minimum settle=%d ms; IRQ level=%d",
#ifdef CONFIG_MM_SPI_STARTUP_PROBE
             (int)probe_settle_ms, gpio_get_level(CONFIG_MM_SPI_IRQ));
#else
             CONFIG_MM_RESET_SETTLE_MS, gpio_get_level(CONFIG_MM_SPI_IRQ));
#endif
}

void mmhal_wlan_spi_cs_assert(void)
{
    gpio_set_level(CONFIG_MM_SPI_CS, 0);
}

void mmhal_wlan_spi_cs_deassert(void)
{
    gpio_set_level(CONFIG_MM_SPI_CS, 1);
#ifdef CONFIG_MM_SPI_RX_ALIGNMENT_TRIAL
    /* Only a real deassert ends the stream; repeated asserts do not. */
    probe_rx_previous = 0xff;
#endif
}

uint8_t mmhal_wlan_spi_rw(uint8_t data)
{
    uint8_t readval = 0xff;
    spi_master_rw(&data, &readval, 1);
#ifdef CONFIG_MM_SPI_STARTUP_PROBE
    if (probe_capture)
    {
        if (probe_rx_count < sizeof(probe_rx))
        {
            probe_rx[probe_rx_count] = readval;
        }
        probe_rx_count++;
    }
#endif
    return readval;
}

void mmhal_wlan_spi_read_buf(uint8_t *buf, unsigned len)
{
    spi_master_rw(NULL, buf, len);
}

void mmhal_wlan_spi_write_buf(const uint8_t *buf, unsigned len)
{
    spi_master_rw(buf, NULL, len);
}

void mmhal_wlan_send_training_seq(void)
{
    mmhal_wlan_spi_cs_deassert();
    /* Send >74 clock pulses to card to stabilize CLK.
     * This method of stacking up the TX data is described in RM0090 rev 19 Figure 253.
     * It results is a reduction in the time between bytes of ~85% (316ns -> 48ns).
     * Could not get this to work for the other transactions however.
     */
    uint8_t buf[BYTE_TRAIN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    spi_master_rw(buf, NULL, BYTE_TRAIN);
}

#ifdef CONFIG_MM_SPI_STARTUP_PROBE
/* Override only the SDK's weak startup hook. Command framing, response
 * validation, chip-ID validation and all data transfers remain in the SDK. */
static int wlan_hal_probe_cmd(uint8_t cmd)
{
    probe_rx_count = 0;
#ifdef CONFIG_MM_SPI_RX_ALIGNMENT_TRIAL
    probe_alignment_raw_count = 0;
#endif
    probe_capture = true;
    int ret = mmhal_wlan_sdio_cmd(cmd, 0, NULL);
    probe_capture = false;
    unsigned captured = probe_rx_count < sizeof(probe_rx) ?
                        probe_rx_count : sizeof(probe_rx);
    ESP_LOGI(TAG, "Probe CMD%u: result=%d, RX bytes=%u; first %u bytes below",
             (unsigned)cmd, ret, probe_rx_count, captured);
    if (captured > 0)
    {
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, probe_rx, captured, ESP_LOG_INFO);
    }
#ifdef CONFIG_MM_SPI_RX_ALIGNMENT_TRIAL
    if (probe_rx_alignment && probe_alignment_raw_count > 0)
    {
        ESP_LOGI(TAG, "ALIGN raw RX from transfer start (includes command TX); decoded polling bytes above:");
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, probe_alignment_raw, probe_alignment_raw_count, ESP_LOG_INFO);
    }
#endif
    return ret;
}

#ifdef CONFIG_MM_SPI_GPIO_PROBE
/* Register/window definitions match the SDK porting_assistant's MM6108
 * chip_cfg.c and sdio_spi.c. Only address-window control registers are
 * written; chip ID is read using the SDK's CMD53 and data CRC validation. */
#define PROBE_MM6108_CHIP_ID_REG 0x10054d20UL
#define PROBE_ADDRESS_WINDOW_0  0x10000UL
#define PROBE_ACCESS_4BYTE      2

static int wlan_hal_probe_chip_id(uint32_t *chip_id)
{
    const uint8_t window[] = {
        (uint8_t)(PROBE_MM6108_CHIP_ID_REG >> 16),
        (uint8_t)(PROBE_MM6108_CHIP_ID_REG >> 24),
        PROBE_ACCESS_4BYTE,
    };
    for (unsigned i = 0; i < sizeof(window); i++)
    {
        uint32_t arg = mmhal_make_cmd52_arg(MMHAL_SDIO_WRITE, MMHAL_SDIO_FUNCTION_1,
                                            PROBE_ADDRESS_WINDOW_0 + i, window[i]);
        int ret = mmhal_wlan_sdio_cmd(52, arg, NULL);
        if (ret != 0)
        {
            return ret;
        }
    }
    uint32_t aligned_data = 0;
    struct mmhal_wlan_sdio_cmd53_read_args args = {
        .sdio_arg = mmhal_make_cmd53_arg(MMHAL_SDIO_READ, MMHAL_SDIO_FUNCTION_1,
                        MMHAL_SDIO_MODE_BYTE, PROBE_MM6108_CHIP_ID_REG & 0xffff, 4),
        .data = (uint8_t *)&aligned_data,
        .transfer_length = 4,
        .block_size = 0,
    };
    int ret = mmhal_wlan_sdio_cmd53_read(&args);
    if (ret == 0)
    {
        const uint8_t *bytes = (const uint8_t *)&aligned_data;
        *chip_id = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
                   ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
    }
    return ret;
}

#ifdef CONFIG_MM_SPI_RX_ALIGNMENT_TRIAL
/* Exercise the actual C transform before any corrected bus traffic.
 * Include a known FE 01 -> FF 00 pattern and exhaustive carry/byte checks. */
static bool wlan_hal_alignment_selftest(void)
{
    const uint8_t raw[] = {0xff, 0xfe, 0x01, 0xff};
    const uint8_t expected[] = {0xff, 0xff, 0x00, 0xff};
    uint8_t previous = 0xff;
    for (unsigned i = 0; i < sizeof(raw); i++)
    {
        if (wlan_hal_align_rx_byte(raw[i], &previous) != expected[i])
        {
            return false;
        }
    }
    for (unsigned carry = 0; carry < 2; carry++)
    {
        for (unsigned byte = 0; byte < 256; byte++)
        {
            previous = (uint8_t)carry;
            uint8_t want = (uint8_t)(carry * 128 + byte / 2);
            if (wlan_hal_align_rx_byte((uint8_t)byte, &previous) != want || previous != byte)
            {
                return false;
            }
        }
    }
    return true;
}

static bool wlan_hal_probe_rx_alignment(void)
{
    if (!wlan_hal_alignment_selftest())
    {
        ESP_LOGE(TAG, "ALIGN self-test failed; correction remains disabled");
        return false;
    }
    ESP_LOGW(TAG, "ALIGN TRIAL: self-test passed; experimental one-bit RX offset, 1 MHz, unchanged ACK/data CRC checks");
    probe_clock_hz = 1000000;
    probe_settle_ms = CONFIG_MM_RESET_SETTLE_MS > 200 ? CONFIG_MM_RESET_SETTLE_MS : 200;
    probe_rx_alignment = true;
    uint32_t reference_id = 0;
    for (unsigned boot = 1; boot <= 2; boot++)
    {
        probe_spi_errors = 0;
        probe_rx_previous = 0xff;
        mmhal_wlan_hard_reset();
        mmhal_wlan_send_training_seq();
        int ret = MMHAL_SDIO_OTHER_ERROR;
        for (unsigned attempt = 0; attempt < 3; attempt++)
        {
            ret = wlan_hal_probe_cmd(63);
            if (ret == 0)
            {
                break;
            }
            (void)wlan_hal_probe_cmd(0);
        }
        if (ret != 0 || probe_spi_errors != 0)
        {
            ESP_LOGE(TAG, "ALIGN boot %u: handshake failed, status=%d, SPI errors=%u",
                     boot, ret, probe_spi_errors);
            goto fail;
        }
        unsigned consecutive = 0;
        for (unsigned attempt = 1; attempt <= 4 && consecutive < 2; attempt++)
        {
            uint32_t id = 0;
            ret = wlan_hal_probe_chip_id(&id);
            bool valid = ret == 0 && probe_spi_errors == 0 &&
                         (id == 0x206 || id == 0x306 || id == 0x406);
            if (valid && reference_id == 0)
            {
                reference_id = id;
            }
            valid = valid && id == reference_id;
            consecutive = valid ? consecutive + 1 : 0;
            ESP_LOGI(TAG, "ALIGN boot %u ID read %u: status=%d id=0x%08lx CRC/ID valid=%d consecutive=%u",
                     boot, attempt, ret, (unsigned long)id, valid, consecutive);
        }
        if (consecutive < 2)
        {
            ESP_LOGE(TAG, "ALIGN boot %u: repeatable chip ID/CRC gate failed", boot);
            goto fail;
        }
    }
    ESP_LOGW(TAG, "ALIGN VALIDATED: same chip ID with CRC twice on each of two resets; retain 1 MHz RX correction and continue SDK boot. Scan is not yet validated.");
    return true;

fail:
    probe_rx_alignment = false;
    mmhal_wlan_spi_cs_deassert();
    ESP_LOGE(TAG, "ALIGN REJECTED: correction disabled; no firmware load/scan on this trial");
    return false;
}
#endif

/* These fixed-length raw captures are control experiments, not SDIO commands
 * accepted by the driver. Use identical bytes in all three control states.
 * Never infer a working device or shift/accept an ACK from these samples. */
static void wlan_hal_probe_raw_cmd(const char *phase, uint8_t cmd, bool selected)
{
    const uint8_t tx[6] = {(uint8_t)(0x40 | cmd), 0, 0, 0, 0, 0xff};
    uint8_t during_tx[sizeof(tx)];
    uint8_t after_tx[16];

    gpio_set_level(CONFIG_MM_SPI_CS, selected ? 0 : 1);
    esp_rom_delay_us(5);
    /* Match the ready-poll clock byte seen before CMD0 in the field log.
     * This raw experiment records it; it does not wait for or accept ready. */
    int ready = cmd == 0 ? wlan_hal_gpio_rw(0xff) : -1;
    int reset_pad = gpio_get_level(CONFIG_MM_RESET_N);
    int cs_pad = gpio_get_level(CONFIG_MM_SPI_CS);
    for (unsigned i = 0; i < sizeof(tx); i++)
    {
        during_tx[i] = wlan_hal_gpio_rw(tx[i]);
    }
    for (unsigned i = 0; i < sizeof(after_tx); i++)
    {
        after_tx[i] = wlan_hal_gpio_rw(0xff);
    }
    gpio_set_level(CONFIG_MM_SPI_CS, 1);

    ESP_LOGW(TAG, "CONTROL %s CMD%u: P4 pad RESET=%d CS=%d, ready=%d (not polled=-1)",
             phase, (unsigned)cmd, reset_pad, cs_pad, ready);
    ESP_LOGI(TAG, "CONTROL TX command (6 bytes):");
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, tx, sizeof(tx), ESP_LOG_INFO);
    ESP_LOGI(TAG, "CONTROL MISO during command (6 bytes):");
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, during_tx, sizeof(during_tx), ESP_LOG_INFO);
    ESP_LOGI(TAG, "CONTROL MISO after command (16 bytes; raw, not ACK validation):");
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, after_tx, sizeof(after_tx), ESP_LOG_INFO);
}

static void wlan_hal_probe_controls(void)
{
    const gpio_num_t outputs[] = {CONFIG_MM_RESET_N, CONFIG_MM_SPI_CS};
    for (unsigned i = 0; i < sizeof(outputs) / sizeof(outputs[0]); i++)
    {
        /* Preserve output drive and enable the P4 pad input for readback.
         * This is host-side evidence only, not a module-side measurement. */
        esp_err_t err = gpio_input_enable(outputs[i]);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "CONTROL PROBE unavailable: GPIO%d input enable: %s",
                     (int)outputs[i], esp_err_to_name(err));
            return;
        }
    }

    ESP_LOGW(TAG, "CONTROL PROBE: baseline vs RESET held low vs CS held high; GPIO mode 0, no RF");
    ESP_LOGW(TAG, "P4 pad readback cannot verify module-side continuity, supply or timing.");
    const struct { const char *name; bool reset_held; bool selected; } cases[] = {
        {"BASELINE", false, true},
        {"RESET_HELD_LOW", true, true},
        {"CS_HELD_HIGH", false, false},
    };
    const uint8_t cmds[] = {63, 0, 63};
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
    {
        gpio_set_level(CONFIG_MM_SPI_CS, 1);
        if (cases[c].reset_held)
        {
            gpio_set_level(CONFIG_MM_RESET_N, 0);
            wlan_hal_reset_delay_ms(20);
        }
        else
        {
            mmhal_wlan_hard_reset();
        }
        mmhal_wlan_send_training_seq();
        ESP_LOGW(TAG, "CONTROL %s: commanded RESET=%d, CS during commands=%d",
                 cases[c].name, cases[c].reset_held ? 0 : 1, cases[c].selected ? 0 : 1);
        for (unsigned i = 0; i < sizeof(cmds); i++)
        {
            wlan_hal_probe_raw_cmd(cases[c].name, cmds[i], cases[c].selected);
        }
    }
    ESP_LOGW(TAG, "CONTROL PROBE complete: compare the three captures; no PASS or hardware-fault verdict is inferred.");
}

/* A host-pad comparison, not a continuity or voltage measurement. Keep MISO
 * input-only, CS high and SCK low; vary only MOSI and weak input resistors.
 * No command clocks or module register writes occur in this experiment. */
static void wlan_hal_probe_miso_bias(void)
{
    gpio_set_level(CONFIG_MM_SPI_CS, 1);
    gpio_set_level(CONFIG_MM_SPI_SCK, 0);
    gpio_set_level(CONFIG_MM_SPI_MOSI, 1);
    mmhal_wlan_hard_reset();
    esp_err_t err = gpio_input_enable(CONFIG_MM_SPI_MOSI);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "BIAS PROBE unavailable: MOSI readback: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGW(TAG, "BIAS PROBE: CS=1 SCK=0 RESET=1, no clocks, MISO stays input-only");
    gpio_dump_io_configuration(stdout, (1ULL << CONFIG_MM_SPI_MOSI) |
                               (1ULL << CONFIG_MM_SPI_MISO));
    const struct { const char *name; gpio_pull_mode_t pull; } cases[] = {
        {"FLOAT", GPIO_FLOATING},
        {"PULLUP", GPIO_PULLUP_ONLY},
        {"PULLDOWN", GPIO_PULLDOWN_ONLY},
    };
    const unsigned levels[] = {0, 1, 0, 1};
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
    {
        err = gpio_set_pull_mode(CONFIG_MM_SPI_MISO, cases[c].pull);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "BIAS %s unavailable: %s", cases[c].name, esp_err_to_name(err));
            continue;
        }
        for (unsigned i = 0; i < sizeof(levels) / sizeof(levels[0]); i++)
        {
            gpio_set_level(CONFIG_MM_SPI_MOSI, levels[i]);
            esp_rom_delay_us(20);
            int early = gpio_get_level(CONFIG_MM_SPI_MISO);
            /* Allow a weakly biased pad to settle before repeated sampling. */
            esp_rom_delay_us(2000);
            unsigned high_count = 0;
            for (unsigned sample = 0; sample < 16; sample++)
            {
                high_count += gpio_get_level(CONFIG_MM_SPI_MISO);
                esp_rom_delay_us(20);
            }
            ESP_LOGW(TAG, "BIAS %s step=%u: MOSI set=%u pad=%d, MISO at20us=%d, after2ms high=%u/16",
                     cases[c].name, i + 1, levels[i],
                     gpio_get_level(CONFIG_MM_SPI_MOSI), early, high_count);
        }
    }
#ifdef CONFIG_MM_SPI_MISO_PULLUP
    err = gpio_set_pull_mode(CONFIG_MM_SPI_MISO, GPIO_PULLUP_ONLY);
#else
    err = gpio_set_pull_mode(CONFIG_MM_SPI_MISO, GPIO_FLOATING);
#endif
    gpio_set_level(CONFIG_MM_SPI_MOSI, 1);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "BIAS PROBE could not restore MISO bias: %s", esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "BIAS PROBE complete: host-pad observations only; no short/open/supply verdict");
}

static void wlan_hal_probe_gpio(void)
{
    ESP_LOGW(TAG, "GPIO SPI PROBE: same pins, mode 0, 5 us minimum half-period; diagnostic only");
    esp_err_t err = wlan_hal_gpio_probe_begin();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "GPIO SPI PROBE unavailable: %s", esp_err_to_name(err));
        return;
    }
    probe_settle_ms = CONFIG_MM_RESET_SETTLE_MS > 200 ? CONFIG_MM_RESET_SETTLE_MS : 200;
    mmhal_wlan_hard_reset();
    mmhal_wlan_send_training_seq();
    int ret = MMHAL_SDIO_OTHER_ERROR;
    for (unsigned attempt = 0; attempt < 3; attempt++)
    {
        ret = wlan_hal_probe_cmd(63);
        if (ret == 0)
        {
            break;
        }
        (void)wlan_hal_probe_cmd(0);
    }
    if (ret != 0)
    {
        ESP_LOGE(TAG, "GPIO SPI PROBE FAIL: CMD63 not acknowledged; comparing reset and chip-select controls");
        wlan_hal_probe_controls();
        wlan_hal_probe_miso_bias();
    }
    else
    {
        ESP_LOGI(TAG, "GPIO SPI PROBE: CMD63 acknowledged; checking chip ID and data CRC");
        unsigned valid_reads = 0;
        uint32_t previous_id = 0;
        for (unsigned attempt = 0; attempt < 3 && valid_reads < 2; attempt++)
        {
            uint32_t chip_id = 0;
            ret = wlan_hal_probe_chip_id(&chip_id);
            bool valid = ret == 0 &&
                         (chip_id == 0x206 || chip_id == 0x306 || chip_id == 0x406);
            ESP_LOGI(TAG, "GPIO SPI chip ID read %u: result=%d, id=0x%08lx, valid=%d",
                     attempt + 1, ret, (unsigned long)chip_id, valid);
            valid_reads = valid ? (chip_id == previous_id ? valid_reads + 1 : 1) : 0;
            previous_id = chip_id;
        }
        if (valid_reads >= 2)
        {
            ESP_LOGW(TAG, "GPIO SPI PROBE PASS: two matching valid MM6108 IDs with CRC. Hardware SPI path/timing needs investigation; RF scan not tested.");
        }
        else
        {
            ESP_LOGE(TAG, "GPIO SPI PROBE PARTIAL: handshake passed but repeatable chip ID/CRC validation failed");
        }
    }
    mmhal_wlan_spi_cs_deassert();
    gpio_set_level(CONFIG_MM_SPI_SCK, 0);
    gpio_set_level(CONFIG_MM_RESET_N, 0);
    ESP_LOGW(TAG, "GPIO comparison complete; host RESET_N driven low (module-side level unmeasured). Initialization stops; GPIO SPI is not used for scanning.");
}
#endif

int mmhal_wlan_sdio_startup(void)
{
#ifdef CONFIG_MM_SPI_RX_ALIGNMENT_TRIAL
    /* Revalidate on every startup; never infer permanent calibration from a
     * previous successful boot or feed corrected bytes to the raw baseline. */
    probe_rx_alignment = false;
    probe_rx_previous = 0xff;
#endif
    const uint32_t long_settle_ms = CONFIG_MM_RESET_SETTLE_MS > 200 ?
                                    CONFIG_MM_RESET_SETTLE_MS : 200;
    const struct { uint32_t clock_hz; uint32_t settle_ms; } stages[] = {
        {CONFIG_MM_SPI_CLOCK_MHZ * 1000000, CONFIG_MM_RESET_SETTLE_MS},
        {CONFIG_MM_SPI_CLOCK_MHZ * 1000000, long_settle_ms},
        {1000000, long_settle_ms},
    };
    int ret = MMHAL_SDIO_OTHER_ERROR;
    for (unsigned stage = 0; stage < sizeof(stages) / sizeof(stages[0]); stage++)
    {
        probe_clock_hz = stages[stage].clock_hz;
        probe_settle_ms = stages[stage].settle_ms;
        probe_spi_errors = 0;
        ESP_LOGI(TAG, "STARTUP PROBE %u/3: requested=%lu Hz, settle=%lu ms, CS high during reset",
                 stage + 1, (unsigned long)probe_clock_hz, (unsigned long)probe_settle_ms);
        mmhal_wlan_hard_reset();
        mmhal_wlan_send_training_seq();
        int actual_khz = 0;
        esp_err_t freq_status = spi_device_get_actual_freq(spi_handle, &actual_khz);
        if (freq_status == ESP_OK)
        {
            ESP_LOGI(TAG, "STARTUP PROBE actual clock=%d kHz", actual_khz);
        }
        else
        {
            ESP_LOGE(TAG, "Cannot read SPI clock: %s", esp_err_to_name(freq_status));
        }
        /* Same CMD63/CMD0 sequence and retry count as the SDK startup hook. */
        for (unsigned attempt = 0; attempt < 3; attempt++)
        {
            ret = wlan_hal_probe_cmd(63);
            if (ret == 0)
            {
                break;
            }
            (void)wlan_hal_probe_cmd(0);
        }
        if (probe_spi_errors != 0)
        {
            ret = MMHAL_SDIO_HW_ERROR;
        }
        if (ret == 0)
        {
            ESP_LOGI(TAG, "STARTUP PROBE %u/3 PASS: CMD63 acknowledged; retaining clock and settle settings. Chip ID still requires validation.",
                     stage + 1);
            return 0;
        }
        ESP_LOGW(TAG, "STARTUP PROBE %u/3 FAIL: result=%d, SPI API errors=%u",
                 stage + 1, ret, probe_spi_errors);
    }
    ESP_LOGE(TAG, "All hardware SPI startup probes failed.");
#ifdef CONFIG_MM_SPI_RX_ALIGNMENT_TRIAL
    if (wlan_hal_probe_rx_alignment())
    {
        return 0;
    }
#endif
#ifdef CONFIG_MM_SPI_GPIO_PROBE
    wlan_hal_probe_gpio();
#else
    ESP_LOGE(TAG, "Check module-side power, RESET_N and SPI waveforms; IRQ level alone cannot validate wiring.");
#endif
    return ret;
}
#endif

void mmhal_wlan_register_spi_irq_handler(mmhal_irq_handler_t handler)
{
    spi_irq_handler = handler;
    gpio_isr_handler_add(CONFIG_MM_SPI_IRQ, (gpio_isr_t)spi_irq_handler, NULL);
}

bool mmhal_wlan_spi_irq_is_asserted(void)
{
    return !gpio_get_level(CONFIG_MM_SPI_IRQ);
}

void mmhal_wlan_set_spi_irq_enabled(bool enabled)
{
    if (enabled)
    {
        gpio_set_intr_type(CONFIG_MM_SPI_IRQ, GPIO_INTR_LOW_LEVEL);
    }
    else
    {
        gpio_set_intr_type(CONFIG_MM_SPI_IRQ, GPIO_INTR_DISABLE);
    }
}

void mmhal_wlan_init(void)
{
    wlan_hal_gpio_init();
    wlan_hal_spi_init();
    /* Raise the RESET_N line to enable the WLAN transceiver. */
    gpio_set_level(CONFIG_MM_RESET_N, 1);
}

void mmhal_wlan_deinit(void)
{
    /* Lower the RESET_N line to disable the WLAN transceiver. This will put the transceiver in its
     * lowest power state. */
    gpio_set_level(CONFIG_MM_RESET_N, 0);

    wlan_hal_spi_deinit();

    /* Clean up any ISR handlers that have been added. These will be added again if the WLAN
     * interface is brought back up. */
    gpio_isr_handler_remove(CONFIG_MM_SPI_IRQ);
#ifdef CONFIG_HALOW_PS_MODE
    gpio_isr_handler_remove(CONFIG_MM_BUSY);
#endif /* CONFIG_HALOW_PS_MODE */
}

#ifdef CONFIG_HALOW_PS_MODE

void mmhal_wlan_wake_assert(void)
{
    gpio_set_level(CONFIG_MM_WAKE, 1);
}

void mmhal_wlan_wake_deassert(void)
{
    gpio_set_level(CONFIG_MM_WAKE, 0);
}

bool mmhal_wlan_busy_is_asserted(void)
{
    return gpio_get_level(CONFIG_MM_BUSY);
}

void mmhal_wlan_register_busy_irq_handler(mmhal_irq_handler_t handler)
{
    busy_irq_handler = handler;
    gpio_isr_handler_add(CONFIG_MM_BUSY, (gpio_isr_t)busy_irq_handler, NULL);
}

void mmhal_wlan_set_busy_irq_enabled(bool enabled)
{
    if (enabled)
    {
        gpio_set_intr_type(CONFIG_MM_BUSY, GPIO_INTR_POSEDGE);
    }
    else
    {
        gpio_set_intr_type(CONFIG_MM_BUSY, GPIO_INTR_DISABLE);
    }
}

#else

void mmhal_wlan_wake_assert(void)
{
}

void mmhal_wlan_wake_deassert(void)
{
}

bool mmhal_wlan_busy_is_asserted(void)
{
    return false;
}

void mmhal_wlan_register_busy_irq_handler(mmhal_irq_handler_t handler)
{
    (void)(handler);
}

void mmhal_wlan_set_busy_irq_enabled(bool enabled)
{
    (void)(enabled);
}

#endif /* CONFIG_HALOW_PS_MODE */

/**
 * Generate a stable, device-unique MAC address based on the ESP32 MAC address. This address is not
 * globally unique, but is consistent across boots on the same device and marked as locally
 * administered (0x02 prefix).
 *
 * @param mac_addr Location where the MAC address will be stored.
 */
static void generate_stable_mac_addr(uint8_t *mac_addr)
{
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac_addr));

    /* Set MAC address as locally administered */
    mac_addr[0] = 0x02;
    mac_addr[1] = 0x00;
}

void mmhal_read_mac_addr(uint8_t *mac_addr)
{
    /*
     * MAC address is determined using the following precedence:
     *
     * 1. The MAC address in transceiver OTP (i.e., the value of mac_addr passed into this function,
     *    if non-zero).
     *
     * 2. A stable MAC address generated based on the ESP32 MAC address. This value is consistent
     *    across boots for the same device, but unique to each ESP32.
     */

    if (!mm_mac_addr_is_zero(mac_addr))
    {
        return;
    }

    generate_stable_mac_addr(mac_addr);
}

const struct mmhal_chip *mmhal_get_chip(void)
{
    /* This is a define that is set by the build system in idf.py menuconfig to select the Morse
     * chip type. See the API documentation for mmhal_get_chip() for more information. */
#if CONFIG_MM_CHIP_MM6108
    return &mmhal_mm6108;
#elif CONFIG_MM_CHIP_MM8108
    return &mmhal_mm8108;
#endif
}
