#include "quota_portable_service.h"
#include "quota_direct.h"
#include "quota_logic.h"
#include "quota_store.h"
#include "quota_portal.h"
#include "sdk.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static uint64_t now_ms = 100, wall = 1800000000;
static bool common_locked, usb_hold, connected, network_ok = true, store_fail, cache_fail,
                                                acquire_fail, credential_fail;
typedef struct {
    char ssid[33];
    bool available, has_ipv4;
} fake_wifi_network_t;
static fake_wifi_network_t fake_wifi_networks[QUOTA_PORTABLE_NETWORKS];
static unsigned fake_wifi_network_count;
typedef struct {
    uint8_t ssid[33];
    int8_t rssi;
} fake_scan_record_t;
static fake_scan_record_t fake_scan_records[512];
static unsigned fake_scan_record_count;
static unsigned fake_scan_read_cursor;
static unsigned fake_scan_total_records_read;
static bool fake_driver_initialized, fake_driver_started, fake_scan_active, fake_scan_complete;
static bool fake_retire_pending;
static bool fake_hold_prepare;
static uint64_t fake_wifi_deadline_ms = UINT64_MAX;
static bool fake_scan_auto_done = true, fake_scan_done_success = true, fake_scan_start_fail;
static int fake_scan_read_fail_after = -1;
static uint32_t fake_driver_request_id;
static wifi_mode_t fake_driver_mode;
static unsigned fake_scan_starts, fake_scan_clears;
static bool associated;
static char associated_ssid[33];
static void tick(bool sleeping);
static unsigned config_generation = 1, query_calls, refresh_calls, retry_calls, release_calls,
                wifi_connects, deepseek_queries;
static quota_service_view_t public_view;
static quota_direct_credential_t credentials[QUOTA_MAX_ACCOUNTS];
static bool used[QUOTA_MAX_ACCOUNTS];
static quota_snapshot_t saved_cache;
static quota_direct_credential_t *acquired;
static quota_portal_callbacks_t portal;
static bool portal_active, portal_fail, sntp_fail;
static unsigned portal_starts;
static esp_sntp_config_t sntp_config;
static void unlocked(void)
{
    assert(!common_locked);
}
static int fake_gettimeofday(struct timeval *out, void *zone)
{
    (void)zone;
    out->tv_sec = (time_t)wall;
    out->tv_usec = 0;
    return 0;
}
static int fake_settimeofday(const struct timeval *in, const struct timezone *zone)
{
    (void)zone;
    wall = (uint64_t)in->tv_sec;
    return 0;
}
/* The public snapshot is shared with the UI: observation merges into it must hold the common lock.
 */
static void checked_copy_observation(quota_snapshot_t *target, size_t target_index,
                                     const quota_snapshot_t *source, size_t source_index)
{
    if (target == &public_view.snapshot)
        assert(common_locked);
    quota_catalog_copy_observation(target, target_index, source, source_index);
}
#define quota_catalog_copy_observation checked_copy_observation
#define gettimeofday fake_gettimeofday
#define settimeofday fake_settimeofday
#include "quota_portable_service.c"
#undef quota_catalog_copy_observation
#undef gettimeofday
#undef settimeofday
struct quota_direct {
    quota_direct_hooks_t hooks;
    quota_direct_credential_t *pending;
    bool active;
};
static struct quota_direct provider;
static quota_direct_result_code_t query_code = QUOTA_DIRECT_OK, refresh_code = QUOTA_DIRECT_OK,
                                  login_code = QUOTA_DIRECT_WAITING,
                                  login_begin_code = QUOTA_DIRECT_WAITING;
int64_t esp_timer_get_time(void)
{
    return (int64_t)now_ms * 1000;
}
const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t description = {.version = "3.0.0-test"};
    return &description;
}
uint32_t esp_random(void)
{
    static uint32_t n = 7; /* a linear congruential generator; its high bits do not repeat */
    n = n * 1664525u + 1013904223u;
    return n >> 8;
}
size_t esp_get_free_heap_size(void)
{
    return 60000;
}
size_t esp_get_minimum_free_heap_size(void)
{
    return 1000;
}
size_t heap_caps_get_largest_free_block(int caps)
{
    (void)caps;
    return 30000;
}
UBaseType_t uxTaskGetStackHighWaterMark(void *task)
{
    (void)task;
    return 1024;
}
static esp_netif_t netif;
esp_netif_t *esp_netif_create_default_wifi_ap(void)
{
    unlocked();
    return &netif;
}
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key)
{
    (void)key;
    unlocked();
    return &netif;
}
esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *out)
{
    (void)n;
    unlocked();
    out->ip.addr = connected ? 1 : 0;
    return ESP_OK;
}
esp_err_t esp_wifi_set_mode(int mode)
{
    (void)mode;
    unlocked();
    return ESP_OK;
}
static wifi_config_t wifi_config;
static void fake_wifi_set_network(const char *ssid, bool available, bool has_ipv4)
{
    assert(fake_wifi_network_count < QUOTA_PORTABLE_NETWORKS);
    fake_wifi_network_t *network = &fake_wifi_networks[fake_wifi_network_count++];
    snprintf(network->ssid, sizeof(network->ssid), "%s", ssid);
    network->available = available;
    network->has_ipv4 = has_ipv4;
}
static void fake_scan_set_record(const char *ssid, int8_t rssi)
{
    size_t length = strlen(ssid);
    assert(length <= 32 && fake_scan_record_count < 512);
    fake_scan_record_t *record = &fake_scan_records[fake_scan_record_count++];
    memset(record, 0, sizeof(*record));
    memcpy(record->ssid, ssid, length);
    record->rssi = rssi;
}
static void fake_scan_show(const char *ssid)
{
    fake_scan_set_record(ssid, -40);
}
static void tick_until_wifi_connects(unsigned target)
{
    for (unsigned i = 0; i < 100 && wifi_connects < target; i++) {
        now_ms += 10;
        tick(false);
    }
    assert(wifi_connects >= target);
}
static void tick_until_network_attempt(unsigned target)
{
    for (unsigned i = 0; i < 100 && s_network_attempt_index != target; i++) {
        now_ms += 10;
        tick(false);
    }
    assert(s_network_attempt_index == target);
}
static void tick_until_setup_ready(void)
{
    for (unsigned i = 0; i < 20 && !public_view.portable.setup_ready; i++) {
        tick(false);
        if (public_view.portable.setup_ready)
            break;
        uint64_t deadline = quota_portable_service_next_deadline_ms(false);
        now_ms = deadline != UINT64_MAX && deadline > now_ms ? deadline : now_ms + 10;
    }
}
static void tick_until_portal_starts(unsigned target)
{
    for (unsigned i = 0; i < 20 && portal_starts < target; i++) {
        tick(false);
        if (portal_starts >= target)
            break;
        uint64_t deadline = quota_portable_service_next_deadline_ms(false);
        now_ms = deadline != UINT64_MAX && deadline > now_ms ? deadline : now_ms + 10;
    }
    assert(portal_starts >= target);
}
static void wifi_disconnected(uint8_t reason, const char *ssid)
{
    size_t length = strnlen(ssid, sizeof(wifi_config.sta.ssid));
    if (associated && length == strlen(associated_ssid) && !memcmp(associated_ssid, ssid, length)) {
        associated = false;
        associated_ssid[0] = 0;
        connected = false;
    }
    quota_portable_service_disconnected(reason, (const uint8_t *)ssid, length);
}
static bool fake_wifi_network(const char *ssid, bool *has_ipv4)
{
    for (unsigned i = 0; i < fake_wifi_network_count; i++) {
        if (!strcmp(fake_wifi_networks[i].ssid, ssid)) {
            *has_ipv4 = fake_wifi_networks[i].has_ipv4;
            return fake_wifi_networks[i].available;
        }
    }
    *has_ipv4 = true;
    return network_ok;
}
static void fake_wifi_associate(const char *ssid, bool has_ipv4)
{
    associated = true;
    snprintf(associated_ssid, sizeof(associated_ssid), "%s", ssid);
    connected = has_ipv4;
}
esp_err_t esp_wifi_set_config(int iface, const wifi_config_t *value)
{
    unlocked();
    if (iface == WIFI_IF_STA)
        wifi_config = *value;
    return ESP_OK;
}
esp_err_t esp_wifi_connect(void)
{
    unlocked();
    wifi_connects++;
    /* A password that contains "bad" is refused, as a real access point would. */
    bool has_ipv4 = true;
    char ssid[sizeof(wifi_config.sta.ssid) + 1] = {0};
    memcpy(ssid, wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid));
    associated = fake_wifi_network(ssid, &has_ipv4) &&
                 !strstr((const char *)wifi_config.sta.password, "bad");
    snprintf(associated_ssid, sizeof(associated_ssid), "%s", associated ? ssid : "");
    connected = associated && has_ipv4;
    return ESP_OK;
}
esp_err_t esp_wifi_disconnect(void)
{
    unlocked();
    associated = false;
    associated_ssid[0] = 0;
    connected = false;
    return ESP_OK;
}
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *out)
{
    unlocked();
    if (!associated)
        return ESP_FAIL;
    memset(out, 0, sizeof(*out));
    memcpy(out->ssid, associated_ssid, strlen(associated_ssid));
    return ESP_OK;
}
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config)
{
    unlocked();
    sntp_config = *config;
    return ESP_OK;
}
esp_err_t esp_netif_sntp_sync_wait(unsigned timeout)
{
    (void)timeout;
    unlocked();
    return sntp_fail ? ESP_FAIL : ESP_OK;
}
void esp_netif_sntp_deinit(void)
{
    unlocked();
}
/* The shape of an access code, written out independently of the production check: sixteen
 * characters of 0-9 and A-Z without I, L, O and U, in four groups divided by dashes. */
static bool valid_access_code(const char *code)
{
    if (strlen(code) != 19)
        return false;
    for (unsigned i = 0; i < 19; i++) {
        if (i % 5 == 4 ? code[i] != '-' : !strchr("0123456789ABCDEFGHJKMNPQRSTVWXYZ", code[i]))
            return false;
    }
    return true;
}
bool quota_portal_start(const char *secret, const quota_portal_callbacks_t *callbacks)
{
    unlocked();
    portal_starts++;
    if (portal_fail)
        return false;
    assert(valid_access_code(secret));
    portal = *callbacks;
    portal_active = true;
    return true;
}
void quota_portal_stop(void)
{
    unlocked();
    portal_active = false;
}
static unsigned factory_resets, restarts;
static quota_factory_reset_result_t factory_reset_result = QUOTA_FACTORY_RESET_OK;
quota_factory_reset_result_t quota_store_factory_reset(void)
{
    unlocked();
    factory_resets++;
    return factory_reset_result;
}
void esp_restart(void)
{
    restarts++;
}
bool quota_store_init(void)
{
    unlocked();
    return true;
}
quota_portable_credential_t *quota_store_credential_acquire(void)
{
    unlocked();
    if (acquired || acquire_fail)
        return NULL;
    acquired = calloc(1, sizeof(*acquired));
    return acquired;
}
void quota_store_credential_release(quota_portable_credential_t *value)
{
    unlocked();
    assert(value == acquired);
    assert(provider.pending != value);
    quota_portable_clear_secret(value, sizeof(*value));
    free(value);
    acquired = NULL;
    release_calls++;
}
static quota_store_read_result_t credential_read_error = QUOTA_STORE_READ_OK;
static bool disconnect_during_credential_load;
static quota_store_read_result_t orphan_error = QUOTA_STORE_READ_OK;
static unsigned orphan_releases;
quota_store_read_result_t quota_store_release_orphan_credentials(void)
{
    unlocked();
    orphan_releases++;
    if (orphan_error != QUOTA_STORE_READ_OK)
        return orphan_error;
    for (unsigned i = 0; i < QUOTA_MAX_ACCOUNTS; i++)
        if (used[i] && !credentials[i].tombstone) {
            credentials[i].tombstone = true;
            credentials[i].access_token[0] = credentials[i].refresh_token[0] =
                credentials[i].api_key[0] = 0;
        }
    return QUOTA_STORE_READ_OK;
}
quota_store_read_result_t quota_store_load_credential_result(uint8_t slot,
                                                             quota_portable_credential_t *out)
{
    unlocked();
    assert(out == acquired);
    memset(out, 0, sizeof(*out));
    if (credential_read_error != QUOTA_STORE_READ_OK)
        return credential_read_error;
    if (!used[slot])
        return QUOTA_STORE_READ_MISSING;
    *out = credentials[slot];
    if (disconnect_during_credential_load) {
        disconnect_during_credential_load = false;
        wifi_disconnected(201, "old-hotspot");
    }
    return QUOTA_STORE_READ_OK;
}
bool quota_store_save_credential(uint8_t slot, const quota_portable_credential_t *in)
{
    unlocked();
    assert(in == acquired);
    if (store_fail || credential_fail)
        return false;
    credentials[slot] = *in;
    used[slot] = true;
    return true;
}
bool quota_store_remove_credential(uint8_t slot, const char *id, uint32_t gen)
{
    unlocked();
    assert(!acquired);
    if (store_fail)
        return false;
    memset(&credentials[slot], 0, sizeof(credentials[slot]));
    strcpy(credentials[slot].id, id);
    credentials[slot].slot = slot;
    credentials[slot].generation = gen;
    credentials[slot].provider = QUOTA_PROVIDER_CODEX;
    credentials[slot].tombstone = true;
    used[slot] = true;
    return true;
}
quota_direct_t *quota_direct_create(const quota_direct_hooks_t *hooks,
                                    quota_direct_transport_t transport, void *context)
{
    unlocked();
    (void)transport;
    (void)context;
    provider.hooks = *hooks;
    return &provider;
}
bool quota_direct_has_pending_persist(const quota_direct_t *direct)
{
    return direct && direct->pending;
}
void quota_direct_login_cancel(quota_direct_t *direct)
{
    if (direct)
        direct->active = false;
}
static quota_direct_result_t answer(quota_direct_result_code_t code,
                                    const quota_direct_credential_t *c)
{
    quota_direct_result_t r = {0};
    r.code = code;
    r.http_status = 200;
    if (code == QUOTA_DIRECT_OK) {
        r.source_valid = true;
        r.source_epoch = wall;
        strcpy(r.account.id, c->id);
        r.account.provider = c->provider;
        r.account.has_observed_at = true;
        r.account.observed_at = wall;
        r.account.status = QUOTA_STATUS_OK;
    }
    if (code == QUOTA_DIRECT_RATE_LIMITED)
        r.retry_after_seconds = 90;
    return r;
}
quota_direct_result_t quota_direct_login_begin(quota_direct_t *direct,
                                               const quota_direct_credential_t *c, uint64_t time,
                                               uint64_t epoch)
{
    unlocked();
    (void)time;
    (void)epoch;
    assert(direct->hooks.admit(NULL, c->id, c->generation));
    direct->active = true;
    return answer(login_begin_code, c);
}
quota_direct_result_t quota_direct_login_step(quota_direct_t *direct, quota_direct_credential_t *c,
                                              uint64_t time, uint64_t epoch)
{
    unlocked();
    (void)time;
    (void)epoch;
    if (login_code == QUOTA_DIRECT_PERSIST_PENDING) {
        strcpy(c->access_token, "received-login-token");
        c->auth_state = QUOTA_PORTABLE_AUTH_READY;
        direct->pending = c;
    }
    return answer(login_code, c);
}
void quota_direct_login_view(const quota_direct_t *direct, uint64_t time,
                             quota_direct_login_view_t *out)
{
    (void)time;
    memset(out, 0, sizeof(*out));
    out->active = direct->active;
    strcpy(out->verification_url, "https://auth.openai.com/codex/device");
    strcpy(out->user_code, "fake-code");
}
quota_direct_result_t quota_direct_refresh(quota_direct_t *direct, quota_direct_credential_t *c,
                                           uint64_t epoch)
{
    unlocked();
    (void)epoch;
    refresh_calls++;
    assert(direct->hooks.admit(NULL, c->id, c->generation));
    if (refresh_code == QUOTA_DIRECT_PERSIST_PENDING) {
        strcpy(c->refresh_token, "received-rotation");
        direct->pending = c;
    }
    return answer(refresh_code, c);
}
quota_direct_result_t quota_direct_query(quota_direct_t *direct, quota_direct_credential_t *c,
                                         uint64_t epoch)
{
    unlocked();
    (void)epoch;
    if (!direct->hooks.admit(NULL, c->id, c->generation))
        return answer(QUOTA_DIRECT_DEFERRED, c);
    query_calls++;
    if (c->provider == QUOTA_PROVIDER_DEEPSEEK)
        deepseek_queries++;
    return answer(query_code, c);
}
quota_direct_result_t quota_direct_retry_persist(quota_direct_t *direct)
{
    unlocked();
    retry_calls++;
    assert(direct->pending == acquired);
    if (!direct->hooks.persist(NULL, direct->pending))
        return answer(QUOTA_DIRECT_PERSIST_PENDING, direct->pending);
    quota_direct_result_t out = answer(QUOTA_DIRECT_OK, direct->pending);
    direct->pending = NULL;
    return out;
}
static void common_lock(void)
{
    assert(!common_locked);
    common_locked = true;
}
static void common_unlock(void)
{
    assert(common_locked);
    common_locked = false;
}
static bool common_try(void)
{
    if (common_locked)
        return false;
    common_lock();
    return true;
}
static uint32_t generation_locked(void)
{
    assert(common_locked);
    return config_generation;
}
static void changed_locked(void)
{
    assert(common_locked);
    config_generation++;
}
static bool usb_enabled, usb_window, usb_scratch;
static uint64_t usb_deadline, usb_opened;
static unsigned usb_closes;
bool quota_usb_blocked(void)
{
    return usb_enabled ? usb_scratch : usb_hold;
}
bool quota_usb_active(void)
{
    return usb_enabled && usb_window && now_ms < usb_deadline;
}
uint64_t quota_usb_deadline_ms(void)
{
    return usb_deadline;
}
uint64_t quota_usb_window_id(void)
{
    return usb_opened;
}
void quota_usb_close_window(void)
{
    usb_window = false;
    usb_closes++;
}
static void usb_open(void)
{
    usb_enabled = true;
    usb_window = true;
    usb_opened = now_ms;
    usb_deadline = now_ms + 120000;
    assert(quota_portable_service_prepare_usb());
}
bool quota_wifi_start(void)
{
    unlocked();
    if (!fake_driver_initialized)
        return false;
    fake_driver_started = true;
    return true;
}
bool quota_wifi_stop(void)
{
    unlocked();
    quota_wifi_retire_begin();
    return true;
}
bool quota_wifi_started(void)
{
    return fake_driver_started;
}
quota_wifi_result_t quota_wifi_prepare(uint32_t request_id, wifi_mode_t mode)
{
    unlocked();
    if (!request_id)
        return QUOTA_WIFI_FAILED;
    if (fake_retire_pending || fake_hold_prepare)
        return QUOTA_WIFI_WAIT;
    if (fake_driver_initialized &&
        (fake_driver_request_id != request_id || fake_driver_mode != mode)) {
        /* The controller fake completes retirement synchronously. Driver unit tests exercise
         * the real asynchronous gate and both event fences. */
        quota_wifi_retire_begin();
        return QUOTA_WIFI_WAIT;
    }
    if (!fake_driver_initialized) {
        fake_driver_initialized = true;
        fake_driver_request_id = request_id;
        fake_driver_mode = mode;
    }
    return QUOTA_WIFI_READY;
}
void quota_wifi_retire_begin(void)
{
    if (fake_retire_pending)
        return;
    if (!fake_driver_initialized && !fake_driver_started && !fake_scan_active &&
        !fake_scan_complete)
        return;
    fake_retire_pending = true;
    fake_driver_started = false;
    fake_driver_initialized = false;
    fake_driver_request_id = 0;
}
quota_wifi_result_t quota_wifi_retire_poll(void)
{
    if (fake_retire_pending) {
        fake_retire_pending = false;
        if (fake_scan_active || fake_scan_complete)
            fake_scan_clears++;
        fake_scan_active = fake_scan_complete = false;
        associated = connected = false;
        associated_ssid[0] = 0;
    }
    return QUOTA_WIFI_READY;
}
bool quota_wifi_operation_active(uint32_t request_id, wifi_mode_t mode)
{
    return request_id && fake_driver_started && fake_driver_request_id == request_id &&
           fake_driver_mode == mode && !fake_retire_pending;
}
uint64_t quota_wifi_next_deadline_ms(bool sleeping)
{
    (void)sleeping;
    return fake_wifi_deadline_ms;
}
bool quota_wifi_scan_start(const wifi_scan_config_t *config)
{
    unlocked();
    assert(config && config->scan_type == WIFI_SCAN_TYPE_ACTIVE && config->channel == 0);
    if (fake_scan_start_fail || !fake_driver_started || fake_driver_mode != WIFI_MODE_STA)
        return false;
    fake_scan_starts++;
    fake_scan_active = true;
    fake_scan_complete = false;
    fake_scan_read_cursor = 0;
    if (fake_scan_auto_done) {
        fake_scan_active = false;
        fake_scan_complete = fake_scan_done_success;
        quota_portable_service_scan_done(fake_scan_done_success);
    }
    return true;
}
esp_err_t quota_wifi_scan_get_ap_num(uint16_t *number)
{
    if (!number || !fake_scan_complete)
        return ESP_FAIL;
    *number = (uint16_t)fake_scan_record_count;
    return ESP_OK;
}
esp_err_t quota_wifi_scan_get_ap_record(wifi_ap_record_t *record)
{
    if (!record || !fake_scan_complete || fake_scan_read_cursor >= fake_scan_record_count ||
        (fake_scan_read_fail_after >= 0 && (int)fake_scan_read_cursor >= fake_scan_read_fail_after))
        return ESP_FAIL;
    fake_scan_record_t *source = &fake_scan_records[fake_scan_read_cursor++];
    fake_scan_total_records_read++;
    memset(record, 0, sizeof(*record));
    memcpy(record->ssid, source->ssid, sizeof(record->ssid));
    record->rssi = source->rssi;
    return ESP_OK;
}
esp_err_t quota_wifi_scan_clear_ap_list(void)
{
    fake_scan_clears++;
    fake_scan_active = fake_scan_complete = false;
    fake_scan_read_cursor = 0;
    return ESP_OK;
}
static void notify(void)
{
    unlocked();
}
static void hook_wake(void)
{
    unlocked();
}
static bool display_current(uint32_t generation)
{
    (void)generation;
    return !s_sleeping;
}

