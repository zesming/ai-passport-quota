#include "quota_service.h"
#include "quota_portable_service.h"
#include "quota_store.h"

#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/semphr.h"
#include "freertos/task.h"

#include <assert.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "quota_service";

#define EVENT_QUEUE_DEPTH 16
#define NETWORK_TASK_STACK 10240
#define NETWORK_TASK_PRIORITY 5
#define WIFI_RETRY_MIN_MS 2000
#define WIFI_RETRY_MAX_MS 30000
#define HTTP_TIMEOUT_MS 8000
#define HTTP_BODY_BYTES QUOTA_MAX_SNAPSHOT_BYTES
#define NVS_NAMESPACE "ai_quota"
#define NVS_CONFIG_KEY "device_cfg"
#define NVS_SNAPSHOT_KEY "quota_cache"
#define NVS_SCREEN_TIMEOUT_KEY "screen_to"
#define NVS_BALANCE_KEY "balance_cache"
#define SNAPSHOT_POLL_MS 10000
#define SNAPSHOT_RETRY_MS 30000
#define SERVER_TIME_PERSIST_MS 900000
/* Drop long-offline snapshots after a month; their source observations are already stale. */
#define SNAPSHOT_CACHE_MAX_AGE_SECONDS (30ULL * 24 * 60 * 60)
#define SNAPSHOT_CACHE_WRITE_INTERVAL_SECONDS (SERVER_TIME_PERSIST_MS / 1000ULL)
#define SELECTION_PERSIST_RETRY_MS 30000
#define STORED_CONFIG_MAGIC 0x41514931U
#define STORED_CONFIG_VERSION 1U
#define STORED_SNAPSHOT_MAGIC 0x41515331U
#define STORED_SNAPSHOT_VERSION 1U
#define STORED_BALANCE_MAGIC 0x41514231U

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t config_size;
    uint32_t crc32;
    quota_device_config_t config;
} stored_config_t;

typedef struct {
    uint64_t revision;
    uint8_t account_count;
    quota_account_t accounts[QUOTA_MAX_ACCOUNTS];
} cached_snapshot_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t snapshot_size;
    uint32_t config_identity;
    uint64_t stored_at;
    cached_snapshot_t snapshot;
    uint32_t crc32;
} stored_snapshot_t;

typedef struct {
    uint32_t magic;
    uint32_t config_identity;
    uint64_t revision;
    uint64_t stored_at;
    quota_balance_t balances[QUOTA_MAX_ACCOUNTS];
    uint32_t crc32;
} stored_balance_t;

typedef struct {
    char bytes[HTTP_BODY_BYTES + 1];
    size_t length;
    bool overflow;
    bool redirect;
} http_body_t;

typedef struct {
    bool sleeping;
    bool wake_fetch_pending;
    uint32_t generation;
} display_scheduler_t;

typedef struct {
    bool sleeping;
    uint32_t generation;
} display_state_t;

typedef enum {
    HTTP_REQUEST_NOT_ADMITTED,
    HTTP_REQUEST_ADMITTED,
    HTTP_REQUEST_DEFERRED,
} http_request_outcome_t;

static QueueHandle_t s_events;
static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_network_task;
static atomic_bool s_pairing_requested;
static portMUX_TYPE s_display_state_mux = portMUX_INITIALIZER_UNLOCKED;
static display_scheduler_t s_display_scheduler = {.generation = 1};
static quota_service_view_t s_view;
static quota_device_config_t s_config;
static bool s_nvs_ready;
static bool s_has_config;
static bool s_wifi_stack_ready;
static bool s_wifi_initialized;
static bool s_wifi_started;
static esp_netif_t *s_sta_netif;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static bool s_handlers_registered;
static bool s_wifi_retry_pending;
static uint32_t s_wifi_retry_delay_ms = WIFI_RETRY_MIN_MS;
static int64_t s_wifi_retry_at_ms;
static uint32_t s_config_generation;
static bool s_fetch_after_connect;
static uint16_t s_saved_screen_timeout_seconds = QUOTA_SCREEN_TIMEOUT_DEFAULT_SECONDS;
static bool s_pairing_screen_open;
static int64_t s_pairing_opened_at_ms;
static uint64_t s_snapshot_cache_saved_at;

/* Mutually exclusive short-lived transport, migration and USB scratch. */
typedef union {
    struct { http_body_t http_body; quota_device_config_t network_config; };
    struct { stored_snapshot_t stored_snapshot; stored_balance_t stored_balances; };
    quota_device_config_t provision_config;
} companion_workspace_t;

static companion_workspace_t *s_companion_work;
static quota_frame_decoder_t *s_usb_decoder;
static bool s_balance_cache_present;

static companion_workspace_t *companion_work(void)
{
    assert(s_companion_work != NULL);
    return s_companion_work;
}

/* Keep the existing protocol helpers on one explicitly leased workspace. */
#define s_http_body (companion_work()->http_body)
#define s_network_config (companion_work()->network_config)
#define s_provision_config (companion_work()->provision_config)
#define s_stored_snapshot (companion_work()->stored_snapshot)
#define s_stored_balances (companion_work()->stored_balances)

static bool acquire_companion_work(void)
{
    if (!s_companion_work) s_companion_work = calloc(1, sizeof(*s_companion_work));
    return s_companion_work != NULL;
}

static void release_companion_work(void)
{
    if (s_companion_work) {
        quota_portable_clear_secret(s_companion_work, sizeof(*s_companion_work));
        free(s_companion_work);
        s_companion_work = NULL;
    }
}

static bool pairing_requested(void)
{
    return atomic_load(&s_pairing_requested);
}

