#include "quota_service.h"

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

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "quota_service";

#define EVENT_QUEUE_DEPTH 16
#define NETWORK_TASK_STACK 8192
#define SERIAL_TASK_STACK 8192
#define NETWORK_TASK_PRIORITY 5
#define SERIAL_TASK_PRIORITY 4
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
static TaskHandle_t s_serial_task;
static portMUX_TYPE s_display_state_mux = portMUX_INITIALIZER_UNLOCKED;
static display_scheduler_t s_display_scheduler = {.generation = 1};
static quota_service_view_t s_view;
static uint32_t s_refreshing_display_generation;
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
static bool s_refresh_requested;
static bool s_settings_pending;
static uint16_t s_pending_refresh_seconds;
static bool s_pending_auto_refresh;
static uint16_t s_pending_screen_timeout_seconds;
static uint16_t s_saved_screen_timeout_seconds = QUOTA_SCREEN_TIMEOUT_DEFAULT_SECONDS;
static bool s_selection_pending;
static char s_pending_account_id[QUOTA_ACCOUNT_ID_BYTES + 1];
static bool s_pairing_screen_open;
static int64_t s_pairing_opened_at_ms;
static uint64_t s_last_server_time_persist_ms;
static uint64_t s_snapshot_cache_saved_at;
static uint64_t s_snapshot_cache_last_attempt_ms;
static bool s_snapshot_cache_present;
static bool s_snapshot_cache_dirty;
static bool s_snapshot_cache_attempted;
static uint64_t s_selection_persist_retry_at_ms;

/* Large protocol workspaces live in BSS, never on the 4–8 KiB task stacks. */
static http_body_t s_http_body;
static quota_device_config_t s_network_config;
static quota_device_config_t s_provision_config;
static quota_snapshot_t s_snapshot_work;
static stored_config_t s_stored_config;
static stored_snapshot_t s_stored_snapshot;
static cached_snapshot_t s_snapshot_cache_candidate;
static stored_balance_t s_stored_balances;
static bool s_balance_cache_present;
static quota_frame_decoder_t s_frame_decoder;

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

static void finish_wake_fetch(uint32_t generation, bool connection_current)
{
    if (!connection_current) return;
    portENTER_CRITICAL(&s_display_state_mux);
    if (!s_display_scheduler.sleeping &&
        s_display_scheduler.generation == generation) {
        s_display_scheduler.wake_fetch_pending = false;
    }
    portEXIT_CRITICAL(&s_display_state_mux);
}

static bool valid_refresh_seconds(uint16_t seconds)
{
    return seconds == 60 || seconds == 300 || seconds == 900 || seconds == 1800;
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
           config->server_time > 0 && valid_refresh_seconds(config->refresh_seconds) &&
           strncmp(config->server_cert_pem, "-----BEGIN CERTIFICATE-----", 27) == 0 &&
           strstr(config->server_cert_pem, "-----END CERTIFICATE-----") != NULL;
}

static bool nvs_save_config_locked(const quota_device_config_t *config)
{
    if (!s_nvs_ready || !config_is_well_formed(config)) return false;
    memset(&s_stored_config, 0, sizeof(s_stored_config));
    s_stored_config.magic = STORED_CONFIG_MAGIC;
    s_stored_config.version = STORED_CONFIG_VERSION;
    s_stored_config.config_size = sizeof(*config);
    s_stored_config.config = *config;
    s_stored_config.crc32 = crc32_bytes(&s_stored_config.config,
                                        sizeof(s_stored_config.config));
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, NVS_CONFIG_KEY, &s_stored_config,
                           sizeof(s_stored_config));
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "device config save failed (%s)", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool nvs_load_config(void)
{
    if (!s_nvs_ready) return false;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return false;
    memset(&s_stored_config, 0, sizeof(s_stored_config));
    size_t length = sizeof(s_stored_config);
    err = nvs_get_blob(handle, NVS_CONFIG_KEY, &s_stored_config, &length);
    nvs_close(handle);
    if (err != ESP_OK || length != sizeof(s_stored_config) ||
        s_stored_config.magic != STORED_CONFIG_MAGIC ||
        s_stored_config.version != STORED_CONFIG_VERSION ||
        s_stored_config.config_size != sizeof(s_stored_config.config) ||
        s_stored_config.crc32 != crc32_bytes(&s_stored_config.config,
                                             sizeof(s_stored_config.config)) ||
        !config_is_well_formed(&s_stored_config.config)) {
        return false;
    }
    s_config = s_stored_config.config;
    return true;
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

static bool nvs_save_screen_timeout_locked(uint16_t seconds)
{
    if (!s_nvs_ready || !quota_screen_timeout_is_valid(seconds)) return false;
    if (seconds == s_saved_screen_timeout_seconds) return true;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, NVS_SCREEN_TIMEOUT_KEY, seconds);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "screen timeout save failed (%s)", esp_err_to_name(err));
        return false;
    }
    s_saved_screen_timeout_seconds = seconds;
    return true;
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

