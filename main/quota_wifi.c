#include "quota_wifi.h"
#include "quota_portable_service.h"
#include "quota_service.h"
#include "quota_testable.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include <string.h>

#define WIFI_RETRY_MIN_MS 2000U
#define WIFI_RETRY_MAX_MS 30000U
#define WIFI_FENCE_RETRY_MS 50U
#define WIFI_STAGE_TIMEOUT_MS 1000U
#define WIFI_QUARANTINE_MS 30000U

static const char *TAG = "quota_wifi";
static const char *const QUOTA_WIFI_FENCE_EVENT = "QUOTA_WIFI_FENCE";
enum { WIFI_FENCE_1 = 1, WIFI_FENCE_2 = 2 };

typedef enum {
    RETIRE_IDLE,
    RETIRE_STOP,
    RETIRE_STOP_EVENT,
    RETIRE_DEINIT,
    RETIRE_FENCE_1,
    RETIRE_NETIF_STOP,
    RETIRE_FENCE_2,
    RETIRE_VERIFY,
    RETIRE_QUARANTINED,
} retire_stage_t;

static bool s_netif_stack_ready;
static bool s_event_loop_ready;
static bool s_default_handlers_ready;
QUOTA_TESTABLE bool s_wifi_initialized;
QUOTA_TESTABLE bool s_wifi_started;
static bool s_sta_netif_attached;
static esp_netif_t *s_sta_netif;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_scan_handler;
static esp_event_handler_instance_t s_stop_handler;
static esp_event_handler_instance_t s_ip_handler;
static esp_event_handler_instance_t s_fence1_handler;
static esp_event_handler_instance_t s_fence2_handler;
QUOTA_TESTABLE bool s_handlers_registered;
QUOTA_TESTABLE bool s_wifi_retry_pending;
QUOTA_TESTABLE uint32_t s_wifi_retry_delay_ms = WIFI_RETRY_MIN_MS;
QUOTA_TESTABLE int64_t s_wifi_retry_at_ms;
QUOTA_TESTABLE wifi_mode_t s_driver_mode = WIFI_MODE_NULL;
QUOTA_TESTABLE uint32_t s_admitted_request_id;
QUOTA_TESTABLE retire_stage_t s_retire_stage;
QUOTA_TESTABLE bool s_scan_active;
QUOTA_TESTABLE bool s_scan_complete;
static bool s_scan_list_owned;
static bool s_stop_event_seen;
static bool s_fence_ack;
static bool s_retiring;
static bool s_quarantined;
static retire_stage_t s_quarantine_resume;
static uint32_t s_fence_token, s_expected_fence_token;
static int32_t s_expected_fence_id;
static uint64_t s_stage_started_ms, s_next_fence_post_ms, s_quarantine_retry_ms;
static portMUX_TYPE s_event_mux = portMUX_INITIALIZER_UNLOCKED;

void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
void scan_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
void stop_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
void fence_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);

static bool mode_has_sta(wifi_mode_t mode)
{
    return mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA;
}

static uint32_t next_id(uint32_t *value)
{
    uint32_t result = ++*value;
    if (!result)
        result = ++*value;
    return result;
}

static void update_disconnected_view(void)
{
    quota_service_lock();
    bool was_connected = quota_service_view()->connected;
    quota_service_view()->connected = false;
    quota_service_unlock();
    if (was_connected)
        quota_service_post(QUOTA_APP_EVENT_CONNECTION);
}