static void mutex_lock(void)
{
    if (s_mutex != NULL) (void)xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void mutex_unlock(void)
{
    if (s_mutex != NULL) (void)xSemaphoreGive(s_mutex);
}

static uint64_t current_epoch(void)
{
    time_t now = time(NULL);
    return now > 0 ? (uint64_t)now : 0;
}

static uint64_t monotonic_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static display_state_t display_state_snapshot(void)
{
    display_state_t state;
    portENTER_CRITICAL(&s_display_state_mux);
    state.sleeping = s_display_scheduler.sleeping;
    state.generation = s_display_scheduler.generation;
    portEXIT_CRITICAL(&s_display_state_mux);
    return state;
}

static bool display_generation_is_current(uint32_t generation)
{
    bool current;
    portENTER_CRITICAL(&s_display_state_mux);
    current = !s_display_scheduler.sleeping &&
              s_display_scheduler.generation == generation;
    portEXIT_CRITICAL(&s_display_state_mux);
    return current;
}



static uint32_t crc32_update(uint32_t crc, const void *data, size_t length)
{
    const uint8_t *bytes = data;
    for (size_t i = 0; i < length; i++) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return crc;
}

static uint32_t crc32_bytes(const void *data, size_t length)
{
    return ~crc32_update(UINT32_MAX, data, length);
}

static bool bounded_c_string(const char *value, size_t capacity, bool allow_empty)
{
    if (value == NULL) return false;
    const char *end = memchr(value, '\0', capacity);
    return end != NULL && (allow_empty || end != value);
}

static bool config_is_well_formed(const quota_device_config_t *config)
{
    char host[16];
    return config != NULL &&
           bounded_c_string(config->request_id, sizeof(config->request_id), true) &&
           bounded_c_string(config->ssid, sizeof(config->ssid), false) &&
           bounded_c_string(config->password, sizeof(config->password), true) &&
           bounded_c_string(config->base_url, sizeof(config->base_url), false) &&
           bounded_c_string(config->pair_token, sizeof(config->pair_token), false) &&
           bounded_c_string(config->server_cert_pem, sizeof(config->server_cert_pem), false) &&
           bounded_c_string(config->selected_account_id,
                            sizeof(config->selected_account_id), true) &&
           strlen(config->ssid) <= QUOTA_SSID_MAX_BYTES &&
           strlen(config->password) <= QUOTA_PASSWORD_MAX_BYTES &&
           strlen(config->base_url) <= QUOTA_BASE_URL_MAX_BYTES &&
           strlen(config->pair_token) == QUOTA_PAIR_TOKEN_BYTES &&
           strlen(config->server_cert_pem) <= QUOTA_CERT_MAX_BYTES &&
           quota_url_is_private_ipv4(config->base_url, host) &&
           quota_pair_token_is_valid(config->pair_token) &&
           (config->selected_account_id[0] == '\0' ||
            quota_id_is_valid(config->selected_account_id)) &&
           config->server_time > 0 && quota_refresh_seconds_is_valid(config->refresh_seconds) &&
           strncmp(config->server_cert_pem, "-----BEGIN CERTIFICATE-----", 27) == 0 &&
           strstr(config->server_cert_pem, "-----END CERTIFICATE-----") != NULL;
}





static quota_store_read_result_t nvs_read_config_result(quota_device_config_t *out)
{
    if(!s_nvs_ready||!out)return QUOTA_STORE_READ_IO_ERROR;
    stored_config_t *record=calloc(1,sizeof(*record));if(!record)return QUOTA_STORE_READ_NO_MEMORY;
    nvs_handle_t handle;esp_err_t err=nvs_open(NVS_NAMESPACE,NVS_READONLY,&handle);size_t length=sizeof(*record);
    if(err==ESP_OK){err=nvs_get_blob(handle,NVS_CONFIG_KEY,record,&length);nvs_close(handle);}
    quota_store_read_result_t result=err==ESP_ERR_NVS_NOT_FOUND?QUOTA_STORE_READ_MISSING:(err==ESP_OK||err==ESP_ERR_NVS_INVALID_LENGTH)?QUOTA_STORE_READ_INVALID:QUOTA_STORE_READ_IO_ERROR;
    if(err==ESP_OK&&length==sizeof(*record)&&record->magic==STORED_CONFIG_MAGIC&&record->version==STORED_CONFIG_VERSION&&record->config_size==sizeof(record->config)&&record->crc32==crc32_bytes(&record->config,sizeof(record->config))&&config_is_well_formed(&record->config)){*out=record->config;result=QUOTA_STORE_READ_OK;}
    quota_portable_clear_secret(record,sizeof(*record));free(record);return result;
}
/* Separate key: never change the existing device_cfg size/version/CRC contract. */
static uint16_t nvs_load_screen_timeout(void)
{
    uint16_t seconds = QUOTA_SCREEN_TIMEOUT_DEFAULT_SECONDS;
    nvs_handle_t handle;
    if (s_nvs_ready && nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        uint16_t saved = 0;
        if (nvs_get_u16(handle, NVS_SCREEN_TIMEOUT_KEY, &saved) == ESP_OK &&
            quota_screen_timeout_is_valid(saved)) seconds = saved;
        nvs_close(handle);
    }
    s_saved_screen_timeout_seconds = seconds;
    return seconds;
}

static uint32_t config_cache_identity(const quota_device_config_t *config)
{
    uint32_t crc = UINT32_MAX;
    crc = crc32_update(crc, config->base_url, strlen(config->base_url) + 1);
    crc = crc32_update(crc, config->pair_token, strlen(config->pair_token) + 1);
    crc = crc32_update(crc, config->server_cert_pem,
                       strlen(config->server_cert_pem) + 1);
    return ~crc;
}

static bool cached_snapshot_is_well_formed(const cached_snapshot_t *snapshot)
{
    if (snapshot == NULL || snapshot->account_count > QUOTA_MAX_ACCOUNTS) return false;
    for (size_t i = 0; i < snapshot->account_count; i++) {
        const quota_account_t *account = &snapshot->accounts[i];
        if (!bounded_c_string(account->id, sizeof(account->id), false) ||
            !quota_id_is_valid(account->id) ||
            !bounded_c_string(account->email, sizeof(account->email), true) ||
            !bounded_c_string(account->plan, sizeof(account->plan), true) ||
            !quota_utf8_is_valid(account->email, strlen(account->email)) ||
            !quota_utf8_is_valid(account->plan, strlen(account->plan)) ||
            (account->provider != QUOTA_PROVIDER_CODEX &&
             account->provider != QUOTA_PROVIDER_CLAUDE &&
             account->provider != QUOTA_PROVIDER_DEEPSEEK) ||
            (account->provider == QUOTA_PROVIDER_DEEPSEEK &&
             (account->five_hour.present || account->seven_day.present)) ||
            (account->status != QUOTA_STATUS_OK &&
             account->status != QUOTA_STATUS_WAITING &&
             account->status != QUOTA_STATUS_EXPIRED &&
             account->status != QUOTA_STATUS_ERROR &&
             account->status != QUOTA_STATUS_UNSUPPORTED) ||
            (account->has_observed_at && account->observed_at > UINT32_MAX) ||
            (account->five_hour.has_resets_at &&
             account->five_hour.resets_at > UINT32_MAX) ||
            (account->seven_day.has_resets_at &&
             account->seven_day.resets_at > UINT32_MAX) ||
            (account->five_hour.present && account->five_hour.remaining_percent > 100) ||
            (account->seven_day.present && account->seven_day.remaining_percent > 100)) {
            return false;
        }
        for (size_t previous = 0; previous < i; previous++) {
            if (strcmp(snapshot->accounts[previous].id, account->id) == 0) return false;
        }
    }
    return true;
}

static quota_store_read_result_t nvs_load_snapshot_inventory_result(const quota_device_config_t *config)
{
    if(!s_nvs_ready||!config)return QUOTA_STORE_READ_IO_ERROR;
    nvs_handle_t handle;esp_err_t err=nvs_open(NVS_NAMESPACE,NVS_READONLY,&handle);
    if(err==ESP_ERR_NVS_NOT_FOUND)return QUOTA_STORE_READ_MISSING;
    if(err!=ESP_OK)return QUOTA_STORE_READ_IO_ERROR;
    memset(&s_stored_snapshot,0,sizeof(s_stored_snapshot));size_t length=sizeof(s_stored_snapshot);
    err=nvs_get_blob(handle,NVS_SNAPSHOT_KEY,&s_stored_snapshot,&length);nvs_close(handle);
    if(err==ESP_ERR_NVS_NOT_FOUND)return QUOTA_STORE_READ_MISSING;
    if(err==ESP_ERR_NVS_INVALID_LENGTH)return QUOTA_STORE_READ_INVALID;
    if(err!=ESP_OK)return QUOTA_STORE_READ_IO_ERROR;
    if(length!=sizeof(s_stored_snapshot)||s_stored_snapshot.magic!=STORED_SNAPSHOT_MAGIC||s_stored_snapshot.version!=STORED_SNAPSHOT_VERSION||s_stored_snapshot.snapshot_size!=sizeof(s_stored_snapshot.snapshot)||!s_stored_snapshot.stored_at||s_stored_snapshot.stored_at>UINT32_MAX||s_stored_snapshot.crc32!=crc32_bytes(&s_stored_snapshot,offsetof(stored_snapshot_t,crc32))||!cached_snapshot_is_well_formed(&s_stored_snapshot.snapshot)){
        memset(&s_stored_snapshot,0,sizeof(s_stored_snapshot));return QUOTA_STORE_READ_INVALID;
    }
    if(s_stored_snapshot.config_identity!=config_cache_identity(config)){memset(&s_stored_snapshot,0,sizeof(s_stored_snapshot));return QUOTA_STORE_READ_MISSING;}
    s_snapshot_cache_saved_at=s_stored_snapshot.stored_at;return QUOTA_STORE_READ_OK;
}

static void nvs_load_balance_snapshot(const quota_device_config_t *config)
{
    s_balance_cache_present = false;
    memset(&s_stored_balances, 0, sizeof(s_stored_balances));
    nvs_handle_t handle;
    if (!s_nvs_ready || nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    size_t length = sizeof(s_stored_balances);
    esp_err_t err = nvs_get_blob(handle, NVS_BALANCE_KEY, &s_stored_balances, &length);
    nvs_close(handle);
    bool valid = err == ESP_OK && length == sizeof(s_stored_balances) &&
        s_stored_balances.magic == STORED_BALANCE_MAGIC &&
        s_stored_balances.config_identity == config_cache_identity(config) &&
        s_stored_balances.revision == s_stored_snapshot.snapshot.revision &&
        s_stored_balances.stored_at == s_stored_snapshot.stored_at &&
        s_stored_balances.crc32 == crc32_bytes(&s_stored_balances, offsetof(stored_balance_t, crc32));
    for (size_t i = 0; valid && i < QUOTA_MAX_ACCOUNTS; i++) {
        valid = quota_balance_is_valid(&s_stored_balances.balances[i]);
    }
    if (valid) {
        s_balance_cache_present = true;
    } else {
        memset(&s_stored_balances, 0, sizeof(s_stored_balances));
    }
}







static void post_event(const quota_app_event_t *event, TickType_t wait)
{
    if (s_events != NULL && event != NULL) (void)xQueueSend(s_events, event, wait);
}

static void post_simple_event(quota_app_event_kind_t kind)
{
    quota_app_event_t event = {.kind = kind};
    post_event(&event, 0);
}

static void set_system_time_if_newer(uint64_t epoch)
{
    if (epoch == 0 || epoch > UINT32_MAX) return;
    uint64_t current = current_epoch();
    if (current >= epoch) return;
    struct timeval value = {.tv_sec = (time_t)epoch, .tv_usec = 0};
    (void)settimeofday(&value, NULL);
}

static bool pairing_active_locked(uint64_t now_ms)
{
    return quota_pairing_window_active(s_pairing_screen_open, now_ms,
                                       (uint64_t)s_pairing_opened_at_ms);
}

static bool pairing_active(void)
{
    mutex_lock();
    bool active = pairing_active_locked(monotonic_ms());
    mutex_unlock();
    return active;
}

static bool prepare_network_stack(void)
{
    if (s_wifi_stack_ready) return true;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto failed;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto failed;

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_WIFI_STA();
    s_sta_netif = esp_netif_new(&netif_config);
    if (s_sta_netif == NULL) {
        err = ESP_ERR_NO_MEM;
        goto failed;
    }
    err = esp_netif_attach_wifi_station(s_sta_netif);
    if (err != ESP_OK) goto failed;
    err = esp_wifi_set_default_wifi_sta_handlers();
    if (err != ESP_OK) goto failed;

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_config);
    if (err != ESP_OK) goto failed;
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

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
#ifdef ESP_PLATFORM
    if (data) quota_portable_service_disconnected(((wifi_event_sta_disconnected_t *)data)->reason);
#endif
    if (id != WIFI_EVENT_STA_DISCONNECTED) return;
    bool sleeping = display_state_snapshot().sleeping;
    mutex_lock();
    bool was_connected = s_view.connected;
    s_view.connected = false;
    /* Keep disconnect facts even if stop failed; the sleeping worker never connects. */
    s_wifi_retry_pending = s_wifi_started;
    s_wifi_retry_at_ms = (int64_t)monotonic_ms() + (sleeping ? 0 : s_wifi_retry_delay_ms);
    if (!sleeping && s_wifi_retry_delay_ms < WIFI_RETRY_MAX_MS) {
        s_wifi_retry_delay_ms *= 2;
        if (s_wifi_retry_delay_ms > WIFI_RETRY_MAX_MS) {
            s_wifi_retry_delay_ms = WIFI_RETRY_MAX_MS;
        }
    }
    mutex_unlock();
    if (was_connected) post_simple_event(QUOTA_APP_EVENT_CONNECTION);
    if (!sleeping && s_network_task != NULL) xTaskNotifyGive(s_network_task);
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id != IP_EVENT_STA_GOT_IP) return;
    /* An old queued IP event can arrive after restart, before the new association. */
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;
    mutex_lock();
    if (!s_wifi_started || display_state_snapshot().sleeping) {
        mutex_unlock();
        return;
    }
    s_view.connected = true;
    s_fetch_after_connect = true;
    s_wifi_retry_delay_ms = WIFI_RETRY_MIN_MS;
    s_wifi_retry_pending = false;
    mutex_unlock();
    post_simple_event(QUOTA_APP_EVENT_CONNECTION);
    if (s_network_task != NULL) xTaskNotifyGive(s_network_task);
}

