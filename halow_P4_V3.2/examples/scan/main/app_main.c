/*
 * Copyright 2026 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mmhalow.h"
#include "mmutils.h"
#include "mmwlan.h"
#include "mmwlan_stats.h"
#include "nvs_flash.h"

static const char *TAG = "scan";

#define SCAN_ROUNDS 5U
#define SCAN_DONE BIT0
#define SCAN_COMPLETE BIT1
#define TARGET_SSID "ThinkNode-G4_2a0ea0"
static EventGroupHandle_t scan_events;
/* Callbacks are serialized by mmwlan. Main reads these only after SCAN_DONE. */
static unsigned responses, target_responses;

static void scan_cb(enum mmwlan_scan_state state, void *args)
{
    (void)args;
    switch (state)
    {
        case MMWLAN_SCAN_SUCCESSFUL:
            ESP_LOGI(TAG, "All channels scanned: responses=%u, target responses=%u",
                     responses, target_responses);
            break;
        case MMWLAN_SCAN_TERMINATED:
            ESP_LOGE(TAG, "Scan terminated before completion; results may be incomplete");
            break;
        case MMWLAN_SCAN_RUNNING:
            ESP_LOGI(TAG, "Scan still running");
            return;
        default:
            ESP_LOGE(TAG, "Unexpected scan state: %d", (int)state);
            break;
    }
    /* Publish last: no callback accesses the per-round counters afterwards. */
    xEventGroupSetBits(scan_events, SCAN_DONE |
                      (state == MMWLAN_SCAN_SUCCESSFUL ? SCAN_COMPLETE : 0));
}

static int akm_str_len(uint32_t akm_suite)
{
    switch (akm_suite) {
        case MM_AKM_SUITE_NONE:
            return 4;
        case MM_AKM_SUITE_OTHER:
            return 5;
        default:
            return 3;
    }
}

static void rx_cb(const struct mmwlan_scan_result *entry, void *args)
{
    (void)args;
    ++responses;
    if (entry->ssid_len == sizeof(TARGET_SSID) - 1 &&
        memcmp(entry->ssid, TARGET_SSID, entry->ssid_len) == 0)
    {
        ++target_responses;
    }
    struct mm_rsn_information rsn = {0};
    mm_parse_rsn_information(entry->ies, entry->ies_len, &rsn);

    ESP_LOGI(TAG, "\n"
        "%.*s" "\n"
        "\t" "Operating BW: %d MHz" "\n"
        "\t" "RX frequency: %lu kHz; RX BW: %u MHz" "\n"
        "\t" "BSSID: %02x:%02x:%02x:%02x:%02x:%02x" "\n"
        "\t" "RSSI: %3d" "\n"
        "\t" "Beacon Interval (TUs): %u" "\n"
        "\t" "Capability Info: 0x%04x" "\n"
        "\t" "Security: %.*s %.*s" "\n"
        ,
        entry->ssid_len,
        entry->ssid,
        entry->op_bw_mhz,
        (unsigned long)(entry->channel_freq_hz / 1000), entry->bw_mhz,
        entry->bssid[0],
        entry->bssid[1],
        entry->bssid[2],
        entry->bssid[3],
        entry->bssid[4],
        entry->bssid[5],
        entry->rssi,
        entry->beacon_interval,
        entry->capability_info,
        rsn.num_akm_suites ? akm_str_len(rsn.akm_suites[0]) : 4,
        rsn.num_akm_suites ? mm_akm_suite_to_string(rsn.akm_suites[0]) : "None",
        rsn.num_akm_suites > 1 ? akm_str_len(rsn.akm_suites[1]) : 0,
        rsn.num_akm_suites > 1 ? mm_akm_suite_to_string(rsn.akm_suites[1]) : ""
    );
}

static void log_radio_stats(void)
{
    struct mmwlan_stats_umac_data stats = {0};
    enum mmwlan_status status = mmwlan_get_umac_stats(&stats);
    if (status != MMWLAN_SUCCESS)
    {
        ESP_LOGW(TAG, "Radio statistics unavailable: %d", status);
        return;
    }
    ESP_LOGI(TAG, "Cumulative radio counters: scans=%u, restarts=%u, RX read failures=%lu, RX alloc failures=%lu, RX queue drops=%lu, TX status timeouts=%lu",
             stats.num_scans_complete, stats.hw_restart_counter,
             (unsigned long)stats.datapath_driver_rx_read_failures,
             (unsigned long)stats.datapath_driver_rx_alloc_failures,
             (unsigned long)stats.datapath_rxq_frames_dropped,
             (unsigned long)stats.datapath_driver_tx_pending_status_timeout);
}