static quota_model_t durable_model;
static uint64_t durable_sequence;
static quota_store_read_result_t model_read_error = QUOTA_STORE_READ_OK;
static bool model_fail, model_unknown, model_apply_unknown;
quota_store_read_result_t quota_store_load_model_result(quota_model_t *out, uint64_t *sequence)
{
    unlocked();
    if (model_read_error != QUOTA_STORE_READ_OK)
        return model_read_error;
    if (!durable_sequence)
        return QUOTA_STORE_READ_MISSING;
    *out = durable_model;
    *sequence = durable_sequence;
    return QUOTA_STORE_READ_OK;
}
quota_model_write_result_t quota_store_save_model_verified(uint64_t previous,
                                                           const quota_model_t *candidate,
                                                           uint64_t sequence)
{
    unlocked();
    if (!quota_catalog_valid(candidate))
        fprintf(
            stderr,
            "invalid model networks=%u selected=%u refresh=%u timeout=%u entries=%u intent=%u\n",
            candidate->network_count, candidate->selected_network, candidate->refresh_seconds,
            candidate->screen_timeout_seconds, candidate->entry_count, candidate->intent.kind);
    assert(quota_catalog_valid(candidate));
    if (durable_sequence == sequence && !memcmp(candidate, &durable_model, sizeof(*candidate)))
        return QUOTA_MODEL_APPLIED;
    if (model_unknown) {
        if (model_apply_unknown) {
            durable_model = *candidate;
            durable_sequence = sequence;
        }
        return QUOTA_MODEL_WRITE_UNKNOWN;
    }
    if (model_fail || store_fail)
        return QUOTA_MODEL_NOT_APPLIED;
    assert(durable_sequence == previous);
    durable_model = *candidate;
    durable_sequence = sequence;
    return QUOTA_MODEL_APPLIED;
}
bool quota_store_save_observations(const quota_model_t *model, const quota_snapshot_t *snapshot,
                                   uint64_t time)
{
    unlocked();
    (void)model;
    (void)time;
    if (cache_fail)
        return false;
    saved_cache = *snapshot;
    return true;
}
quota_store_read_result_t quota_store_load_observations(const quota_model_t *model, uint64_t time,
                                                        quota_snapshot_t *snapshot)
{
    unlocked();
    (void)model;
    (void)time;
    for (unsigned i = 0; i < saved_cache.account_count; i++) {
        int index = quota_find_account_by_id(snapshot, saved_cache.accounts[i].id);
        if (index >= 0)
            quota_catalog_copy_observation(snapshot, (size_t)index, &saved_cache, i);
    }
    return QUOTA_STORE_READ_OK;
}
static void initialize(void)
{
    for (unsigned i = 0; i < 2; i++) {
        used[i] = true;
        credentials[i].slot = i;
        credentials[i].generation = 1;
        credentials[i].provider = i ? QUOTA_PROVIDER_DEEPSEEK : QUOTA_PROVIDER_CODEX;
        credentials[i].auth_state = QUOTA_PORTABLE_AUTH_READY;
        snprintf(credentials[i].id, sizeof(credentials[i].id), "%032x", i + 1);
        strcpy(credentials[i].server_account_id, "fake-server");
        strcpy(credentials[i].access_token, "old-access");
        strcpy(credentials[i].refresh_token, "old-refresh");
        strcpy(credentials[i].api_key, "old-key");
        credentials[i].expires_at = wall + 3600;
    }
    saved_cache.account_count = 1;
    saved_cache.refresh_seconds = 300;
    saved_cache.accounts[0].provider = QUOTA_PROVIDER_CODEX;
    strcpy(saved_cache.accounts[0].id, credentials[0].id);
    saved_cache.accounts[0].has_observed_at = true;
    saved_cache.accounts[0].observed_at = wall;
    saved_cache.accounts[0].five_hour.present = true;
    saved_cache.accounts[0].five_hour.remaining_percent = 17;
}
static bool fresh_device;
static void seed_model(void);
static void boot(void)
{
    if (!durable_sequence && !fresh_device)
        seed_model();
    quota_portable_service_hooks_t hooks = {.view = &public_view,
                                            .lock = common_lock,
                                            .unlock = common_unlock,
                                            .try_lock = common_try,
                                            .config_generation_locked = generation_locked,
                                            .config_changed_locked = changed_locked,
                                            .notify = notify,
                                            .wake = hook_wake,
                                            .display_current = display_current};
    assert(quota_portable_service_init(&hooks));
}
static void tick(bool sleeping)
{
    quota_portable_service_tick(sleeping, 1);
    assert(!common_locked);
}
static void ready(void)
{
    tick(false);
    now_ms += 500;
    tick(false);
    assert(public_view.portable.network_state == QUOTA_PORTABLE_NETWORK_READY);
}
static void begin_preferred_fallback_scan(void)
{
    tick(false);
    assert(wifi_connects == 1);
    now_ms += 25000;
    tick(false);
    now_ms += 10;
    tick(false);
    assert(fake_scan_starts == 1);
}
static void phone(void)
{
    quota_portable_service_open();
    for (unsigned i = 0; i < 10 && !public_view.portable.setup_ready; i++) {
        tick(false);
        now_ms += 500;
    }
    assert(portal_active && public_view.portable.setup_ready);
}
static void submit_command(quota_portable_command_t *command)
{
    command->phone_utc = wall;
    assert(portal.submit(command, NULL) == QUOTA_PORTABLE_SUBMIT_ACCEPTED);
    tick(false);
}
static job_t *job(const char *id)
{
    for (unsigned i = 0; i < s_job_count; i++)
        if (!strcmp(s_jobs[i].id, id))
            return &s_jobs[i];
    assert(false);
    return NULL;
}
static void close_phone(void)
{
    quota_portable_service_close();
    tick(false);
    now_ms += 500;
    ready();
}
/* Public state as the page reads it: parsed, so tests look at the contract rather than text. */
static cJSON *state_of(quota_setup_transport_t transport)
{
    static char text[QUOTA_PORTABLE_STATE_BYTES + 1];
    size_t length = 0;
    assert(quota_portable_service_state_json(text, sizeof(text), &length, transport));
    cJSON *root = cJSON_Parse(text);
    assert(root);
    return root;
}
/* One member of an array item of the state, for example ("network.saved_networks", 0, ...). */
static const char *state_item(cJSON *root, const char *list, unsigned index, const char *member)
{
    cJSON *array = cJSON_GetObjectItemCaseSensitive(root, list);
    if (!strcmp(list, "saved_networks"))
        array = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "network"),
                                                 list);
    cJSON *item = cJSON_GetArrayItem(array, (int)index);
    cJSON *value = cJSON_GetObjectItemCaseSensitive(item, member);
    assert(cJSON_IsString(value));
    return value->valuestring;
}
/* The validation of Wi-Fi network `index` as the page sees it ("pending", "ok", "failed"). */
static const char *network_state(unsigned index, const char **error)
{
    static char validation[16], code[48];
    cJSON *root = state_of(QUOTA_SETUP_USB);
    snprintf(validation, sizeof(validation), "%s",
             state_item(root, "saved_networks", index, "validation"));
    snprintf(code, sizeof(code), "%s", state_item(root, "saved_networks", index, "error_code"));
    cJSON_Delete(root);
    if (error)
        *error = code;
    return validation;
}
static const char *account_state(unsigned index, const char **error)
{
    static char validation[16], code[48];
    cJSON *root = state_of(QUOTA_SETUP_USB);
    snprintf(validation, sizeof(validation), "%s",
             state_item(root, "accounts", index, "validation"));
    snprintf(code, sizeof(code), "%s", state_item(root, "accounts", index, "error_code"));
    cJSON_Delete(root);
    if (error)
        *error = code;
    return validation;
}
static unsigned account_count_in_state(void)
{
    cJSON *root = state_of(QUOTA_SETUP_USB);
    unsigned count =
        (unsigned)cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(root, "accounts"));
    cJSON_Delete(root);
    return count;
}
static void submit_usb(quota_portable_command_t *command)
{
    command->phone_utc = wall;
    assert(quota_portable_service_submit(command, QUOTA_SETUP_USB) ==
           QUOTA_PORTABLE_SUBMIT_ACCEPTED);
    tick(false);
}
/* Let the network task run until the validation pass has ended. */
static void settle(void)
{
    for (unsigned i = 0; i < 400 && (s_validation.active || s_validation.requested); i++) {
        now_ms += 500;
        tick(false);
    }
    assert(!s_validation.active && !s_validation.requested);
}
static void run_until_login_started(void)
{
    for (unsigned i = 0; i < 40 && !s_operation.started; i++) {
        now_ms += 500;
        tick(false);
    }
    assert(s_operation.kind == OP_LOGIN && s_operation.started);
}
static void seed_model(void)
{
    model_defaults(&durable_model);
    durable_model.network_count = 1;
    strcpy(durable_model.networks[0].ssid, "old-hotspot");
    strcpy(durable_model.networks[0].password, "password");
    for (unsigned i = 0; i < QUOTA_MAX_ACCOUNTS; i++) {
        if (!used[i])
            continue;
        quota_catalog_entry_t *e = &durable_model.entries[durable_model.entry_count++];
        strcpy(e->logical_id, credentials[i].id);
        e->provider = credentials[i].provider;
        e->source = QUOTA_ACCOUNT_DEVICE;
        e->activity = QUOTA_ACCOUNT_ACTIVE;
        e->row_generation = 1;
        e->binding.native.slot = i;
        strcpy(e->binding.native.credential_id, credentials[i].id);
        e->binding.native.credential_generation = credentials[i].generation;
    }
    durable_sequence = 1;
}
static void seed_intent(void)
{
    seed_model();
    quota_model_intent_t *intent = &durable_model.intent;
    intent->kind = QUOTA_INTENT_UPSERT_NATIVE;
    intent->provider = QUOTA_PROVIDER_DEEPSEEK;
    intent->slot = 1;
    strcpy(intent->logical_id, credentials[1].id);
    intent->expected_row_generation = 1;
    strcpy(intent->previous_credential_id, credentials[1].id);
    intent->previous_credential_generation = 1;
    strcpy(intent->target_credential_id, credentials[1].id);
    intent->target_credential_generation = 2;
    strcpy(intent->desired_label, "recovered alias");
}
static void all_native(void)
{
    for (unsigned i = 2; i < 8; i++) {
        credentials[i] = credentials[1];
        credentials[i].slot = i;
        snprintf(credentials[i].id, sizeof(credentials[i].id), "%032x", i + 1);
        used[i] = true;
    }
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    initialize();
    if (!strcmp(argv[1], "boot")) {
        credentials[0].refresh_inflight = true;
        acquire_fail = true;
        boot();
        assert(!public_view.snapshot_valid);
        acquire_fail = false;
        now_ms += 5000;
        tick(false);
        assert(public_view.snapshot.account_count == 2 &&
               public_view.snapshot.accounts[0].five_hour.remaining_percent == 17);
        assert(public_view.snapshot.accounts[0].status == QUOTA_STATUS_EXPIRED);
        assert(!public_view.snapshot.accounts[1].has_observed_at);
        assert(!acquired);
    } else if (!strcmp(argv[1], "pending")) {
        credentials[0].expires_at = 1;
        refresh_code = QUOTA_DIRECT_PERSIST_PENDING;
        boot();
        ready();
        quota_portable_service_refresh();
        tick(false);
        assert(acquired && provider.pending == acquired && s_operation.kind == OP_SAVE);
        uint64_t due = s_operation.retry_at;
        assert(!quota_portable_service_prepare_usb());
        unsigned http = refresh_calls + query_calls;
        store_fail = true;
        now_ms = due;
        tick(true);
        assert(retry_calls == 1 && acquired);
        now_ms += 5000;
        tick(true);
        assert(retry_calls == 2);
        store_fail = false;
        now_ms += 30000;
        tick(true);
        assert(!provider.pending && !acquired && s_operation.kind == OP_NONE);
        assert(refresh_calls + query_calls == http);
        assert(!strcmp(credentials[0].refresh_token, "received-rotation"));
        assert(quota_portable_service_next_deadline_ms(true) == UINT64_MAX);
    } else if (!strcmp(argv[1], "key")) {
        /* A key is stored at once as it is, pending. Nothing asks DeepSeek and no record is held.
         */
        boot();
        phone();
        quota_portable_command_t cmd = {.op = QUOTA_PORTABLE_OP_DEEPSEEK_SAVE};
        strcpy(cmd.request_id, "12345678");
        strcpy(cmd.account_id, credentials[1].id);
        strcpy(cmd.api_key, "candidate-key");
        strcpy(cmd.label, "candidate");
        submit_command(&cmd);
        assert(!acquired && !strcmp(credentials[1].api_key, "candidate-key") &&
               credentials[1].generation == 2 &&
               credentials[1].auth_state == QUOTA_PORTABLE_AUTH_PENDING);
        assert(durable_model.intent.kind == QUOTA_INTENT_NONE &&
               !strcmp(durable_model.entries[1].label, "candidate"));
        assert(job(cmd.request_id)->state == 2 && query_calls == 0);
        assert(!strcmp(account_state(1, NULL), "pending") && !strcmp(account_state(0, NULL), "ok"));
        /* Closing the hotspot from the Passport saves nothing and validates nothing. */
        close_phone();
        for (unsigned i = 0; i < 6; i++) {
            now_ms += 500;
            tick(false);
        }
        assert(s_validate_runs == 0 && !strcmp(account_state(1, NULL), "pending"));
        assert(!public_view.snapshot.accounts[1].has_observed_at); /* not asked in the background */
        unsigned background = query_calls;
        /* setup_close does: the hotspot closes, then the one validation function runs. */
        phone();
        quota_portable_command_t close = {.op = QUOTA_PORTABLE_OP_SETUP_CLOSE};
        strcpy(close.request_id, "22345678");
        submit_command(&close);
        assert(portal_active && s_validate_runs == 0);
        now_ms += 500;
        tick(false);
        assert(!portal_active && s_validate_runs == 1);
        settle();
        assert(query_calls == background + 1 && !strcmp(account_state(1, NULL), "ok") &&
               credentials[1].auth_state == QUOTA_PORTABLE_AUTH_READY &&
               !strcmp(credentials[1].api_key, "candidate-key"));
    } else if (!strcmp(argv[1], "cancel")) {
        /* A queued ChatGPT authorization is only a note in RAM until validation reaches it. */
        boot();
        phone();
        quota_portable_command_t cmd = {.op = QUOTA_PORTABLE_OP_CODEX_QUEUE};
        strcpy(cmd.request_id, "12345678");
        strcpy(cmd.label, "Work");
        submit_command(&cmd);
        assert(s_login_queue.present && s_login_queue.is_new && !acquired &&
               job(cmd.request_id)->state == 2);
        assert(public_view.snapshot.account_count == 2 && account_count_in_state() == 3);
        assert(!strcmp(account_state(2, NULL), "pending") &&
               durable_model.intent.kind == QUOTA_INTENT_NONE);
        quota_portable_command_t again = {.op = QUOTA_PORTABLE_OP_CODEX_QUEUE};
        strcpy(again.request_id, "22345678");
        submit_command(&again); /* one authorization at a time */
        assert(job(again.request_id)->state == 3 &&
               !strcmp(job(again.request_id)->error, "login_pending"));
        quota_portable_command_t cancel = {.op = QUOTA_PORTABLE_OP_OPERATION_CANCEL};
        strcpy(cancel.request_id, "32345678");
        strcpy(cancel.target_request_id, cmd.request_id);
        submit_command(&cancel);
        assert(!s_login_queue.present && account_count_in_state() == 2 && !acquired);
    } else if (!strcmp(argv[1], "queue")) {
        boot();
        phone();
        quota_portable_command_t cmd = {.op = QUOTA_PORTABLE_OP_NETWORK_SAVE, .network_index = 0};
        strcpy(cmd.request_id, "12345678");
        strcpy(cmd.ssid, "new-hotspot");
        strcpy(cmd.password, "password");
        assert(portal.submit(&cmd, NULL) == QUOTA_PORTABLE_SUBMIT_ACCEPTED);
        assert(portal.submit(&cmd, NULL) == QUOTA_PORTABLE_SUBMIT_ACCEPTED);
        common_lock();
        config_generation++;
        common_unlock();
        tick(false);
        assert(!strcmp(job(cmd.request_id)->error, "configuration_changed") &&
               !strcmp(durable_model.networks[0].ssid, "old-hotspot"));
        strcpy(cmd.request_id, "22345678");
        submit_command(&cmd);
        /* New credentials for the network in use wait for validation; the stored ones stay. */
        assert(job(cmd.request_id)->state == 2 && durable_model.network_count == 1 &&
               !strcmp(durable_model.networks[0].ssid, "old-hotspot"));
        assert(s_staged.present && !strcmp(s_staged.network.ssid, "new-hotspot"));
        assert(!strcmp(network_state(0, NULL), "pending") && !s_candidate_pending &&
               s_validate_runs == 0);
        cJSON *root = state_of(QUOTA_SETUP_USB);
        assert(!strcmp(state_item(root, "saved_networks", 0, "ssid"), "new-hotspot"));
        cJSON_Delete(root);
        assert(public_view.portable.saved_network_count == 1 &&
               !strcmp(public_view.portable.saved_network_ssids[0], "old-hotspot") &&
               public_view.portable.saved_network_validation[0] == QUOTA_VALIDATION_PENDING &&
               public_view.portable.pending_items == 1);
    } else if (!strcmp(argv[1], "cadence")) {
        boot();
        ready();
        uint64_t deadline = s_next_refresh;
        tick(true);
        now_ms += 100;
        tick(false);
        assert(s_next_refresh == deadline);
        phone();
        assert(s_next_refresh == deadline);
        quota_portable_service_close();
        tick(false);
        assert(s_next_refresh == deadline);
    } else if (!strcmp(argv[1], "state-contract")) {
        boot();
        ready();
        quota_portable_service_refresh();
        for (unsigned i = 0; i < 5; i++) {
            now_ms += 500;
            tick(false);
        }
        assert(query_calls >= 2);
        assert(public_view.refresh_seconds == 300);
        phone();
        quota_portable_command_t settings = {.op = QUOTA_PORTABLE_OP_SETTINGS_SAVE,
                                             .refresh_seconds = 900,
                                             .screen_timeout_seconds = 120};
        strcpy(settings.request_id, "12345678");
        submit_command(&settings);
        assert(public_view.refresh_seconds == 900 && job(settings.request_id)->state == 2);
        char json[QUOTA_PORTABLE_STATE_BYTES];
        size_t length;
        assert(state_json(json, sizeof(json), &length, NULL));
        assert(!strstr(json, "pending_accounts") && !strstr(json, "pending_network") &&
               !strstr(json, "\"source\"") && !strstr(json, "source_changed"));
        assert(!strstr(json, "old-key") && !strstr(json, "old-access") &&
               !strstr(json, "\"mode\""));
        /* Protocol 3: identity, validation, and no leftover of removed features. */
        assert(strstr(json, "\"protocol\":3,\"firmware\":\"3.0.0-test\"") &&
               strstr(json, "\"validating\":false") && strstr(json, "\"validation\":\"ok\""));
        assert(!strstr(json, "collector") && !strstr(json, "\"candidate\"") &&
               !strstr(json, "\"kind\":\"network\""));
        assert(!strcmp(network_state(0, NULL), "ok") && !strcmp(account_state(0, NULL), "ok"));
    } else if (!strcmp(argv[1], "physical-gate")) {
        boot();
        ready();
        assert(quota_portable_service_http_allowed());
        quota_portable_service_open();
        assert(!quota_portable_service_http_allowed());
        assert(!admit(NULL, credentials[0].id, 1));
        tick_until_setup_ready();
        assert(portal_active);
    } else if (!strcmp(argv[1], "typed-read")) {
        credential_read_error = QUOTA_STORE_READ_IO_ERROR;
        boot();
        assert(!public_view.snapshot_valid && durable_sequence == 1);
        assert(!strcmp(s_storage_error, "storage_io_error"));
        assert(!quota_portable_service_http_allowed());
        credential_read_error = QUOTA_STORE_READ_OK;
        now_ms += 5000;
        tick(false);
        assert(public_view.snapshot.account_count == 2);
    } else if (!strcmp(argv[1], "unknown")) {
        boot();
        phone();
        model_unknown = true;
        model_apply_unknown = true;
        quota_portable_command_t cmd = {.op = QUOTA_PORTABLE_OP_SETTINGS_SAVE,
                                        .refresh_seconds = 900,
                                        .screen_timeout_seconds = 120};
        strcpy(cmd.request_id, "12345678");
        submit_command(&cmd);
        assert(s_dirty_model && public_view.refresh_seconds == 300 &&
               job(cmd.request_id)->state == 1);
        assert(!quota_portable_service_http_allowed());
        model_unknown = false;
        now_ms += 1000;
        tick(true);
        assert(!s_dirty_model && public_view.refresh_seconds == 900 &&
               job(cmd.request_id)->state == 2);
    } else if (!strcmp(argv[1], "delete")) {
        boot();
        phone();
        quota_portable_command_t cmd = {.op = QUOTA_PORTABLE_OP_ACCOUNT_REMOVE};
        strcpy(cmd.request_id, "12345678");
        strcpy(cmd.account_id, credentials[1].id);
        submit_command(&cmd);
        tick(false);
        assert(public_view.snapshot.account_count == 1 && credentials[1].tombstone &&
               credentials[1].generation == 2);
        assert(durable_model.intent.kind == QUOTA_INTENT_NONE);
    } else if (!strcmp(argv[1], "generation")) {
        credentials[1].generation = UINT32_MAX;
        boot();
        phone();
        quota_portable_command_t cmd = {.op = QUOTA_PORTABLE_OP_DEEPSEEK_SAVE};
        strcpy(cmd.request_id, "12345678");
        strcpy(cmd.account_id, credentials[1].id);
        strcpy(cmd.api_key, "candidate-key");
        submit_command(&cmd);
        assert(job(cmd.request_id)->state == 3 &&
               !strcmp(job(cmd.request_id)->error, "generation_exhausted"));
        assert(!acquired);
    } else if (!strcmp(argv[1], "recovery-previous")) {
        seed_intent();
        boot();
        assert(durable_model.intent.kind == QUOTA_INTENT_NONE &&
               durable_model.entries[1].binding.native.credential_generation == 1);
        assert(!query_calls && !refresh_calls && !acquired);
    } else if (!strcmp(argv[1], "recovery-target")) {
        seed_intent();
        credentials[1].generation = 2;
        strcpy(credentials[1].api_key, "durable-received-key");
        boot();
        assert(durable_model.intent.kind == QUOTA_INTENT_NONE &&
               durable_model.entries[1].binding.native.credential_generation == 2 &&
               durable_model.entries[1].row_generation == 2);
        assert(!query_calls && !refresh_calls && !acquired);
        assert(!strcmp(durable_model.entries[1].label, "recovered alias"));
    } else if (!strcmp(argv[1], "recovery-conflict")) {
        seed_intent();
        used[1] = false;
        boot();
        assert(durable_model.intent.kind == QUOTA_INTENT_UPSERT_NATIVE &&
               !strcmp(s_storage_error, "recovery_conflict"));
        assert(!query_calls && !refresh_calls && !quota_portable_service_http_allowed());
    } else if (!strcmp(argv[1], "invalid-model")) {
        model_read_error = QUOTA_STORE_READ_INVALID;
        boot();
        assert(durable_sequence == 1 && !public_view.snapshot_valid &&
               !strcmp(s_storage_error, "storage_invalid"));
        now_ms += 10000;
        tick(false);
        assert(durable_sequence == 1 && !query_calls);
    } else if (!strcmp(argv[1], "full-native")) {
        all_native();
        boot();
        assert(public_view.snapshot.account_count == 8);
        phone();
        quota_portable_command_t cmd = {.op = QUOTA_PORTABLE_OP_DEEPSEEK_SAVE};
        strcpy(cmd.request_id, "12345678");
        strcpy(cmd.account_id, credentials[1].id);
        strcpy(cmd.api_key, "candidate-key");
        submit_command(&cmd);
        assert(credentials[1].generation == 2 && !acquired && job(cmd.request_id)->state == 2 &&
               durable_model.entry_count == 8);
    } else if (!strcmp(argv[1], "received-cancel")) {
        /* A key that reached the Passport is never dropped: it is saved before anything else. */
        boot();
        phone();
        quota_portable_command_t cmd = {.op = QUOTA_PORTABLE_OP_DEEPSEEK_SAVE};
        strcpy(cmd.request_id, "12345678");
        strcpy(cmd.account_id, credentials[1].id);
        strcpy(cmd.api_key, "received-key");
        credential_fail = true;
        submit_command(&cmd);
        assert(acquired && s_operation.kind == OP_SAVE && s_operation.received);
        quota_portable_service_cancel_auth();
        quota_portable_service_open();
        usb_hold = true;
        tick(true);
        assert(acquired && s_model.intent.kind == QUOTA_INTENT_UPSERT_NATIVE);
        unsigned http = query_calls + refresh_calls;
        credential_fail = false;
        now_ms += 1000;
        tick(true);
        assert(!acquired && !strcmp(credentials[1].api_key, "received-key") &&
               durable_model.intent.kind == QUOTA_INTENT_NONE);
        assert(job(cmd.request_id)->state == 2 && query_calls + refresh_calls == http);
    } else if (!strcmp(argv[1], "physical-login")) {
        /* A hotspot requested on the Passport ends a running authorization and its validation. */
        boot();
        ready();
        usb_open();
        quota_portable_command_t queue = {.op = QUOTA_PORTABLE_OP_CODEX_QUEUE};
        strcpy(queue.request_id, "12345678");
        strcpy(queue.label, "Chat");
        submit_usb(&queue);
        assert(s_login_queue.present && !acquired && s_operation.kind == OP_NONE);
        quota_portable_command_t validate = {.op = QUOTA_PORTABLE_OP_VALIDATE};
        strcpy(validate.request_id, "22345678");
        submit_usb(&validate);
        run_until_login_started();
        assert(s_validation.active && acquired && s_operation.job_id[0]);
        quota_portable_service_open();
        assert(!quota_portable_service_http_allowed());
        tick_until_setup_ready();
        assert(portal_active && !acquired && s_operation.kind == OP_NONE &&
               durable_model.intent.kind == QUOTA_INTENT_NONE);
        /* The account stays queued, failed, for the next 完成设置. */
        assert(!s_validation.active && s_login_queue.present && s_login_queue.is_new &&
               !strcmp(s_login_queue.error, "canceled") && public_view.snapshot.account_count == 2);
        assert(!strcmp(account_state(2, NULL), "failed") && public_view.portable.failed_items == 1);
        assert(job(validate.request_id)->state == 3 &&
               !strcmp(job(validate.request_id)->error, "canceled"));
    } else if (!strcmp(argv[1], "received-model-unknown")) {
        boot();
        phone();
        quota_portable_command_t cmd = {.op = QUOTA_PORTABLE_OP_DEEPSEEK_SAVE};
        strcpy(cmd.request_id, "12345678");
        strcpy(cmd.account_id, credentials[1].id);
        strcpy(cmd.api_key, "received-key");
        model_unknown = true;
        model_apply_unknown = true;
        submit_command(&cmd);
        assert(acquired && s_dirty_model && s_operation.kind == OP_SAVE);
        unsigned calls = query_calls;
        model_unknown = false;
        now_ms += 1000;
        tick(true);
        assert(!s_dirty_model && !acquired && s_model.intent.kind == QUOTA_INTENT_NONE &&
               credentials[1].generation == 2 && !strcmp(credentials[1].api_key, "received-key"));
        assert(query_calls == calls);
    } else if (!strcmp(argv[1], "sleep-open-deadline")) {
        boot();
        tick(true);
        quota_portable_service_open();
        assert(quota_portable_service_next_deadline_ms(true) == UINT64_MAX);
        tick(true);
        assert(s_open && !portal_active &&
               quota_portable_service_next_deadline_ms(true) == UINT64_MAX);
        tick(false);
        assert(portal_active);
    } else if (!strcmp(argv[1], "prepare-unknown")) {
        /* The authorization record is prepared when validation reaches it; an unknown write result
         * holds everything until it is resolved. */
        boot();
        ready();
        usb_open();
        quota_portable_command_t queue = {.op = QUOTA_PORTABLE_OP_CODEX_QUEUE};
        strcpy(queue.request_id, "12345678");
        submit_usb(&queue);
        model_unknown = true;
        quota_portable_command_t validate = {.op = QUOTA_PORTABLE_OP_VALIDATE};
        strcpy(validate.request_id, "22345678");
        submit_usb(&validate);
        assert(s_dirty_model && job(validate.request_id)->state == 1 &&
               public_view.portable.login_state == QUOTA_PORTABLE_LOGIN_CONNECTING);
        assert(!quota_portable_service_http_allowed() && !s_operation.started);
        model_unknown = false;
        now_ms += 1000;
        tick(false);
        assert(!s_dirty_model && durable_model.intent.kind == QUOTA_INTENT_UPSERT_NATIVE &&
               s_validation.active);
    } else if (!strcmp(argv[1], "prepare-unknown-cancel")) {
        boot();
        ready();
        usb_open();
        quota_portable_command_t queue = {.op = QUOTA_PORTABLE_OP_CODEX_QUEUE};
        strcpy(queue.request_id, "12345678");
        submit_usb(&queue);
        model_unknown = true;
        quota_portable_command_t validate = {.op = QUOTA_PORTABLE_OP_VALIDATE};
        strcpy(validate.request_id, "22345678");
        submit_usb(&validate);
        quota_portable_command_t cancel = {.op = QUOTA_PORTABLE_OP_OPERATION_CANCEL};
        strcpy(cancel.request_id, "32345678");
        strcpy(cancel.target_request_id, validate.request_id);
        submit_usb(&cancel);
        assert(s_operation.cancel_requested && s_dirty_model && acquired);
        model_unknown = false;
        now_ms += 1000;
        tick(false);
        settle();
        assert(!s_dirty_model && !acquired && durable_model.intent.kind == QUOTA_INTENT_NONE &&
               job(validate.request_id)->state == 3 &&
               !strcmp(job(validate.request_id)->error, "canceled"));
        assert(!query_calls && !refresh_calls);
    } else if (!strcmp(argv[1], "unknown-corruption")) {
        boot();
        phone();
        model_unknown = true;
        quota_portable_command_t cmd = {.op = QUOTA_PORTABLE_OP_SETTINGS_SAVE,
                                        .refresh_seconds = 900,
                                        .screen_timeout_seconds = 120};
        strcpy(cmd.request_id, "12345678");
        submit_command(&cmd);
        model_read_error = QUOTA_STORE_READ_INVALID;
        now_ms += 1000;
        tick(true);
        assert(s_dirty_model && s_recovery_blocked && !strcmp(s_storage_error, "storage_invalid") &&
               public_view.refresh_seconds == 300);
        assert(quota_portable_service_next_deadline_ms(true) == UINT64_MAX);
    } else if (!strcmp(argv[1], "unknown-conflict")) {
        boot();
        phone();
        model_unknown = true;
        quota_portable_command_t cmd = {.op = QUOTA_PORTABLE_OP_SETTINGS_SAVE,
                                        .refresh_seconds = 900,
                                        .screen_timeout_seconds = 120};
        strcpy(cmd.request_id, "12345678");
        submit_command(&cmd);
        durable_sequence += 5;
        now_ms += 1000;
        tick(true);
        assert(s_dirty_model && s_recovery_blocked &&
               !strcmp(s_storage_error, "recovery_conflict") && public_view.refresh_seconds == 300);
        assert(quota_portable_service_next_deadline_ms(true) == UINT64_MAX);
    } else if (!strcmp(argv[1], "label-only-full")) {
        all_native();
        credentials[1].generation = UINT32_MAX;
        boot();
        phone();
        uint32_t generation = durable_model.entries[1].row_generation;
        quota_portable_command_t cmd = {.op = QUOTA_PORTABLE_OP_DEEPSEEK_SAVE};
        strcpy(cmd.request_id, "12345678");
        strcpy(cmd.account_id, credentials[1].id);
        strcpy(cmd.label, "renamed alias");
        submit_command(&cmd);
        assert(job(cmd.request_id)->state == 2 && !acquired &&
               credentials[1].generation == UINT32_MAX &&
               durable_model.entries[1].row_generation == generation &&
               !strcmp(durable_model.entries[1].label, "renamed alias"));
        assert(durable_model.intent.kind == QUOTA_INTENT_NONE && !query_calls);
    } else if (!strcmp(argv[1], "offline-deadlines")) {
        boot();
        ready();
        s_model.auto_refresh = false;
        s_cache_at = now_ms - 1;
        s_cache_dirty = true;
        s_clock_ready = false;
        s_view.network_state = QUOTA_PORTABLE_NETWORK_ERROR;
        unsigned calls = query_calls;
        assert(quota_portable_service_next_deadline_ms(false) > now_ms);
        source_tick();
        assert(query_calls == calls);
    } else if (!strcmp(argv[1], "dirty-expired-deadline")) {
        boot();
        phone();
        model_unknown = true;
        quota_portable_command_t cmd = {.op = QUOTA_PORTABLE_OP_DEEPSEEK_SAVE};
        strcpy(cmd.request_id, "12345678");
        strcpy(cmd.api_key, "candidate-key");
        submit_command(&cmd);
        now_ms += QUOTA_PORTABLE_SETUP_MS + 60001;
        tick(true);
        assert(s_dirty_model && acquired && quota_portable_service_next_deadline_ms(true) > now_ms);
        assert(!query_calls && !refresh_calls);
    } else if (!strcmp(argv[1], "v2-reauth-cache")) {
        seed_model();
        credentials[0].refresh_inflight = true;
        boot();
        assert(public_view.snapshot.accounts[0].five_hour.remaining_percent == 17 &&
               public_view.snapshot.accounts[0].status == QUOTA_STATUS_EXPIRED &&
               !strcmp(public_view.portable.account_errors[0], "auth_required"));
        assert(!query_calls && !refresh_calls);
    } else if (!strcmp(argv[1], "unknown-valid-prior")) {
        boot();
        phone();
        model_unknown = true;
        quota_portable_command_t cmd = {.op = QUOTA_PORTABLE_OP_SETTINGS_SAVE,
                                        .refresh_seconds = 900,
                                        .screen_timeout_seconds = 120};
        strcpy(cmd.request_id, "12345678");
        submit_command(&cmd);
        quota_model_t *held = s_dirty_model;
        now_ms += 1000;
        tick(true);
        assert(s_dirty_model == held && !s_recovery_blocked && public_view.refresh_seconds == 300);
        model_unknown = false;
        now_ms += 5000;
        tick(true);
        assert(!s_dirty_model && public_view.refresh_seconds == 900);
    }

    else if (!strcmp(argv[1], "usb-key-network")) {
        boot();
        ready();
        usb_open();
        assert(!portal_active && quota_portable_service_http_allowed());
        quota_portable_command_t key = {.op = QUOTA_PORTABLE_OP_DEEPSEEK_SAVE,
                                        .phone_utc = 1800000000};
        strcpy(key.request_id, "12345678");
        strcpy(key.account_id, credentials[1].id);
        strcpy(key.api_key, "usb-new-key");
        strcpy(key.label, "USB account");
        assert(quota_portable_service_submit(&key, QUOTA_SETUP_USB) ==
               QUOTA_PORTABLE_SUBMIT_ACCEPTED);
        assert(quota_portable_service_submit(&key, QUOTA_SETUP_USB) ==
                   QUOTA_PORTABLE_SUBMIT_ACCEPTED &&
               s_count == 1);
        strcpy(key.api_key, "conflicting-key");
        assert(quota_portable_service_submit(&key, QUOTA_SETUP_USB) ==
               QUOTA_PORTABLE_SUBMIT_CONFLICT);
        tick(false);
        /* Saved and pending: nothing was asked of DeepSeek, and the session stays open. */
        assert(!acquired && !strcmp(credentials[1].api_key, "usb-new-key") &&
               job(key.request_id)->state == 2 && usb_window && !s_queue);
        assert(!strcmp(account_state(1, NULL), "pending") && query_calls == 0);
        quota_portable_command_t net = {.op = QUOTA_PORTABLE_OP_NETWORK_SAVE, .network_index = 0};
        strcpy(net.request_id, "22345678");
        strcpy(net.ssid, "USB network");
        strcpy(net.password, "password");
        assert(quota_portable_service_submit(&net, QUOTA_SETUP_USB) ==
               QUOTA_PORTABLE_SUBMIT_ACCEPTED);
        tick(false);
        assert(!s_candidate_pending && !strcmp(durable_model.networks[0].ssid, "old-hotspot") &&
               !strcmp(s_staged.network.ssid, "USB network") &&
               !strcmp(network_state(0, NULL), "pending") && usb_window);
        now_ms += 500;
        tick(false);
        assert(query_calls == 0 && s_validate_runs == 0);
        usb_scratch = true;
        assert(!quota_portable_service_http_allowed());
        unsigned calls = query_calls;
        tick(false);
        assert(query_calls == calls);
        usb_scratch = false;
        quota_portable_command_t close = {.op = QUOTA_PORTABLE_OP_SETUP_CLOSE};
        strcpy(close.request_id, "32345678");
        assert(quota_portable_service_submit(&close, QUOTA_SETUP_USB) ==
               QUOTA_PORTABLE_SUBMIT_ACCEPTED);
        tick(false);
        usb_window = false;
        now_ms++;
        usb_open();
        now_ms += 500;
        tick(false);
        assert(usb_window && usb_closes == 0); /* Old close cannot revoke a new window. */
        usb_window = false;
        assert(quota_portable_service_submit(&key, QUOTA_SETUP_USB) ==
               QUOTA_PORTABLE_SUBMIT_CLOSED);
    } else if (!strcmp(argv[1], "usb-login-reopen-save")) {
        boot();
        ready();
        usb_open();
        quota_portable_command_t queue = {.op = QUOTA_PORTABLE_OP_CODEX_QUEUE};
        strcpy(queue.request_id, "12345678");
        strcpy(queue.label, "Chat");
        submit_usb(&queue);
        assert(!portal_active && s_operation.kind == OP_NONE && s_login_queue.present && !acquired);
        quota_portable_command_t validate = {.op = QUOTA_PORTABLE_OP_VALIDATE};
        strcpy(validate.request_id, "22345678");
        submit_usb(&validate);
        run_until_login_started();
        quota_direct_credential_t *held = acquired;
        assert(held && usb_window && s_operation.deadline == s_login_deadline &&
               s_validation.active);
        assert(public_view.portable.auth_hold_awake && public_view.portable.validating);
        strcpy(s_view.login_user_code, "TEST-CODE");
        s_view.login_state = QUOTA_PORTABLE_LOGIN_WAITING;
        char json[QUOTA_PORTABLE_STATE_BYTES + 1];
        size_t length = 0;
        assert(quota_portable_service_state_json(json, sizeof(json), &length, QUOTA_SETUP_USB));
        assert(strstr(json, "TEST-CODE") && strstr(json, QUOTA_DIRECT_VERIFICATION_URL) &&
               !strstr(json, "old-refresh") && strstr(json, "\"validation_step\":\"chatgpt\""));
        assert(quota_portable_service_state_json(json, sizeof(json), &length, QUOTA_SETUP_AP));
        assert(!strstr(json, "TEST-CODE") && !strstr(json, "verification_url"));
        /* The window ends or is opened again: the authorization and its validation carry on. */
        usb_window = false;
        now_ms += 500;
        tick(false);
        assert(s_operation.started && s_validation.active && acquired == held);
        usb_open();
        assert(s_operation.started && acquired == held && s_validation.active);
        quota_portable_command_t close = {.op = QUOTA_PORTABLE_OP_SETUP_CLOSE};
        strcpy(close.request_id, "32345678");
        assert(quota_portable_service_submit(&close, QUOTA_SETUP_USB) ==
               QUOTA_PORTABLE_SUBMIT_ACCEPTED);
        tick(false);
        now_ms += 500;
        tick(false);
        assert(!usb_window && usb_closes >= 1 && s_operation.started && acquired == held);
        uint8_t login_slot = held->slot;
        usb_open();
        login_code = QUOTA_DIRECT_PERSIST_PENDING;
        store_fail = true;
        tick(false);
        assert(provider.pending == held && s_operation.kind == OP_SAVE);
        usb_window = false;
        assert(!quota_portable_service_prepare_usb() && provider.pending == held);
        store_fail = false;
        now_ms = s_operation.retry_at;
        tick(true);
        assert(!provider.pending && !acquired &&
               !strcmp(credentials[login_slot].access_token, "received-login-token"));
        usb_open();
        settle();
        assert(job(validate.request_id)->state == 2 && s_operation.kind == OP_NONE &&
               !s_login_queue.present && durable_model.entry_count == 3 &&
               !strcmp(durable_model.entries[2].label, "Chat"));
        assert(!strcmp(account_state(2, NULL), "ok") && account_count_in_state() == 3);
    } else if (!strcmp(argv[1], "open-while-active")) {
        /* A second open request is a no-op: it keeps the session, never spins, and never reopens a
         * new hotspot after expiry. */
        boot();
        phone();
        char ssid[40];
        strcpy(ssid, public_view.portable.setup_ssid);
        unsigned starts = portal_starts;
        quota_portable_service_open();
        quota_portable_service_renew();
        assert(!s_open);
        assert(quota_portable_service_next_deadline_ms(false) > now_ms);
        s_open = true; /* a request latched before the session opened must also be consumed, not
                          spun on */
        tick(false);
        assert(!s_open && portal_starts == starts &&
               !strcmp(public_view.portable.setup_ssid, ssid) &&
               quota_portable_service_next_deadline_ms(false) > now_ms);
        now_ms += QUOTA_PORTABLE_SETUP_MS;
        tick(false);
        assert(!public_view.portable.setup_active && !s_open);
        now_ms += 500;
        tick(false);
        assert(!public_view.portable.setup_active && portal_starts == starts &&
               quota_portable_service_next_deadline_ms(false) > now_ms);
    } else if (!strcmp(argv[1], "open-failure-backoff")) {
        /* A failing hotspot start waits between bounded attempts instead of looping with a zero
         * deadline. */
        boot();
        ready();
        portal_fail = true;
        quota_portable_service_open();
        tick_until_portal_starts(1);
        assert(portal_starts == 1 && s_open && !public_view.portable.setup_active);
        for (int i = 0; i < 1000; i++) {
            if (quota_portable_service_next_deadline_ms(false) > now_ms)
                break;
            tick(false);
        }
        assert(portal_starts == 1 && s_open_retry_at == now_ms + OPEN_RETRY_MS &&
               quota_portable_service_next_deadline_ms(false) > now_ms &&
               quota_portable_service_next_deadline_ms(false) <= s_open_retry_at);
        now_ms += OPEN_RETRY_MS - 1;
        tick(false);
        assert(portal_starts == 1);
        now_ms += 1;
        tick_until_portal_starts(2);
        assert(portal_starts == 2 && s_open);
        now_ms += OPEN_RETRY_MS;
        tick_until_portal_starts(3);
        assert(portal_starts == 3 && !s_open);
        assert(quota_portable_service_next_deadline_ms(false) > now_ms);
        now_ms += 60000;
        tick(false);
        assert(portal_starts == 3);
        portal_fail = false;
        quota_portable_service_renew();
        tick_until_portal_starts(4);
        assert(portal_starts == 4 && public_view.portable.setup_active && !s_open);
    } else if (!strcmp(argv[1], "open-prepare-wait-deadline")) {
        boot();
        ready();
        now_ms = 650;
        fake_wifi_deadline_ms = 30650;
        quota_portable_service_open();
        tick(false); /* STA must retire before the APSTA operation can be admitted. */
        assert(s_open && s_open_config_pending && s_open_failures == 0);
        assert(s_view.network_ip[0] == 0);
        uint64_t deadline = quota_portable_service_next_deadline_ms(false);
        assert(s_open_retry_at > now_ms && deadline > now_ms && deadline <= 30650);
        now_ms = s_open_retry_at;
        tick(false);
        assert(portal_active && public_view.portable.setup_ready && !s_open &&
               s_open_failures == 0);
    } else if (!strcmp(argv[1], "http-ready-disconnect-refresh")) {
        boot();
        ready();
        assert(quota_portable_service_http_allowed() && fake_driver_started && connected);
        unsigned queries = query_calls;
        unsigned refreshes = refresh_calls;
        wifi_disconnected(201, "old-hotspot");
        quota_portable_service_refresh();
        now_ms += 500;
        tick(false);
        assert(fake_retire_pending && !fake_driver_started && !connected);
        assert(s_view.network_ip[0] == 0 && !quota_portable_service_http_allowed() &&
               query_calls == queries && refresh_calls == refreshes);
    } else if (!strcmp(argv[1], "http-load-disconnect-gate")) {
        boot();
        ready();
        quota_portable_service_refresh();
        disconnect_during_credential_load = true;
        unsigned queries = query_calls;
        now_ms += 500;
        tick(false);
        assert(!disconnect_during_credential_load && !connected && s_wifi_disconnect_count == 1 &&
               query_calls == queries && !quota_portable_service_http_allowed());
    } else if (!strcmp(argv[1], "http-ready-reconnect-expiry")) {
        boot();
        ready();
        assert(quota_portable_service_http_allowed() && fake_driver_started && connected);
        s_model.auto_refresh = true;
        credentials[0].expires_at = wall + 100;
        s_next_refresh = now_ms + 500;
        unsigned queries = query_calls;
        unsigned refreshes = refresh_calls;
        quota_portable_service_reconnect();
        now_ms += 500;
        tick(false);
        assert(fake_retire_pending && !fake_driver_started && !connected);
        assert(s_view.network_ip[0] == 0 && !quota_portable_service_http_allowed() &&
               query_calls == queries && refresh_calls == refreshes);
    } else if (!strcmp(argv[1], "sntp-servers")) {
        boot();
        ready();
        assert(s_sntp && sntp_config.start && sntp_config.num_of_servers == 3 &&
               sntp_config.num_of_servers <= CONFIG_LWIP_SNTP_MAX_SERVERS);
        assert(!strcmp(sntp_config.servers[0], "time.cloudflare.com") &&
               !strcmp(sntp_config.servers[1], "ntp.aliyun.com") &&
               !strcmp(sntp_config.servers[2], "pool.ntp.org"));
    } else if (!strcmp(argv[1], "usb-pauses-auto-refresh")) {
        boot();
        ready();
        s_model.auto_refresh = true;
        s_next_refresh = now_ms;
        usb_open();
        unsigned calls = query_calls;
        tick(false);
        now_ms += 500;
        tick(false);
        assert(query_calls == calls && !s_cycle);
        quota_portable_service_refresh();
        tick(false);
        assert(query_calls > calls); /* an explicit refresh still runs */
        for (int i = 0; i < 4 && s_cycle; i++) {
            now_ms += 500;
            tick(false);
        }
        calls = query_calls;
        s_next_refresh = now_ms;
        tick(false);
        assert(query_calls == calls);
        usb_window = false;
        now_ms += 500;
        tick(false);
        assert(query_calls > calls); /* scheduled polling resumes once the window ends */
    } else if (!strcmp(argv[1], "fresh-device")) {
        /* No catalog (new device, or one that only had retired data): start empty. The hotspot is
         * not opened by itself: the welcome screen waits for OK. */
        fresh_device = true;
        boot();
        assert(durable_sequence == 1 && public_view.configured && public_view.snapshot_valid);
        assert(public_view.snapshot.account_count == 0 && durable_model.entry_count == 0 &&
               durable_model.network_count == 0 && !s_open);
        assert(orphan_releases == 1 && credentials[0].tombstone && credentials[1].tombstone &&
               !credentials[1].api_key[0]); /* Stale credentials are freed, never adopted. */
        tick(false);
        assert(!portal_active && !public_view.portable.setup_active &&
               !public_view.portable.setup_opening);
        quota_portable_service_open(); /* what OK on the welcome screen does */
        assert(public_view.portable.setup_opening || s_open);
        tick(false);
        assert(portal_active && public_view.portable.setup_ready && !s_open);
        assert(!public_view.portable.setup_opening);
    } else if (!strcmp(argv[1], "fresh-orphan-retry")) {
        /* A failed release blocks the new catalog and is retried; nothing is committed meanwhile.
         */
        fresh_device = true;
        orphan_error = QUOTA_STORE_READ_IO_ERROR;
        boot();
        assert(!durable_sequence && !public_view.snapshot_valid &&
               !strcmp(s_storage_error, "storage_io_error") && !credentials[0].tombstone);
        orphan_error = QUOTA_STORE_READ_OK;
        now_ms += 5000;
        tick(false);
        assert(durable_sequence == 1 && public_view.snapshot_valid && credentials[0].tombstone &&
               orphan_releases == 2);
    } else if (!strcmp(argv[1], "account-limit")) {
        /* A full catalog refuses a ninth account instead of keeping a hidden row. */
        all_native();
        boot();
        assert(durable_model.entry_count == 8 && public_view.snapshot.account_count == 8);
        phone();
        quota_portable_command_t codex = {.op = QUOTA_PORTABLE_OP_CODEX_QUEUE};
        strcpy(codex.request_id, "12345678");
        submit_command(&codex);
        assert(job(codex.request_id)->state == 3 &&
               !strcmp(job(codex.request_id)->error, "account_limit") && !acquired &&
               durable_model.entry_count == 8);
        quota_portable_command_t key = {.op = QUOTA_PORTABLE_OP_DEEPSEEK_SAVE};
        strcpy(key.request_id, "22345678");
        strcpy(key.api_key, "new-key");
        submit_command(&key);
        assert(job(key.request_id)->state == 3 &&
               !strcmp(job(key.request_id)->error, "account_limit") && !acquired &&
               durable_model.entry_count == 8);
    } else if (!strcmp(argv[1], "network-limit")) {
        seed_model();
        durable_model.network_count = 3;
        for (unsigned i = 1; i < 3; i++) {
            snprintf(durable_model.networks[i].ssid, sizeof(durable_model.networks[i].ssid),
                     "hotspot-%u", i);
            strcpy(durable_model.networks[i].password, "password");
        }
        boot();
        phone();
        quota_portable_command_t net = {.op = QUOTA_PORTABLE_OP_NETWORK_SAVE,
                                        .network_index = UINT8_MAX};
        strcpy(net.request_id, "12345678");
        strcpy(net.ssid, "fourth-hotspot");
        strcpy(net.password, "password");
        submit_command(&net);
        assert(job(net.request_id)->state == 3 &&
               !strcmp(job(net.request_id)->error, "network_limit") && !s_candidate_pending &&
               durable_model.network_count == 3);
    } else if (!strcmp(argv[1], "usb-validate-e2e")) {
        /* Acceptance: a wrong Wi-Fi password is saved, validated (failed), corrected and
         * validated again over USB without a single key pressed on the Passport. */
        boot();
        ready();
        usb_open();
        quota_portable_command_t net = {.op = QUOTA_PORTABLE_OP_NETWORK_SAVE, .network_index = 0};
        strcpy(net.request_id, "12345678");
        strcpy(net.ssid, "Home");
        strcpy(net.password, "wrong-password");
        submit_usb(&net);
        unsigned connects = wifi_connects;
        assert(job(net.request_id)->state == 2 && !strcmp(network_state(0, NULL), "pending"));
        assert(s_validate_runs == 0 && !s_candidate_pending && wifi_connects == connects);
        network_ok = false;
        quota_portable_command_t validate = {.op = QUOTA_PORTABLE_OP_VALIDATE};
        strcpy(validate.request_id, "22345678");
        submit_usb(&validate);
        assert(s_validate_runs == 1 && s_validation.active && s_candidate_pending);
        cJSON *root = state_of(QUOTA_SETUP_USB);
        assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "validating")));
        cJSON_Delete(root);
        now_ms += 500;
        tick(false);
        wifi_disconnected(WIFI_REASON_AUTH_FAIL, (const char *)wifi_config.sta.ssid);
        settle();
        const char *error = NULL;
        assert(!strcmp(network_state(0, &error), "failed") && !strcmp(error, "wifi_auth_failed"));
        assert(job(validate.request_id)->state == 2 && usb_window && usb_closes == 0);
        /* The failed row is editable and only pending or failed rows are validated again. */
        strcpy(net.request_id, "32345678");
        strcpy(net.password, "right-password");
        submit_usb(&net);
        assert(!strcmp(network_state(0, NULL), "pending") &&
               !strcmp(durable_model.networks[0].password, "password")); /* still the old ones */
        network_ok = true;
        strcpy(validate.request_id, "42345678");
        submit_usb(&validate);
        assert(s_validate_runs == 2);
        settle();
        assert(!strcmp(network_state(0, &error), "ok") && !error[0] &&
               s_model.selected_network == 0);
        /* Only now are the new credentials stored. */
        assert(!strcmp(durable_model.networks[0].password, "right-password") && !s_staged.present);
        assert(job(validate.request_id)->state == 2 && usb_window && usb_closes == 0);
        connects = wifi_connects;
        unsigned queries = query_calls;
        strcpy(validate.request_id, "52345678");
        submit_usb(&validate); /* nothing is pending: it asks nothing */
        settle();
        assert(s_validate_runs == 3 && wifi_connects == connects && query_calls == queries &&
               job(validate.request_id)->state == 2);
    } else if (!strcmp(argv[1], "validate-only-pending")) {
        boot();
        ready();
        usb_open();
        quota_portable_command_t key = {.op = QUOTA_PORTABLE_OP_DEEPSEEK_SAVE};
        strcpy(key.request_id, "12345678");
        strcpy(key.api_key, "first-key");
        strcpy(key.label, "First");
        submit_usb(&key);
        strcpy(key.request_id, "22345678");
        strcpy(key.api_key, "second-key");
        strcpy(key.label, "Second");
        submit_usb(&key);
        assert(durable_model.entry_count == 4 && !strcmp(account_state(2, NULL), "pending") &&
               !strcmp(account_state(3, NULL), "pending"));
        /* Every key is refused. */
        query_code = QUOTA_DIRECT_AUTH_REQUIRED;
        quota_portable_command_t validate = {.op = QUOTA_PORTABLE_OP_VALIDATE};
        strcpy(validate.request_id, "32345678");
        unsigned before = deepseek_queries;
        submit_usb(&validate);
        settle();
        const char *error = NULL;
        assert(deepseek_queries == before + 2);
        assert(!strcmp(account_state(0, NULL), "ok") && !strcmp(account_state(1, NULL), "ok"));
        assert(!strcmp(account_state(2, &error), "failed") &&
               !strcmp(error, "deepseek_invalid_key"));
        assert(!strcmp(account_state(3, NULL), "failed"));
        /* Replacing one key resets only that row; both failed rows are validated again. */
        query_code = QUOTA_DIRECT_OK;
        strcpy(key.request_id, "42345678");
        strcpy(key.account_id, durable_model.entries[2].logical_id);
        strcpy(key.api_key, "fixed-key");
        strcpy(key.label, "First");
        submit_usb(&key);
        assert(!strcmp(account_state(2, NULL), "pending") &&
               !strcmp(account_state(3, NULL), "failed") && credentials[2].generation == 2);
        before = deepseek_queries;
        strcpy(validate.request_id, "52345678");
        submit_usb(&validate);
        settle();
        assert(deepseek_queries == before + 2);
        assert(!strcmp(account_state(2, NULL), "ok") && !strcmp(account_state(3, NULL), "ok"));
        /* Everything is ok: another validate asks nothing. */
        before = deepseek_queries;
        unsigned connects = wifi_connects;
        strcpy(validate.request_id, "62345678");
        submit_usb(&validate);
        settle();
        assert(deepseek_queries == before && wifi_connects == connects);
    } else if (!strcmp(argv[1], "validate-busy-and-expiry")) {
        boot();
        ready();
        usb_open();
        quota_portable_command_t net = {.op = QUOTA_PORTABLE_OP_NETWORK_SAVE, .network_index = 0};
        strcpy(net.request_id, "12345678");
        strcpy(net.ssid, "Home");
        strcpy(net.password, "right-password");
        submit_usb(&net);
        quota_portable_command_t validate = {.op = QUOTA_PORTABLE_OP_VALIDATE, .phone_utc = wall};
        strcpy(validate.request_id, "22345678");
        assert(quota_portable_service_submit(&validate, QUOTA_SETUP_USB) ==
               QUOTA_PORTABLE_SUBMIT_ACCEPTED);
        /* From the moment validate is accepted, every change is refused. */
        quota_portable_command_t late[] = {
            {.op = QUOTA_PORTABLE_OP_NETWORK_SAVE, .network_index = 0},
            {.op = QUOTA_PORTABLE_OP_NETWORK_REMOVE},
            {.op = QUOTA_PORTABLE_OP_DEEPSEEK_SAVE},
            {.op = QUOTA_PORTABLE_OP_CODEX_QUEUE},
            {.op = QUOTA_PORTABLE_OP_ACCOUNT_REMOVE},
            {.op = QUOTA_PORTABLE_OP_SETTINGS_SAVE, .refresh_seconds = 300},
            {.op = QUOTA_PORTABLE_OP_VALIDATE},
            {.op = QUOTA_PORTABLE_OP_REFRESH},
            {.op = QUOTA_PORTABLE_OP_RECONNECT},
        };
        for (unsigned i = 0; i < sizeof(late) / sizeof(late[0]); i++) {
            snprintf(late[i].request_id, sizeof(late[i].request_id), "a%07u", i);
            strcpy(late[i].ssid, "Other");
            strcpy(late[i].password, "other-password");
            assert(quota_portable_service_submit(&late[i], QUOTA_SETUP_USB) ==
                   QUOTA_PORTABLE_SUBMIT_BUSY);
        }
        assert(quota_portable_service_submit(&validate, QUOTA_SETUP_USB) ==
               QUOTA_PORTABLE_SUBMIT_ACCEPTED); /* the same request again is a retry */
        tick(false);
        assert(s_validation.active && s_candidate_pending);
        /* State reads work meanwhile and say what is going on. */
        cJSON *root = state_of(QUOTA_SETUP_USB);
        assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "validating")));
        cJSON *step = cJSON_GetObjectItemCaseSensitive(root, "validation_step");
        assert(cJSON_IsString(step) && !strcmp(step->valuestring, "wifi"));
        cJSON_Delete(root);
        assert(quota_portable_service_submit(&late[0], QUOTA_SETUP_USB) ==
               QUOTA_PORTABLE_SUBMIT_BUSY);
        /* The window runs out in the middle of the pass: validation still finishes and records. */
        usb_window = false;
        assert(quota_portable_service_submit(&late[0], QUOTA_SETUP_USB) ==
               QUOTA_PORTABLE_SUBMIT_CLOSED);
        settle();
        assert(!strcmp(network_state(0, NULL), "ok") && job(validate.request_id)->state == 2);
        assert(!s_candidate_pending && !s_validation.requested);
        /* A validate that was accepted before a change landed is told the settings moved on. */
        usb_open();
        quota_portable_command_t settings = {.op = QUOTA_PORTABLE_OP_SETTINGS_SAVE,
                                             .phone_utc = wall,
                                             .refresh_seconds = 900,
                                             .screen_timeout_seconds = 120,
                                             .auto_refresh = true};
        strcpy(settings.request_id, "b2345678");
        assert(quota_portable_service_submit(&settings, QUOTA_SETUP_USB) ==
               QUOTA_PORTABLE_SUBMIT_ACCEPTED);
        strcpy(validate.request_id, "c2345678");
        assert(quota_portable_service_submit(&validate, QUOTA_SETUP_USB) ==
               QUOTA_PORTABLE_SUBMIT_ACCEPTED);
        tick(false);
        assert(job(settings.request_id)->state == 2 && s_model.refresh_seconds == 900);
        tick(false);
        assert(job(validate.request_id)->state == 3 &&
               !strcmp(job(validate.request_id)->error, "configuration_changed") &&
               !s_validation.requested && !s_validation.active && s_validate_runs == 1);
        strcpy(validate.request_id, "d2345678");
        submit_usb(&validate);
        settle();
        assert(!strcmp(network_state(0, NULL), "ok") && s_validate_runs == 2 &&
               job(validate.request_id)->state == 2);
    } else if (!strcmp(argv[1], "validate-hotspot")) {
        /* The hotspot saves and then validates after the access point has closed. The USB
         * validate command is not accepted over the hotspot. */
        boot();
        phone();
        quota_portable_command_t validate = {.op = QUOTA_PORTABLE_OP_VALIDATE};
        strcpy(validate.request_id, "01234567");
        assert(portal.submit(&validate, NULL) == QUOTA_PORTABLE_SUBMIT_INVALID);
        quota_portable_command_t net = {.op = QUOTA_PORTABLE_OP_NETWORK_SAVE,
                                        .network_index = UINT8_MAX};
        strcpy(net.request_id, "12345678");
        strcpy(net.ssid, "Office");
        strcpy(net.password, "office-password");
        submit_command(&net);
        quota_portable_command_t key = {.op = QUOTA_PORTABLE_OP_DEEPSEEK_SAVE};
        strcpy(key.request_id, "22345678");
        strcpy(key.api_key, "office-key");
        strcpy(key.label, "Office key");
        submit_command(&key);
        assert(durable_model.network_count == 2 && s_model.selected_network == 0 &&
               !strcmp(network_state(1, NULL), "pending") &&
               !strcmp(account_state(2, NULL), "pending"));
        now_ms += 500;
        tick(false);
        assert(s_validate_runs == 0 && !s_candidate_pending && portal_active);
        quota_portable_command_t close = {.op = QUOTA_PORTABLE_OP_SETUP_CLOSE};
        strcpy(close.request_id, "32345678");
        submit_command(&close);
        assert(portal_active && s_validate_runs == 0 && job(close.request_id)->state == 2);
        now_ms += 500;
        tick(false);
        assert(!portal_active && s_validate_runs == 1 && s_validation.active);
        /* This session's 完成设置 puts the result on the device screen; the account rows follow. */
        assert(public_view.portable.validating && public_view.portable.setup_result);
        settle();
        assert(!public_view.portable.validating && public_view.portable.setup_result);
        assert(public_view.portable.account_validation[0] == QUOTA_VALIDATION_OK);
        assert(!strcmp(network_state(1, NULL), "ok") && s_model.selected_network == 1);
        assert(!strcmp(account_state(2, NULL), "ok") &&
               credentials[2].auth_state == QUOTA_PORTABLE_AUTH_READY);
        /* Opening the hotspot again drops the old result: it is not this session's. */
        quota_portable_service_open();
        /* No validation when the access point closes for another reason. */
        phone();
        assert(!public_view.portable.setup_result && !public_view.portable.setup_opening);
        quota_portable_command_t more = {.op = QUOTA_PORTABLE_OP_DEEPSEEK_SAVE};
        strcpy(more.request_id, "42345678");
        strcpy(more.api_key, "later-key");
        strcpy(more.label, "Later");
        submit_command(&more);
        quota_portable_service_close();
        tick(false);
        now_ms += 500;
        tick(false);
        assert(!portal_active && s_validate_runs == 1 &&
               !strcmp(account_state(3, NULL), "pending"));
    } else if (!strcmp(argv[1], "keys-wait-for-network")) {
        boot();
        ready();
        usb_open();
        connected = false;
        network_ok = false;
        quota_portable_command_t key = {.op = QUOTA_PORTABLE_OP_DEEPSEEK_SAVE};
        strcpy(key.request_id, "12345678");
        strcpy(key.api_key, "some-key");
        strcpy(key.label, "Key");
        submit_usb(&key);
        quota_portable_command_t validate = {.op = QUOTA_PORTABLE_OP_VALIDATE};
        strcpy(validate.request_id, "22345678");
        submit_usb(&validate);
        for (unsigned i = 0; i < 20 && s_validation.active; i++) {
            now_ms += 500;
            tick(false);
        }
        assert(s_validation.active && query_calls == 0); /* still waiting for the network */
        settle();
        const char *error = NULL;
        assert(!strcmp(account_state(2, &error), "failed") &&
               !strcmp(error, "network_unavailable") && query_calls == 0 &&
               job(validate.request_id)->state == 2);
    } else if (!strcmp(argv[1], "ap-top-up")) {
        boot();
        phone();
        uint64_t opened = s_setup_opened;
        assert(s_setup_deadline == opened + QUOTA_PORTABLE_SETUP_MS);
        char json[QUOTA_PORTABLE_STATE_BYTES + 1];
        size_t length = 0;
        now_ms += 60000;
        assert(state_json(json, sizeof(json), &length, NULL));
        quota_portable_command_t refresh = {.op = QUOTA_PORTABLE_OP_REFRESH};
        strcpy(refresh.request_id, "01234567");
        submit_command(&refresh);
        assert(s_setup_deadline == opened + QUOTA_PORTABLE_SETUP_MS); /* neither extends it */
        quota_portable_command_t settings = {.op = QUOTA_PORTABLE_OP_SETTINGS_SAVE,
                                             .refresh_seconds = 300,
                                             .screen_timeout_seconds = 120,
                                             .auto_refresh = true};
        /* Near the end of the ten minutes a change gives five more. */
        now_ms = opened + QUOTA_PORTABLE_SETUP_MS - 60000;
        strcpy(settings.request_id, "11111111");
        submit_command(&settings);
        assert(s_setup_deadline == now_ms + QUOTA_SESSION_TOPUP_MS);
        uint64_t topped = s_setup_deadline;
        strcpy(settings.request_id, "22222222"); /* five minutes left already: unchanged */
        submit_command(&settings);
        assert(s_setup_deadline == topped);
        /* Steady use ends at twenty minutes after the hotspot opened. */
        now_ms = opened + 13 * 60000;
        strcpy(settings.request_id, "33333333");
        submit_command(&settings);
        assert(s_setup_deadline == now_ms + QUOTA_SESSION_TOPUP_MS);
        now_ms = opened + 16 * 60000;
        strcpy(settings.request_id, "66666666");
        submit_command(&settings);
        assert(s_setup_deadline == opened + QUOTA_SESSION_MAX_MS);
        now_ms = opened + QUOTA_SESSION_MAX_MS - 1;
        strcpy(settings.request_id, "44444444");
        submit_command(&settings);
        assert(s_setup_deadline == opened + QUOTA_SESSION_MAX_MS);
        now_ms = opened + QUOTA_SESSION_MAX_MS;
        strcpy(settings.request_id, "55555555");
        assert(portal.submit(&settings, NULL) == QUOTA_PORTABLE_SUBMIT_CLOSED);
        tick(false);
        assert(!public_view.portable.setup_active);
    } else if (!strcmp(argv[1], "ap-network-mutation")) {
        boot();
        phone();
        assert(s_view.setup_active && s_view.setup_ready && fake_driver_started &&
               fake_driver_mode == WIFI_MODE_APSTA &&
               quota_wifi_operation_active(fake_driver_request_id, WIFI_MODE_APSTA));
        quota_portable_command_t add = {.op = QUOTA_PORTABLE_OP_NETWORK_SAVE,
                                        .network_index = UINT8_MAX};
        strcpy(add.request_id, "12345678");
        strcpy(add.ssid, "ap-saved");
        strcpy(add.password, "saved-password");
        submit_command(&add);
        assert(s_model.network_count == 2 && s_view.setup_active && s_view.setup_ready &&
               fake_driver_started && fake_driver_mode == WIFI_MODE_APSTA && !fake_retire_pending);

        model_unknown = true;
        model_apply_unknown = true;
        quota_portable_command_t remove = {.op = QUOTA_PORTABLE_OP_NETWORK_REMOVE,
                                           .network_index = 1};
        strcpy(remove.request_id, "22345678");
        submit_command(&remove);
        assert(s_dirty_model && s_model.network_count == 2 && s_view.setup_active &&
               s_view.setup_ready && fake_driver_started && fake_driver_mode == WIFI_MODE_APSTA &&
               !fake_retire_pending);
        char json[QUOTA_PORTABLE_STATE_BYTES + 1];
        size_t length = 0;
        assert(state_json(json, sizeof(json), &length, NULL) && length > 0);

        model_unknown = false;
        now_ms += 1000;
        tick(false);
        assert(!s_dirty_model && s_model.network_count == 1 && s_view.setup_active &&
               s_view.setup_ready && fake_driver_started && fake_driver_mode == WIFI_MODE_APSTA &&
               !fake_retire_pending);
        quota_portable_command_t settings = {.op = QUOTA_PORTABLE_OP_SETTINGS_SAVE,
                                             .refresh_seconds = 900,
                                             .screen_timeout_seconds = 120};
        strcpy(settings.request_id, "32345678");
        submit_command(&settings);
        assert(s_model.refresh_seconds == 900 && s_view.setup_active && s_view.setup_ready &&
               fake_driver_started && fake_driver_mode == WIFI_MODE_APSTA && !fake_retire_pending);
    } else if (!strcmp(argv[1], "login-queue")) {
        boot();
        phone();
        quota_portable_command_t queue = {.op = QUOTA_PORTABLE_OP_CODEX_QUEUE};
        strcpy(queue.request_id, "12345678");
        strcpy(queue.account_id, credentials[0].id);
        submit_command(&queue); /* re-authorize the ChatGPT row that exists */
        assert(s_login_queue.present && !s_login_queue.is_new && account_count_in_state() == 2 &&
               !strcmp(account_state(0, NULL), "pending"));
        quota_portable_command_t wrong = {.op = QUOTA_PORTABLE_OP_CODEX_QUEUE};
        strcpy(wrong.request_id, "22345678");
        strcpy(wrong.account_id, credentials[1].id); /* a DeepSeek row cannot be authorized */
        submit_command(&wrong);
        assert(job(wrong.request_id)->state == 3);
        quota_portable_command_t drop = {.op = QUOTA_PORTABLE_OP_ACCOUNT_REMOVE};
        strcpy(drop.request_id, "32345678");
        strcpy(drop.account_id, credentials[0].id);
        submit_command(&drop);
        assert(!s_login_queue.present && public_view.snapshot.account_count == 1);
        /* A new ChatGPT account takes a place while it waits, and removing it frees the place. */
        quota_portable_command_t fresh = {.op = QUOTA_PORTABLE_OP_CODEX_QUEUE};
        strcpy(fresh.request_id, "42345678");
        submit_command(&fresh);
        assert(s_login_queue.is_new && account_count_in_state() == 2);
        strcpy(drop.request_id, "52345678");
        strcpy(drop.account_id, s_login_queue.id);
        submit_command(&drop);
        assert(!s_login_queue.present && account_count_in_state() == 1 &&
               durable_model.entry_count == 1);
    } else if (!strcmp(argv[1], "queue-counts-toward-limit")) {
        all_native();
        used[7] = false; /* seven accounts */
        boot();
        assert(durable_model.entry_count == 7);
        phone();
        quota_portable_command_t queue = {.op = QUOTA_PORTABLE_OP_CODEX_QUEUE};
        strcpy(queue.request_id, "12345678");
        submit_command(&queue);
        assert(s_login_queue.present && job(queue.request_id)->state == 2);
        quota_portable_command_t key = {.op = QUOTA_PORTABLE_OP_DEEPSEEK_SAVE};
        strcpy(key.request_id, "22345678");
        strcpy(key.api_key, "eighth-key");
        strcpy(key.label, "Eighth");
        submit_command(&key); /* the waiting ChatGPT account is the eighth */
        assert(job(key.request_id)->state == 3 &&
               !strcmp(job(key.request_id)->error, "account_limit") && !acquired &&
               durable_model.entry_count == 7);
        strcpy(key.request_id, "32345678");
        strcpy(key.account_id, credentials[1].id); /* changing a key is not a new account */
        submit_command(&key);
        assert(job(key.request_id)->state == 2);
    } else if (!strcmp(argv[1], "network-remove")) {
        seed_model();
        durable_model.network_count = 3;
        for (unsigned i = 1; i < 3; i++) {
            snprintf(durable_model.networks[i].ssid, sizeof(durable_model.networks[i].ssid),
                     "hotspot-%u", i);
            strcpy(durable_model.networks[i].password, "password");
        }
        durable_model.selected_network = 2;
        boot();
        phone();
        s_network_validation[2] = QUOTA_VALIDATION_FAILED;
        strcpy(s_network_error[2], "wifi_auth_failed");
        quota_portable_command_t remove_first = {.op = QUOTA_PORTABLE_OP_NETWORK_REMOVE,
                                                 .network_index = 0};
        strcpy(remove_first.request_id, "12345678");
        submit_command(&remove_first);
        const char *error = NULL;
        assert(durable_model.network_count == 2 && durable_model.selected_network == 1 &&
               !strcmp(durable_model.networks[0].ssid, "hotspot-1") &&
               !strcmp(durable_model.networks[1].ssid, "hotspot-2"));
        assert(!strcmp(network_state(1, &error), "failed") && !strcmp(error, "wifi_auth_failed") &&
               !strcmp(network_state(0, NULL), "saved"));
        quota_portable_command_t remove_used = {.op = QUOTA_PORTABLE_OP_NETWORK_REMOVE,
                                                .network_index = 1};
        strcpy(remove_used.request_id, "22345678");
        submit_command(&remove_used);
        assert(durable_model.network_count == 1 && durable_model.selected_network == 0 &&
               !strcmp(durable_model.networks[0].ssid, "hotspot-1"));
        quota_portable_command_t missing = {.op = QUOTA_PORTABLE_OP_NETWORK_REMOVE,
                                            .network_index = 2};
        strcpy(missing.request_id, "32345678");
        submit_command(&missing);
        assert(job(missing.request_id)->state == 3 &&
               !strcmp(job(missing.request_id)->error, "invalid_request"));
        strcpy(remove_first.request_id, "42345678");
        submit_command(&remove_first);
        assert(durable_model.network_count == 0 && durable_model.selected_network == 0 &&
               public_view.portable.saved_network_count == 0);
    } else if (!strcmp(argv[1], "access-code")) {
        boot();
        phone();
        char first[QUOTA_PORTABLE_ACCESS_CODE_BYTES + 1], url[QUOTA_PORTABLE_URL_BYTES + 1];
        strcpy(first, public_view.portable.setup_secret);
        assert(strlen(first) == 19 && valid_access_code(first));
        snprintf(url, sizeof(url), "http://192.168.4.1/#code=%s", first);
        assert(!strcmp(public_view.portable.setup_page_url, url));
        quota_portable_service_close();
        tick(false);
        assert(!public_view.portable.setup_secret[0] && !public_view.portable.setup_page_url[0]);
        phone();
        assert(valid_access_code(public_view.portable.setup_secret) &&
               strcmp(first, public_view.portable.setup_secret) != 0);
        char json[QUOTA_PORTABLE_STATE_BYTES + 1];
        size_t length = 0;
        assert(state_json(json, sizeof(json), &length, NULL));
        assert(!strstr(json, public_view.portable.setup_secret) &&
               !strstr(json, public_view.portable.setup_password));
    } else if (!strcmp(argv[1], "network-saved-state")) {
        boot();
        /* After a restart a saved network is neither waiting nor failed. */
        assert(!strcmp(network_state(0, NULL), "saved") &&
               public_view.portable.pending_items == 0 && public_view.portable.failed_items == 0);
        assert(!quota_portable_validate_pending() && !s_validation.active);
        /* Connecting to it for real makes it ok. */
        ready();
        assert(fake_scan_starts == 0); /* A successful preferred link does not scan. */
        assert(!strcmp(network_state(0, NULL), "ok") &&
               public_view.portable.saved_network_validation[0] == QUOTA_VALIDATION_OK);
        /* The Passport is refused by the network in use: failed, and fine again once it connects.
         */
        connected = false;
        network_ok = false;
        quota_portable_service_reconnect();
        tick(false);
        now_ms += 500;
        tick(false);
        wifi_disconnected(WIFI_REASON_AUTH_FAIL, (const char *)wifi_config.sta.ssid);
        tick(false);
        const char *error = NULL;
        assert(!strcmp(network_state(0, &error), "failed") && !strcmp(error, "wifi_auth_failed"));
        assert(public_view.portable.failed_items == 1 &&
               !strcmp(public_view.portable.saved_network_errors[0], "wifi_auth_failed"));
        network_ok = true;
        quota_portable_service_reconnect();
        for (unsigned i = 0; i < 6; i++) {
            now_ms += 500;
            tick(false);
        }
        assert(!strcmp(network_state(0, &error), "ok") && !error[0] &&
               public_view.portable.failed_items == 0);
    } else if (!strcmp(argv[1], "scan-rssi-order")) {
        seed_model();
        durable_model.network_count = 3;
        durable_model.selected_network = 0;
        strcpy(durable_model.networks[0].ssid, "saved-a");
        strcpy(durable_model.networks[1].ssid, "01234567890123456789012345678901");
        strcpy(durable_model.networks[2].ssid, "saved-c");
        strcpy(durable_model.networks[1].password, "password-b");
        strcpy(durable_model.networks[2].password, "password-c");
        fake_wifi_set_network("saved-a", false, true);
        fake_wifi_set_network("01234567890123456789012345678901", false, true);
        fake_wifi_set_network("saved-c", true, true);
        fake_scan_set_record("", -1);
        fake_scan_set_record("unrelated-strong", -5);
        fake_scan_set_record("01234567890123456789012345678901", -70);
        for (unsigned i = 0; i < 300; i++) {
            char noise[32];
            snprintf(noise, sizeof(noise), "unrelated-%03u", i);
            fake_scan_set_record(noise, -60);
        }
        /* The strongest duplicate appears after more than 255 unrelated results. A full
         * 32-byte SSID still matches exactly, and its tie with saved-c keeps row order. */
        fake_scan_set_record("01234567890123456789012345678901", -25);
        fake_scan_set_record("saved-c", -25);
        boot();
        tick(false);
        assert(wifi_connects == 1 && !strcmp((const char *)wifi_config.sta.ssid, "saved-a"));
        now_ms += 25000;
        tick(false);
        /* The strongest observation is accumulated across the full list. The late 32-byte
         * saved-b record ties saved-c, so stable row ordering tries saved-b first. */
        tick_until_wifi_connects(2);
        assert(fake_scan_record_count == 305 && fake_scan_total_records_read == 305 &&
               wifi_connects == 2 &&
               !memcmp(wifi_config.sta.ssid, "01234567890123456789012345678901",
                       sizeof(wifi_config.sta.ssid)));
    } else if (!strcmp(argv[1], "scan-timeout-cleanup")) {
        seed_model();
        durable_model.network_count = 2;
        strcpy(durable_model.networks[1].ssid, "saved-b");
        strcpy(durable_model.networks[1].password, "password-b");
        fake_wifi_set_network("old-hotspot", false, true);
        fake_wifi_set_network("saved-b", true, true);
        fake_scan_auto_done = false;
        boot();
        begin_preferred_fallback_scan();
        assert(fake_scan_active && s_network_round_stage == NETWORK_ROUND_SCAN_WAIT);
        now_ms += 5000;
        tick(false);
        assert(s_network_round_stage == NETWORK_ROUND_TERMINAL && fake_retire_pending &&
               !strcmp(s_view.network_error, "wifi_no_available"));
        quota_portable_service_scan_done(true); /* A stale service latch cannot revive the round. */
        tick(false);
        assert(!fake_retire_pending && !fake_scan_active && fake_scan_starts == 1 &&
               fake_scan_clears >= 2 && s_network_round_stage == NETWORK_ROUND_TERMINAL);
    } else if (!strcmp(argv[1], "scan-read-failure-cleanup")) {
        seed_model();
        durable_model.network_count = 2;
        strcpy(durable_model.networks[1].ssid, "saved-b");
        strcpy(durable_model.networks[1].password, "password-b");
        fake_wifi_set_network("old-hotspot", false, true);
        fake_wifi_set_network("saved-b", true, true);
        fake_scan_set_record("saved-b", -40);
        fake_scan_read_fail_after = 0;
        boot();
        begin_preferred_fallback_scan();
        tick(false); /* Accept SCAN_DONE, fail its first record read, and retire the list. */
        assert(s_network_round_stage == NETWORK_ROUND_TERMINAL && fake_retire_pending &&
               !strcmp(s_view.network_error, "wifi_no_available"));
        tick(false);
        assert(!fake_retire_pending && fake_scan_starts == 1 && fake_scan_clears >= 2 &&
               s_scan_candidate_count == 0 && s_network_round_stage == NETWORK_ROUND_TERMINAL);
    } else if (!strcmp(argv[1], "scan-sleep-resume")) {
        seed_model();
        durable_model.network_count = 2;
        durable_model.screen_timeout_seconds = 30;
        strcpy(durable_model.networks[1].ssid, "saved-b");
        strcpy(durable_model.networks[1].password, "password-b");
        fake_wifi_set_network("old-hotspot", false, true);
        fake_wifi_set_network("saved-b", true, true);
        fake_scan_auto_done = false;
        boot();
        quota_display_state_t display = {.last_input_ms = now_ms};
        for (unsigned i = 0; i < 100 && !fake_scan_active; i++) {
            now_ms += 500;
            quota_display_tick(&display, now_ms, public_view.screen_timeout_seconds, false);
            tick(display.sleeping);
        }
        assert(fake_scan_active && fake_scan_starts == 1 && !display.sleeping);
        for (unsigned i = 0; i < 20 && !display.sleeping; i++) {
            now_ms += 500;
            quota_display_tick(&display, now_ms, public_view.screen_timeout_seconds, false);
            tick(display.sleeping);
        }
        assert(display.sleeping && fake_scan_starts == 1 && fake_retire_pending &&
               s_network_round_stage == NETWORK_ROUND_SCAN_PREPARE);
        now_ms += 500;
        quota_display_tick(&display, now_ms, public_view.screen_timeout_seconds, false);
        tick(display.sleeping); /* Retire and clear results without starting new work asleep. */
        assert(fake_scan_starts == 1 && !fake_scan_active &&
               s_network_round_stage == NETWORK_ROUND_SCAN_PREPARE);
        assert(!quota_display_handle_key(&display, now_ms, QUOTA_KEY_PRESS, false));
        quota_display_tick(&display, now_ms, public_view.screen_timeout_seconds, false);
        tick(display.sleeping);
        assert(!display.sleeping && fake_scan_starts == 2 && fake_scan_active &&
               s_network_round_stage == NETWORK_ROUND_SCAN_WAIT && !s_scan_visible_mask);
    } else if (!strcmp(argv[1], "multiwifi-fallback")) {
        seed_model();
        durable_model.network_count = 3;
        durable_model.selected_network = 0;
        strcpy(durable_model.networks[0].ssid, "saved-a");
        strcpy(durable_model.networks[1].ssid, "saved-b");
        strcpy(durable_model.networks[2].ssid, "saved-c");
        strcpy(durable_model.networks[1].password, "password-b");
        strcpy(durable_model.networks[2].password, "password-c");
        fake_wifi_set_network("saved-a", false, true);
        fake_wifi_set_network("saved-b", false, true);
        fake_wifi_set_network("saved-c", true, true);
        fake_scan_show("saved-b");
        fake_scan_show("saved-c");
        uint64_t initial_sequence = durable_sequence;
        boot();
        tick(false);
        assert(wifi_connects == 1 && !strcmp((const char *)wifi_config.sta.ssid, "saved-a") &&
               !associated && !connected);
        now_ms += 500;
        tick(false);
        assert(wifi_connects == 1 && !associated && !connected);
        now_ms += 25000;
        tick(false);
        tick_until_wifi_connects(2);
        assert(wifi_connects == 2 && !strcmp((const char *)wifi_config.sta.ssid, "saved-b") &&
               !associated && !connected);
        now_ms += 25000;
        tick(false);
        tick_until_wifi_connects(3);
        assert(wifi_connects == 3 && !strcmp((const char *)wifi_config.sta.ssid, "saved-c") &&
               associated && connected);
        sntp_fail = true;
        now_ms += 500;
        tick(false);
        assert(s_network_attempt_index == UINT8_MAX && s_model.selected_network == 2 &&
               durable_model.selected_network == 2 && durable_sequence == initial_sequence + 1 &&
               !strcmp(network_state(2, NULL), "ok") && public_view.portable.failed_items == 0);
        unsigned connects = wifi_connects;
        for (unsigned i = 0; i < 3; i++) {
            now_ms += 30000;
            tick(false);
        }
        assert(connected && wifi_connects == connects && durable_sequence == initial_sequence + 1);
        uint64_t refresh_deadline = s_next_refresh;
        tick(true);
        assert(wifi_connects == connects && s_next_refresh == refresh_deadline &&
               public_view.portable.network_state == QUOTA_PORTABLE_NETWORK_OFF);
        tick(false);
        assert(wifi_connects == connects + 1 && s_model.selected_network == 2 &&
               !strcmp((const char *)wifi_config.sta.ssid, "saved-c"));
        now_ms += 500;
        tick(false);
        assert(connected && s_next_refresh == refresh_deadline &&
               durable_sequence == initial_sequence + 1);
        quota_portable_service_reconnect();
        tick(false);
        tick_until_wifi_connects(connects + 2);
        assert(wifi_connects == connects + 2 &&
               !strcmp((const char *)wifi_config.sta.ssid, "saved-c"));
    } else if (!strcmp(argv[1], "multiwifi-sleep-resume")) {
        seed_model();
        durable_model.network_count = 3;
        durable_model.selected_network = 0;
        durable_model.screen_timeout_seconds = 30;
        strcpy(durable_model.networks[1].ssid, "saved-b");
        strcpy(durable_model.networks[1].password, "password-b");
        strcpy(durable_model.networks[2].ssid, "saved-c");
        strcpy(durable_model.networks[2].password, "password-c");
        fake_wifi_set_network("old-hotspot", false, true);
        fake_wifi_set_network("saved-b", false, true);
        fake_wifi_set_network("saved-c", true, true);
        fake_scan_show("saved-b");
        fake_scan_show("saved-c");
        sntp_fail = true;
        boot();
        quota_display_state_t display = {.last_input_ms = now_ms};
        quota_display_tick(&display, now_ms, public_view.screen_timeout_seconds, false);
        tick(display.sleeping);
        for (unsigned i = 0; i < 50; i++) {
            now_ms += 500;
            quota_display_tick(&display, now_ms, public_view.screen_timeout_seconds, false);
            tick(display.sleeping);
        }
        tick_until_wifi_connects(2);
        assert(!display.sleeping && s_network_attempt_index == 1 && wifi_connects == 2 &&
               s_network_tried == 3 && !strcmp((const char *)wifi_config.sta.ssid, "saved-b"));
        for (unsigned i = 0; i < 10; i++) {
            now_ms += 500;
            quota_display_tick(&display, now_ms, public_view.screen_timeout_seconds, false);
            tick(display.sleeping);
        }
        assert(display.sleeping && s_network_attempt_index == 1 && s_network_tried == 3 &&
               wifi_connects == 2 &&
               public_view.portable.network_state == QUOTA_PORTABLE_NETWORK_OFF);
        now_ms += 30000;
        assert(!quota_display_handle_key(&display, now_ms, QUOTA_KEY_PRESS, false));
        quota_display_tick(&display, now_ms, public_view.screen_timeout_seconds, false);
        tick(display.sleeping);
        tick_until_wifi_connects(3);
        assert(!display.sleeping && s_network_attempt_index == 1 && wifi_connects == 3 &&
               !strcmp((const char *)wifi_config.sta.ssid, "saved-b"));
        for (unsigned i = 0; i < 40; i++) {
            now_ms += 500;
            quota_display_tick(&display, now_ms, public_view.screen_timeout_seconds, false);
            tick(display.sleeping);
        }
        for (unsigned i = 0; i < 4; i++) {
            now_ms += 500;
            quota_display_tick(&display, now_ms, public_view.screen_timeout_seconds, false);
            tick(display.sleeping);
        }
        assert(!display.sleeping && wifi_connects == 4 &&
               !strcmp((const char *)wifi_config.sta.ssid, "saved-c") &&
               (s_network_attempt_index == 2 ||
                (s_model.selected_network == 2 && s_connected_network_index == 2 && connected)));
        now_ms += 500;
        quota_display_tick(&display, now_ms, public_view.screen_timeout_seconds, false);
        tick(display.sleeping);
        assert(s_model.selected_network == 2 && durable_model.selected_network == 2 && connected);
        for (unsigned i = 0; i < 19; i++) {
            now_ms += 500;
            quota_display_tick(&display, now_ms, public_view.screen_timeout_seconds, false);
            tick(display.sleeping);
        }
        assert(display.sleeping && wifi_connects == 4 &&
               public_view.portable.network_state == QUOTA_PORTABLE_NETWORK_OFF);
        now_ms += 30000;
        assert(!quota_display_handle_key(&display, now_ms, QUOTA_KEY_PRESS, false));
        quota_display_tick(&display, now_ms, public_view.screen_timeout_seconds, false);
        tick(display.sleeping);
        assert(!display.sleeping && wifi_connects == 5 &&
               !strcmp((const char *)wifi_config.sta.ssid, "saved-c"));
    } else if (!strcmp(argv[1], "sleep-first-prepare-wait-budget")) {
        seed_model();
        durable_model.network_count = 2;
        durable_model.selected_network = 0;
        strcpy(durable_model.networks[0].ssid, "saved-a");
        strcpy(durable_model.networks[1].ssid, "saved-b");
        fake_wifi_set_network("saved-a", false, true);
        fake_wifi_set_network("saved-b", false, true);
        fake_scan_show("saved-b");
        boot();
        begin_preferred_fallback_scan();
        fake_hold_prepare = true;
        for (unsigned i = 0; i < 20 && s_network_attempt_index != 1; i++) {
            now_ms += 10;
            tick(false);
        }
        assert(s_network_attempt_index == 1 && s_network_round_stage == NETWORK_ROUND_CANDIDATES &&
               !s_connect_at && !s_retry_at && wifi_connects == 1);
        tick(true);
        assert(s_network_attempt_paused && s_network_paused_remaining_ms == NETWORK_TIMEOUT_MS);

        fake_hold_prepare = false;
        now_ms += 500;
        tick(false);
        assert(s_network_attempt_index == 1 && s_connect_at && wifi_connects == 2 &&
               s_retry_at - now_ms == NETWORK_TIMEOUT_MS);

        now_ms = s_retry_at;
        tick(true);
        assert(s_network_attempt_paused && s_network_paused_remaining_ms == 0);
        now_ms += 500;
        tick(false);
        assert(wifi_connects == 3 && s_network_attempt_index == UINT8_MAX);
    } else if (!strcmp(argv[1], "directory-change-clears-resume-budget")) {
        seed_model();
        durable_model.network_count = 3;
        durable_model.selected_network = 0;
        strcpy(durable_model.networks[0].ssid, "saved-a");
        strcpy(durable_model.networks[1].ssid, "saved-b");
        strcpy(durable_model.networks[2].ssid, "saved-c");
        strcpy(durable_model.networks[2].password, "password-c");
        fake_wifi_set_network("saved-a", false, true);
        fake_wifi_set_network("saved-b", false, true);
        fake_wifi_set_network("saved-c", false, true);
        fake_scan_show("saved-b");
        boot();
        begin_preferred_fallback_scan();
        tick_until_wifi_connects(2);
        assert(s_network_attempt_index == 1 && wifi_connects == 2);
        now_ms += 20000;
        tick(false);
        uint64_t remaining = s_retry_at - now_ms;
        assert(remaining > 0 && remaining <= 5000);
        tick(true);
        assert(s_network_attempt_paused && s_network_paused_remaining_ms == remaining);

        fake_hold_prepare = true;
        now_ms += 500;
        tick(false);
        assert(s_resume_budget_pending && s_resume_budget_ms == remaining &&
               s_network_attempt_index == 1 && wifi_connects == 2);

        usb_open();
        model_unknown = true;
        quota_portable_command_t change = {.op = QUOTA_PORTABLE_OP_NETWORK_SAVE,
                                           .network_index = 2};
        strcpy(change.request_id, "12345678");
        strcpy(change.ssid, "saved-c");
        strcpy(change.password, "changed-password-c");
        submit_usb(&change);
        assert(s_dirty_model && s_resume_budget_pending && s_resume_budget_ms == remaining &&
               !strcmp(s_model.networks[2].password, "password-c"));

        now_ms += 1000;
        tick(false);
        assert(s_dirty_model && s_resume_budget_pending && s_resume_budget_ms == remaining);
        model_apply_unknown = true;
        model_unknown = true;
        fake_hold_prepare = false;
        now_ms = s_storage_retry_at;
        tick(false);
        assert(!s_dirty_model && !strcmp(s_model.networks[2].password, "changed-password-c"));
        assert(!s_resume_budget_pending && !s_resume_budget_ms && !s_network_attempt_paused &&
               !s_network_paused_remaining_ms);
        assert(s_network_attempt_index == 0 && s_connect_at && wifi_connects == 3 &&
               s_retry_at - now_ms == NETWORK_TIMEOUT_MS &&
               !strcmp((const char *)wifi_config.sta.ssid, "saved-a"));
    } else if (!strcmp(argv[1], "sleep-resume-budget-prepare-wait")) {
        seed_model();
        durable_model.network_count = 3;
        durable_model.selected_network = 0;
        strcpy(durable_model.networks[0].ssid, "saved-a");
        strcpy(durable_model.networks[1].ssid, "saved-b");
        strcpy(durable_model.networks[2].ssid, "saved-c");
        fake_wifi_set_network("saved-a", false, true);
        fake_wifi_set_network("saved-b", false, true);
        fake_wifi_set_network("saved-c", false, true);
        fake_scan_show("saved-b");
        fake_scan_show("saved-c");
        sntp_fail = true;
        boot();
        tick(false);
        now_ms += 25000;
        tick(false);
        tick_until_wifi_connects(2); /* saved-b is the first RSSI-ranked candidate. */
        assert(s_network_attempt_index == 1 && s_network_round_stage == NETWORK_ROUND_CANDIDATES);
        now_ms += 25000;
        tick(false); /* B budget expires. */
        tick(false);
        tick_until_wifi_connects(3); /* saved-c is the last candidate in this round. */
        assert(s_network_attempt_index == 2 && s_scan_candidate_cursor == 2 &&
               s_network_round_stage == NETWORK_ROUND_CANDIDATES);
        now_ms += 10000;
        tick(false);
        uint64_t remaining = s_retry_at - now_ms;
        assert(remaining >= 14000 && remaining <= 15000);
        uint64_t refresh_deadline = s_next_refresh;
        tick(true);
        assert(s_network_attempt_paused && s_network_paused_remaining_ms == remaining &&
               s_next_refresh == refresh_deadline);

        fake_hold_prepare = true;
        fake_wifi_deadline_ms = now_ms + 1234;
        now_ms += 500;
        tick(false); /* Wake resumes the request, but the driver has not admitted it yet. */
        assert(s_resume_budget_pending && s_resume_budget_ms == remaining &&
               s_network_attempt_index == 2 && wifi_connects == 3);
        uint64_t prepare_deadline = quota_portable_service_next_deadline_ms(false);
        assert(prepare_deadline > now_ms && prepare_deadline <= fake_wifi_deadline_ms);
        fake_wifi_deadline_ms = UINT64_MAX;
        now_ms += 50;
        tick(true); /* A second short sleep must keep the untouched 15-second budget. */
        assert(s_network_attempt_paused && s_network_paused_remaining_ms == remaining &&
               s_next_refresh == refresh_deadline);

        fake_hold_prepare = false;
        now_ms += 500;
        tick(false);
        assert(wifi_connects == 4 && s_network_attempt_index == 2 &&
               s_network_round_stage == NETWORK_ROUND_CANDIDATES && s_retry_at > now_ms &&
               s_retry_at - now_ms == remaining && s_next_refresh == refresh_deadline);
        now_ms += remaining - 1;
        tick(false);
        assert(s_network_attempt_index == 2 && s_network_round_stage == NETWORK_ROUND_CANDIDATES &&
               s_next_refresh == refresh_deadline);
        now_ms += 1;
        tick(false);
        assert(s_network_attempt_index == UINT8_MAX &&
               s_network_round_stage == NETWORK_ROUND_CANDIDATES);
        tick(false);
        assert(s_network_round_stage == NETWORK_ROUND_TERMINAL &&
               !strcmp(s_view.network_error, "wifi_no_available") &&
               s_next_refresh == refresh_deadline);
    } else if (!strcmp(argv[1], "multiwifi-ip-timeout")) {
        seed_model();
        durable_model.network_count = 2;
        strcpy(durable_model.networks[1].ssid, "saved-b");
        strcpy(durable_model.networks[1].password, "password-b");
        fake_wifi_set_network("old-hotspot", true, false);
        fake_wifi_set_network("saved-b", true, true);
        fake_scan_show("saved-b");
        boot();
        tick(false);
        assert(wifi_connects == 1 && associated && !connected &&
               !strcmp(associated_ssid, "old-hotspot"));
        now_ms += 500;
        tick(false);
        assert(wifi_connects == 1 && s_network_attempt_index == 0 && associated && !connected);
        now_ms += 25000;
        tick(false);
        tick_until_network_attempt(1);
        assert(wifi_connects == 2 && s_network_attempt_index == 1 && associated && connected &&
               !strcmp((const char *)wifi_config.sta.ssid, "saved-b"));
        now_ms += 500;
        tick(false);
        assert(s_model.selected_network == 1 && durable_model.selected_network == 1);
    } else if (!strcmp(argv[1], "multiwifi-round-backoff")) {
        seed_model();
        durable_model.network_count = 2;
        strcpy(durable_model.networks[1].ssid, "saved-b");
        strcpy(durable_model.networks[1].password, "password-b");
        fake_wifi_set_network("old-hotspot", false, true);
        fake_wifi_set_network("saved-b", false, true);
        fake_scan_show("saved-b");
        boot();
        tick(false);
        assert(wifi_connects == 1 && !strcmp((const char *)wifi_config.sta.ssid, "old-hotspot"));
        now_ms += 25000;
        tick(false);
        tick_until_network_attempt(1);
        assert(wifi_connects == 2 && !strcmp((const char *)wifi_config.sta.ssid, "saved-b"));
        now_ms += 25000;
        tick(false);
        tick(false);
        assert(wifi_connects == 2 && s_network_round_retry_at == now_ms + 30000);
        now_ms += 29999;
        tick(false);
        assert(wifi_connects == 2);
        now_ms++;
        tick(false);
        assert(wifi_connects == 3 && !strcmp((const char *)wifi_config.sta.ssid, "old-hotspot"));
    } else if (!strcmp(argv[1], "multiwifi-late-disconnect")) {
        seed_model();
        durable_model.network_count = 2;
        strcpy(durable_model.networks[0].password, "bad-password");
        strcpy(durable_model.networks[1].ssid, "saved-b");
        strcpy(durable_model.networks[1].password, "password-b");
        fake_wifi_set_network("old-hotspot", true, true);
        fake_wifi_set_network("saved-b", true, true);
        fake_scan_show("saved-b");
        boot();
        tick(false);
        assert(wifi_connects == 1 && !associated &&
               !strcmp((const char *)wifi_config.sta.ssid, "old-hotspot"));
        wifi_disconnected(WIFI_REASON_AUTH_FAIL, "old-hotspot");
        now_ms += 500;
        tick(false);
        tick_until_network_attempt(1);
        assert(s_network_attempt_index == 1 && associated && connected &&
               !strcmp((const char *)wifi_config.sta.ssid, "saved-b"));
        wifi_disconnected(WIFI_REASON_AUTH_FAIL, "old-hotspot");
        assert(!s_disconnect_reason && associated && connected &&
               !strcmp(associated_ssid, "saved-b"));
        now_ms += 500;
        tick(false);
        const char *error = NULL;
        assert(s_model.selected_network == 1 && !strcmp(network_state(0, &error), "failed") &&
               !strcmp(error, "wifi_auth_failed") && !strcmp(network_state(1, NULL), "ok"));
    } else if (!strcmp(argv[1], "multiwifi-stale-association")) {
        seed_model();
        durable_model.network_count = 2;
        strcpy(durable_model.networks[1].ssid, "saved-b");
        strcpy(durable_model.networks[1].password, "password-b");
        fake_wifi_set_network("old-hotspot", false, true);
        fake_wifi_set_network("saved-b", true, true);
        fake_scan_show("saved-b");
        boot();
        tick(false);
        now_ms += 25000;
        tick(false);
        tick_until_wifi_connects(2);
        assert(s_network_attempt_index == 1 && connected &&
               !strcmp((const char *)wifi_config.sta.ssid, "saved-b"));
        fake_wifi_associate("old-hotspot", true); /* A late event restores A's stale AP/IP view. */
        now_ms += 500;
        tick(false);
        assert(s_network_attempt_index == 1 && s_connected_network_index == UINT8_MAX &&
               s_model.selected_network == 0 && associated && connected &&
               !strcmp(associated_ssid, "old-hotspot"));
        fake_wifi_associate("saved-b", true);
        now_ms += 500;
        tick(false);
        assert(s_model.selected_network == 1 && durable_model.selected_network == 1);
    } else if (!strcmp(argv[1], "multiwifi-environment-change")) {
        seed_model();
        durable_model.network_count = 2;
        strcpy(durable_model.networks[1].ssid, "saved-b");
        strcpy(durable_model.networks[1].password, "password-b");
        fake_wifi_set_network("old-hotspot", true, true);
        fake_wifi_set_network("saved-b", true, true);
        fake_scan_show("saved-b");
        boot();
        ready();
        assert(s_model.selected_network == 0 && s_connected_network_index == 0 && connected);
        fake_wifi_networks[0].available = false; /* Previously successful AP left the area. */
        wifi_disconnected(9, "old-hotspot");
        now_ms += 500;
        tick(false);
        tick_until_wifi_connects(2);
        assert(s_network_attempt_index == 0 && wifi_connects == 2 &&
               !strcmp((const char *)wifi_config.sta.ssid, "old-hotspot"));
        now_ms += 25000;
        tick(false);
        tick_until_network_attempt(1);
        assert(s_network_attempt_index == 1 && wifi_connects == 3 && associated && connected &&
               !strcmp((const char *)wifi_config.sta.ssid, "saved-b"));
        now_ms += 500;
        tick(false);
        assert(s_model.selected_network == 1 && durable_model.selected_network == 1 &&
               s_connected_network_index == 1);
    } else if (!strcmp(argv[1], "staged-fallback-unknown")) {
        seed_model();
        durable_model.network_count = 2;
        strcpy(durable_model.networks[1].ssid, "saved-b");
        strcpy(durable_model.networks[1].password, "password-b");
        fake_wifi_set_network("old-hotspot", false, true);
        fake_wifi_set_network("saved-b", true, true);
        fake_scan_show("saved-b");
        uint64_t initial_sequence = durable_sequence;
        boot();
        usb_open();
        quota_portable_command_t net = {.op = QUOTA_PORTABLE_OP_NETWORK_SAVE, .network_index = 0};
        strcpy(net.request_id, "12345678");
        strcpy(net.ssid, "old-hotspot");
        strcpy(net.password, "bad-password");
        submit_usb(&net);
        quota_portable_command_t validate = {.op = QUOTA_PORTABLE_OP_VALIDATE};
        strcpy(validate.request_id, "22345678");
        submit_usb(&validate);
        now_ms += 500;
        tick(false);
        assert(s_candidate_pending &&
               !strcmp((const char *)wifi_config.sta.password, "bad-password"));
        wifi_disconnected(WIFI_REASON_AUTH_FAIL, "old-hotspot");
        now_ms += 500;
        tick(false);
        assert(!s_candidate_pending && s_staged.present &&
               !strcmp(network_state(0, NULL), "failed"));
        now_ms += 500;
        tick(false);
        assert(s_network_attempt_index == 0 &&
               !strcmp((const char *)wifi_config.sta.password, "password"));
        now_ms += 25000;
        tick(false);
        tick_until_wifi_connects(3);
        tick_until_network_attempt(1);
        assert(s_network_attempt_index == 1 && associated && connected &&
               !strcmp((const char *)wifi_config.sta.ssid, "saved-b"));
        model_unknown = true;
        model_apply_unknown = true;
        now_ms += 500;
        tick(false);
        assert(s_dirty_model && s_staged.present && s_model.selected_network == 0 &&
               s_network_selection.kind == NETWORK_SELECTION_NORMAL &&
               durable_model.selected_network == 1 && durable_sequence == initial_sequence + 1 &&
               !quota_portable_service_http_allowed());
        unsigned connects = wifi_connects;
        model_unknown = false;
        now_ms += 1000;
        tick(true);
        const char *error = NULL;
        assert(!s_dirty_model && !s_staged.present && s_model.selected_network == 1 &&
               durable_model.selected_network == 1 && durable_sequence == initial_sequence + 1 &&
               wifi_connects == connects &&
               !strcmp(durable_model.networks[0].password, "bad-password") &&
               !strcmp(network_state(0, &error), "failed") && !strcmp(error, "wifi_auth_failed") &&
               !strcmp(network_state(1, NULL), "ok"));
    } else if (!strcmp(argv[1], "network-index-unknown")) {
        seed_model();
        durable_model.network_count = 3;
        durable_model.selected_network = 2;
        strcpy(durable_model.networks[1].ssid, "saved-b");
        strcpy(durable_model.networks[1].password, "password-b");
        strcpy(durable_model.networks[2].ssid, "saved-c");
        strcpy(durable_model.networks[2].password, "password-c");
        fake_wifi_set_network("old-hotspot", true, true);
        fake_wifi_set_network("saved-b", true, true);
        fake_wifi_set_network("saved-c", false, true);
        boot();
        tick(false);
        assert(s_network_attempt_index == 2 && s_network_tried == (1u << 2) &&
               !strcmp((const char *)wifi_config.sta.ssid, "saved-c"));
        usb_open();
        model_unknown = true;
        quota_portable_command_t remove = {.op = QUOTA_PORTABLE_OP_NETWORK_REMOVE,
                                           .network_index = 0};
        strcpy(remove.request_id, "12345678");
        submit_usb(&remove);
        assert(s_dirty_model && s_model.network_count == 3 && s_model.selected_network == 2 &&
               s_network_attempt_index == 2 && s_network_tried == (1u << 2));
        model_unknown = false;
        now_ms += 1000;
        tick(false);
        assert(!s_dirty_model && s_model.network_count == 2 && s_model.selected_network == 1 &&
               s_network_attempt_index == 1 && s_network_tried == (1u << 1) &&
               !strcmp(s_model.networks[1].ssid, "saved-c") &&
               !strcmp((const char *)wifi_config.sta.ssid, "saved-c"));
    } else if (!strcmp(argv[1], "staged-duplicate-settings")) {
        seed_model();
        durable_model.network_count = 3;
        durable_model.selected_network = 1;
        strcpy(durable_model.networks[1].ssid, durable_model.networks[0].ssid);
        strcpy(durable_model.networks[1].password, durable_model.networks[0].password);
        strcpy(durable_model.networks[2].ssid, "saved-c");
        strcpy(durable_model.networks[2].password, "password-c");
        boot();
        usb_open();
        quota_portable_command_t net = {.op = QUOTA_PORTABLE_OP_NETWORK_SAVE, .network_index = 1};
        strcpy(net.request_id, "12345678");
        strcpy(net.ssid, "old-hotspot");
        strcpy(net.password, "staged-password");
        submit_usb(&net);
        assert(s_staged.present && s_staged.index == 1);
        quota_portable_command_t settings = {.op = QUOTA_PORTABLE_OP_SETTINGS_SAVE,
                                             .refresh_seconds = 900,
                                             .screen_timeout_seconds = 120};
        strcpy(settings.request_id, "22345678");
        submit_usb(&settings);
        assert(s_model.refresh_seconds == 900 && s_staged.present && s_staged.index == 1 &&
               !strcmp(s_staged.network.password, "staged-password"));
    } else if (!strcmp(argv[1], "staged-duplicate-remove-before-unknown")) {
        seed_model();
        network_ok = false;
        durable_model.network_count = 3;
        durable_model.selected_network = 2;
        strcpy(durable_model.networks[1].ssid, durable_model.networks[0].ssid);
        strcpy(durable_model.networks[1].password, durable_model.networks[0].password);
        strcpy(durable_model.networks[2].ssid, durable_model.networks[0].ssid);
        strcpy(durable_model.networks[2].password, durable_model.networks[0].password);
        boot();
        usb_open();
        quota_portable_command_t net = {.op = QUOTA_PORTABLE_OP_NETWORK_SAVE, .network_index = 2};
        strcpy(net.request_id, "12345678");
        strcpy(net.ssid, "old-hotspot");
        strcpy(net.password, "staged-password");
        submit_usb(&net);
        assert(s_staged.present && s_staged.index == 2 && s_network_attempt_index == 2);
        model_unknown = true;
        quota_portable_command_t remove = {.op = QUOTA_PORTABLE_OP_NETWORK_REMOVE,
                                           .network_index = 0};
        strcpy(remove.request_id, "22345678");
        submit_usb(&remove);
        assert(s_dirty_model && s_model.network_count == 3 && s_staged.index == 2 &&
               s_network_attempt_index == 2);
        model_unknown = false;
        now_ms += 1000;
        tick(false);
        assert(!s_dirty_model && s_model.network_count == 2 && s_staged.present &&
               s_staged.index == 1 && s_network_attempt_index == 1 &&
               !strcmp(s_staged.network.password, "staged-password"));
    } else if (!strcmp(argv[1], "staged-duplicate-remove-self-unknown")) {
        seed_model();
        durable_model.network_count = 3;
        durable_model.selected_network = 2;
        strcpy(durable_model.networks[1].ssid, durable_model.networks[0].ssid);
        strcpy(durable_model.networks[1].password, durable_model.networks[0].password);
        strcpy(durable_model.networks[2].ssid, durable_model.networks[0].ssid);
        strcpy(durable_model.networks[2].password, durable_model.networks[0].password);
        boot();
        usb_open();
        quota_portable_command_t net = {.op = QUOTA_PORTABLE_OP_NETWORK_SAVE, .network_index = 2};
        strcpy(net.request_id, "12345678");
        strcpy(net.ssid, "old-hotspot");
        strcpy(net.password, "staged-password");
        submit_usb(&net);
        assert(s_staged.present && s_staged.index == 2);
        model_unknown = true;
        quota_portable_command_t remove = {.op = QUOTA_PORTABLE_OP_NETWORK_REMOVE,
                                           .network_index = 2};
        strcpy(remove.request_id, "22345678");
        submit_usb(&remove);
        assert(s_dirty_model && s_staged.present && s_staged.index == 2);
        model_unknown = false;
        now_ms += 1000;
        tick(false);
        assert(!s_dirty_model && s_model.network_count == 2 && !s_staged.present &&
               !s_staged.network.password[0]);
    } else if (!strcmp(argv[1], "staged-duplicate-replace-unknown")) {
        seed_model();
        network_ok = false;
        durable_model.network_count = 3;
        durable_model.selected_network = 2;
        strcpy(durable_model.networks[1].ssid, durable_model.networks[0].ssid);
        strcpy(durable_model.networks[1].password, durable_model.networks[0].password);
        strcpy(durable_model.networks[2].ssid, durable_model.networks[0].ssid);
        strcpy(durable_model.networks[2].password, durable_model.networks[0].password);
        boot();
        usb_open();
        quota_portable_command_t staged = {.op = QUOTA_PORTABLE_OP_NETWORK_SAVE,
                                           .network_index = 2};
        strcpy(staged.request_id, "12345678");
        strcpy(staged.ssid, "old-hotspot");
        strcpy(staged.password, "staged-password");
        submit_usb(&staged);
        assert(s_staged.present && s_staged.index == 2 && s_network_attempt_index == 2);
        model_unknown = true;
        quota_portable_command_t replace = {.op = QUOTA_PORTABLE_OP_NETWORK_SAVE,
                                            .network_index = 1};
        strcpy(replace.request_id, "22345678");
        strcpy(replace.ssid, "old-hotspot");
        strcpy(replace.password, "replacement-password");
        submit_usb(&replace);
        assert(s_dirty_model && s_model.networks[1].password[0] == 'p' && s_staged.index == 2);
        model_unknown = false;
        now_ms += 1000;
        tick(false);
        assert(!s_dirty_model && s_model.networks[1].password[0] == 'r' && s_staged.present &&
               s_staged.index == 2 && s_network_attempt_index == 2);
    } else if (!strcmp(argv[1], "candidate-deadline-disconnect")) {
        seed_model();
        fake_wifi_set_network("old-hotspot", true, true);
        boot();
        ready();
        usb_open();
        quota_portable_command_t net = {.op = QUOTA_PORTABLE_OP_NETWORK_SAVE, .network_index = 0};
        strcpy(net.request_id, "12345678");
        strcpy(net.ssid, "old-hotspot");
        strcpy(net.password, "bad-password");
        submit_usb(&net);
        quota_portable_command_t validate = {.op = QUOTA_PORTABLE_OP_VALIDATE};
        strcpy(validate.request_id, "22345678");
        submit_usb(&validate);
        now_ms += 500;
        tick(false);
        assert(s_candidate_pending &&
               !strcmp((const char *)wifi_config.sta.password, "bad-password"));
        now_ms = s_candidate_deadline - 1;
        wifi_disconnected(WIFI_REASON_AUTH_FAIL, "old-hotspot");
        now_ms = s_candidate_deadline;
        tick(false);
        const char *error = NULL;
        assert(!s_candidate_pending && !strcmp(network_state(0, &error), "failed") &&
               !strcmp(error, "wifi_auth_failed"));
    } else if (!strcmp(argv[1], "staged-credentials")) {
        boot();
        ready();
        usb_open();
        quota_portable_command_t net = {.op = QUOTA_PORTABLE_OP_NETWORK_SAVE, .network_index = 0};
        strcpy(net.request_id, "12345678");
        strcpy(net.ssid, "old-hotspot");
        strcpy(net.password, "wrong-password");
        submit_usb(&net);
        /* The working password stays stored and in use; the new one is only a candidate. */
        assert(!strcmp(durable_model.networks[0].password, "password") && s_staged.present);
        quota_portable_command_t validate = {.op = QUOTA_PORTABLE_OP_VALIDATE};
        strcpy(validate.request_id, "22345678");
        network_ok = false;
        submit_usb(&validate);
        /* This request has the same SSID as the active link, but it owns a different
         * staged password. While driver retirement is pending, the old DHCP/IP state remains
         * observable to the fake SDK and must not validate or commit the candidate. */
        assert(s_candidate_pending && fake_retire_pending && associated && connected);
        assert(!strcmp(durable_model.networks[0].password, "password"));
        now_ms += 500;
        tick(false);
        assert(!fake_retire_pending && !associated && !connected &&
               !strcmp((char *)wifi_config.sta.password, "wrong-password"));
        wifi_disconnected(WIFI_REASON_AUTH_FAIL, (const char *)wifi_config.sta.ssid);
        network_ok = true;
        settle();
        const char *error = NULL;
        assert(!strcmp(network_state(0, &error), "failed") && !strcmp(error, "wifi_auth_failed"));
        assert(!strcmp(durable_model.networks[0].password, "password") && s_staged.present);
        for (unsigned i = 0; i < 4; i++) { /* back on the stored credentials */
            now_ms += 500;
            tick(false);
        }
        assert(!strcmp((char *)wifi_config.sta.password, "password"));
        /* Edited again, validated, and only then stored. */
        strcpy(net.request_id, "32345678");
        strcpy(net.password, "better-password");
        submit_usb(&net);
        assert(!strcmp(durable_model.networks[0].password, "password"));
        strcpy(validate.request_id, "42345678");
        submit_usb(&validate);
        settle();
        assert(!strcmp(network_state(0, NULL), "ok") && !s_staged.present &&
               !strcmp(durable_model.networks[0].password, "better-password"));
        /* Another network that is not in use is stored at once. */
        strcpy(net.request_id, "52345678");
        strcpy(net.ssid, "second");
        net.network_index = UINT8_MAX;
        submit_usb(&net);
        assert(durable_model.network_count == 2 && !s_staged.present &&
               !strcmp(network_state(1, NULL), "pending"));
    } else if (!strcmp(argv[1], "login-needs-network")) {
        boot();
        ready();
        usb_open();
        quota_portable_command_t queue = {.op = QUOTA_PORTABLE_OP_CODEX_QUEUE};
        strcpy(queue.request_id, "12345678");
        strcpy(queue.label, "Chat");
        submit_usb(&queue);
        /* The network is gone: authorization waits for it, then gives up without dropping the
         * queued account. */
        connected = false;
        network_ok = false;
        quota_portable_service_reconnect();
        tick(false);
        quota_portable_command_t validate = {.op = QUOTA_PORTABLE_OP_VALIDATE};
        strcpy(validate.request_id, "22345678");
        submit_usb(&validate);
        for (unsigned i = 0; i < 120 && s_validation.active; i++) {
            now_ms += 500;
            tick(false);
        }
        assert(!s_validation.active && s_operation.kind == OP_NONE && !acquired);
        assert(job(validate.request_id)->state == 3 &&
               !strcmp(job(validate.request_id)->error, "network_unavailable"));
        const char *error = NULL;
        assert(s_login_queue.present && !strcmp(account_state(2, &error), "failed") &&
               !strcmp(error, "network_unavailable"));
        /* The same account asked for again is armed again; it runs once the network is back. */
        network_ok = true;
        quota_portable_service_reconnect();
        ready();
        strcpy(queue.request_id, "32345678");
        submit_usb(&queue);
        assert(!strcmp(account_state(2, NULL), "pending"));
        strcpy(validate.request_id, "42345678");
        submit_usb(&validate);
        run_until_login_started();
        /* Cancelling the authorization keeps the account, failed, to try again. */
        quota_portable_command_t cancel = {.op = QUOTA_PORTABLE_OP_OPERATION_CANCEL};
        strcpy(cancel.request_id, "52345678");
        strcpy(cancel.target_request_id, validate.request_id);
        submit_usb(&cancel);
        settle();
        assert(s_login_queue.present && !strcmp(account_state(2, &error), "failed") &&
               !strcmp(error, "canceled") && !acquired && durable_model.entry_count == 2);
        assert(job(validate.request_id)->state == 3);
    } else if (!strcmp(argv[1], "hotspot-aborts-unstarted-login")) {
        boot();
        ready();
        usb_open();
        quota_portable_command_t queue = {.op = QUOTA_PORTABLE_OP_CODEX_QUEUE};
        strcpy(queue.request_id, "12345678");
        strcpy(queue.label, "Chat");
        submit_usb(&queue);
        login_begin_code = QUOTA_DIRECT_DEFERRED; /* the authorization has not started asking */
        quota_portable_command_t validate = {.op = QUOTA_PORTABLE_OP_VALIDATE};
        strcpy(validate.request_id, "22345678");
        submit_usb(&validate);
        now_ms += 500;
        tick(false);
        assert(s_validation.active && s_operation.kind == OP_LOGIN && !s_operation.started);
        quota_portable_service_open();
        tick_until_setup_ready();
        assert(portal_active && !s_validation.active && s_operation.kind == OP_NONE && !acquired);
        assert(durable_model.intent.kind == QUOTA_INTENT_NONE && s_login_queue.present &&
               !s_login_queue.error[0] && !strcmp(account_state(2, NULL), "pending"));
        assert(job(validate.request_id)->state == 3);
    } else if (!strcmp(argv[1], "staged-fail-stays")) {
        /* A refused staged password is not forgotten when the Passport falls back to the stored
         * one: connecting with those says nothing about the new ones. */
        boot();
        ready();
        usb_open();
        quota_portable_command_t net = {.op = QUOTA_PORTABLE_OP_NETWORK_SAVE, .network_index = 0};
        strcpy(net.request_id, "12345678");
        strcpy(net.ssid, "old-hotspot");
        strcpy(net.password, "bad-password");
        submit_usb(&net);
        quota_portable_command_t validate = {.op = QUOTA_PORTABLE_OP_VALIDATE};
        strcpy(validate.request_id, "22345678");
        submit_usb(&validate);
        for (unsigned i = 0; i < 400 && (s_validation.active || s_validation.requested); i++) {
            now_ms += 500;
            tick(false);
            if (!connected && strstr((const char *)wifi_config.sta.password, "bad"))
                wifi_disconnected(WIFI_REASON_AUTH_FAIL, (const char *)wifi_config.sta.ssid);
        }
        const char *error = NULL;
        assert(!strcmp(network_state(0, &error), "failed") && !strcmp(error, "wifi_auth_failed"));
        assert(s_staged.present && !strcmp(durable_model.networks[0].password, "password"));
        for (unsigned i = 0; i < 12; i++) { /* back on the stored password, connected again */
            now_ms += 500;
            tick(false);
        }
        assert(connected && !strcmp((char *)wifi_config.sta.password, "password"));
        assert(!strcmp(network_state(0, &error), "failed") && !strcmp(error, "wifi_auth_failed"));
        assert(s_staged.present && public_view.portable.failed_items == 1 &&
               public_view.portable.pending_items == 0);
        /* So there is still something to validate, and fixing the password resolves it. */
        strcpy(net.request_id, "32345678");
        strcpy(net.password, "fixed-password");
        submit_usb(&net);
        strcpy(validate.request_id, "42345678");
        submit_usb(&validate);
        settle();
        assert(!strcmp(network_state(0, NULL), "ok") && !s_staged.present &&
               !strcmp(durable_model.networks[0].password, "fixed-password"));
    } else if (!strcmp(argv[1], "staged-stale")) {
        /* Staged credentials never outlive the network being in use. */
        boot();
        ready();
        usb_open();
        quota_portable_command_t add = {.op = QUOTA_PORTABLE_OP_NETWORK_SAVE,
                                        .network_index = UINT8_MAX};
        strcpy(add.request_id, "02345678");
        strcpy(add.ssid, "second");
        strcpy(add.password, "good-password");
        submit_usb(&add);
        quota_portable_command_t net = {.op = QUOTA_PORTABLE_OP_NETWORK_SAVE, .network_index = 0};
        strcpy(net.request_id, "12345678");
        strcpy(net.ssid, "old-hotspot");
        strcpy(net.password, "bad-password");
        submit_usb(&net);
        assert(s_staged.present && s_staged.index == 0);
        quota_portable_command_t validate = {.op = QUOTA_PORTABLE_OP_VALIDATE};
        strcpy(validate.request_id, "22345678");
        submit_usb(&validate);
        for (unsigned i = 0; i < 400 && (s_validation.active || s_validation.requested); i++) {
            now_ms += 500;
            tick(false);
            if (!connected && strstr((const char *)wifi_config.sta.password, "bad"))
                wifi_disconnected(WIFI_REASON_AUTH_FAIL, (const char *)wifi_config.sta.ssid);
        }
        /* The second network took over; the refused password was stored for the first one, which
         * is no longer in use, and it still counts as failed. */
        const char *error = NULL;
        assert(s_model.selected_network == 1 && !s_staged.present);
        assert(!strcmp(durable_model.networks[0].ssid, "old-hotspot") &&
               !strcmp(durable_model.networks[0].password, "bad-password") &&
               !strcmp(durable_model.networks[1].password, "good-password"));
        assert(!strcmp(network_state(1, NULL), "ok") &&
               !strcmp(network_state(0, &error), "failed") && !strcmp(error, "wifi_auth_failed"));
        /* Fixing the first one through the normal path really fixes it. */
        strcpy(net.request_id, "32345678");
        strcpy(net.password, "fixed-password");
        submit_usb(&net);
        assert(!s_staged.present && !strcmp(durable_model.networks[0].password, "fixed-password"));
        assert(!strcmp(network_state(0, NULL), "pending"));
        strcpy(validate.request_id, "42345678");
        submit_usb(&validate);
        char tried[QUOTA_PASSWORD_MAX_BYTES + 1] = "";
        for (unsigned i = 0; i < 400 && (s_validation.active || s_validation.requested); i++) {
            if (s_candidate_pending && !tried[0])
                snprintf(tried, sizeof(tried), "%s", s_candidate.password);
            now_ms += 500;
            tick(false);
        }
        assert(!strcmp(tried, "fixed-password") && !strcmp(network_state(0, NULL), "ok"));
        /* The first network is in use again: a staged password stays through the removal of
         * another network, and goes with its own network. */
        assert(s_model.selected_network == 0);
        strcpy(net.request_id, "52345678");
        strcpy(net.password, "other-password");
        submit_usb(&net);
        assert(s_staged.present && s_staged.index == 0);
        quota_portable_command_t drop = {.op = QUOTA_PORTABLE_OP_NETWORK_REMOVE,
                                         .network_index = 1};
        strcpy(drop.request_id, "62345678");
        submit_usb(&drop);
        assert(s_staged.present && s_staged.index == 0 && durable_model.network_count == 1);
        drop.network_index = 0;
        strcpy(drop.request_id, "72345678");
        submit_usb(&drop);
        assert(!s_staged.present && durable_model.network_count == 0);
    } else if (!strcmp(argv[1], "factory-reset")) {
        boot();
        ready();
        usb_open();
        phone();
        /* The request is only a flag: nothing is erased until the network task ticks. */
        quota_portable_service_factory_reset();
        assert(factory_resets == 0 && restarts == 0);
        tick(false);
        assert(factory_resets == 1 && restarts == 1);
        assert(!public_view.portable.setup_active && !portal_active && !usb_window);
        tick(false); /* one request, one erase */
        assert(factory_resets == 1 && restarts == 1);
        /* The catalog is deleted but erasing the rest failed: restart, never carry on writing. */
        factory_reset_result = QUOTA_FACTORY_RESET_CATALOG_GONE;
        quota_portable_service_factory_reset();
        tick(false);
        assert(factory_resets == 2 && restarts == 2 && !public_view.portable.factory_reset_failed);
        /* Nothing deleted: the device carries on, with its own flag, no storage error. */
        factory_reset_result = QUOTA_FACTORY_RESET_FAILED;
        quota_portable_service_factory_reset();
        tick(false);
        assert(factory_resets == 3 && restarts == 2);
        assert(public_view.portable.factory_reset_failed && !public_view.portable.storage_error[0]);
        /* A new request clears the old failure before it is tried again. */
        factory_reset_result = QUOTA_FACTORY_RESET_OK;
        quota_portable_service_factory_reset();
        assert(!public_view.portable.factory_reset_failed);
        tick(false);
        assert(factory_resets == 4 && restarts == 3);
    } else if (!strcmp(argv[1], "view-results")) {
        boot();
        ready();
        /* The device screen gets the firmware version and a result for every account. */
        assert(!strcmp(public_view.portable.firmware, "3.0.0-test"));
        assert(public_view.snapshot.account_count > 0);
        for (unsigned i = 0; i < public_view.snapshot.account_count; i++)
            assert(public_view.portable.account_validation[i] == QUOTA_VALIDATION_OK);
        assert(public_view.portable.saved_network_validation[0] == QUOTA_VALIDATION_OK);
        usb_open();
        quota_portable_command_t key = {.op = QUOTA_PORTABLE_OP_DEEPSEEK_SAVE};
        strcpy(key.request_id, "12345678");
        strcpy(key.api_key, "sk-new-key");
        strcpy(key.label, "new");
        submit_usb(&key);
        bool pending = false;
        for (unsigned i = 0; i < public_view.snapshot.account_count; i++)
            pending =
                pending || public_view.portable.account_validation[i] == QUOTA_VALIDATION_PENDING;
        assert(pending || public_view.portable.pending_items > 0);
        /* At rest nothing is being saved; "saving" follows storage work, not a login state. */
        assert(!public_view.portable.saving);
        assert(!public_view.portable.setup_result && !public_view.portable.setup_opening);
    } else
        assert(false);
    puts("whole controller runtime passed");
    return 0;
}