static bool register_handlers(void)
{
    if (s_handlers_registered)
        return true;

    esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                        wifi_event_handler, NULL, &s_wifi_handler);
    if (err != ESP_OK)
        goto failed;
    err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, scan_event_handler,
                                              NULL, &s_scan_handler);
    if (err != ESP_OK)
        goto failed;
    err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_STOP, stop_event_handler,
                                              NULL, &s_stop_handler);
    if (err != ESP_OK)
        goto failed;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL,
                                              &s_ip_handler);
    if (err != ESP_OK)
        goto failed;
    err = esp_event_handler_instance_register(QUOTA_WIFI_FENCE_EVENT, WIFI_FENCE_1,
                                              fence_event_handler, NULL, &s_fence1_handler);
    if (err != ESP_OK)
        goto failed;
    err = esp_event_handler_instance_register(QUOTA_WIFI_FENCE_EVENT, WIFI_FENCE_2,
                                              fence_event_handler, NULL, &s_fence2_handler);
    if (err != ESP_OK)
        goto failed;
    s_handlers_registered = true;
    return true;

failed:
    ESP_LOGW(TAG, "Wi-Fi event registration failed (%s)", esp_err_to_name(err));
    if (s_fence1_handler)
        (void)esp_event_handler_instance_unregister(QUOTA_WIFI_FENCE_EVENT, WIFI_FENCE_1,
                                                    s_fence1_handler);
    if (s_ip_handler)
        (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler);
    if (s_stop_handler)
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_STOP,
                                                    s_stop_handler);
    if (s_scan_handler)
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                                    s_scan_handler);
    if (s_wifi_handler)
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                    s_wifi_handler);
    if (s_fence2_handler)
        (void)esp_event_handler_instance_unregister(QUOTA_WIFI_FENCE_EVENT, WIFI_FENCE_2,
                                                    s_fence2_handler);
    s_fence1_handler = s_fence2_handler = s_ip_handler = NULL;
    s_stop_handler = s_scan_handler = s_wifi_handler = NULL;
    return false;
}

static bool prepare_network_stack(void)
{
    esp_err_t err;
    if (!s_netif_stack_ready) {
        err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
            goto failed;
        s_netif_stack_ready = true;
    }
    if (!s_event_loop_ready) {
        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
            goto failed;
        s_event_loop_ready = true;
    }
    if (!s_sta_netif) {
        esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_WIFI_STA();
        s_sta_netif = esp_netif_new(&netif_config);
        if (!s_sta_netif) {
            err = ESP_ERR_NO_MEM;
            goto failed;
        }
    }
    if (!s_sta_netif_attached) {
        err = esp_netif_attach_wifi_station(s_sta_netif);
        if (err != ESP_OK)
            goto failed;
        s_sta_netif_attached = true;
    }
    if (!s_default_handlers_ready) {
        err = esp_wifi_set_default_wifi_sta_handlers();
        if (err != ESP_OK)
            goto failed;
        s_default_handlers_ready = true;
    }
    return register_handlers();

failed:
    ESP_LOGW(TAG, "Wi-Fi stack setup failed (%s)", esp_err_to_name(err));
    return false;
}

static void begin_stage(retire_stage_t stage)
{
    uint64_t now = quota_monotonic_ms();
    portENTER_CRITICAL(&s_event_mux);
    s_retire_stage = stage;
    s_fence_ack = false;
    s_expected_fence_token = 0;
    s_expected_fence_id = 0;
    if (stage == RETIRE_STOP_EVENT)
        s_stop_event_seen = false;
    portEXIT_CRITICAL(&s_event_mux);
    s_stage_started_ms = now;
    s_next_fence_post_ms = now;
}

static void quarantine(retire_stage_t resume)
{
    portENTER_CRITICAL(&s_event_mux);
    s_quarantine_resume = resume;
    s_retire_stage = RETIRE_QUARANTINED;
    s_quarantined = true;
    portEXIT_CRITICAL(&s_event_mux);
    s_quarantine_retry_ms = quota_monotonic_ms() + WIFI_QUARANTINE_MS;
    ESP_LOGW(TAG, "Wi-Fi lifecycle quarantined; retry in %u ms", WIFI_QUARANTINE_MS);
}

static void clear_admission(void)
{
    portENTER_CRITICAL(&s_event_mux);
    s_admitted_request_id = 0;
    s_driver_mode = WIFI_MODE_NULL;
    portEXIT_CRITICAL(&s_event_mux);
}