void app_main(void)
{
    ESP_LOGI(TAG, "HaLow SPI diagnostic v8; configured clock=%d MHz, RESET_N=%d, settle=%d ms",
             CONFIG_MM_SPI_CLOCK_MHZ, CONFIG_MM_RESET_N, CONFIG_MM_RESET_SETTLE_MS);
    ESP_LOGI(TAG, "Scan comparison v9: 5 sequential scans; first default dwell, then 150 ms/channel");
    ESP_LOGI(TAG, "HaLow board profile: stacked headers, IRQ=%d, BCF=%s",
             CONFIG_MM_SPI_IRQ, CONFIG_MM_BCF_FILE);
    ESP_LOGI(TAG, "Booted. Initializing Wi-Fi HaLow...");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
    scan_events = xEventGroupCreate();
    if (scan_events == NULL)
    {
        ESP_LOGE(TAG, "No memory for scan completion events");
        return;
    }

    esp_err_t status = mmhalow_init(NULL);
    if (status != ESP_OK)
    {
        ESP_LOGE(TAG, "Wi-Fi HaLow initialization failed (status %d)", status);
#ifdef CONFIG_MM_SPI_GPIO_PROBE
        ESP_LOGW(TAG, "Check ALIGN trial results if present. If GPIO SPI PROBE ran, that diagnostic always stops before scanning.");
#endif
        return;
    }

    ESP_LOGI(TAG, "Wi-Fi HaLow Initialised");

    unsigned rounds_with_target = 0, total_responses = 0;
    for (unsigned round = 1; round <= SCAN_ROUNDS; ++round)
    {
        struct mmwlan_scan_req request = MMWLAN_SCAN_REQ_INIT;
        request.scan_complete_cb = scan_cb;
        request.scan_rx_cb = rx_cb;
        if (round > 1) request.args.dwell_time_ms = 150;
        responses = target_responses = 0;
        xEventGroupClearBits(scan_events, SCAN_DONE | SCAN_COMPLETE);
        ESP_LOGI(TAG, "Scan round %u/%u: wildcard, full configured country list, dwell=%lu ms/channel",
                 round, SCAN_ROUNDS, (unsigned long)request.args.dwell_time_ms);
        enum mmwlan_status scan_status = mmwlan_scan_request(&request);
        if (scan_status != MMWLAN_SUCCESS)
        {
            ESP_LOGE(TAG, "Scan request failed: %d; stopping comparison", scan_status);
            return;
        }
        EventBits_t bits = xEventGroupWaitBits(scan_events, SCAN_DONE, pdFALSE,
                                             pdFALSE, pdMS_TO_TICKS(60000));
        if (!(bits & SCAN_DONE))
        {
            ESP_LOGE(TAG, "Scan timed out after 60 s; aborting, no further scans will be started");
            (void)mmwlan_scan_abort();
            (void)xEventGroupWaitBits(scan_events, SCAN_DONE, pdFALSE,
                                     pdFALSE, pdMS_TO_TICKS(5000));
            /* Callback state is static and remains valid after returning. */
            return;
        }
        log_radio_stats();
        if (!(bits & SCAN_COMPLETE))
        {
            ESP_LOGE(TAG, "Incomplete scan; stopping comparison");
            return;
        }
        total_responses += responses;
        if (target_responses) ++rounds_with_target;
        if (!responses)
        {
            ESP_LOGW(TAG, "No AP responses this round; channel scan completion does not mean an AP was found");
        }
        if (round < SCAN_ROUNDS) vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGI(TAG, "Comparison finished: %u/%u rounds observed %s; total responses=%u (duplicates included)",
             rounds_with_target, SCAN_ROUNDS, TARGET_SSID, total_responses);
    if (!rounds_with_target)
    {
        ESP_LOGW(TAG, "Target was not observed. No authentication or DHCP was attempted. Zero error counters do not prove radio/SPI health.");
    }
}
