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
#include "mmwlan_stats.h"
#include "p4_camera.h"

static const char *TAG = "p4_camera_app";

/* HaLow WPA3-SAE credentials supplied for this board. */
#define HALOW_WIFI_SSID     "ThinkNode-G4_2a0ea0"
#define HALOW_WIFI_PASSWORD "elecrow.com"
static EventGroupHandle_t network_events;
#define GOT_IP BIT0
#define STA_ASSOCIATED BIT1
#define DISCOVERY_DONE BIT2
#define DISCOVERY_TARGET BIT3
#define DISCOVERY_COMPLETE BIT4
#define HALOW_SCAN_DWELL_MS 150
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
    ESP_LOGI(TAG, "HaLow AP: %.*s; RSSI=%d dBm, freq=%lu kHz, RX BW=%u MHz, operating BW=%u MHz, security=%s%s%s, target=%s",
             entry->ssid_len, (const char *)entry->ssid, entry->rssi,
             (unsigned long)(entry->channel_freq_hz / 1000), entry->bw_mhz, entry->op_bw_mhz, security,
             rsn.num_akm_suites > 1 ? "/" : "",
             rsn.num_akm_suites > 1 ? mm_akm_suite_to_string(rsn.akm_suites[1]) : "",
             target ? "yes" : "no");
    ESP_LOGI(TAG, "  BSSID=%02x:%02x:%02x:%02x:%02x:%02x; beacon=%u TUs",
             entry->bssid[0], entry->bssid[1], entry->bssid[2],
             entry->bssid[3], entry->bssid[4], entry->bssid[5], entry->beacon_interval);
}

static void log_radio_stats(void)
{
    struct mmwlan_stats_umac_data stats = {0};
    enum mmwlan_status status = mmwlan_get_umac_stats(&stats);
    if (status != MMWLAN_SUCCESS) {
        ESP_LOGW(TAG, "HaLow stats unavailable: %d", status);
        return;
    }
    ESP_LOGI(TAG, "HaLow counters (cumulative): scans=%u, restarts=%u, RX read failures=%lu, RX alloc failures=%lu, RX queue drops=%lu, RX mgmt queue peak=%u, TX status timeouts=%lu",
             stats.num_scans_complete, stats.hw_restart_counter,
             (unsigned long)stats.datapath_driver_rx_read_failures,
             (unsigned long)stats.datapath_driver_rx_alloc_failures,
             (unsigned long)stats.datapath_rxq_frames_dropped,
             stats.datapath_rx_mgmt_q_high_water_mark,
             (unsigned long)stats.datapath_driver_tx_pending_status_timeout);
}

static void discovery_complete(enum mmwlan_scan_state state, void *arg)
{
    (void)arg;
    if (state == MMWLAN_SCAN_RUNNING) return;
    ESP_LOGI(TAG, "Discovery complete: state=%d (%s), responses=%u, target responses=%u",
             state, state == MMWLAN_SCAN_SUCCESSFUL ? "all channels scanned" : "incomplete",
             scan_responses, scan_target_responses);
    /* Publish the results last. The main task must not start STA or reuse the
     * counters until this scan's final callback has finished using them. */
    EventBits_t bits = DISCOVERY_DONE;
    if (scan_target_responses) bits |= DISCOVERY_TARGET;
    if (state == MMWLAN_SCAN_SUCCESSFUL) bits |= DISCOVERY_COMPLETE;
    xEventGroupSetBits(network_events, bits);
}

static bool discover_ap(uint32_t dwell_ms, bool *found)
{
    struct mmwlan_scan_req request = MMWLAN_SCAN_REQ_INIT;
    request.scan_rx_cb = station_scan_result;
    request.scan_complete_cb = discovery_complete;
    request.args.dwell_time_ms = dwell_ms;
    scan_responses = scan_target_responses = 0;
    xEventGroupClearBits(network_events, DISCOVERY_DONE | DISCOVERY_TARGET | DISCOVERY_COMPLETE);
    ESP_LOGI(TAG, "Discovery: wildcard US scan, dwell=%lu ms/channel; STA/camera/SD not started",
             (unsigned long)dwell_ms);
    enum mmwlan_status status = mmwlan_scan_request(&request);
    if (status != MMWLAN_SUCCESS) {
        ESP_LOGE(TAG, "Discovery request failed: %d", status);
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(network_events, DISCOVERY_DONE, pdFALSE,
                                         pdFALSE, pdMS_TO_TICKS(60000));
    if (!(bits & DISCOVERY_DONE)) {
        ESP_LOGE(TAG, "Discovery timed out; aborting scan and stopping startup");
        (void)mmwlan_scan_abort();
        /* All callback state is static and remains valid after this timeout. */
        (void)xEventGroupWaitBits(network_events, DISCOVERY_DONE, pdFALSE,
                                 pdFALSE, pdMS_TO_TICKS(5000));
        return false;
    }
    log_radio_stats();
    *found = (bits & DISCOVERY_TARGET) != 0;
    return (bits & DISCOVERY_COMPLETE) != 0;
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "HaLow IP: " IPSTR "; browser: http://" IPSTR "/",
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.ip));
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