static void finish_retirement(void)
{
    clear_admission();
    portENTER_CRITICAL(&s_event_mux);
    s_scan_active = false;
    s_scan_complete = false;
    s_scan_list_owned = false;
    s_retiring = false;
    s_quarantined = false;
    s_retire_stage = RETIRE_IDLE;
    s_fence_ack = false;
    s_expected_fence_token = 0;
    s_expected_fence_id = 0;
    portEXIT_CRITICAL(&s_event_mux);
}

static void start_fence(int32_t id)
{
    begin_stage(id == WIFI_FENCE_1 ? RETIRE_FENCE_1 : RETIRE_FENCE_2);
    portENTER_CRITICAL(&s_event_mux);
    s_expected_fence_id = id;
    s_expected_fence_token = next_id(&s_fence_token);
    portEXIT_CRITICAL(&s_event_mux);
}

static quota_wifi_result_t poll_fence(void)
{
    uint64_t now = quota_monotonic_ms();
    portENTER_CRITICAL(&s_event_mux);
    bool acknowledged = s_fence_ack;
    portEXIT_CRITICAL(&s_event_mux);
    if (acknowledged) {
        if (s_retire_stage == RETIRE_FENCE_1)
            begin_stage(RETIRE_NETIF_STOP);
        else
            begin_stage(RETIRE_VERIFY);
        return QUOTA_WIFI_WAIT;
    }
    if (now - s_stage_started_ms >= WIFI_STAGE_TIMEOUT_MS) {
        quarantine(s_retire_stage);
        return QUOTA_WIFI_WAIT;
    }
    if (now >= s_next_fence_post_ms) {
        uint32_t token = s_expected_fence_token;
        esp_err_t err =
            esp_event_post(QUOTA_WIFI_FENCE_EVENT, s_expected_fence_id, &token, sizeof(token), 0);
        s_next_fence_post_ms = now + WIFI_FENCE_RETRY_MS;
        if (err != ESP_OK)
            ESP_LOGW(TAG, "Wi-Fi fence post failed (%s)", esp_err_to_name(err));
    }
    return QUOTA_WIFI_WAIT;
}

void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_STOP) {
        portENTER_CRITICAL(&s_event_mux);
        if (s_retire_stage == RETIRE_STOP_EVENT)
            s_stop_event_seen = true;
        portEXIT_CRITICAL(&s_event_mux);
        quota_service_wake_network();
        return;
    }
    if (id != WIFI_EVENT_STA_DISCONNECTED)
        return;

    const wifi_event_sta_disconnected_t *event = data;
    bool sleeping = quota_service_display_sleeping();
    if (event) {
        size_t length = event->ssid_len;
        if (length > sizeof(event->ssid))
            length = sizeof(event->ssid);
        ESP_LOGW(TAG, "Wi-Fi disconnected: reason=%u, sleeping=%u", (unsigned)event->reason,
                 (unsigned)sleeping);
    } else {
        ESP_LOGW(TAG, "Wi-Fi disconnected: reason=unknown, sleeping=%u", (unsigned)sleeping);
    }
    portENTER_CRITICAL(&s_event_mux);
    bool accepted = s_retire_stage == RETIRE_IDLE && s_wifi_started && mode_has_sta(s_driver_mode);
    portEXIT_CRITICAL(&s_event_mux);
    if (!accepted || !event)
        return;
    size_t length = event->ssid_len;
    if (length > sizeof(event->ssid))
        length = sizeof(event->ssid);
    quota_portable_service_disconnected(event->reason, event->ssid, length);
    update_disconnected_view();
    int64_t retry_at = (int64_t)quota_monotonic_ms();
    portENTER_CRITICAL(&s_event_mux);
    s_wifi_retry_pending = true;
    s_wifi_retry_at_ms = retry_at + (sleeping ? 0 : s_wifi_retry_delay_ms);
    if (!sleeping && s_wifi_retry_delay_ms < WIFI_RETRY_MAX_MS) {
        s_wifi_retry_delay_ms *= 2;
        if (s_wifi_retry_delay_ms > WIFI_RETRY_MAX_MS)
            s_wifi_retry_delay_ms = WIFI_RETRY_MAX_MS;
    }
    portEXIT_CRITICAL(&s_event_mux);
    if (!sleeping)
        quota_service_wake_network();
}