static bool init_wifi(void)
{
    if (!prepare_network_stack()) return false;
    esp_err_t err;
    if (!s_handlers_registered) {
        err = esp_event_handler_instance_register(WIFI_EVENT,
            WIFI_EVENT_STA_DISCONNECTED, wifi_event_handler, NULL, &s_wifi_handler);
        if (err != ESP_OK) goto failed;
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
            ip_event_handler, NULL, &s_ip_handler);
        if (err != ESP_OK) {
            (void)esp_event_handler_instance_unregister(WIFI_EVENT,
                WIFI_EVENT_STA_DISCONNECTED, s_wifi_handler);
            goto failed;
        }
        s_handlers_registered = true;
        err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (err != ESP_OK) goto failed;
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) goto failed;
    }
    if (s_wifi_started) return true;
    err = esp_wifi_start();
    if (err != ESP_OK) goto failed;
    /* Restarting retains the RAM configuration and the provider deadline. */
    mutex_lock();
    s_wifi_started = true;
    s_wifi_retry_pending = true;
    s_wifi_retry_at_ms = (int64_t)monotonic_ms();
    mutex_unlock();
    return true;

failed:
    ESP_LOGW(TAG, "Wi-Fi init failed (%s)", esp_err_to_name(err));
    if (s_handlers_registered) {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT,
            WIFI_EVENT_STA_DISCONNECTED, s_wifi_handler);
        (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
            s_ip_handler);
        s_handlers_registered = false;
    }
    return false;
}