static bool nvs_erase_snapshot(void)
{
    if (!s_nvs_ready) return false;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_erase_key(handle, NVS_SNAPSHOT_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        if (err == ESP_OK) {
            err = nvs_erase_key(handle, NVS_BALANCE_KEY);
            if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        }
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "quota cache cleanup failed (%s)", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool nvs_load_snapshot(const quota_device_config_t *config)
{
    if (!s_nvs_ready || config == NULL) return false;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return false;
    memset(&s_stored_snapshot, 0, sizeof(s_stored_snapshot));
    size_t length = sizeof(s_stored_snapshot);
    err = nvs_get_blob(handle, NVS_SNAPSHOT_KEY, &s_stored_snapshot, &length);
    nvs_close(handle);

    uint64_t now = current_epoch();
    bool valid = err == ESP_OK && length == sizeof(s_stored_snapshot) &&
        s_stored_snapshot.magic == STORED_SNAPSHOT_MAGIC &&
        s_stored_snapshot.version == STORED_SNAPSHOT_VERSION &&
        s_stored_snapshot.snapshot_size == sizeof(s_stored_snapshot.snapshot) &&
        s_stored_snapshot.config_identity == config_cache_identity(config) &&
        s_stored_snapshot.stored_at > 0 && s_stored_snapshot.stored_at <= UINT32_MAX &&
        s_stored_snapshot.crc32 == crc32_bytes(&s_stored_snapshot,
                                               offsetof(stored_snapshot_t, crc32)) &&
        cached_snapshot_is_well_formed(&s_stored_snapshot.snapshot) &&
        !(now >= s_stored_snapshot.stored_at &&
          now - s_stored_snapshot.stored_at >= SNAPSHOT_CACHE_MAX_AGE_SECONDS);
    if (!valid) {
        if (err != ESP_ERR_NVS_NOT_FOUND) (void)nvs_erase_snapshot();
        memset(&s_stored_snapshot, 0, sizeof(s_stored_snapshot));
        return false;
    }
    s_snapshot_cache_present = true;
    s_snapshot_cache_saved_at = s_stored_snapshot.stored_at;
    return true;
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
        memcpy(s_view.snapshot.balances, s_stored_balances.balances, sizeof(s_stored_balances.balances));
        s_balance_cache_present = true;
    } else {
        memset(&s_stored_balances, 0, sizeof(s_stored_balances));
    }
}

static void prepare_cached_snapshot(const quota_snapshot_t *snapshot,
                                    cached_snapshot_t *cached)
{
    memset(cached, 0, sizeof(*cached));
    cached->revision = snapshot->revision;
    cached->account_count = snapshot->account_count;
    memcpy(cached->accounts, snapshot->accounts,
           (size_t)snapshot->account_count * sizeof(snapshot->accounts[0]));
}

static bool nvs_maybe_save_snapshot_locked(const quota_device_config_t *config,
                                           const quota_snapshot_t *snapshot)
{
    if (!s_nvs_ready || !config_is_well_formed(config) || snapshot == NULL ||
        snapshot->account_count > QUOTA_MAX_ACCOUNTS || snapshot->server_time == 0) {
        return false;
    }
    prepare_cached_snapshot(snapshot, &s_snapshot_cache_candidate);
    if (!cached_snapshot_is_well_formed(&s_snapshot_cache_candidate)) return false;
    if (s_snapshot_cache_present && !s_snapshot_cache_dirty &&
        memcmp(&s_snapshot_cache_candidate, &s_stored_snapshot.snapshot,
               sizeof(s_snapshot_cache_candidate)) == 0 && s_balance_cache_present &&
        memcmp(snapshot->balances, s_stored_balances.balances, sizeof(snapshot->balances)) == 0) {
        return true;
    }

    uint64_t now_ms = monotonic_ms();
    if (s_snapshot_cache_attempted &&
        now_ms - s_snapshot_cache_last_attempt_ms < SERVER_TIME_PERSIST_MS) {
        return true;
    }
    if (s_snapshot_cache_present &&
        (snapshot->server_time < s_snapshot_cache_saved_at ||
         snapshot->server_time - s_snapshot_cache_saved_at <
             SNAPSHOT_CACHE_WRITE_INTERVAL_SECONDS)) {
        return true;
    }

    s_snapshot_cache_attempted = true;
    s_snapshot_cache_last_attempt_ms = now_ms;
    memset(&s_stored_snapshot, 0, sizeof(s_stored_snapshot));
    s_stored_snapshot.magic = STORED_SNAPSHOT_MAGIC;
    s_stored_snapshot.version = STORED_SNAPSHOT_VERSION;
    s_stored_snapshot.snapshot_size = sizeof(s_stored_snapshot.snapshot);
    s_stored_snapshot.config_identity = config_cache_identity(config);
    s_stored_snapshot.stored_at = snapshot->server_time;
    s_stored_snapshot.snapshot = s_snapshot_cache_candidate;
    s_stored_snapshot.crc32 = crc32_bytes(&s_stored_snapshot,
                                          offsetof(stored_snapshot_t, crc32));
    memset(&s_stored_balances, 0, sizeof(s_stored_balances));
    s_stored_balances.magic = STORED_BALANCE_MAGIC;
    s_stored_balances.config_identity = s_stored_snapshot.config_identity;
    s_stored_balances.revision = snapshot->revision;
    s_stored_balances.stored_at = snapshot->server_time;
    memcpy(s_stored_balances.balances, snapshot->balances, sizeof(snapshot->balances));
    s_stored_balances.crc32 = crc32_bytes(&s_stored_balances, offsetof(stored_balance_t, crc32));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, NVS_SNAPSHOT_KEY, &s_stored_snapshot,
                           sizeof(s_stored_snapshot));
        if (err == ESP_OK) err = nvs_set_blob(handle, NVS_BALANCE_KEY, &s_stored_balances,
                                            sizeof(s_stored_balances));
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        s_snapshot_cache_dirty = true;
        ESP_LOGW(TAG, "quota cache save failed (%s)", esp_err_to_name(err));
        return false;
    }
    s_snapshot_cache_present = true;
    s_balance_cache_present = true;
    s_snapshot_cache_dirty = false;
    s_snapshot_cache_saved_at = snapshot->server_time;
    return true;
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
    if (id != WIFI_EVENT_STA_DISCONNECTED) return;
    mutex_lock();
    bool was_connected = s_view.connected;
    s_view.connected = false;
    s_wifi_retry_pending = true;
    s_wifi_retry_at_ms = (int64_t)monotonic_ms() + s_wifi_retry_delay_ms;
    if (s_wifi_retry_delay_ms < WIFI_RETRY_MAX_MS) {
        s_wifi_retry_delay_ms *= 2;
        if (s_wifi_retry_delay_ms > WIFI_RETRY_MAX_MS) {
            s_wifi_retry_delay_ms = WIFI_RETRY_MAX_MS;
        }
    }
    mutex_unlock();
    if (was_connected) post_simple_event(QUOTA_APP_EVENT_CONNECTION);
    if (s_network_task != NULL) xTaskNotifyGive(s_network_task);
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id != IP_EVENT_STA_GOT_IP) return;
    mutex_lock();
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
    if (s_handlers_registered) return true;
    esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT,
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
    err = esp_wifi_start();
    if (err != ESP_OK) goto failed;
    s_wifi_started = true;
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