void scan_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id != WIFI_EVENT_SCAN_DONE)
        return;
    const wifi_event_sta_scan_done_t *event = data;
    portENTER_CRITICAL(&s_event_mux);
    bool accepted = s_retire_stage == RETIRE_IDLE && s_wifi_started &&
                    mode_has_sta(s_driver_mode) && s_scan_active;
    bool success = event && event->status == ESP_OK;
    if (accepted) {
        s_scan_active = false;
        s_scan_complete = success;
        s_scan_list_owned = true;
    }
    portEXIT_CRITICAL(&s_event_mux);
    if (!accepted)
        return;
    quota_portable_service_scan_done(success);
    quota_service_wake_network();
}

void stop_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    wifi_event_handler(arg, base, id, data);
}

void fence_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (!data)
        return;
    uint32_t token;
    memcpy(&token, data, sizeof(token));
    portENTER_CRITICAL(&s_event_mux);
    bool expected = (id == s_expected_fence_id) && token == s_expected_fence_token &&
                    (s_retire_stage == RETIRE_FENCE_1 || s_retire_stage == RETIRE_FENCE_2);
    if (expected)
        s_fence_ack = true;
    portEXIT_CRITICAL(&s_event_mux);
    if (expected)
        quota_service_wake_network();
}

void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id != IP_EVENT_STA_GOT_IP || quota_service_display_sleeping())
        return;
    portENTER_CRITICAL(&s_event_mux);
    bool accepted = s_retire_stage == RETIRE_IDLE && s_wifi_started && mode_has_sta(s_driver_mode);
    portEXIT_CRITICAL(&s_event_mux);
    if (!accepted)
        return;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK)
        return;
    quota_service_lock();
    quota_service_view()->connected = true;
    portENTER_CRITICAL(&s_event_mux);
    s_wifi_retry_delay_ms = WIFI_RETRY_MIN_MS;
    s_wifi_retry_pending = false;
    portEXIT_CRITICAL(&s_event_mux);
    quota_service_unlock();
    quota_service_post(QUOTA_APP_EVENT_CONNECTION);
    quota_service_wake_network();
}

quota_wifi_result_t quota_wifi_prepare(uint32_t request_id, wifi_mode_t mode)
{
    if (!request_id || (mode != WIFI_MODE_STA && mode != WIFI_MODE_AP && mode != WIFI_MODE_APSTA))
        return QUOTA_WIFI_FAILED;
    portENTER_CRITICAL(&s_event_mux);
    bool retiring = s_retire_stage != RETIRE_IDLE || s_retiring || s_quarantined;
    bool initialized = s_wifi_initialized;
    uint32_t admitted_request_id = s_admitted_request_id;
    wifi_mode_t driver_mode = s_driver_mode;
    portEXIT_CRITICAL(&s_event_mux);
    if (retiring)
        return QUOTA_WIFI_WAIT;
    if (!prepare_network_stack())
        return QUOTA_WIFI_FAILED;
    if (initialized) {
        if (admitted_request_id == request_id && driver_mode == mode)
            return QUOTA_WIFI_READY;
        quota_wifi_retire_begin();
        return QUOTA_WIFI_WAIT;
    }

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi init failed (%s)", esp_err_to_name(err));
        return QUOTA_WIFI_FAILED;
    }
    portENTER_CRITICAL(&s_event_mux);
    s_wifi_initialized = true;
    portEXIT_CRITICAL(&s_event_mux);
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK)
        err = esp_wifi_set_mode(mode);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi prepare failed (%s)", esp_err_to_name(err));
        quota_wifi_retire_begin();
        return QUOTA_WIFI_WAIT;
    }
    portENTER_CRITICAL(&s_event_mux);
    s_admitted_request_id = request_id;
    s_driver_mode = mode;
    s_wifi_retry_pending = false;
    portEXIT_CRITICAL(&s_event_mux);
    return QUOTA_WIFI_READY;
}