static void stop_wifi_for_sleep(void)
{
    if (!s_wifi_started) return;
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi sleep stop failed (%s)", esp_err_to_name(err));
        return;
    }
    mutex_lock();
    s_wifi_started = false;
    bool was_connected = s_view.connected;
    s_view.connected = false;
    s_wifi_retry_pending = false;
    s_fetch_after_connect = false;
    mutex_unlock();
    if (was_connected) post_simple_event(QUOTA_APP_EVENT_CONNECTION);
}

#ifdef ESP_PLATFORM
static bool portable_wifi_stop(void) { stop_wifi_for_sleep(); return !s_wifi_started; }
static void portable_notify(void) { post_simple_event(QUOTA_APP_EVENT_SNAPSHOT); }
static void portable_wake(void) { if (s_network_task) xTaskNotifyGive(s_network_task); }
#endif



static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    if (event == NULL || event->user_data == NULL) return ESP_OK;
    http_body_t *body = event->user_data;
    if (event->event_id == HTTP_EVENT_REDIRECT) {
        body->redirect = true;
    } else if (event->event_id == HTTP_EVENT_ON_DATA && event->data != NULL &&
               event->data_len > 0) {
        size_t length = (size_t)event->data_len;
        if (length > HTTP_BODY_BYTES - body->length) {
            body->overflow = true;
            return ESP_FAIL;
        }
        memcpy(body->bytes + body->length, event->data, length);
        body->length += length;
        body->bytes[body->length] = '\0';
    }
    return ESP_OK;
}