static bool apply_wifi_config(const quota_device_config_t *config,
                              uint32_t display_generation)
{
    if (!display_generation_is_current(display_generation)) return false;
    wifi_config_t wifi_config = {0};
    size_t ssid_length = strlen(config->ssid);
    size_t password_length = strlen(config->password);
    if (ssid_length > sizeof(wifi_config.sta.ssid) ||
        password_length > sizeof(wifi_config.sta.password)) return false;
    memcpy(wifi_config.sta.ssid, config->ssid, ssid_length);
    memcpy(wifi_config.sta.password, config->password, password_length);
    wifi_config.sta.threshold.authmode = password_length == 0
                                      ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    if (!display_generation_is_current(display_generation)) return false;
    esp_err_t err = esp_wifi_disconnect();
    (void)err;
    mutex_lock();
    s_view.connected = false;
    mutex_unlock();
    post_simple_event(QUOTA_APP_EVENT_CONNECTION);
    if (!display_generation_is_current(display_generation)) return false;
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi config failed (%s)", esp_err_to_name(err));
        return false;
    }
    if (!display_generation_is_current(display_generation)) return false;
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi connect request failed (%s)", esp_err_to_name(err));
        return false;
    }
    mutex_lock();
    s_wifi_retry_pending = true;
    s_wifi_retry_at_ms = (int64_t)monotonic_ms() + s_wifi_retry_delay_ms;
    mutex_unlock();
    return true;
}

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

static bool apply_settings(const quota_device_config_t *config, uint16_t seconds,
                           bool auto_refresh, uint16_t screen_timeout_seconds,
                           quota_settings_t *applied,
                           uint32_t config_generation,
                           uint32_t display_generation,
                           http_request_outcome_t *request_outcome)
{
    char request[128];
    snprintf(request, sizeof(request), "{\"refresh_seconds\":%u,\"auto_refresh\":%s,"
             "\"screen_timeout_seconds\":%u}", (unsigned)seconds,
             auto_refresh ? "true" : "false", (unsigned)screen_timeout_seconds);
    return http_request(config, "/v1/settings", HTTP_METHOD_PATCH, request, 200,
                        config_generation, display_generation, request_outcome,
                        &s_http_body) &&
           quota_parse_settings_ack(s_http_body.bytes, s_http_body.length, applied);
}

static bool config_generation_is_current(uint32_t generation)
{
    mutex_lock();
    bool current = s_has_config && s_config_generation == generation;
    mutex_unlock();
    return current;
}

static bool operation_is_current(uint32_t config_generation,
                                 uint32_t display_generation)
{
    if (!config_generation_is_current(config_generation)) return false;
    return display_generation_is_current(display_generation);
}

static bool network_operation_is_current(uint32_t config_generation,
                                         uint32_t display_generation)
{
    mutex_lock();
    bool connected = s_has_config && s_config_generation == config_generation &&
                     s_view.connected;
    mutex_unlock();
    return connected && display_generation_is_current(display_generation);
}

static void clear_stale_refreshing(uint32_t display_generation)
{
    mutex_lock();
    if (s_refreshing_display_generation == display_generation) {
        s_view.refreshing = false;
    }
    mutex_unlock();
}

static bool publish_snapshot(const quota_snapshot_t *snapshot,
                             uint32_t config_generation,
                             uint32_t display_generation)
{
    if (snapshot == NULL) return false;
    mutex_lock();
    if (!s_has_config || s_config_generation != config_generation) {
        mutex_unlock();
        return false;
    }

    uint64_t now_epoch = current_epoch();
    bool committed = false;
    /* Serialize the snapshot commit with sleep transitions, without holding the
       display critical section across NVS or other potentially slow work. */
    portENTER_CRITICAL(&s_display_state_mux);
    if (!s_display_scheduler.sleeping &&
        s_display_scheduler.generation == display_generation) {
        s_view.snapshot = *snapshot;
        s_view.snapshot_valid = true;
        s_view.refresh_seconds = snapshot->refresh_seconds;
        s_view.auto_refresh = snapshot->auto_refresh;
        if (snapshot->has_screen_timeout_seconds) {
            s_view.screen_timeout_seconds = snapshot->screen_timeout_seconds;
        }
        s_view.now_epoch = now_epoch;
        s_view.request_failed = false;
        s_view.clock_synchronized = snapshot->server_time >= 1577836800ULL;
        committed = true;
    }
    portEXIT_CRITICAL(&s_display_state_mux);
    if (!committed) {
        mutex_unlock();
        return false;
    }

    if (snapshot->has_screen_timeout_seconds) {
        if (!nvs_save_screen_timeout_locked(snapshot->screen_timeout_seconds)) {
            ESP_LOGW(TAG, "unable to persist screen timeout");
        }
    }

    if (s_has_config) {
        bool config_changed = false;
        uint64_t now_ms = monotonic_ms();
        if (snapshot->server_time > s_config.server_time &&
            now_ms - s_last_server_time_persist_ms >= SERVER_TIME_PERSIST_MS) {
            s_config.server_time = snapshot->server_time;
            s_last_server_time_persist_ms = now_ms;
            config_changed = true;
        }
        if (s_config.refresh_seconds != snapshot->refresh_seconds ||
            s_config.auto_refresh != snapshot->auto_refresh) {
            s_config.refresh_seconds = snapshot->refresh_seconds;
            s_config.auto_refresh = snapshot->auto_refresh;
            config_changed = true;
        }
        if (config_changed && !nvs_save_config_locked(&s_config)) {
            ESP_LOGW(TAG, "unable to persist server state");
        }
        (void)nvs_maybe_save_snapshot_locked(&s_config, snapshot);
    }
    mutex_unlock();
    if (!display_generation_is_current(display_generation)) return true;
    set_system_time_if_newer(snapshot->server_time);
    post_simple_event(QUOTA_APP_EVENT_SNAPSHOT);
    return true;
}

