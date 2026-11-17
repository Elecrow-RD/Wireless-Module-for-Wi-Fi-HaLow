/*
 * Copyright 2026 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "sdkconfig.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mmhalow.h"
#include "mmutils.h"
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

static const char *TAG = "sta_connect";

/* HaLow WPA3-SAE credentials supplied for this board. */
#define HALOW_WIFI_SSID     CONFIG_WIFI_SSID
#define HALOW_WIFI_PASSWORD CONFIG_WIFI_PSK
static EventGroupHandle_t network_events;
#define GOT_IP BIT0
#define STA_ASSOCIATED BIT1
/* Only the serialized mmwlan scan callbacks access these counters. */
static unsigned scan_responses, scan_target_responses;

static void station_scan_result(const struct mmwlan_scan_result *entry, void *arg)
{
    (void)arg;
    bool target = entry->ssid_len == strlen(HALOW_WIFI_SSID) &&
                  memcmp(entry->ssid, HALOW_WIFI_SSID, entry->ssid_len) == 0;
    ++scan_responses;
    if (target) ++scan_target_responses;
    /* Bound logging in the WLAN task; always report a matching target. */
    if (scan_responses > 8 && !target) return;
    struct mm_rsn_information rsn = {0};
    mm_parse_rsn_information(entry->ies, entry->ies_len, &rsn);
    const char *security = rsn.num_akm_suites ? mm_akm_suite_to_string(rsn.akm_suites[0]) : "None";
    ESP_LOGI(TAG, "HaLow AP: %.*s; RSSI=%d dBm, freq=%lu kHz, security=%s%s%s, target=%s",
             entry->ssid_len, (const char *)entry->ssid, entry->rssi,
             (unsigned long)(entry->channel_freq_hz / 1000), security,
             rsn.num_akm_suites > 1 ? "/" : "",
             rsn.num_akm_suites > 1 ? mm_akm_suite_to_string(rsn.akm_suites[1]) : "",
             target ? "yes" : "no");
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "HaLow IP: " IPSTR "; gateway: " IPSTR,
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.gw));
        xEventGroupSetBits(network_events, GOT_IP);
    } else if (id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "HaLow IP lost; STA state=%d", mmhalow_status());
        xEventGroupClearBits(network_events, GOT_IP);
    }
}

static void link_event(enum mmwlan_sta_state state)
{
    switch (state) {
    case MMWLAN_STA_DISABLED:
        ESP_LOGW(TAG, "HaLow STA disabled");
        xEventGroupClearBits(network_events, GOT_IP | STA_ASSOCIATED);
        break;
    case MMWLAN_STA_CONNECTING:
        ESP_LOGI(TAG, "HaLow STA connecting/scanning; association is not complete");
        xEventGroupClearBits(network_events, GOT_IP | STA_ASSOCIATED);
        break;
    case MMWLAN_STA_CONNECTED:
        ESP_LOGI(TAG, "HaLow STA associated with AP; waiting for DHCP");
        xEventGroupSetBits(network_events, STA_ASSOCIATED);
        break;
    default:
        ESP_LOGW(TAG, "Unknown HaLow STA state: %d", state);
        break;
    }
}

static void station_event(const struct mmwlan_sta_event_cb_args *event, void *arg)
{
    (void)arg;
    switch (event->event) {
    case MMWLAN_STA_EVT_SCAN_REQUEST:
        scan_responses = scan_target_responses = 0;
        ESP_LOGI(TAG, "HaLow: scanning for configured AP");
        break;
    case MMWLAN_STA_EVT_SCAN_COMPLETE:
        ESP_LOGI(TAG, "HaLow scan complete: responses=%u, target responses=%u",
                 scan_responses, scan_target_responses);
        if (!scan_target_responses) {
            ESP_LOGW(TAG, "Configured AP not observed in this scan: %s (US channel list); authentication and DHCP have not started",
                     HALOW_WIFI_SSID);
        }
        break;
    case MMWLAN_STA_EVT_SCAN_ABORT: ESP_LOGW(TAG, "HaLow: AP scan attempt aborted"); break;
    case MMWLAN_STA_EVT_AUTH_REQUEST: ESP_LOGI(TAG, "HaLow: authenticating with AP (SAE)"); break;
    case MMWLAN_STA_EVT_ASSOC_REQUEST: ESP_LOGI(TAG, "HaLow: sending AP association request"); break;
    case MMWLAN_STA_EVT_CTRL_PORT_OPEN: ESP_LOGI(TAG, "HaLow: authenticated data port opened"); break;
    case MMWLAN_STA_EVT_CTRL_PORT_CLOSED: ESP_LOGW(TAG, "HaLow: authenticated data port closed"); break;
    default: ESP_LOGI(TAG, "HaLow STA event: %d", event->event); break;
    }
}