static bool operation_is_current(uint32_t config_generation,
                                 uint32_t display_generation);
static bool network_operation_is_current(uint32_t config_generation,
                                         uint32_t display_generation);

static bool http_request(const quota_device_config_t *config, const char *path,
                         esp_http_client_method_t method, const char *request_json,
                         int expected_status, uint32_t config_generation,
                         uint32_t display_generation,
                         http_request_outcome_t *request_outcome,
                         http_body_t *body)
{
    if (request_outcome != NULL) *request_outcome = HTTP_REQUEST_NOT_ADMITTED;
    char url[QUOTA_BASE_URL_MAX_BYTES + 32];
    int url_length = snprintf(url, sizeof(url), "%s%s", config->base_url, path);
    if (url_length < 0 || (size_t)url_length >= sizeof(url)) return false;
    memset(body, 0, sizeof(*body));
    esp_http_client_config_t client_config = {
        .url = url,
        .method = method,
        .cert_pem = config->server_cert_pem,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .max_authorization_retries = -1,
        .event_handler = http_event_handler,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .user_data = body,
        .skip_cert_common_name_check = false,
    };
    if (!network_operation_is_current(config_generation, display_generation)) {
        if (request_outcome != NULL) *request_outcome = HTTP_REQUEST_DEFERRED;
        return false;
    }
    esp_http_client_handle_t client = esp_http_client_init(&client_config);
    if (client == NULL) return false;
    char authorization[sizeof("Bearer ") + QUOTA_PAIR_TOKEN_BYTES];
    int auth_length = snprintf(authorization, sizeof(authorization), "Bearer %s",
                               config->pair_token);
    esp_err_t err = auth_length > 0 && (size_t)auth_length < sizeof(authorization)
                  ? esp_http_client_set_header(client, "Authorization", authorization)
                  : ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Accept", "application/json");
    if (err == ESP_OK && request_json != NULL) {
        err = esp_http_client_set_header(client, "Content-Type", "application/json");
        if (err == ESP_OK) {
            err = esp_http_client_set_post_field(client, request_json,
                                                  (int)strlen(request_json));
        }
    }
    if (err == ESP_OK &&
        !network_operation_is_current(config_generation, display_generation)) {
        if (request_outcome != NULL) *request_outcome = HTTP_REQUEST_DEFERRED;
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) {
        if (request_outcome != NULL) *request_outcome = HTTP_REQUEST_ADMITTED;
        err = esp_http_client_perform(client);
    }
    int status = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    bool ok = err == ESP_OK && status == expected_status && !body->overflow &&
              !body->redirect;
    esp_http_client_cleanup(client);
    if (!ok) ESP_LOGW(TAG, "HTTPS request failed (%s, status %d)",
                      esp_err_to_name(err), status);
    return ok;
}

static bool json_has_unique_keys(const cJSON *object)
{
    if (!cJSON_IsObject(object)) return false;
    for (const cJSON *first = object->child; first != NULL; first = first->next) {
        if (first->string == NULL) return false;
        for (const cJSON *next = first->next; next != NULL; next = next->next) {
            if (next->string != NULL && strcmp(first->string, next->string) == 0) return false;
        }
    }
    return true;
}

static bool parse_refresh_ack(const http_body_t *body)
{
    if (body == NULL || body->length == 0) return false;
    cJSON *root = cJSON_ParseWithLength(body->bytes, body->length);
    const cJSON *version = root != NULL ? cJSON_GetObjectItemCaseSensitive(root, "v") : NULL;
    const cJSON *accepted = root != NULL ? cJSON_GetObjectItemCaseSensitive(root, "accepted") : NULL;
    bool valid = json_has_unique_keys(root) && cJSON_IsNumber(version) &&
                 version->valuedouble == 1 && cJSON_IsTrue(accepted);
    cJSON_Delete(root);
    return valid;
}

static bool fetch_snapshot(const quota_device_config_t *config,
                           quota_snapshot_t *snapshot,
                           uint32_t config_generation,
                           uint32_t display_generation,
                           http_request_outcome_t *request_outcome)
{
    if (!http_request(config, "/v1/snapshot", HTTP_METHOD_GET, NULL, 200,
                      config_generation, display_generation, request_outcome,
                      &s_http_body) ||
        !quota_parse_snapshot(s_http_body.bytes, s_http_body.length, snapshot)) {
        return false;
    }
    return true;
}

