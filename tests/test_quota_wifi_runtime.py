"""Run the whole Wi-Fi station driver against host stubs: restart, sleep and late events."""
import unittest
from runtime_helpers import compile_and_run


class WifiRuntime(unittest.TestCase):
    def test_disconnect_logs_raw_reason_without_identity_or_credentials(self):
        harness = r'''
#include "quota_service.h"
#include "esp_log.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
static bool sleeping;
static unsigned forwarded;
static uint8_t last_reason;
static quota_service_view_t view;
void quota_service_lock(void) {}
void quota_service_unlock(void) {}
quota_service_view_t *quota_service_view(void) { return &view; }
void quota_service_post(quota_app_event_kind_t kind) {
    assert(kind == QUOTA_APP_EVENT_CONNECTION);
}
void quota_service_wake_network(void) {}
bool quota_service_display_sleeping(void) { return sleeping; }
void quota_portable_service_disconnected(uint8_t reason) {
    forwarded++; last_reason = reason;
}

int main(void) {
    const uint8_t reasons[] = {8, 15, 202, 211, 255};
    for (unsigned sleep = 0; sleep < 2; sleep++) {
        sleeping = sleep != 0;
        for (size_t i = 0; i < sizeof(reasons); i++) {
            wifi_event_sta_disconnected_t event = {.reason = reasons[i]};
            quota_service_view()->connected = true;
            unsigned before = forwarded;
            wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event);
            char expected[96];
            snprintf(expected, sizeof(expected), "Wi-Fi disconnected: reason=%u, sleeping=%u",
                     (unsigned)reasons[i], sleep);
            assert(!strcmp(host_log_line, expected));
            assert(forwarded == before + 1 && last_reason == reasons[i]);
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
    puts("disconnect reason logging passed");
}
'''
        compile_and_run(harness, "ai-quota-wifi-log-", ("main/quota_wifi.c",), host_sdk=True)

    def test_wifi_stop_restart_and_late_events(self):
        harness = r'''
#include "quota_service.h"
#include "quota_wifi.h"
#include <assert.h>
#include <stdio.h>

void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
extern bool s_wifi_started, s_handlers_registered, s_wifi_retry_pending;
extern int64_t s_wifi_retry_at_ms;

static quota_service_view_t view;
static bool sleeping, fail_start, fail_stop, associated;
static unsigned registrations, starts, stops, settings, events, notifications;

void quota_service_lock(void) {}
void quota_service_unlock(void) {}
quota_service_view_t *quota_service_view(void)
{
    return &view;
}
bool quota_service_display_sleeping(void)
{
    return sleeping;
}
void quota_service_post(quota_app_event_kind_t kind)
{
    assert(kind == QUOTA_APP_EVENT_CONNECTION);
    ++events;
}
void quota_service_wake_network(void)
{
    ++notifications;
}
void quota_portable_service_disconnected(uint8_t reason)
{
    (void)reason;
}

esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap)
{
    (void)ap;
    return associated ? ESP_OK : 1;
}
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
                                              esp_event_handler_t handler, void *arg,
                                              esp_event_handler_instance_t *instance)
{
    (void)base;
    (void)id;
    (void)handler;
    (void)arg;
    (void)instance;
    ++registrations;
    return ESP_OK;
}
esp_err_t esp_wifi_set_storage(int value)
{
    (void)value;
    ++settings;
    return ESP_OK;
}
esp_err_t esp_wifi_set_mode(int value)
{
    (void)value;
    ++settings;
    return ESP_OK;
}
esp_err_t esp_wifi_start(void)
{
    ++starts;
    return fail_start ? 1 : ESP_OK;
}
esp_err_t esp_wifi_stop(void)
{
    ++stops;
    if (fail_stop)
        return 1;
    associated = false;
    return ESP_OK;
}

int main(void)
{
    assert(quota_wifi_start() && s_wifi_started && s_handlers_registered);
    assert(registrations == 2 && settings == 2 && starts == 1 && s_wifi_retry_pending);
    associated = true;
    ip_event_handler(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(view.connected && !s_wifi_retry_pending);
    sleeping = true;
    fail_stop = true;
    assert(!quota_wifi_stop());
    assert(s_wifi_started && view.connected);
    associated = false;
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    assert(!view.connected && s_wifi_retry_pending);
    sleeping = false;
    assert(quota_wifi_start() && s_wifi_retry_pending && starts == 1);
    /* Failed stop plus asleep disconnect must retain reconnect work on wake. */
    sleeping = true;
    fail_stop = false;
    assert(quota_wifi_stop());
    assert(!s_wifi_started && !view.connected && !s_wifi_retry_pending);
    unsigned before = events;
    ip_event_handler(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    assert(!view.connected && !s_wifi_retry_pending && events == before);
    assert(quota_wifi_stop() && stops == 2); /* No repeated stop after success. */
    sleeping = false;
    ip_event_handler(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(!view.connected); /* A queued old IP event cannot revive a stopped radio. */
    host_time_us = 70000 * 1000;
    assert(quota_wifi_start());
    assert(starts == 2 && registrations == 2 && settings == 2 && s_wifi_retry_at_ms == 70000);
    before = events;
    ip_event_handler(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(!view.connected && s_wifi_retry_pending && events == before);
    /* The late old IP event after restart cannot cancel the next connect. */
    associated = true;
    ip_event_handler(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(view.connected);
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    assert(!view.connected && s_wifi_retry_pending && s_wifi_retry_at_ms == 72000);
    sleeping = true;
    assert(quota_wifi_stop());
    sleeping = false;
    fail_start = true;
    assert(!quota_wifi_start());
    assert(!s_wifi_started && !s_handlers_registered);
    fail_start = false;
    assert(quota_wifi_start() && s_wifi_started);
    assert(registrations == 4 && settings == 4);
    /* Asleep with the radio still up (a failed stop): a late IP event must not report a link. */
    view.connected = false; sleeping = true; associated = true; before = events;
    ip_event_handler(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(!view.connected && events == before);
    sleeping = false;
    puts("Wi-Fi sleep lifecycle tests passed");
}
'''
        compile_and_run(harness, "ai-quota-wifi-", ("main/quota_wifi.c",), host_sdk=True)


if __name__ == "__main__":
    unittest.main()