static bool begin_refresh(uint32_t config_generation, uint32_t display_generation)
{
    mutex_lock();
    if (!s_has_config || s_config_generation != config_generation) {
        mutex_unlock();
        return false;
    }
    portENTER_CRITICAL(&s_display_state_mux);
    bool current = !s_display_scheduler.sleeping &&
                   s_display_scheduler.generation == display_generation;
    if (current) {
        s_refreshing_display_generation = display_generation;
        s_view.refreshing = true;
        s_view.request_failed = false;
    }
    portEXIT_CRITICAL(&s_display_state_mux);
    mutex_unlock();
    return current;
}

static void finish_refresh(bool success, uint32_t config_generation,
                           uint32_t display_generation)
{
    uint64_t now_epoch = current_epoch();
    mutex_lock();
    if (!s_has_config || s_config_generation != config_generation) {
        if (s_refreshing_display_generation == display_generation) {
            s_view.refreshing = false;
        }
        mutex_unlock();
        return;
    }
    portENTER_CRITICAL(&s_display_state_mux);
    bool current = !s_display_scheduler.sleeping &&
                   s_display_scheduler.generation == display_generation;
    if (current) {
        s_view.refreshing = false;
        s_view.request_failed = !success;
        s_view.now_epoch = now_epoch;
    } else if (s_refreshing_display_generation == display_generation) {
        s_view.refreshing = false;
    }
    portEXIT_CRITICAL(&s_display_state_mux);
    mutex_unlock();
    if (!current) return;
    post_simple_event(QUOTA_APP_EVENT_SNAPSHOT);
}

/* False means deferred before admission; ordinary failures retain the cadence. */
static bool perform_refresh(const quota_device_config_t *config,
                            uint32_t config_generation,
                            uint32_t display_generation)
{
    if (!begin_refresh(config_generation, display_generation)) return false;
    if (!operation_is_current(config_generation, display_generation)) {
        clear_stale_refreshing(display_generation);
        return false;
    }
    post_simple_event(QUOTA_APP_EVENT_SNAPSHOT);

    uint64_t previous_revision = 0;
    bool had_previous = false;
    mutex_lock();
    had_previous = s_view.snapshot_valid;
    previous_revision = s_view.snapshot.revision;
    mutex_unlock();

    http_request_outcome_t post_outcome = HTTP_REQUEST_NOT_ADMITTED;
    bool success = request_refresh(config, config_generation, display_generation,
                                   &post_outcome);
    if (!operation_is_current(config_generation, display_generation)) {
        clear_stale_refreshing(display_generation);
        return post_outcome == HTTP_REQUEST_ADMITTED;
    }
    bool completed_attempt = post_outcome != HTTP_REQUEST_DEFERRED;
    bool fetched = false;
    if (success) {
        for (unsigned attempt = 0; attempt < 6; attempt++) {
            if (!operation_is_current(config_generation, display_generation)) {
                clear_stale_refreshing(display_generation);
                return completed_attempt;
            }
            if (!network_operation_is_current(config_generation, display_generation)) {
                break;
            }
            http_request_outcome_t get_outcome = HTTP_REQUEST_NOT_ADMITTED;
            if (fetch_snapshot(config, &s_snapshot_work, config_generation,
                               display_generation, &get_outcome)) {
                if (!operation_is_current(config_generation, display_generation)) {
                    clear_stale_refreshing(display_generation);
                    return completed_attempt;
                }
                fetched = true;
                bool changed = !had_previous ||
                               s_snapshot_work.revision != previous_revision;
                if (!publish_snapshot(&s_snapshot_work, config_generation,
                                      display_generation)) {
                    if (!operation_is_current(config_generation, display_generation)) {
                        clear_stale_refreshing(display_generation);
                    }
                    return completed_attempt;
                }
                if (changed || attempt == 5) break;
            } else if (get_outcome == HTTP_REQUEST_DEFERRED ||
                       !network_operation_is_current(config_generation,
                                                     display_generation)) {
                break;
            }
            if (attempt < 5) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (!operation_is_current(config_generation, display_generation)) {
                    clear_stale_refreshing(display_generation);
                    return completed_attempt;
                }
            }
        }
        success = success && fetched;
    }
    finish_refresh(success, config_generation, display_generation);
    return completed_attempt;
}

static void perform_snapshot_fetch(const quota_device_config_t *config,
                                   uint32_t config_generation,
                                   uint32_t display_generation,
                                   bool wake_fetch)
{
    /* GET only reads cached data. It does not start a provider quota refresh. */
    if (!network_operation_is_current(config_generation, display_generation)) return;
    http_request_outcome_t get_outcome = HTTP_REQUEST_NOT_ADMITTED;
    bool success = fetch_snapshot(config, &s_snapshot_work, config_generation,
                                  display_generation, &get_outcome);
    if (!operation_is_current(config_generation, display_generation)) return;
    if (success) {
        success = publish_snapshot(&s_snapshot_work, config_generation,
                                   display_generation);
    } else {
        success = false;
    }
    if (wake_fetch && (success ||
        (get_outcome != HTTP_REQUEST_DEFERRED &&
         network_operation_is_current(config_generation, display_generation)))) {
        finish_wake_fetch(display_generation, true);
    }
    finish_refresh(success, config_generation, display_generation);
}