static bool request_refresh(const quota_device_config_t *config,
                           uint32_t config_generation,
                           uint32_t display_generation,
                           http_request_outcome_t *request_outcome)
{
    if (!http_request(config, "/v1/refresh", HTTP_METHOD_POST, "{}", 202,
                      config_generation, display_generation, request_outcome,
                      &s_http_body)) {
        return false;
    }
    return parse_refresh_ack(&s_http_body);
}

static bool config_generation_is_current(uint32_t generation)
{
    mutex_lock(); bool current=s_view.configured && s_config_generation==generation; mutex_unlock(); return current;
}
static bool operation_is_current(uint32_t config_generation,uint32_t display_generation)
{
    return config_generation_is_current(config_generation) && display_generation_is_current(display_generation);
}
static bool network_operation_is_current(uint32_t config_generation,uint32_t display_generation)
{
    if(pairing_requested())return false;
#ifdef ESP_PLATFORM
    if(!quota_portable_service_http_allowed())return false;
#endif
    mutex_lock();bool connected=s_view.configured&&s_config_generation==config_generation&&s_view.connected;mutex_unlock();
    return connected&&display_generation_is_current(display_generation);
}
#ifdef ESP_PLATFORM
static bool portable_try_lock(void) { return xSemaphoreTake(s_mutex,0)==pdTRUE; }
static uint32_t portable_config_generation_locked(void) { return s_config_generation; }
static void portable_config_changed_locked(void) { if(s_config_generation<UINT32_MAX)s_config_generation++; }
static quota_store_read_result_t legacy_config(quota_device_config_t *out,uint16_t *screen_timeout)
{
    quota_store_read_result_t result=nvs_read_config_result(out);
    if(result==QUOTA_STORE_READ_OK){s_config=*out;s_has_config=true;if(screen_timeout)*screen_timeout=nvs_load_screen_timeout();}
    else if(result==QUOTA_STORE_READ_MISSING){s_has_config=false;quota_portable_clear_secret(&s_config,sizeof(s_config));}
    return result;
}
static quota_store_read_result_t legacy_inventory(quota_snapshot_t *out)
{
    if(!out)return QUOTA_STORE_READ_INVALID;
    if(!s_has_config)return QUOTA_STORE_READ_MISSING;
    if(!acquire_companion_work())return QUOTA_STORE_READ_NO_MEMORY;
    memset(out,0,sizeof(*out));
    quota_store_read_result_t result=nvs_load_snapshot_inventory_result(&s_config);
    if(result==QUOTA_STORE_READ_OK) {
        out->server_time=s_snapshot_cache_saved_at;out->revision=s_stored_snapshot.snapshot.revision;
        out->account_count=s_stored_snapshot.snapshot.account_count;
        memcpy(out->accounts,s_stored_snapshot.snapshot.accounts,sizeof(out->accounts));
        nvs_load_balance_snapshot(&s_config);
        if(s_balance_cache_present)memcpy(out->balances,s_stored_balances.balances,sizeof(out->balances));
    }
    release_companion_work();return result;
}
static bool legacy_snapshot(const quota_legacy_endpoint_t *endpoint,bool refresh,uint32_t display_generation,quota_snapshot_t *out,bool *deferred)
{
    if(deferred)*deferred=false;
    if(!endpoint||!endpoint->enabled||!out||!acquire_companion_work())return false;
    memset(&s_network_config,0,sizeof(s_network_config));
    snprintf(s_network_config.base_url,sizeof(s_network_config.base_url),"%s",endpoint->base_url);
    snprintf(s_network_config.pair_token,sizeof(s_network_config.pair_token),"%s",endpoint->pair_token);
    snprintf(s_network_config.server_cert_pem,sizeof(s_network_config.server_cert_pem),"%s",endpoint->server_cert_pem);
    mutex_lock();uint32_t generation=s_config_generation;mutex_unlock();
    http_request_outcome_t outcome=HTTP_REQUEST_NOT_ADMITTED;bool ok=true;
    if(refresh)ok=request_refresh(&s_network_config,generation,display_generation,&outcome);
    if(ok)ok=fetch_snapshot(&s_network_config,out,generation,display_generation,&outcome);
    if(deferred)*deferred=outcome==HTTP_REQUEST_DEFERRED||!operation_is_current(generation,display_generation);
    release_companion_work();return ok;
}
#endif
static void service_pairing_tick(bool sleeping);
static TickType_t network_wait(bool sleeping)
{
    uint64_t now=monotonic_ms();uint64_t deadline=sleeping&&!s_wifi_started?UINT64_MAX:now+500;
#ifdef ESP_PLATFORM
    uint64_t portable=quota_portable_service_next_deadline_ms(sleeping);if(portable<deadline)deadline=portable;
#endif
    if(pairing_requested())deadline=now+20;
    if(deadline==UINT64_MAX)return portMAX_DELAY;
    if(deadline<=now)return 0;
    TickType_t ticks=pdMS_TO_TICKS(deadline-now);return ticks?ticks:1;
}
static void network_task(void *arg)
{
    (void)arg;
    for(;;) {
        display_state_t waiting=display_state_snapshot();
        (void)ulTaskNotifyTake(pdTRUE,network_wait(waiting.sleeping));
        display_state_t display=display_state_snapshot();
        if(!pairing_requested())service_pairing_tick(display.sleeping);
#ifdef ESP_PLATFORM
        quota_portable_service_tick(display.sleeping,display.generation);
#endif
        if(display.sleeping)stop_wifi_for_sleep();
        service_pairing_tick(display.sleeping);
        mutex_lock();s_view.pairing_active=pairing_active_locked(monotonic_ms());
        s_view.pairing_seconds_left=s_view.pairing_active?(uint32_t)((QUOTA_PAIRING_WINDOW_MS-(monotonic_ms()-(uint64_t)s_pairing_opened_at_ms)+999)/1000):0;mutex_unlock();
    }
}