void quota_wifi_retire_begin(void)
{
    portENTER_CRITICAL(&s_event_mux);
    bool already_retiring = s_retire_stage != RETIRE_IDLE || s_retiring;
    if (!already_retiring) {
        s_retiring = true;
        s_quarantined = false;
    }
    portEXIT_CRITICAL(&s_event_mux);
    if (already_retiring)
        return;
    clear_admission();
    portENTER_CRITICAL(&s_event_mux);
    bool needs_stop = s_wifi_initialized || s_wifi_started || s_scan_list_owned;
    portEXIT_CRITICAL(&s_event_mux);
    begin_stage(needs_stop ? RETIRE_STOP : RETIRE_IDLE);
    if (s_retire_stage == RETIRE_IDLE)
        finish_retirement();
}

quota_wifi_result_t quota_wifi_retire_poll(void)
{
    if (s_retire_stage == RETIRE_IDLE)
        return QUOTA_WIFI_READY;
    if (s_retire_stage == RETIRE_QUARANTINED) {
        /* Retirement is cleanup for an already-owned operation. It must keep retrying while
         * asleep so a failed SDK stop cannot leave the radio running indefinitely. Admission
         * remains closed until the complete fence/netif/verification sequence finishes. */
        if (quota_monotonic_ms() < s_quarantine_retry_ms)
            return QUOTA_WIFI_WAIT;
        s_quarantined = false;
        if (s_quarantine_resume == RETIRE_FENCE_1)
            start_fence(WIFI_FENCE_1);
        else if (s_quarantine_resume == RETIRE_FENCE_2)
            start_fence(WIFI_FENCE_2);
        else
            begin_stage(s_quarantine_resume);
    }

    switch (s_retire_stage) {
    case RETIRE_STOP: {
        portENTER_CRITICAL(&s_event_mux);
        bool scan_active = s_scan_active;
        bool scan_list_owned = s_scan_list_owned;
        portEXIT_CRITICAL(&s_event_mux);
        if (scan_active) {
            portENTER_CRITICAL(&s_event_mux);
            s_scan_active = false;
            s_scan_complete = false;
            portEXIT_CRITICAL(&s_event_mux);
            esp_err_t err = esp_wifi_scan_stop();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Wi-Fi scan stop failed (%s)", esp_err_to_name(err));
                quarantine(RETIRE_STOP);
                return QUOTA_WIFI_WAIT;
            }
        }
        if (scan_list_owned) {
            esp_err_t err = esp_wifi_clear_ap_list();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Wi-Fi scan results clear failed (%s)", esp_err_to_name(err));
                quarantine(RETIRE_STOP);
                return QUOTA_WIFI_WAIT;
            }
            portENTER_CRITICAL(&s_event_mux);
            s_scan_list_owned = false;
            portEXIT_CRITICAL(&s_event_mux);
        }
        portENTER_CRITICAL(&s_event_mux);
        bool wifi_started = s_wifi_started;
        bool needs_sta_stop = mode_has_sta(s_driver_mode);
        portEXIT_CRITICAL(&s_event_mux);
        if (wifi_started) {
            if (needs_sta_stop)
                begin_stage(RETIRE_STOP_EVENT);
            esp_err_t err = esp_wifi_stop();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Wi-Fi stop failed (%s)", esp_err_to_name(err));
                quarantine(RETIRE_STOP);
                return QUOTA_WIFI_WAIT;
            }
            portENTER_CRITICAL(&s_event_mux);
            s_wifi_started = false;
            portEXIT_CRITICAL(&s_event_mux);
            update_disconnected_view();
            if (needs_sta_stop)
                return QUOTA_WIFI_WAIT;
        }
        begin_stage(RETIRE_DEINIT);
        return QUOTA_WIFI_WAIT;
    }
    case RETIRE_STOP_EVENT:
        if (s_stop_event_seen) {
            begin_stage(RETIRE_DEINIT);
            return QUOTA_WIFI_WAIT;
        }
        if (quota_monotonic_ms() - s_stage_started_ms >= WIFI_STAGE_TIMEOUT_MS) {
            /* deinit is the stronger terminal when the public STOP notification was lost. */
            quarantine(RETIRE_DEINIT);
        }
        return QUOTA_WIFI_WAIT;
    case RETIRE_DEINIT:
        if (s_wifi_initialized) {
            esp_err_t err = esp_wifi_deinit();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Wi-Fi deinit failed (%s)", esp_err_to_name(err));
                quarantine(RETIRE_DEINIT);
                return QUOTA_WIFI_WAIT;
            }
            portENTER_CRITICAL(&s_event_mux);
            s_wifi_initialized = false;
            portEXIT_CRITICAL(&s_event_mux);
        }
        start_fence(WIFI_FENCE_1);
        return QUOTA_WIFI_WAIT;
    case RETIRE_FENCE_1:
    case RETIRE_FENCE_2:
        return poll_fence();
    case RETIRE_NETIF_STOP:
        if (s_sta_netif)
            esp_netif_action_stop(s_sta_netif, WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL);
        start_fence(WIFI_FENCE_2);
        return QUOTA_WIFI_WAIT;
    case RETIRE_VERIFY: {
        esp_netif_ip_info_t info = {0};
        esp_netif_dhcp_status_t dhcp = ESP_NETIF_DHCP_STOPPED;
        if (!s_sta_netif || esp_netif_is_netif_up(s_sta_netif) ||
            esp_netif_get_ip_info(s_sta_netif, &info) != ESP_OK || info.ip.addr != 0 ||
            esp_netif_dhcpc_get_status(s_sta_netif, &dhcp) != ESP_OK ||
            dhcp == ESP_NETIF_DHCP_STARTED) {
            quarantine(RETIRE_NETIF_STOP);
            return QUOTA_WIFI_WAIT;
        }
        finish_retirement();
        return QUOTA_WIFI_READY;
    }
    case RETIRE_IDLE:
    case RETIRE_QUARANTINED:
        return QUOTA_WIFI_WAIT;
    }
    return QUOTA_WIFI_FAILED;
}