static void restore_pending_settings(uint32_t config_generation,
                                     uint16_t requested_seconds,
                                     bool requested_auto_refresh,
                                     uint16_t requested_screen_timeout)
{
    mutex_lock();
    if (s_has_config && s_config_generation == config_generation &&
        !s_settings_pending) {
        s_settings_pending = true;
        s_pending_refresh_seconds = requested_seconds;
        s_pending_auto_refresh = requested_auto_refresh;
        s_pending_screen_timeout_seconds = requested_screen_timeout;
    }
    bool pending = s_settings_pending;
    mutex_unlock();
    if (pending && s_network_task != NULL) xTaskNotifyGive(s_network_task);
}

static void perform_settings_update(const quota_device_config_t *config,
                                    uint32_t config_generation,
                                    uint32_t display_generation,
                                    uint16_t requested_seconds,
                                    bool requested_auto_refresh,
                                    uint16_t requested_screen_timeout)
{
    if (!operation_is_current(config_generation, display_generation)) {
        restore_pending_settings(config_generation, requested_seconds,
                                 requested_auto_refresh, requested_screen_timeout);
        return;
    }
    quota_settings_t applied = {0};
    http_request_outcome_t request_outcome = HTTP_REQUEST_NOT_ADMITTED;
    bool success = apply_settings(config, requested_seconds, requested_auto_refresh,
                                  requested_screen_timeout, &applied,
                                  config_generation, display_generation,
                                  &request_outcome);

    if (!operation_is_current(config_generation, display_generation)) {
        restore_pending_settings(config_generation, requested_seconds,
                                 requested_auto_refresh, requested_screen_timeout);
        return;
    }
    if (request_outcome == HTTP_REQUEST_DEFERRED) {
        restore_pending_settings(config_generation, requested_seconds,
                                 requested_auto_refresh, requested_screen_timeout);
        return;
    }

    mutex_lock();
    if (!s_has_config || s_config_generation != config_generation) {
        mutex_unlock();
        return;
    }
    portENTER_CRITICAL(&s_display_state_mux);
    bool display_current = !s_display_scheduler.sleeping &&
                          s_display_scheduler.generation == display_generation;
    if (display_current && success) {
        s_config.refresh_seconds = applied.refresh_seconds;
        s_config.auto_refresh = applied.auto_refresh;
        s_view.refresh_seconds = applied.refresh_seconds;
        s_view.auto_refresh = applied.auto_refresh;
        if (applied.has_screen_timeout_seconds) {
            s_view.screen_timeout_seconds = applied.screen_timeout_seconds;
        }
    }
    portEXIT_CRITICAL(&s_display_state_mux);
    if (!display_current) {
        mutex_unlock();
        restore_pending_settings(config_generation, requested_seconds,
                                 requested_auto_refresh, requested_screen_timeout);
        return;
    }
    if (success) {
        if (applied.has_screen_timeout_seconds) {
            if (!nvs_save_screen_timeout_locked(applied.screen_timeout_seconds)) success = false;
        }
        if (!nvs_save_config_locked(&s_config)) success = false;
    }
    uint64_t now_epoch = current_epoch();
    portENTER_CRITICAL(&s_display_state_mux);
    display_current = !s_display_scheduler.sleeping &&
                      s_display_scheduler.generation == display_generation;
    if (display_current) {
        s_view.request_failed = !success;
        s_view.now_epoch = now_epoch;
    }
    portEXIT_CRITICAL(&s_display_state_mux);
    if (!success) {
        applied.refresh_seconds = s_config.refresh_seconds;
        applied.auto_refresh = s_config.auto_refresh;
    }
    applied.screen_timeout_seconds = s_view.screen_timeout_seconds;
    mutex_unlock();

    quota_app_event_t event = {
        .kind = QUOTA_APP_EVENT_SETTINGS_RESULT,
        .success = success,
        .refresh_seconds = applied.refresh_seconds,
        .auto_refresh = applied.auto_refresh,
        .screen_timeout_seconds = applied.screen_timeout_seconds,
        .error_code = success ? 0 : 1,
    };
    if (display_current && display_generation_is_current(display_generation)) {
        post_event(&event, 0);
    }
}