static void send_pairing_result(const char request_id[9], bool ok, const char *error)
{
    static const char empty_id[] = "00000000";
    bool request_valid = request_id != NULL;
    for (size_t i = 0; request_valid && i < 8; i++) {
        char ch = request_id[i];
        request_valid = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    }
    const char *safe_id = request_valid ? request_id : empty_id;
    const char *error_json = "\"invalid_frame\"";
    if (error != NULL) {
        static const struct { const char *error; const char *json; } errors[] = {
            {"pairing_closed", "\"pairing_closed\""},
            {"frame_too_long", "\"frame_too_long\""},
            {"invalid_frame", "\"invalid_frame\""},
            {"invalid_json", "\"invalid_json\""},
            {"invalid_request", "\"invalid_request\""},
            {"unsupported_version", "\"unsupported_version\""},
            {"unsupported_operation", "\"unsupported_operation\""},
            {"invalid_config", "\"invalid_config\""},
            {"storage_error", "\"storage_error\""},
            {"storage_write_unknown", "\"storage_write_unknown\""},
            {"generation_exhausted", "\"generation_exhausted\""},
        };
        for(size_t i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
            if(!strcmp(error,errors[i].error)){error_json=errors[i].json;break;}
        }
    }
    (void)printf("@AIQ:{\"v\":1,\"op\":\"result\",\"request_id\":\"%.8s\","
                 "\"ok\":%s,\"error\":%s}\n", safe_id,
                 ok ? "true" : "false", ok ? "null" : error_json);
    (void)fflush(stdout);
}

static void handle_serial_frame(const char *frame,size_t length)
{
    if(!frame||length<5||memcmp(frame,"@AIQ:",5)||!s_companion_work)return;
    memset(&s_provision_config,0,sizeof(s_provision_config));char request_id[9]="00000000";const char *error=NULL;
    bool valid=quota_parse_provision_frame(frame,length,&s_provision_config,request_id,&error);
    if(!pairing_active())error="pairing_closed";
    bool saved=false;
#ifdef ESP_PLATFORM
    if(valid&&!error)saved=quota_portable_service_configure_legacy(&s_provision_config,&error);
#endif
    if(valid&&!error&&!saved)error="storage_error";
    uint64_t server_time=s_provision_config.server_time;quota_portable_clear_secret(&s_provision_config,sizeof(s_provision_config));
    send_pairing_result(request_id,saved,saved?NULL:error);
    quota_app_event_t event={.kind=QUOTA_APP_EVENT_CONFIGURATION_RESULT,.success=saved};post_event(&event,0);
    if(saved){set_system_time_if_newer(server_time);quota_service_close_pairing_window();}
}

/* One owner reads USB only after its physical pairing window is ready. */
static void service_pairing_tick(bool sleeping)
{
    if (sleeping && pairing_requested()) quota_service_close_pairing_window();
    if (!pairing_requested()) {
        if (s_usb_decoder) { quota_portable_clear_secret(s_usb_decoder, sizeof(*s_usb_decoder)); free(s_usb_decoder); s_usb_decoder = NULL; }
        if (s_companion_work) release_companion_work();
        return;
    }
    if (!s_usb_decoder) {
#ifdef ESP_PLATFORM
        if (!quota_portable_service_prepare_pairing()) return;
#endif
        stop_wifi_for_sleep();
        if (!acquire_companion_work()) return;
        s_usb_decoder = calloc(1, sizeof(*s_usb_decoder));
        if (!s_usb_decoder) return;
        quota_frame_decoder_init(s_usb_decoder);
        unsigned char ignored[64];
        while (read(STDIN_FILENO, ignored, sizeof(ignored)) > 0) {}
        mutex_lock();
        s_pairing_screen_open = true;
        s_pairing_opened_at_ms = (int64_t)monotonic_ms();
        s_view.pairing_preparing = false; s_view.pairing_active = true;
        s_view.pairing_seconds_left = QUOTA_PAIRING_WINDOW_MS / 1000;
        mutex_unlock();
        post_simple_event(QUOTA_APP_EVENT_PAIRING_TICK);
    }
    if (!pairing_active()) { quota_service_close_pairing_window(); return; }
    for (unsigned i = 0; i < 512 && pairing_requested(); i++) {
        unsigned char input;
        if (read(STDIN_FILENO, &input, 1) != 1) break;
        if (!pairing_active()) { quota_service_close_pairing_window(); break; }
        const char *frame = NULL; size_t length = 0;
        quota_frame_result_t result = quota_frame_decoder_feed(s_usb_decoder, (char)input, &frame, &length);
        if (result == QUOTA_FRAME_COMPLETE) handle_serial_frame(frame, length);
        else if (result == QUOTA_FRAME_TOO_LONG) send_pairing_result(NULL, false, "frame_too_long");
    }
}