void app_main(void)
{
    ESP_LOGI(TAG, "Elecrow P4 camera v4: independent discovery before STA; US AP 924 MHz / 8 MHz");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS needs maintenance; preserve settings before erasing NVS");
        return;
    }
    if (ret != ESP_OK) { ESP_LOGE(TAG, "NVS: %s", esp_err_to_name(ret)); return; }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    network_events = xEventGroupCreate();
    if (!network_events) { ESP_LOGE(TAG, "No memory for network events"); return; }
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, ip_event, NULL));

    /* Establish a network baseline before acquiring camera rails, CSI/JPEG
     * resources or the SD host. Earlier IP success occurred with camera init
     * failing, so camera/SD startup must not precede this connection attempt. */
    ESP_LOGI(TAG, "Startup 1/3: connecting HaLow; camera and SD initialization deferred until IP");
    const char *ssid = HALOW_WIFI_SSID;
    const char *password = HALOW_WIFI_PASSWORD;
    mmhalow_wifi_config_t conf = { .sta = MMWLAN_STA_ARGS_INIT };
    size_t ssid_len = strlen(ssid), password_len = strlen(password);
    if (!ssid_len || ssid_len > sizeof(conf.sta.ssid) ||
        !password_len || password_len >= sizeof(conf.sta.passphrase)) {
        ESP_LOGE(TAG, "Invalid hardcoded HaLow credentials in main/p4_main.c");
        return;
    }
    conf.sta.security_type = MMWLAN_SAE;
    conf.sta.sta_evt_cb = station_event;
    conf.sta.scan_rx_cb = station_scan_result;
    /* Keep retries observable while debugging instead of backing off to 512 s. */
    conf.sta.scan_interval_limit_s = 16;
    memcpy(conf.sta.ssid, ssid, ssid_len);
    conf.sta.ssid_len = ssid_len;
    memcpy(conf.sta.passphrase, password, password_len);
    conf.sta.passphrase_len = password_len;
    ret = mmhalow_init(NULL);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "HaLow init failed: %d", ret); return; }
    /* Retain the full US channel list: an 8 MHz AP's centre frequency is not
     * necessarily its primary 1/2 MHz scan frequency. Do not pin scans to 924. */
    bool found = false;
    if (!discover_ap(MMWLAN_SCAN_DEFAULT_DWELL_TIME_MS, &found)) return;
    if (!found && !discover_ap(HALOW_SCAN_DWELL_MS, &found)) return;
    if (!found) {
        ESP_LOGW(TAG, "Target absent in both independent scans; continuing STA attempts. No authentication or DHCP diagnosis yet.");
    }
    struct mmwlan_scan_config scan_config = MMWLAN_SCAN_CONFIG_INIT;
    scan_config.dwell_time_ms = HALOW_SCAN_DWELL_MS;
    enum mmwlan_status status = mmwlan_set_scan_config(&scan_config);
    if (status != MMWLAN_SUCCESS) {
        ESP_LOGE(TAG, "STA scan configuration failed: %d", status);
        return;
    }
    ESP_LOGI(TAG, "STA scan dwell=%u ms/channel; retry interval capped at 16 s", HALOW_SCAN_DWELL_MS);
    ret = mmhalow_set_config(WIFI_IF_STA, &conf);
    if (ret == ESP_OK) ret = mmhalow_connect(link_event);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "HaLow connect failed: %d", ret); return; }
    ESP_LOGI(TAG, "Connecting to %s using WPA3-SAE", ssid);
    while (!(xEventGroupWaitBits(network_events, GOT_IP, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000)) & GOT_IP)) {
        enum mmwlan_sta_state state = mmhalow_status();
        if (state == MMWLAN_STA_CONNECTED) {
            ESP_LOGW(TAG, "STA is associated, but DHCP has not assigned an IP; check the HaLow AP DHCP server");
        } else {
            ESP_LOGW(TAG, "STA is not associated (state=%d); camera/SD initialization has not run; see AP scan results", state);
        }
        log_radio_stats();
    }

    ESP_LOGI(TAG, "Startup 2/3: HaLow obtained IP; starting camera");
    ret = p4_camera_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed (%s). Continuing to web status.", esp_err_to_name(ret));
    }
    ESP_LOGI(TAG, "HaLow after camera startup: STA state=%d, IP event=%s",
             mmhalow_status(), (xEventGroupGetBits(network_events) & GOT_IP) ? "present" : "absent");
    ESP_LOGI(TAG, "Startup 3/3: mounting SD");
    (void)p4_storage_init();
    ESP_LOGI(TAG, "HaLow after SD startup: STA state=%d, IP event=%s",
             mmhalow_status(), (xEventGroupGetBits(network_events) & GOT_IP) ? "present" : "absent");
    ret = p4_web_start();
    if (ret != ESP_OK) ESP_LOGE(TAG, "HTTP startup failed: %s", esp_err_to_name(ret));
}