static void network_task(void *arg)
{
    (void)arg;
    uint32_t applied_config_generation = 0;
    uint64_t next_config_apply_ms = 0;
    uint64_t next_snapshot_poll_ms = 0;
    uint64_t next_provider_refresh_ms = 0;
    uint64_t last_pairing_tick_ms = 0;
    uint16_t last_refresh_seconds = 0;
    bool last_auto_refresh = false;

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        uint64_t now_ms = monotonic_ms();
        bool configured;
        bool connected;
        bool settings_pending;
        bool automatic;
        uint16_t refresh_seconds;
        uint16_t pending_seconds;
        bool pending_auto;
        uint16_t pending_screen_timeout;
        bool selection_pending;
        display_state_t display_state;
        char selected_account_id[QUOTA_ACCOUNT_ID_BYTES + 1];
        uint32_t config_generation;

        mutex_lock();
        configured = s_has_config;
        if (configured) s_network_config = s_config;
        connected = s_view.connected;
        settings_pending = s_settings_pending;
        automatic = s_view.auto_refresh;
        refresh_seconds = s_view.refresh_seconds;
        selection_pending = s_selection_pending;
        memcpy(selected_account_id, s_pending_account_id, sizeof(selected_account_id));
        config_generation = s_config_generation;
        s_view.now_epoch = current_epoch();
        s_view.pairing_active = pairing_active_locked(now_ms);
        s_view.pairing_seconds_left = s_view.pairing_active
            ? (uint32_t)((QUOTA_PAIRING_WINDOW_MS -
               (now_ms - (uint64_t)s_pairing_opened_at_ms) + 999) / 1000) : 0;
        mutex_unlock();

        if (now_ms - last_pairing_tick_ms >= 1000) {
            last_pairing_tick_ms = now_ms;
            post_simple_event(QUOTA_APP_EVENT_PAIRING_TICK);
        }
        if (selection_pending && configured) {
            mutex_lock();
            if (s_has_config && s_config_generation == config_generation &&
                s_selection_pending &&
                strcmp(s_pending_account_id, selected_account_id) == 0 &&
                now_ms >= s_selection_persist_retry_at_ms &&
                quota_id_is_valid(selected_account_id)) {
                memcpy(s_config.selected_account_id, selected_account_id,
                       sizeof(s_config.selected_account_id));
                if (nvs_save_config_locked(&s_config)) {
                    s_selection_pending = false;
                    s_selection_persist_retry_at_ms = 0;
                } else {
                    ESP_LOGW(TAG, "unable to persist selected account");
                    s_selection_persist_retry_at_ms = now_ms + SELECTION_PERSIST_RETRY_MS;
                }
            }
            mutex_unlock();
        }
        if (!configured) continue;
        display_state = display_state_snapshot();
        if (display_state.sleeping ||
            !display_generation_is_current(display_state.generation)) continue;
        if (!init_wifi()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!display_generation_is_current(display_state.generation)) continue;
        if (!s_wifi_started) continue;

        if (config_generation != applied_config_generation && now_ms >= next_config_apply_ms) {
            if (apply_wifi_config(&s_network_config, display_state.generation)) {
                applied_config_generation = config_generation;
                next_snapshot_poll_ms = now_ms + 1000;
                next_provider_refresh_ms = now_ms + 1000;
            } else if (display_generation_is_current(display_state.generation)) {
                next_config_apply_ms = now_ms + 5000;
            }
            continue;
        }

        mutex_lock();
        bool retry_wifi = !s_view.connected && s_wifi_retry_pending &&
                          now_ms >= (uint64_t)s_wifi_retry_at_ms;
        if (retry_wifi) {
            s_wifi_retry_pending = false;
            s_wifi_retry_at_ms = (int64_t)(now_ms + s_wifi_retry_delay_ms);
        }
        mutex_unlock();
        if (retry_wifi) {
            if (!display_generation_is_current(display_state.generation)) {
                mutex_lock();
                s_wifi_retry_pending = true;
                mutex_unlock();
                continue;
            }
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Wi-Fi retry failed (%s)", esp_err_to_name(err));
                mutex_lock();
                s_wifi_retry_pending = true;
                mutex_unlock();
            }
        }
        if (!connected) continue;

        if (settings_pending) {
            mutex_lock();
            portENTER_CRITICAL(&s_display_state_mux);
            bool display_current = !s_display_scheduler.sleeping &&
                s_display_scheduler.generation == display_state.generation;
            if (!s_has_config || s_config_generation != config_generation ||
                !s_view.connected || !s_settings_pending || !display_current) {
                portEXIT_CRITICAL(&s_display_state_mux);
                mutex_unlock();
                continue;
            }
            pending_seconds = s_pending_refresh_seconds;
            pending_auto = s_pending_auto_refresh;
            pending_screen_timeout = s_pending_screen_timeout_seconds;
            /* Consume the latest request atomically; requests during HTTP remain queued. */
            s_settings_pending = false;
            portEXIT_CRITICAL(&s_display_state_mux);
            mutex_unlock();
            perform_settings_update(&s_network_config, config_generation,
                                    display_state.generation,
                                    pending_seconds, pending_auto, pending_screen_timeout);
            continue;
        }

        if (refresh_seconds != last_refresh_seconds || automatic != last_auto_refresh) {
            last_refresh_seconds = refresh_seconds;
            last_auto_refresh = automatic;
            next_provider_refresh_ms = now_ms + (uint64_t)refresh_seconds * 1000;
        }

        enum { NETWORK_ACTION_NONE, NETWORK_ACTION_REFRESH,
               NETWORK_ACTION_SNAPSHOT } action = NETWORK_ACTION_NONE;
        bool wake_fetch_work = false;
        bool manual_refresh_work = false;
        mutex_lock();
        if (s_has_config && s_config_generation == config_generation && s_view.connected) {
            portENTER_CRITICAL(&s_display_state_mux);
            bool display_current = !s_display_scheduler.sleeping;
            if (display_current) {
                display_state.generation = s_display_scheduler.generation;
                if (s_display_scheduler.wake_fetch_pending) {
                    s_fetch_after_connect = false;
                    wake_fetch_work = true;
                    action = NETWORK_ACTION_SNAPSHOT;
                } else if (s_refresh_requested ||
                           (s_view.auto_refresh && now_ms >= next_provider_refresh_ms)) {
                    manual_refresh_work = s_refresh_requested;
                    s_refresh_requested = false;
                    s_fetch_after_connect = false;
                    action = NETWORK_ACTION_REFRESH;
                } else if (s_fetch_after_connect || now_ms >= next_snapshot_poll_ms) {
                    s_fetch_after_connect = false;
                    action = NETWORK_ACTION_SNAPSHOT;
                }
                refresh_seconds = s_view.refresh_seconds;
            }
            portEXIT_CRITICAL(&s_display_state_mux);
        }
        mutex_unlock();

        if (action == NETWORK_ACTION_REFRESH) {
            if (perform_refresh(&s_network_config, config_generation,
                                display_state.generation)) {
                uint64_t completed_ms = monotonic_ms();
                next_provider_refresh_ms = completed_ms + (uint64_t)refresh_seconds * 1000;
                next_snapshot_poll_ms = completed_ms + SNAPSHOT_POLL_MS;
            } else if (manual_refresh_work) {
                /* Sleep/link transitions cannot consume an unadmitted request. */
                mutex_lock();
                if (s_has_config && s_config_generation == config_generation) {
                    s_refresh_requested = true;
                }
                mutex_unlock();
            }
        } else if (action == NETWORK_ACTION_SNAPSHOT) {
            perform_snapshot_fetch(&s_network_config, config_generation,
                                   display_state.generation, wake_fetch_work);
            mutex_lock();
            bool failed = s_view.request_failed;
            mutex_unlock();
            uint64_t completed_ms = monotonic_ms();
            /* Cache synchronization never postpones the provider deadline. */
            next_snapshot_poll_ms = completed_ms +
                (failed ? SNAPSHOT_RETRY_MS : SNAPSHOT_POLL_MS);
        }
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
        static const char *const allowed_errors[] = {
            "pairing_closed", "frame_too_long", "invalid_frame", "invalid_json",
            "invalid_request", "unsupported_version", "unsupported_operation",
            "invalid_config", "storage_error",
        };
        for (size_t i = 0; i < sizeof(allowed_errors) / sizeof(allowed_errors[0]); i++) {
            if (strcmp(error, allowed_errors[i]) == 0) {
                static const char *const json_values[] = {
                    "\"pairing_closed\"", "\"frame_too_long\"", "\"invalid_frame\"",
                    "\"invalid_json\"", "\"invalid_request\"", "\"unsupported_version\"",
                    "\"unsupported_operation\"", "\"invalid_config\"", "\"storage_error\"",
                };
                error_json = json_values[i];
                break;
            }
        }
    }
    (void)printf("@AIQ:{\"v\":1,\"op\":\"result\",\"request_id\":\"%.8s\","
                 "\"ok\":%s,\"error\":%s}\n", safe_id,
                 ok ? "true" : "false", ok ? "null" : error_json);
    (void)fflush(stdout);
}

