"""Run the whole Wi-Fi station driver against host stubs: restart, sleep and late events."""
import unittest
from runtime_helpers import compile_and_run


class WifiRuntime(unittest.TestCase):
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