uint64_t quota_wifi_next_deadline_ms(bool sleeping)
{
    (void)sleeping;
    if (s_retire_stage == RETIRE_IDLE)
        return UINT64_MAX;
    if (s_retire_stage == RETIRE_QUARANTINED)
        return s_quarantine_retry_ms;
    if (s_retire_stage == RETIRE_FENCE_1 || s_retire_stage == RETIRE_FENCE_2) {
        uint64_t deadline = s_stage_started_ms + WIFI_STAGE_TIMEOUT_MS;
        return s_next_fence_post_ms < deadline ? s_next_fence_post_ms : deadline;
    }
    if (s_retire_stage == RETIRE_STOP_EVENT)
        return s_stage_started_ms + WIFI_STAGE_TIMEOUT_MS;
    return quota_monotonic_ms();
}

bool quota_wifi_start(void)
{
    portENTER_CRITICAL(&s_event_mux);
    bool can_start = s_wifi_initialized && s_admitted_request_id && s_retire_stage == RETIRE_IDLE &&
                     !s_retiring && !s_quarantined;
    bool already_started = s_wifi_started;
    portEXIT_CRITICAL(&s_event_mux);
    if (!can_start)
        return false;
    if (already_started)
        return true;
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi start failed (%s)", esp_err_to_name(err));
        quota_wifi_retire_begin();
        return false;
    }
    portENTER_CRITICAL(&s_event_mux);
    s_wifi_started = true;
    s_wifi_retry_pending = mode_has_sta(s_driver_mode);
    s_wifi_retry_at_ms = (int64_t)quota_monotonic_ms();
    portEXIT_CRITICAL(&s_event_mux);
    return true;
}