static void handle_serial_frame(const char *frame, size_t length)
{
    if (frame == NULL || length < 5 || memcmp(frame, "@AIQ:", 5) != 0) return;

    memset(&s_provision_config, 0, sizeof(s_provision_config));
    char request_id[9] = "00000000";
    const char *error = NULL;
    bool valid = quota_parse_provision_frame(frame, length, &s_provision_config,
                                             request_id, &error);
    if (!pairing_active()) {
        send_pairing_result(request_id, false, "pairing_closed");
        return;
    }
    if (!valid) {
        send_pairing_result(request_id, false, error);
        quota_app_event_t event = {
            .kind = QUOTA_APP_EVENT_CONFIGURATION_RESULT,
            .success = false,
            .error_code = 2,
        };
        post_event(&event, 0);
        return;
    }

    mutex_lock();
    if (!pairing_active_locked(monotonic_ms())) {
        mutex_unlock();
        send_pairing_result(request_id, false, "pairing_closed");
        return;
    }
    if (s_has_config) {
        memcpy(s_provision_config.selected_account_id, s_config.selected_account_id,
               sizeof(s_provision_config.selected_account_id));
        s_provision_config.refresh_seconds = s_config.refresh_seconds;
        s_provision_config.auto_refresh = s_config.auto_refresh;
    }
    bool saved = nvs_save_config_locked(&s_provision_config);
    if (saved) {
        (void)nvs_erase_snapshot();
        memset(&s_stored_snapshot, 0, sizeof(s_stored_snapshot));
        s_snapshot_cache_present = false;
        s_balance_cache_present = false;
        memset(&s_stored_balances, 0, sizeof(s_stored_balances));
        s_snapshot_cache_dirty = false;
        s_snapshot_cache_attempted = false;
        s_snapshot_cache_last_attempt_ms = 0;
        s_snapshot_cache_saved_at = 0;
        s_config = s_provision_config;
        s_has_config = true;
        s_config_generation++;
        if (s_config_generation == 0) s_config_generation = 1;
        s_refresh_requested = false;
        s_settings_pending = false;
        s_selection_pending = false;
        s_pending_account_id[0] = '\0';
        s_selection_persist_retry_at_ms = 0;
        s_view.configured = true;
        memset(&s_view.snapshot, 0, sizeof(s_view.snapshot));
        s_view.snapshot_valid = false;
        s_view.connected = false;
        s_view.refreshing = false;
        s_view.refresh_seconds = s_provision_config.refresh_seconds;
        s_view.auto_refresh = s_provision_config.auto_refresh;
        s_view.request_failed = false;
        s_view.clock_synchronized = s_provision_config.server_time >= 1577836800ULL;
        s_pairing_screen_open = false;
        s_last_server_time_persist_ms = monotonic_ms();
        s_view.pairing_active = false;
        s_view.pairing_seconds_left = 0;
    }
    mutex_unlock();

    send_pairing_result(request_id, saved, saved ? NULL : "storage_error");
    quota_app_event_t event = {
        .kind = QUOTA_APP_EVENT_CONFIGURATION_RESULT,
        .success = saved,
        .error_code = saved ? 0 : 3,
    };
    post_event(&event, 0);
    if (saved) {
        set_system_time_if_newer(s_provision_config.server_time);
        if (s_network_task != NULL) xTaskNotifyGive(s_network_task);
    }
}