#define UDP_IP "192.168.12.1"
#define UDP_PORT 12345

static void udp_tx(void)
{
    const char *msg = "Hello from esp32\n";
    struct sockaddr_in dest_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(UDP_PORT),
    };
    if (inet_pton(AF_INET, UDP_IP, &dest_addr.sin_addr) != 1) {
        ESP_LOGE(TAG, "Invalid UDP destination: %s", UDP_IP);
        return;
    }
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "UDP socket failed: errno=%d", errno);
        return;
    }
    int sent = sendto(sock, msg, strlen(msg), 0,
                      (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (sent < 0) {
        ESP_LOGE(TAG, "UDP send failed: errno=%d", errno);
    } else {
        ESP_LOGI(TAG, "UDP sent: %d bytes to %s:%d (peer reception is not confirmed)",
                 sent, UDP_IP, UDP_PORT);
    }
    close(sock);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Elecrow P4 sta_connect v1; country=%s, RESET=%d SCK=%d MOSI=%d MISO=%d CS=%d IRQ=%d",
             CONFIG_HALOW_COUNTRY_CODE, CONFIG_MM_RESET_N, CONFIG_MM_SPI_SCK,
             CONFIG_MM_SPI_MOSI, CONFIG_MM_SPI_MISO, CONFIG_MM_SPI_CS, CONFIG_MM_SPI_IRQ);
    esp_err_t ret = nvs_flash_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS initialization failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    /* Create callback resources before starting the asynchronous connection. */
    network_events = xEventGroupCreate();
    if (!network_events) { ESP_LOGE(TAG, "No memory for network events"); return; }
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, ip_event, NULL));

    mmhalow_wifi_config_t conf = { .sta = MMWLAN_STA_ARGS_INIT };
    const char *ssid = HALOW_WIFI_SSID;
    const char *password = HALOW_WIFI_PASSWORD;
    size_t ssid_len = strlen(ssid), password_len = strlen(password);
    if (!ssid_len || ssid_len > sizeof(conf.sta.ssid) ||
        !password_len || password_len >= sizeof(conf.sta.passphrase)) {
        ESP_LOGE(TAG, "Invalid HaLow credentials; check example configuration");
        return;
    }
    memcpy(conf.sta.ssid, ssid, ssid_len);
    conf.sta.ssid_len = ssid_len;
    memcpy(conf.sta.passphrase, password, password_len);
    conf.sta.passphrase_len = password_len;
    conf.sta.security_type = MMWLAN_SAE;
    conf.sta.sta_evt_cb = station_event;
    conf.sta.scan_rx_cb = station_scan_result;
    ret = mmhalow_init(NULL);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "HaLow initialization failed (status %d)", ret); return; }
    mmhalow_print_version_info();
    ret = mmhalow_set_config(WIFI_IF_STA, &conf);
    if (ret == ESP_OK) ret = mmhalow_connect(link_event);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "HaLow connection startup failed (status %d)", ret); return; }
    ESP_LOGI(TAG, "Connecting to %s using WPA3-SAE", ssid);
    while (!(xEventGroupWaitBits(network_events, GOT_IP, pdFALSE, pdFALSE,
                                pdMS_TO_TICKS(30000)) & GOT_IP)) {
        int state = mmhalow_status();
        if (state == MMWLAN_STA_CONNECTED) {
            ESP_LOGW(TAG, "Associated, still waiting for DHCP; check AP DHCP service");
        } else {
            ESP_LOGW(TAG, "Not associated yet (state=%d); see scan/authentication logs", state);
        }
    }
    udp_tx();
    /* WLAN and IP callbacks remain active after this one-shot UDP example. */
}
