#include "quota_wifi.h"
#include "quota_portable_service.h"
#include "quota_service.h"
#include "quota_testable.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"

#define WIFI_RETRY_MIN_MS 2000
#define WIFI_RETRY_MAX_MS 30000

static const char *TAG = "quota_wifi";

static bool s_wifi_stack_ready;
static bool s_wifi_initialized;
QUOTA_TESTABLE bool s_wifi_started;
static esp_netif_t *s_sta_netif;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
QUOTA_TESTABLE bool s_handlers_registered;
QUOTA_TESTABLE bool s_wifi_retry_pending;
QUOTA_TESTABLE uint32_t s_wifi_retry_delay_ms = WIFI_RETRY_MIN_MS;
QUOTA_TESTABLE int64_t s_wifi_retry_at_ms;

static bool prepare_network_stack(void)
{
    if (s_wifi_stack_ready)
        return true;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        goto failed;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        goto failed;

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_WIFI_STA();
    s_sta_netif = esp_netif_new(&netif_config);
    if (s_sta_netif == NULL) {
        err = ESP_ERR_NO_MEM;
        goto failed;
    }
    err = esp_netif_attach_wifi_station(s_sta_netif);
    if (err != ESP_OK)
        goto failed;
    err = esp_wifi_set_default_wifi_sta_handlers();
    if (err != ESP_OK)
        goto failed;

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_config);
    if (err != ESP_OK)
        goto failed;
    s_wifi_initialized = true;
    s_wifi_stack_ready = true;
    return true;

failed:
    ESP_LOGW(TAG, "Wi-Fi stack setup failed (%s)", esp_err_to_name(err));
    if (s_wifi_initialized) {
        (void)esp_wifi_deinit();
        s_wifi_initialized = false;
    }
    if (s_sta_netif != NULL) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
    return false;
}

QUOTA_TESTABLE void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (data)
        quota_portable_service_disconnected(((wifi_event_sta_disconnected_t *)data)->reason);
    if (id != WIFI_EVENT_STA_DISCONNECTED)
        return;
    bool sleeping = quota_service_display_sleeping();
    quota_service_lock();
    quota_service_view_t *view = quota_service_view();
    bool was_connected = view->connected;
    view->connected = false;
    /* Keep disconnect facts even if stop failed; the sleeping worker never connects. */
    s_wifi_retry_pending = s_wifi_started;
    s_wifi_retry_at_ms = (int64_t)quota_monotonic_ms() + (sleeping ? 0 : s_wifi_retry_delay_ms);
    if (!sleeping && s_wifi_retry_delay_ms < WIFI_RETRY_MAX_MS) {
        s_wifi_retry_delay_ms *= 2;
        if (s_wifi_retry_delay_ms > WIFI_RETRY_MAX_MS) {
            s_wifi_retry_delay_ms = WIFI_RETRY_MAX_MS;
        }
    }
    quota_service_unlock();
    if (was_connected)
        quota_service_post(QUOTA_APP_EVENT_CONNECTION);
    if (!sleeping)
        quota_service_wake_network();
}

QUOTA_TESTABLE void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id != IP_EVENT_STA_GOT_IP)
        return;
    /* An old queued IP event can arrive after restart, before the new association. */
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK)
        return;
    quota_service_lock();
    if (!s_wifi_started || quota_service_display_sleeping()) {
        quota_service_unlock();
        return;
    }
    quota_service_view()->connected = true;
    s_wifi_retry_delay_ms = WIFI_RETRY_MIN_MS;
    s_wifi_retry_pending = false;
    quota_service_unlock();
    quota_service_post(QUOTA_APP_EVENT_CONNECTION);
    quota_service_wake_network();
}

bool quota_wifi_start(void)
{
    if (!prepare_network_stack())
        return false;
    esp_err_t err;
    if (!s_handlers_registered) {
        err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                  wifi_event_handler, NULL, &s_wifi_handler);
        if (err != ESP_OK)
            goto failed;
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler,
                                                  NULL, &s_ip_handler);
        if (err != ESP_OK) {
            (void)esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                        s_wifi_handler);
            goto failed;
        }
        s_handlers_registered = true;
        err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (err != ESP_OK)
            goto failed;
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK)
            goto failed;
    }
    if (s_wifi_started)
        return true;
    err = esp_wifi_start();
    if (err != ESP_OK)
        goto failed;
    /* Restarting retains the RAM configuration and the provider deadline. */
    quota_service_lock();
    s_wifi_started = true;
    s_wifi_retry_pending = true;
    s_wifi_retry_at_ms = (int64_t)quota_monotonic_ms();
    quota_service_unlock();
    return true;

failed:
    ESP_LOGW(TAG, "Wi-Fi init failed (%s)", esp_err_to_name(err));
    if (s_handlers_registered) {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                    s_wifi_handler);
        (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler);
        s_handlers_registered = false;
    }
    return false;
}

bool quota_wifi_stop(void)
{
    if (!s_wifi_started)
        return true;
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi sleep stop failed (%s)", esp_err_to_name(err));
        return false;
    }
    quota_service_lock();
    s_wifi_started = false;
    quota_service_view_t *view = quota_service_view();
    bool was_connected = view->connected;
    view->connected = false;
    s_wifi_retry_pending = false;
    quota_service_unlock();
    if (was_connected)
        quota_service_post(QUOTA_APP_EVENT_CONNECTION);
    return true;
}

bool quota_wifi_started(void)
{
    return s_wifi_started;
}
