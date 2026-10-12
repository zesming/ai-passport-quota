"""Compile the Wi-Fi driver against host stubs and exercise admission and retirement."""
import unittest
from runtime_helpers import compile_and_run


class WifiRuntime(unittest.TestCase):
    def test_disconnect_logs_reason_without_identity_or_credentials(self):
        harness = r'''
#include "quota_service.h"
#include "quota_wifi.h"
#include "esp_log.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
extern bool s_wifi_started;
extern int s_retire_stage;
static bool sleeping;
static unsigned forwarded;
static uint8_t last_reason, last_ssid[32], last_ssid_len;
static quota_service_view_t view;
void quota_service_lock(void) {}
void quota_service_unlock(void) {}
quota_service_view_t *quota_service_view(void) { return &view; }
void quota_service_post(quota_app_event_kind_t kind) { assert(kind == QUOTA_APP_EVENT_CONNECTION); }
void quota_service_wake_network(void) {}
bool quota_service_display_sleeping(void) { return sleeping; }
void quota_portable_service_disconnected(uint8_t reason, const uint8_t *ssid, size_t ssid_len) {
    forwarded++; last_reason = reason; last_ssid_len = (uint8_t)ssid_len;
    memcpy(last_ssid, ssid, ssid_len);
}

int main(void) {
    assert(quota_wifi_prepare(1, WIFI_MODE_STA) == QUOTA_WIFI_READY);
    assert(quota_wifi_start());
    const uint8_t reasons[] = {8, 15, 202, 211, 255};
    for (unsigned sleep = 0; sleep < 2; sleep++) {
        sleeping = sleep != 0;
        for (size_t i = 0; i < sizeof(reasons); i++) {
            wifi_event_sta_disconnected_t event = {.reason = reasons[i], .ssid_len = 9};
            memcpy(event.ssid, "synthetic", 9);
            quota_service_view()->connected = true;
            unsigned before = forwarded;
            wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event);
            char expected[96];
            snprintf(expected, sizeof(expected), "Wi-Fi disconnected: reason=%u, sleeping=%u",
                     (unsigned)reasons[i], sleep);
            assert(!strcmp(host_log_line, expected));
            assert(forwarded == before + 1 && last_reason == reasons[i] &&
                   last_ssid_len == 9 && !memcmp(last_ssid, "synthetic", 9));
            assert(!quota_service_view()->connected);
        }
        unsigned before = forwarded;
        wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
        assert(forwarded == before);
        char expected[96];
        snprintf(expected, sizeof(expected), "Wi-Fi disconnected: reason=unknown, sleeping=%u",
                 sleep);
        assert(!strcmp(host_log_line, expected));
    }
    strcpy(host_log_line, "unchanged");
    quota_service_view()->connected = true;
    wifi_event_sta_disconnected_t unrelated = {.reason = 202};
    unsigned before = forwarded;
    wifi_event_handler(NULL, WIFI_EVENT, -1, &unrelated);
    assert(forwarded == before && quota_service_view()->connected);
    assert(!strcmp(host_log_line, "unchanged"));
    unrelated.ssid_len = 9;
    memcpy(unrelated.ssid, "synthetic", 9);
    s_retire_stage = 1; /* RETIRE_STOP: logs and the reason callback survive retirement. */
    before = forwarded;
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &unrelated);
    assert(forwarded == before && quota_service_view()->connected &&
           !strcmp(host_log_line, "Wi-Fi disconnected: reason=202, sleeping=1"));
    s_retire_stage = 0;
    s_wifi_started = false;
    before = forwarded;
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &unrelated);
    assert(forwarded == before && quota_service_view()->connected &&
           !strcmp(host_log_line, "Wi-Fi disconnected: reason=202, sleeping=1"));
    puts("disconnect reason logging passed");
}
'''
        compile_and_run(harness, "ai-quota-wifi-log-", ("main/quota_wifi.c",), host_sdk=True)

    def test_scan_admission_and_two_fence_retirement(self):
        harness = r'''
#include "quota_service.h"
#include "quota_wifi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

void scan_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
void stop_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
void fence_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
extern bool s_wifi_started, s_wifi_initialized, s_handlers_registered;
extern bool s_scan_active, s_scan_complete;
extern int s_retire_stage;
static esp_netif_t netif;
static bool sleeping, associated, netif_up, fail_fence_posts;
static bool complete_scan_inline;
static int32_t drop_fence_id;
static uint32_t netif_ip;
static esp_netif_dhcp_status_t dhcp_status = ESP_NETIF_DHCP_STARTED;
static unsigned starts, stops, inits, deinits, scan_starts, scan_stops, result_clears;
static unsigned scan_reads, mode_sets, storage_sets, scan_completions, fence_posts;
static bool scan_success;
static uint32_t posted_token;
static int32_t posted_id;
static unsigned handshakes, networks_woken;
static quota_service_view_t view;

void quota_service_lock(void) {}
void quota_service_unlock(void) {}
quota_service_view_t *quota_service_view(void) { return &view; }
bool quota_service_display_sleeping(void) { return sleeping; }
void quota_service_post(quota_app_event_kind_t kind) {
    assert(kind == QUOTA_APP_EVENT_CONNECTION);
    handshakes++;
}
void quota_service_wake_network(void) { networks_woken++; }
void quota_portable_service_disconnected(uint8_t reason, const uint8_t *ssid, size_t len) {
    (void)reason; (void)ssid; (void)len;
}
void quota_portable_service_scan_done(bool success) {
    scan_completions++; scan_success = success;
}
esp_netif_t *esp_netif_new(const esp_netif_config_t *config) {
    (void)config; return &netif;
}
esp_err_t esp_netif_attach_wifi_station(esp_netif_t *value) {
    assert(value == &netif); return ESP_OK;
}
esp_err_t esp_wifi_set_storage(int storage) {
    assert(storage == WIFI_STORAGE_RAM); storage_sets++; return ESP_OK;
}
esp_err_t esp_wifi_set_mode(wifi_mode_t mode) {
    assert(mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA || mode == WIFI_MODE_AP);
    mode_sets++; return ESP_OK;
}
esp_err_t esp_wifi_init(const wifi_init_config_t *config) {
    (void)config; inits++; return ESP_OK;
}
esp_err_t esp_wifi_deinit(void) { deinits++; return ESP_OK; }
esp_err_t esp_wifi_start(void) { starts++; return ESP_OK; }
esp_err_t esp_wifi_stop(void) {
    stops++;
    stop_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL);
    return ESP_OK;
}
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *record) {
    (void)record; return associated ? ESP_OK : ESP_FAIL;
}
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block) {
    (void)config; assert(!block); scan_starts++;
    if (complete_scan_inline) {
        wifi_event_sta_scan_done_t done = {.status = ESP_OK};
        scan_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &done);
    }
    return ESP_OK;
}
esp_err_t esp_wifi_scan_stop(void) { scan_stops++; return ESP_OK; }
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *number) {
    *number = 7; return ESP_OK;
}
esp_err_t esp_wifi_scan_get_ap_record(wifi_ap_record_t *record) {
    (void)record; scan_reads++; return ESP_OK;
}
esp_err_t esp_wifi_clear_ap_list(void) { result_clears++; return ESP_OK; }
esp_err_t esp_event_post(esp_event_base_t base, int32_t id, const void *data, size_t size,
                         TickType_t wait) {
    (void)base; assert(size == sizeof(posted_token) && wait == 0);
    fence_posts++;
    if (fail_fence_posts) return ESP_FAIL;
    posted_id = id; memcpy(&posted_token, data, sizeof(posted_token));
    if (id != drop_fence_id)
        fence_event_handler(NULL, base, id, (void *)data);
    return ESP_OK;
}
void esp_netif_action_stop(void *value, esp_event_base_t base, int32_t id, void *data) {
    (void)base; (void)id; (void)data;
    assert(value == &netif); netif_up = false; netif_ip = 0; dhcp_status = ESP_NETIF_DHCP_STOPPED;
}
bool esp_netif_is_netif_up(esp_netif_t *value) { assert(value == &netif); return netif_up; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *value, esp_netif_ip_info_t *info) {
    assert(value == &netif); info->ip.addr = netif_ip; return ESP_OK;
}
esp_err_t esp_netif_dhcpc_get_status(esp_netif_t *value, esp_netif_dhcp_status_t *status) {
    assert(value == &netif); *status = dhcp_status; return ESP_OK;
}

static void retire_to_completion(void) {
    for (unsigned i = 0; i < 20; i++)
        if (quota_wifi_retire_poll() == QUOTA_WIFI_READY) return;
    assert(false && "retirement did not complete");
}

static void retire_to_quarantine(void) {
    for (unsigned i = 0; i < 30 && s_retire_stage != 8; i++) {
        (void)quota_wifi_retire_poll();
        host_time_us += 100000;
    }
    assert(s_retire_stage == 8);
}

int main(void) {
    wifi_scan_config_t config = {.scan_type = WIFI_SCAN_TYPE_ACTIVE};
    assert(quota_wifi_prepare(11, WIFI_MODE_STA) == QUOTA_WIFI_READY);
    assert(s_wifi_initialized && !s_wifi_started && s_handlers_registered && inits == 1 &&
           storage_sets == 1 && mode_sets == 1 &&
           !quota_wifi_operation_active(11, WIFI_MODE_STA));
    assert(quota_wifi_prepare(11, WIFI_MODE_STA) == QUOTA_WIFI_READY && inits == 1);
    assert(quota_wifi_start() && starts == 1);
    assert(quota_wifi_operation_active(11, WIFI_MODE_STA));
    assert(quota_wifi_scan_get_ap_num(&(uint16_t){0}) == ESP_ERR_INVALID_STATE);
    complete_scan_inline = true;
    assert(quota_wifi_scan_start(&config) && !s_scan_active && s_scan_complete &&
           scan_starts == 1 && scan_completions == 1);
    assert(quota_wifi_scan_clear_ap_list() == ESP_OK && result_clears == 1);
    complete_scan_inline = false;
    assert(quota_wifi_scan_start(&config) && s_scan_active && scan_starts == 2);
    assert(quota_wifi_scan_get_ap_num(&(uint16_t){0}) == ESP_ERR_INVALID_STATE);
    wifi_event_sta_scan_done_t done = {.status = ESP_OK};
    scan_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &done);
    assert(!s_scan_active && s_scan_complete && scan_completions == 2 && scan_success);
    uint16_t count = 0;
    wifi_ap_record_t record = {0};
    assert(quota_wifi_scan_get_ap_num(&count) == ESP_OK && count == 7);
    assert(quota_wifi_scan_get_ap_record(&record) == ESP_OK && scan_reads == 1);
    assert(quota_wifi_scan_clear_ap_list() == ESP_OK && result_clears == 2);
    assert(quota_wifi_scan_start(&config) && s_scan_active && scan_starts == 3);
    assert(quota_wifi_prepare(12, WIFI_MODE_APSTA) == QUOTA_WIFI_WAIT);
    assert(starts == 1 && mode_sets == 1 && s_wifi_started &&
           !quota_wifi_operation_active(11, WIFI_MODE_STA) && s_scan_active);
    scan_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &done);
    assert(s_scan_active && !s_scan_complete && scan_completions == 2);
    retire_to_completion();
    assert(!s_wifi_initialized && !s_wifi_started && result_clears == 3 && scan_stops == 1 &&
           stops == 1 &&
           deinits == 1 && netif_up == false && netif_ip == 0 &&
           dhcp_status == ESP_NETIF_DHCP_STOPPED);
    assert(quota_wifi_start() == false); /* start cannot bypass a fresh admission. */
    assert(quota_wifi_prepare(12, WIFI_MODE_APSTA) == QUOTA_WIFI_READY && inits == 2 &&
           mode_sets == 2 && storage_sets == 2);
    assert(quota_wifi_start() && starts == 2);
    assert(quota_wifi_operation_active(12, WIFI_MODE_APSTA));
    sleeping = true;
    assert(quota_wifi_stop());
    assert(!quota_wifi_started()); /* True means the radio stopped; fences are still pending. */
    retire_to_completion();
    sleeping = false;

    assert(quota_wifi_prepare(13, WIFI_MODE_STA) == QUOTA_WIFI_READY);
    assert(quota_wifi_start());
    drop_fence_id = 1;
    quota_wifi_retire_begin();
    retire_to_quarantine();
    uint32_t old_token = posted_token;
    assert(quota_wifi_prepare(14, WIFI_MODE_APSTA) == QUOTA_WIFI_WAIT &&
           !quota_wifi_start() && !s_wifi_started);
    host_time_us += 30000000;
    drop_fence_id = 0;
    assert(quota_wifi_retire_poll() == QUOTA_WIFI_WAIT);
    assert(posted_id == 1 && posted_token != 0 && posted_token != old_token);
    retire_to_completion();
    assert(quota_wifi_prepare(14, WIFI_MODE_APSTA) == QUOTA_WIFI_READY && inits == 4 &&
           mode_sets == 4);
    puts("Wi-Fi scan admission and retirement tests passed");
}
'''
        compile_and_run(harness, "ai-quota-wifi-retire-", ("main/quota_wifi.c",), host_sdk=True)

    def test_sleeping_quarantine_retries_failed_stop(self):
        harness = r'''
#include "quota_service.h"
#include "quota_wifi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

void stop_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
void fence_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
extern bool s_wifi_started;
extern int s_retire_stage;
extern int64_t host_time_us;
static bool sleeping, fail_stop, netif_up;
static unsigned stops;
static uint32_t netif_ip;
static esp_netif_dhcp_status_t dhcp_status = ESP_NETIF_DHCP_STOPPED;
static esp_netif_t netif;
static quota_service_view_t view;

void quota_service_lock(void) {}
void quota_service_unlock(void) {}
quota_service_view_t *quota_service_view(void) { return &view; }
bool quota_service_display_sleeping(void) { return sleeping; }
void quota_service_post(quota_app_event_kind_t kind) { (void)kind; }
void quota_service_wake_network(void) {}
void quota_portable_service_disconnected(uint8_t reason, const uint8_t *ssid, size_t len) {
    (void)reason; (void)ssid; (void)len;
}
esp_netif_t *esp_netif_new(const esp_netif_config_t *config) {
    (void)config; return &netif;
}
esp_err_t esp_netif_attach_wifi_station(esp_netif_t *value) {
    assert(value == &netif); return ESP_OK;
}
esp_err_t esp_wifi_set_storage(int storage) { (void)storage; return ESP_OK; }
esp_err_t esp_wifi_set_mode(wifi_mode_t mode) { (void)mode; return ESP_OK; }
esp_err_t esp_wifi_init(const wifi_init_config_t *config) { (void)config; return ESP_OK; }
esp_err_t esp_wifi_deinit(void) { return ESP_OK; }
esp_err_t esp_wifi_start(void) { return ESP_OK; }
esp_err_t esp_wifi_stop(void) {
    stops++;
    if (fail_stop) return ESP_FAIL;
    stop_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL);
    return ESP_OK;
}
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *record) {
    (void)record; return ESP_FAIL;
}
esp_err_t esp_event_post(esp_event_base_t base, int32_t id, const void *data, size_t size,
                         TickType_t wait) {
    assert(size == sizeof(uint32_t) && wait == 0);
    fence_event_handler(NULL, base, id, (void *)data);
    return ESP_OK;
}
void esp_netif_action_stop(void *value, esp_event_base_t base, int32_t id, void *data) {
    (void)base; (void)id; (void)data;
    assert(value == &netif); netif_up = false; netif_ip = 0;
    dhcp_status = ESP_NETIF_DHCP_STOPPED;
}
bool esp_netif_is_netif_up(esp_netif_t *value) { assert(value == &netif); return netif_up; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *value, esp_netif_ip_info_t *info) {
    assert(value == &netif); info->ip.addr = netif_ip; return ESP_OK;
}
esp_err_t esp_netif_dhcpc_get_status(esp_netif_t *value, esp_netif_dhcp_status_t *status) {
    assert(value == &netif); *status = dhcp_status; return ESP_OK;
}

int main(void) {
    assert(quota_wifi_prepare(1, WIFI_MODE_STA) == QUOTA_WIFI_READY);
    assert(quota_wifi_start());
    fail_stop = true;
    quota_wifi_retire_begin();
    assert(quota_wifi_retire_poll() == QUOTA_WIFI_WAIT && stops == 1 &&
           quota_wifi_started());
    uint64_t deadline = quota_wifi_next_deadline_ms(true);
    assert(deadline != UINT64_MAX && deadline > (uint64_t)(host_time_us / 1000));

    sleeping = true;
    host_time_us += 31000000;
    fail_stop = false;
    assert(quota_wifi_prepare(2, WIFI_MODE_APSTA) == QUOTA_WIFI_WAIT &&
           !quota_wifi_start() && quota_wifi_started());
    assert(quota_wifi_retire_poll() == QUOTA_WIFI_WAIT && stops == 2 &&
           !quota_wifi_started());
    assert(quota_wifi_prepare(2, WIFI_MODE_APSTA) == QUOTA_WIFI_WAIT && !quota_wifi_start());
    for (unsigned i = 0; i < 20; i++) {
        if (quota_wifi_retire_poll() == QUOTA_WIFI_READY) break;
        host_time_us += 1;
    }
    assert(quota_wifi_prepare(2, WIFI_MODE_APSTA) == QUOTA_WIFI_READY);
    puts("sleeping quarantine stop retry passed");
}
'''
        compile_and_run(harness, "ai-quota-wifi-sleep-stop-", ("main/quota_wifi.c",), host_sdk=True)


if __name__ == "__main__":
    unittest.main()