static void serial_task(void *arg)
{
    (void)arg;
    quota_frame_decoder_init(&s_frame_decoder);
    for (;;) {
        int input = fgetc(stdin);
        if (input == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        const char *frame = NULL;
        size_t length = 0;
        quota_frame_result_t result = quota_frame_decoder_feed(&s_frame_decoder, (char)input,
                                                                &frame, &length);
        if (result == QUOTA_FRAME_COMPLETE) {
            handle_serial_frame(frame, length);
        } else if (result == QUOTA_FRAME_TOO_LONG && pairing_active()) {
            send_pairing_result(NULL, false, "frame_too_long");
        }
    }
}

bool quota_service_init(void)
{
    if (s_events != NULL) return true;
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return false;
    s_events = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(quota_app_event_t));
    if (s_events == NULL) return false;

    esp_err_t err = nvs_flash_init();
    s_nvs_ready = err == ESP_OK;
    if (!s_nvs_ready) {
        ESP_LOGE(TAG, "NVS init failed (%s); stored credentials are unavailable", esp_err_to_name(err));
    }
    s_has_config = nvs_load_config();
    s_view.screen_timeout_seconds = nvs_load_screen_timeout();
    if (s_has_config) {
        s_config_generation = 1;
        s_view.configured = true;
        s_view.refresh_seconds = s_config.refresh_seconds;
        s_view.auto_refresh = s_config.auto_refresh;
        s_last_server_time_persist_ms = monotonic_ms();
        set_system_time_if_newer(s_config.server_time);
        if (nvs_load_snapshot(&s_config)) {
            s_view.snapshot.server_time = s_snapshot_cache_saved_at;
            s_view.snapshot.revision = s_stored_snapshot.snapshot.revision;
            s_view.snapshot.refresh_seconds = s_config.refresh_seconds;
            s_view.snapshot.auto_refresh = s_config.auto_refresh;
            s_view.snapshot.account_count = s_stored_snapshot.snapshot.account_count;
            memcpy(s_view.snapshot.accounts, s_stored_snapshot.snapshot.accounts,
                   (size_t)s_view.snapshot.account_count *
                       sizeof(s_view.snapshot.accounts[0]));
            nvs_load_balance_snapshot(&s_config);
            s_view.snapshot_valid = true;
            s_view.now_epoch = current_epoch();
            set_system_time_if_newer(s_snapshot_cache_saved_at);
            ESP_LOGI(TAG, "restored cached quota snapshot");
        }
    } else {
        s_view.configured = false;
        s_view.refresh_seconds = QUOTA_REFRESH_DEFAULT_SECONDS;
        s_view.auto_refresh = true;
        if (s_nvs_ready) (void)nvs_erase_snapshot();
    }
    s_pairing_screen_open = !s_has_config;
    s_pairing_opened_at_ms = (int64_t)monotonic_ms();
    return true;
}

bool quota_service_start(void)
{
    if (s_events == NULL || s_mutex == NULL) return false;
    if (s_network_task == NULL && xTaskCreate(network_task, "quota_network",
            NETWORK_TASK_STACK, NULL, NETWORK_TASK_PRIORITY, &s_network_task) != pdPASS) {
        s_network_task = NULL;
        return false;
    }
    if (s_serial_task == NULL && xTaskCreate(serial_task, "quota_serial",
            SERIAL_TASK_STACK, NULL, SERIAL_TASK_PRIORITY, &s_serial_task) != pdPASS) {
        s_serial_task = NULL;
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
    if (display_state.sleeping ||
        s_refreshing_display_generation != display_state.generation) {
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
    mutex_unlock();
}

void quota_service_get_selected_account_id(char account_id[QUOTA_ACCOUNT_ID_BYTES + 1])
{
    if (account_id == NULL) return;
    mutex_lock();
    if (s_has_config) {
        const char *selected = s_selection_pending ? s_pending_account_id
                                                   : s_config.selected_account_id;
        memcpy(account_id, selected, QUOTA_ACCOUNT_ID_BYTES + 1);
    } else {
        account_id[0] = '\0';
    }
    mutex_unlock();
}

void quota_service_request_refresh(void)
{
    mutex_lock();
    s_refresh_requested = true;
    mutex_unlock();
    if (s_network_task != NULL) xTaskNotifyGive(s_network_task);
}

void quota_service_request_settings(uint16_t refresh_seconds, bool auto_refresh,
                                    uint16_t screen_timeout_seconds)
{
    if (!valid_refresh_seconds(refresh_seconds) ||
        !quota_screen_timeout_is_valid(screen_timeout_seconds)) return;
    mutex_lock();
    s_settings_pending = true;
    s_pending_refresh_seconds = refresh_seconds;
    s_pending_auto_refresh = auto_refresh;
    s_pending_screen_timeout_seconds = screen_timeout_seconds;
    mutex_unlock();
    if (s_network_task != NULL) xTaskNotifyGive(s_network_task);
}

void quota_service_select_account(const char *account_id)
{
    if (account_id == NULL || !quota_id_is_valid(account_id)) return;
    mutex_lock();
    if (s_has_config) {
        memcpy(s_pending_account_id, account_id, QUOTA_ACCOUNT_ID_BYTES + 1);
        s_selection_pending = true;
        s_selection_persist_retry_at_ms = 0;
    }
    mutex_unlock();
    if (s_network_task != NULL) xTaskNotifyGive(s_network_task);
}

void quota_service_open_pairing_window(void)
{
    mutex_lock();
    s_pairing_screen_open = true;
    s_pairing_opened_at_ms = (int64_t)monotonic_ms();
    s_view.pairing_active = true;
    s_view.pairing_seconds_left = QUOTA_PAIRING_WINDOW_MS / 1000;
    mutex_unlock();
    post_simple_event(QUOTA_APP_EVENT_PAIRING_TICK);
}

void quota_service_close_pairing_window(void)
{
    mutex_lock();
    s_pairing_screen_open = false;
    s_view.pairing_active = false;
    s_view.pairing_seconds_left = 0;
    mutex_unlock();
}