bool quota_service_init(void)
{
    if(s_events)return true;
    s_mutex=xSemaphoreCreateMutex();if(!s_mutex)return false;
    s_events=xQueueCreate(EVENT_QUEUE_DEPTH,sizeof(quota_app_event_t));if(!s_events)return false;
    esp_err_t err=nvs_flash_init();s_nvs_ready=err==ESP_OK;
    if(!s_nvs_ready)ESP_LOGE(TAG,"NVS init failed (%s); stored credentials are unavailable",esp_err_to_name(err));
    quota_store_read_result_t legacy_result=nvs_read_config_result(&s_config);
    s_has_config=legacy_result==QUOTA_STORE_READ_OK;s_config_generation=1;
    s_view.screen_timeout_seconds=nvs_load_screen_timeout();
    s_view.refresh_seconds=QUOTA_REFRESH_DEFAULT_SECONDS;s_view.auto_refresh=true;
    if(s_has_config)set_system_time_if_newer(s_config.server_time);
#ifdef ESP_PLATFORM
    quota_portable_service_hooks_t hooks={.view=&s_view,.lock=mutex_lock,.unlock=mutex_unlock,.try_lock=portable_try_lock,
        .config_generation_locked=portable_config_generation_locked,.config_changed_locked=portable_config_changed_locked,
        .pairing_requested=pairing_requested,.wifi_ready=init_wifi,.wifi_stop=portable_wifi_stop,
        .notify=portable_notify,.wake=portable_wake,.display_current=display_generation_is_current,
        .legacy_inventory=legacy_inventory,.legacy_snapshot=legacy_snapshot,
        .legacy_config=legacy_config};
    if(!quota_portable_service_init(s_has_config?&s_config:NULL,&hooks))return false;
#endif
    s_pairing_screen_open=false;s_pairing_opened_at_ms=(int64_t)monotonic_ms();return true;
}

bool quota_service_start(void)
{
    if (s_events == NULL || s_mutex == NULL) return false;
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) return false;
    if (s_network_task == NULL && xTaskCreate(network_task, "quota_network",
            NETWORK_TASK_STACK, NULL, NETWORK_TASK_PRIORITY, &s_network_task) != pdPASS) {
        s_network_task = NULL;
        return false;
    }
    return true;
}

QueueHandle_t quota_service_event_queue(void)
{
    return s_events;
}

void quota_service_send_button(bsp_btn_t button, bsp_btn_ev_t event)
{
    quota_app_event_t app_event = {
        .kind = QUOTA_APP_EVENT_BUTTON,
        .button = button,
        .button_event = event,
    };
    post_event(&app_event, 0);
}

void quota_service_set_display_sleeping(bool sleeping)
{
    bool changed = false;
    portENTER_CRITICAL(&s_display_state_mux);
    if (s_display_scheduler.sleeping != sleeping) {
        s_display_scheduler.sleeping = sleeping;
        s_display_scheduler.generation++;
        if (s_display_scheduler.generation == 0) s_display_scheduler.generation = 1;
        /* Wake reads the companion cache; source refresh keeps its own deadline. */
        s_display_scheduler.wake_fetch_pending = !sleeping;
        changed = true;
    }
    portEXIT_CRITICAL(&s_display_state_mux);
    if (!changed) return;

    if (sleeping && s_mutex != NULL && xSemaphoreTake(s_mutex, 0) == pdTRUE) {
        s_view.refreshing = false;
        (void)xSemaphoreGive(s_mutex);
    }
    if (s_network_task != NULL) xTaskNotifyGive(s_network_task);
}

void quota_service_get_view(quota_service_view_t *view)
{
    if (view == NULL) return;
    mutex_lock();
    *view = s_view;
    display_state_t display_state = display_state_snapshot();
    if (display_state.sleeping) {
        view->refreshing = false;
    }
    view->now_epoch = current_epoch();
    view->pairing_active = pairing_active_locked(monotonic_ms());
    if (view->pairing_active) {
        uint64_t elapsed = monotonic_ms() - (uint64_t)s_pairing_opened_at_ms;
        view->pairing_seconds_left = (uint32_t)((QUOTA_PAIRING_WINDOW_MS - elapsed + 999) / 1000);
    } else {
        view->pairing_seconds_left = 0;
    }
#ifdef ESP_PLATFORM
    quota_portable_service_countdown_overlay_locked(view);
#endif
    mutex_unlock();
}

void quota_service_get_selected_account_id(char account_id[QUOTA_ACCOUNT_ID_BYTES+1])
{
    if(!account_id)return;
    account_id[0]=0;
#ifdef ESP_PLATFORM
    (void)quota_portable_service_selected(account_id);
#endif
}

void quota_service_request_refresh(void) { quota_portable_service_refresh(); }

void quota_service_request_settings(uint16_t refresh_seconds,bool auto_refresh,uint16_t screen_timeout_seconds)
{
    quota_portable_service_settings(refresh_seconds,auto_refresh,screen_timeout_seconds);
}

void quota_service_select_account(const char *account_id) { quota_portable_service_select(account_id); }

void quota_service_open_pairing_window(void)
{
    atomic_store(&s_pairing_requested, true);
    mutex_lock();
    s_pairing_screen_open = false;
    s_view.pairing_preparing = true; s_view.pairing_active = false;
    s_view.pairing_seconds_left = 0;
    mutex_unlock();
    if (s_network_task) xTaskNotifyGive(s_network_task);
    post_simple_event(QUOTA_APP_EVENT_PAIRING_TICK);
}

void quota_service_close_pairing_window(void)
{
    atomic_store(&s_pairing_requested, false);
    mutex_lock();
    s_pairing_screen_open = false;
    s_view.pairing_preparing = s_view.pairing_active = false;
    s_view.pairing_seconds_left = 0;
    mutex_unlock();
    if (s_network_task) xTaskNotifyGive(s_network_task);
}

void quota_service_open_phone(void) { quota_portable_service_open(); }
void quota_service_close_phone(void) { quota_portable_service_close(); }
void quota_service_renew_phone(void) { quota_portable_service_renew(); }
void quota_service_cancel_auth(void) { quota_portable_service_cancel_auth(); }
void quota_service_reconnect(void) { quota_portable_service_reconnect(); }