bool quota_wifi_stop(void)
{
    quota_wifi_retire_begin();
    /* Advance through the synchronous SDK stop call so the bool describes radio state. */
    (void)quota_wifi_retire_poll();
    return !quota_wifi_started();
}

bool quota_wifi_started(void)
{
    portENTER_CRITICAL(&s_event_mux);
    bool started = s_wifi_started;
    portEXIT_CRITICAL(&s_event_mux);
    return started;
}

bool quota_wifi_operation_active(uint32_t request_id, wifi_mode_t mode)
{
    portENTER_CRITICAL(&s_event_mux);
    bool active = request_id && s_wifi_initialized && s_wifi_started &&
                  s_admitted_request_id == request_id && s_driver_mode == mode &&
                  s_retire_stage == RETIRE_IDLE && !s_retiring && !s_quarantined;
    portEXIT_CRITICAL(&s_event_mux);
    return active;
}

bool quota_wifi_scan_start(const wifi_scan_config_t *config)
{
    if (!config)
        return false;
    portENTER_CRITICAL(&s_event_mux);
    bool admitted = s_wifi_started && s_admitted_request_id && mode_has_sta(s_driver_mode) &&
                    s_retire_stage == RETIRE_IDLE && !s_retiring && !s_quarantined &&
                    !s_scan_active && !s_scan_complete;
    if (admitted) {
        /* The SDK may dispatch SCAN_DONE synchronously in tests or on an error path. */
        s_scan_active = true;
        s_scan_list_owned = true;
    }
    portEXIT_CRITICAL(&s_event_mux);
    if (!admitted)
        return false;
    esp_err_t err = esp_wifi_scan_start(config, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan start failed (%s)", esp_err_to_name(err));
        portENTER_CRITICAL(&s_event_mux);
        if (s_scan_active) {
            s_scan_active = false;
            s_scan_list_owned = false;
        }
        portEXIT_CRITICAL(&s_event_mux);
        return false;
    }
    return true;
}

esp_err_t quota_wifi_scan_get_ap_num(uint16_t *number)
{
    portENTER_CRITICAL(&s_event_mux);
    bool complete = s_scan_complete && s_retire_stage == RETIRE_IDLE;
    portEXIT_CRITICAL(&s_event_mux);
    if (!number || !complete)
        return ESP_ERR_INVALID_STATE;
    return esp_wifi_scan_get_ap_num(number);
}

esp_err_t quota_wifi_scan_get_ap_record(wifi_ap_record_t *record)
{
    portENTER_CRITICAL(&s_event_mux);
    bool complete = s_scan_complete && s_retire_stage == RETIRE_IDLE;
    portEXIT_CRITICAL(&s_event_mux);
    if (!record || !complete)
        return ESP_ERR_INVALID_STATE;
    return esp_wifi_scan_get_ap_record(record);
}

esp_err_t quota_wifi_scan_clear_ap_list(void)
{
    portENTER_CRITICAL(&s_event_mux);
    bool initialized = s_wifi_initialized;
    if (!initialized) {
        s_scan_active = false;
        s_scan_complete = false;
        s_scan_list_owned = false;
        portEXIT_CRITICAL(&s_event_mux);
        return ESP_OK;
    }
    bool busy = s_scan_active || s_retire_stage != RETIRE_IDLE || s_retiring || s_quarantined;
    portEXIT_CRITICAL(&s_event_mux);
    if (busy)
        return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_wifi_clear_ap_list();
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_event_mux);
        s_scan_complete = false;
        s_scan_list_owned = false;
        portEXIT_CRITICAL(&s_event_mux);
    }
    return err;
}
