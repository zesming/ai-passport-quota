#include "quota_portable_service.h"
#include "quota_direct.h"
#include "quota_portal.h"
#include "quota_store.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define QUEUE_DEPTH 4
#define NETWORK_TIMEOUT_MS 25000ULL
#define CACHE_INTERVAL_MS 900000ULL
static const char *TAG = "quota_portable";
typedef struct {
    bool used;
    quota_portable_account_ref_t ref;
    quota_portable_auth_state_t auth;
    char error[49];
    uint64_t retry_ms;
} account_meta_t;
typedef struct {
    char id[9];
    quota_portable_op_t op;
    uint32_t hash;
    unsigned state; /* 0 queued, 1 running, 2 succeeded, 3 failed */
    char error[49];
} job_t;
static SemaphoreHandle_t s_mutex;
static quota_portable_service_hooks_t s_hooks;
static quota_portable_config_t s_config;
static quota_portable_view_t s_view;
static quota_snapshot_t s_snapshot, s_legacy_snapshot;
static account_meta_t s_accounts[QUOTA_MAX_ACCOUNTS];
static quota_portable_command_t s_queue[QUEUE_DEPTH];
static unsigned s_head, s_count, s_job_count;
static job_t s_jobs[QUEUE_DEPTH];
static bool s_store_ready, s_clock_ready, s_sntp, s_sleeping, s_wifi_active;
static bool s_open, s_close, s_cancel, s_refresh, s_reconnect, s_selection_dirty;
static bool s_refreshing, s_failed, s_cache_dirty;
static uint32_t s_display_generation;
static uint64_t s_setup_deadline, s_close_at, s_next_refresh, s_cache_at;
static uint64_t s_connect_at, s_retry_at, s_login_deadline;
static uint64_t s_login_trace_at;
static uint8_t s_refresh_slot;
static bool s_cycle;
static esp_netif_t *s_ap;
static quota_direct_t *s_direct;
static quota_direct_credential_t *s_login, *s_key;
static bool s_login_started, s_login_new, s_key_new;
static char s_login_job[9], s_key_job[9], s_network_job[9];
static quota_portable_network_t s_candidate;
static uint8_t s_candidate_index;
static bool s_candidate_pending;
static uint8_t s_disconnect_reason;
static char s_selected_pending[33];

static uint64_t millis(void) { return (uint64_t)esp_timer_get_time() / 1000; }
static uint64_t epoch(void) { struct timeval t; gettimeofday(&t, NULL); return t.tv_sec > 0 ? (uint64_t)t.tv_sec : 0; }
static void lock(void) { xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mutex); }
static void copy(char *out, size_t capacity, const char *in) { snprintf(out, capacity, "%s", in ? in : ""); }
static void changed(void) { if (s_hooks.notify) s_hooks.notify(); }
static void wake(void) { if (s_hooks.wake) s_hooks.wake(); }
static void free_credential(quota_direct_credential_t **value)
{
    if (*value) { quota_portable_clear_secret(*value, sizeof(**value)); free(*value); *value = NULL; }
}
static int slot_for(const char *id)
{
    for (int i = 0; i < QUOTA_MAX_ACCOUNTS; i++) if (s_accounts[i].used && !strcmp(s_accounts[i].ref.id, id)) return i;
    return -1;
}
static int snapshot_index(const char *id) { return quota_find_account_by_id(&s_snapshot, id); }
static int free_slot(void)
{
    for (int i = 0; i < QUOTA_MAX_ACCOUNTS; i++) if (!s_accounts[i].used) return i;
    return -1;
}
static void random_text(char *out, size_t length, const char *alphabet)
{
    size_t count = strlen(alphabet);
    for (size_t i = 0; i < length; i++) out[i] = alphabet[esp_random() % count];
    out[length] = 0;
}
static void initialize_account(quota_direct_credential_t *credential, quota_provider_t provider, int slot, const char *label)
{
    memset(credential, 0, sizeof(*credential));
    random_text(credential->id, QUOTA_ACCOUNT_ID_BYTES, "0123456789abcdef");
    credential->generation = 1; credential->slot = (uint8_t)slot;
    credential->provider = provider; credential->auth_state = QUOTA_PORTABLE_AUTH_PENDING;
    copy(credential->label, sizeof(credential->label), label);
}
static void publish_credential(const quota_direct_credential_t *credential)
{
    lock();
    account_meta_t *meta = &s_accounts[credential->slot];
    meta->used = true; copy(meta->ref.id, sizeof(meta->ref.id), credential->id);
    meta->ref.provider = credential->provider; meta->ref.generation = credential->generation;
    meta->auth = credential->refresh_inflight ? QUOTA_PORTABLE_AUTH_REAUTH : credential->auth_state;
    int index = snapshot_index(credential->id);
    if (index < 0 && s_snapshot.account_count < QUOTA_MAX_ACCOUNTS) index = s_snapshot.account_count++;
    if (index >= 0) {
        quota_account_t *account = &s_snapshot.accounts[index];
        copy(account->id, sizeof(account->id), credential->id); account->provider = credential->provider;
        copy(account->email, sizeof(account->email), credential->email);
        copy(account->plan, sizeof(account->plan), credential->plan);
        copy(s_snapshot.balances[index].label, sizeof(s_snapshot.balances[index].label), credential->label);
        if (!account->has_observed_at) account->status = QUOTA_STATUS_WAITING;
        if (meta->auth == QUOTA_PORTABLE_AUTH_REAUTH) account->status = QUOTA_STATUS_EXPIRED;
    }
    unlock();
}
static bool account_current(void *context, const char *id, uint32_t generation)
{
    (void)context; lock(); int slot = slot_for(id);
    bool ok = slot >= 0 && s_accounts[slot].ref.generation == generation;
    unlock(); return ok;
}
static bool admit(void *context, const char *id, uint32_t generation)
{
    (void)context; lock();
    bool ok = !s_sleeping && s_config.mode == QUOTA_MODE_DIRECT && !s_view.setup_active &&
        s_view.network_state == QUOTA_PORTABLE_NETWORK_READY;
    uint32_t display_generation = s_display_generation; unlock();
    return ok && s_hooks.display_current(display_generation) && account_current(NULL, id, generation);
}
static bool persist(void *context, const quota_direct_credential_t *credential)
{
    (void)context;
    if (!account_current(NULL, credential->id, credential->generation) ||
        !quota_store_save_credential(credential->slot, credential)) return false;
    publish_credential(credential); changed(); return true;
}
static bool save_config(quota_portable_config_t *config)
{
    if (!quota_store_save_config(config)) return false;
    lock(); s_config = *config; s_view.mode = config->mode;
    s_snapshot.refresh_seconds = config->refresh_seconds; s_snapshot.auto_refresh = config->auto_refresh;
    s_snapshot.has_screen_timeout_seconds = true; s_snapshot.screen_timeout_seconds = config->screen_timeout_seconds;
    unlock(); changed(); return true;
}
static uint32_t command_hash(const quota_portable_command_t *command)
{
    const unsigned char *bytes = (const unsigned char *)command; uint32_t hash = 2166136261U;
    for (size_t i = 0; i < sizeof(*command); i++) hash = (hash ^ bytes[i]) * 16777619U;
    return hash;
}
static void finish_job(const char *id, const char *error)
{
    lock();
    for (unsigned i = 0; i < s_job_count; i++) if (!strcmp(s_jobs[i].id, id)) {
        s_jobs[i].state = error ? 3 : 2; copy(s_jobs[i].error, sizeof(s_jobs[i].error), error); break;
    }
    unlock(); changed();
}
static bool session_active(void *context)
{
    (void)context; lock(); bool active = s_view.setup_ready && !s_sleeping && millis() < s_setup_deadline; unlock(); return active;
}
static quota_portable_submit_result_t submit(const quota_portable_command_t *command, void *context)
{
    (void)context; uint32_t hash = command_hash(command);
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) return QUOTA_PORTABLE_SUBMIT_BUSY;
    if (!s_view.setup_ready || s_sleeping || millis() >= s_setup_deadline) { unlock(); return QUOTA_PORTABLE_SUBMIT_CLOSED; }
    for (unsigned i = 0; i < s_job_count; i++) if (!strcmp(s_jobs[i].id, command->request_id)) {
        bool same = s_jobs[i].hash == hash; unlock(); return same ? QUOTA_PORTABLE_SUBMIT_ACCEPTED : QUOTA_PORTABLE_SUBMIT_CONFLICT;
    }
    if (s_count == QUEUE_DEPTH) { unlock(); return QUOTA_PORTABLE_SUBMIT_BUSY; }
    if (s_job_count == QUEUE_DEPTH) {
        unsigned completed = 0;
        while (completed < s_job_count && s_jobs[completed].state < 2) completed++;
        if (completed == s_job_count) { unlock(); return QUOTA_PORTABLE_SUBMIT_BUSY; }
        memmove(&s_jobs[completed], &s_jobs[completed + 1], sizeof(s_jobs[0]) * (s_job_count - completed - 1)); s_job_count--;
    }
    job_t *job = &s_jobs[s_job_count++]; memset(job, 0, sizeof(*job));
    copy(job->id, sizeof(job->id), command->request_id); job->op = command->op; job->hash = hash;
    s_queue[(s_head + s_count) % QUEUE_DEPTH] = *command; s_count++;
    unlock(); wake(); return QUOTA_PORTABLE_SUBMIT_ACCEPTED;
}
static const char *op_name(quota_portable_op_t op)
{
    static const char *names[] = {"invalid", "network_save", "network_scan", "deepseek_save", "codex_queue", "codex_launch", "account_remove", "settings_save", "mode_select", "setup_close", "refresh", "reconnect"};
    return (unsigned)op < sizeof(names) / sizeof(names[0]) ? names[op] : "invalid";
}
static cJSON *window_json(const quota_window_t *window)
{
    cJSON *json = cJSON_CreateObject(); cJSON_AddBoolToObject(json, "present", window->present);
    if (window->present) { cJSON_AddNumberToObject(json, "remaining_percent", window->remaining_percent); if (window->has_resets_at) cJSON_AddNumberToObject(json, "resets_at", (double)window->resets_at); }
    return json;
}
static bool state_json(char *buffer, size_t capacity, size_t *length, void *context)
{
    (void)context; cJSON *json = cJSON_CreateObject(); if (!json) return false;
    lock();
    cJSON_AddStringToObject(json, "mode", s_config.mode == QUOTA_MODE_DIRECT ? "direct" : "companion");
    cJSON *session = cJSON_AddObjectToObject(json, "session");
    cJSON_AddNumberToObject(session, "remaining_seconds", millis() < s_setup_deadline ? (double)((s_setup_deadline - millis() + 999) / 1000) : 0);
    cJSON *network = cJSON_AddObjectToObject(json, "network");
    bool connected = s_view.network_state == QUOTA_PORTABLE_NETWORK_READY || s_view.network_state == QUOTA_PORTABLE_NETWORK_CONNECTED;
    cJSON_AddBoolToObject(network, "connected", connected); cJSON_AddStringToObject(network, "state", connected ? "connected" : "disconnected");
    cJSON_AddStringToObject(network, "ssid", s_view.network_ssid); cJSON_AddStringToObject(network, "ip", s_view.network_ip);
    cJSON *networks = cJSON_AddArrayToObject(network, "saved_networks");
    for (unsigned i = 0; i < s_config.network_count; i++) {
        cJSON *item = cJSON_CreateObject(); cJSON_AddNumberToObject(item, "index", i);
        cJSON_AddStringToObject(item, "ssid", s_config.networks[i].ssid); cJSON_AddBoolToObject(item, "selected", i == s_config.selected_network); cJSON_AddItemToArray(networks, item);
    }
    cJSON *clock = cJSON_AddObjectToObject(json, "clock");
    cJSON_AddNumberToObject(clock, "epoch", (double)epoch()); cJSON_AddBoolToObject(clock, "synchronized", s_clock_ready);
    cJSON *settings = cJSON_AddObjectToObject(json, "settings");
    cJSON_AddNumberToObject(settings, "refresh_seconds", s_config.refresh_seconds); cJSON_AddBoolToObject(settings, "auto_refresh", s_config.auto_refresh);
    cJSON_AddNumberToObject(settings, "screen_timeout_seconds", s_config.screen_timeout_seconds);
    cJSON *accounts = cJSON_AddArrayToObject(json, "accounts");
    const quota_snapshot_t *snapshot = s_config.mode == QUOTA_MODE_DIRECT ? &s_snapshot : &s_legacy_snapshot;
    static const char *status_names[] = {"ok", "waiting", "expired", "error", "unsupported"};
    static const char *auth_names[] = {"pending", "ready", "expired", "error"};
    for (unsigned i = 0; i < snapshot->account_count; i++) {
        const quota_account_t *account = &snapshot->accounts[i]; cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", account->id); cJSON_AddStringToObject(item, "provider", account->provider == QUOTA_PROVIDER_CODEX ? "codex" : account->provider == QUOTA_PROVIDER_DEEPSEEK ? "deepseek" : "claude");
        cJSON_AddStringToObject(item, "email", account->email); cJSON_AddStringToObject(item, "plan", account->plan);
        cJSON_AddStringToObject(item, "label", snapshot->balances[i].label);
        cJSON_AddStringToObject(item, "status", status_names[(unsigned)account->status <= QUOTA_STATUS_UNSUPPORTED ? account->status : QUOTA_STATUS_ERROR]);
        int slot = slot_for(account->id);
        if (s_config.mode == QUOTA_MODE_DIRECT && slot >= 0) {
            cJSON_AddStringToObject(item, "auth_state", auth_names[s_accounts[slot].auth]); cJSON_AddStringToObject(item, "error_code", s_accounts[slot].error);
        }
        if (account->has_observed_at) cJSON_AddNumberToObject(item, "observed_at", (double)account->observed_at);
        cJSON_AddItemToObject(item, "five_hour", window_json(&account->five_hour)); cJSON_AddItemToObject(item, "seven_day", window_json(&account->seven_day));
        cJSON *balance = cJSON_AddObjectToObject(item, "balance"); cJSON *infos = cJSON_AddArrayToObject(balance, "balance_infos");
        const quota_currency_balance_t *cny = quota_balance_cny(&snapshot->balances[i]);
        if (cny) { cJSON *info = cJSON_CreateObject(); cJSON_AddStringToObject(info, "currency", "CNY"); cJSON_AddStringToObject(info, "total_balance", cny->total_balance); cJSON_AddItemToArray(infos, info); }
        cJSON_AddItemToArray(accounts, item);
    }
    cJSON *jobs = cJSON_AddArrayToObject(json, "jobs");
    static const char *job_states[] = {"queued", "running", "succeeded", "failed"};
    for (unsigned i = 0; i < s_job_count; i++) {
        cJSON *job = cJSON_CreateObject(); cJSON_AddStringToObject(job, "request_id", s_jobs[i].id); cJSON_AddStringToObject(job, "op", op_name(s_jobs[i].op));
        cJSON_AddStringToObject(job, "status", job_states[s_jobs[i].state]); cJSON_AddStringToObject(job, "error_code", s_jobs[i].error); cJSON_AddItemToArray(jobs, job);
    }
    unlock(); bool ok = cJSON_PrintPreallocated(json, buffer, (int)capacity, false); cJSON_Delete(json);
    if (ok) *length = strlen(buffer);
    return ok;
}
static void clock_from_phone(uint64_t utc)
{
    if (utc < 1704067200ULL || utc > 4102444800ULL) return;
    struct timeval time = {.tv_sec = (time_t)utc}; settimeofday(&time, NULL);
    lock(); s_clock_ready = true; unlock();
}
static void stop_clock(void)
{
    if (s_sntp) { esp_netif_sntp_deinit(); s_sntp = false; }
}
static void remove_snapshot(const char *id);
static void close_setup(void)
{
    if (s_login && s_view.login_state == QUOTA_PORTABLE_LOGIN_QUEUED && !quota_direct_has_pending_persist(s_direct)) {
        quota_direct_login_cancel(s_direct);
        if (s_login_new) { remove_snapshot(s_login->id); lock(); memset(&s_accounts[s_login->slot], 0, sizeof(s_accounts[0])); unlock(); }
        free_credential(&s_login); s_login_started = false;
        lock(); s_view.login_state = QUOTA_PORTABLE_LOGIN_CANCELED; s_view.auth_hold_awake = false; unlock();
    }
    quota_portal_stop(); lock(); s_view.setup_active = false; s_view.setup_ready = false;
    quota_portable_clear_secret(s_view.setup_password, sizeof(s_view.setup_password));
    quota_portable_clear_secret(s_view.setup_secret, sizeof(s_view.setup_secret)); s_view.setup_page_url[0] = 0;
    s_setup_deadline = 0; unlock();
    if (s_wifi_active) { (void)esp_wifi_set_mode(WIFI_MODE_STA); (void)esp_wifi_disconnect(); }
    s_connect_at = 0; s_retry_at = 0; changed();
}
static bool open_setup(void)
{
    if (s_login && s_login_started) return false;
    if (!s_hooks.wifi_ready()) return false;
    s_wifi_active = true;
    if (!s_ap) s_ap = esp_netif_create_default_wifi_ap();
    if (!s_ap) return false;
    wifi_config_t ap = {0}; char suffix[5], password[17], secret[44];
    random_text(suffix, 4, "0123456789ABCDEF"); random_text(password, 16, "ABCDEFGHJKLMNPQRSTUVWXYZ23456789");
    random_text(secret, QUOTA_PORTABLE_SESSION_BYTES, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_");
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "Passport-%s", suffix);
    copy((char *)ap.ap.password, sizeof(ap.ap.password), password);
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid); ap.ap.channel = 1; ap.ap.max_connection = 1; ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    /* Generate secrets while RF entropy is available; configure AP while stopped. */
    if (!s_hooks.wifi_stop()) {
        quota_portable_clear_secret(password, sizeof(password)); quota_portable_clear_secret(secret, sizeof(secret)); quota_portable_clear_secret(&ap, sizeof(ap));
        return false;
    }
    s_wifi_active = false;
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK || esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK || !s_hooks.wifi_ready()) {
        quota_portable_clear_secret(password, sizeof(password)); quota_portable_clear_secret(secret, sizeof(secret)); quota_portable_clear_secret(&ap, sizeof(ap));
        (void)esp_wifi_set_mode(WIFI_MODE_STA); return false;
    }
    s_wifi_active = true;
    (void)esp_wifi_disconnect(); stop_clock(); s_connect_at = 0;
    lock(); copy(s_view.setup_ssid, sizeof(s_view.setup_ssid), (char *)ap.ap.ssid);
    copy(s_view.setup_password, sizeof(s_view.setup_password), password); copy(s_view.setup_secret, sizeof(s_view.setup_secret), secret);
    snprintf(s_view.setup_page_url, sizeof(s_view.setup_page_url), "http://192.168.4.1/#s=%s", secret);
    s_view.setup_active = true; s_view.setup_ready = true; s_view.network_state = QUOTA_PORTABLE_NETWORK_AP;
    s_setup_deadline = millis() + QUOTA_PORTABLE_SETUP_MS; unlock();
    quota_portal_callbacks_t callbacks = {.submit = submit, .state_json = state_json, .session_active = session_active};
    bool ok = quota_portal_start(secret, &callbacks);
    quota_portable_clear_secret(password, sizeof(password)); quota_portable_clear_secret(secret, sizeof(secret)); quota_portable_clear_secret(&ap, sizeof(ap));
    if (!ok) close_setup();
    ESP_LOGI(TAG, "phone setup ready=%u free=%lu largest=%lu", (unsigned)ok,
             (unsigned long)esp_get_free_heap_size(), (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    changed(); return ok;
}
static bool connected_ip(char ip[16])
{
    wifi_ap_record_t ap; esp_netif_ip_info_t info;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta || esp_wifi_sta_get_ap_info(&ap) != ESP_OK || esp_netif_get_ip_info(sta, &info) != ESP_OK || !info.ip.addr) return false;
    snprintf(ip, 16, IPSTR, IP2STR(&info.ip)); return true;
}
static bool connect_network(const quota_portable_network_t *network)
{
    if (!network->ssid[0] || !s_hooks.wifi_ready()) return false;
    s_wifi_active = true; wifi_config_t config = {0};
    memcpy(config.sta.ssid, network->ssid, strlen(network->ssid)); memcpy(config.sta.password, network->password, strlen(network->password));
    config.sta.threshold.authmode = network->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true; config.sta.pmf_cfg.required = false;
    (void)esp_wifi_disconnect();
    bool ok = esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK && esp_wifi_set_config(WIFI_IF_STA, &config) == ESP_OK && esp_wifi_connect() == ESP_OK;
    quota_portable_clear_secret(&config, sizeof(config)); s_connect_at = millis(); s_retry_at = millis() + NETWORK_TIMEOUT_MS;
    lock(); s_view.network_state = ok ? QUOTA_PORTABLE_NETWORK_CONNECTING : QUOTA_PORTABLE_NETWORK_ERROR;
    copy(s_view.network_ssid, sizeof(s_view.network_ssid), network->ssid); s_view.network_ip[0] = 0;
    copy(s_view.network_error, sizeof(s_view.network_error), ok ? "" : "network_unavailable"); s_disconnect_reason = 0; unlock(); changed(); return ok;
}
static void maintain_network(void)
{
    char ip[16]; uint64_t now = millis();
    const quota_portable_network_t *target = s_candidate_pending ? &s_candidate : s_config.network_count ? &s_config.networks[s_config.selected_network] : NULL;
    if (!s_connect_at && target) { connect_network(target); return; }
    wifi_ap_record_t association;
    bool matching = target && esp_wifi_sta_get_ap_info(&association) == ESP_OK &&
        strnlen((char *)association.ssid, sizeof(association.ssid)) == strlen(target->ssid) &&
        !memcmp(association.ssid, target->ssid, strlen(target->ssid));
    if (matching && connected_ip(ip)) {
        if (s_candidate_pending) {
            quota_portable_config_t config = s_config;
            config.networks[s_candidate_index] = s_candidate;
            if (s_candidate_index == config.network_count) config.network_count++;
            config.selected_network = s_candidate_index;
            bool ok = save_config(&config); finish_job(s_network_job, ok ? NULL : "storage_failed");
            s_candidate_pending = false; quota_portable_clear_secret(&s_candidate, sizeof(s_candidate));
            if (!ok) { s_connect_at = 0; (void)esp_wifi_disconnect(); return; }
        }
        if (!s_sntp) {
            esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("time.cloudflare.com"); config.start = true;
            s_sntp = esp_netif_sntp_init(&config) == ESP_OK;
        }
        if (s_sntp && esp_netif_sntp_sync_wait(0) == ESP_OK) { lock(); s_clock_ready = epoch() >= 1704067200ULL; unlock(); }
        lock(); s_view.network_state = s_clock_ready ? QUOTA_PORTABLE_NETWORK_READY : QUOTA_PORTABLE_NETWORK_CONNECTED;
        copy(s_view.network_ip, sizeof(s_view.network_ip), ip); copy(s_view.network_error, sizeof(s_view.network_error), s_clock_ready ? "" : "time_required"); unlock();
        return;
    }
    stop_clock();
    if (s_connect_at && now < s_retry_at) {
        lock(); s_view.network_state = QUOTA_PORTABLE_NETWORK_CONNECTING; s_view.network_ip[0] = 0; unlock(); return;
    }
    if (s_candidate_pending && s_connect_at) {
        lock(); uint8_t reason = s_disconnect_reason; unlock();
        finish_job(s_network_job, reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ? "wifi_auth_failed" : "wifi_not_found");
        s_candidate_pending = false; quota_portable_clear_secret(&s_candidate, sizeof(s_candidate)); s_connect_at = 0;
    }
    if (s_connect_at && now < s_retry_at + 30000) {
        lock(); s_view.network_state = QUOTA_PORTABLE_NETWORK_ERROR; copy(s_view.network_error, sizeof(s_view.network_error), "network_unavailable"); s_view.network_ip[0] = 0; unlock(); return;
    }
    const quota_portable_network_t *network = s_candidate_pending ? &s_candidate : s_config.network_count ? &s_config.networks[s_config.selected_network] : NULL;
    if (network) connect_network(network);
    else { lock(); s_view.network_state = QUOTA_PORTABLE_NETWORK_OFF; unlock(); }
}
static const char *result_error(quota_direct_result_code_t code)
{
    switch (code) {
        case QUOTA_DIRECT_AUTH_REQUIRED: return "auth_required";
        case QUOTA_DIRECT_LOGIN_DISABLED: return "login_disabled";
        case QUOTA_DIRECT_EXPIRED: return "auth_expired";
        case QUOTA_DIRECT_RATE_LIMITED: return "rate_limited";
        case QUOTA_DIRECT_STORAGE_ERROR: case QUOTA_DIRECT_PERSIST_PENDING: return "storage_failed";
        case QUOTA_DIRECT_TIME_REQUIRED: return "time_required";
        case QUOTA_DIRECT_PROTOCOL_ERROR: return "provider_response_invalid";
        case QUOTA_DIRECT_NO_MEMORY: return "no_memory";
        default: return "network_unavailable";
    }
}
static void remove_snapshot(const char *id)
{
    lock(); int index = snapshot_index(id);
    if (index >= 0) {
        unsigned count = s_snapshot.account_count - (unsigned)index - 1;
        memmove(&s_snapshot.accounts[index], &s_snapshot.accounts[index + 1], count * sizeof(s_snapshot.accounts[0]));
        memmove(&s_snapshot.balances[index], &s_snapshot.balances[index + 1], count * sizeof(s_snapshot.balances[0]));
        memmove(&s_snapshot.codex_extras[index], &s_snapshot.codex_extras[index + 1], count * sizeof(s_snapshot.codex_extras[0]));
        s_snapshot.account_count--; memset(&s_snapshot.accounts[s_snapshot.account_count], 0, sizeof(s_snapshot.accounts[0]));
        memset(&s_snapshot.balances[s_snapshot.account_count], 0, sizeof(s_snapshot.balances[0])); memset(&s_snapshot.codex_extras[s_snapshot.account_count], 0, sizeof(s_snapshot.codex_extras[0])); s_cache_dirty = true;
    }
    unlock();
}
static void apply_result(int slot, const quota_direct_result_t *result)
{
    if (result->code == QUOTA_DIRECT_OK && result->source_valid && result->source_epoch >= 1704067200ULL && result->source_epoch <= 4102444800ULL) {
        struct timeval time = {.tv_sec = (time_t)result->source_epoch}; settimeofday(&time, NULL);
    }
    lock(); int index = snapshot_index(s_accounts[slot].ref.id); account_meta_t *meta = &s_accounts[slot];
    if (result->code == QUOTA_DIRECT_OK && result->source_valid && index >= 0) {
        s_snapshot.accounts[index] = result->account; s_snapshot.balances[index] = result->balance;
        s_snapshot.codex_extras[index] = result->extras;
        s_snapshot.server_time = result->source_epoch; s_snapshot.revision++;
        meta->auth = QUOTA_PORTABLE_AUTH_READY; meta->error[0] = 0; meta->retry_ms = 0; s_cache_dirty = true;
    } else if (result->code != QUOTA_DIRECT_DEFERRED && result->code != QUOTA_DIRECT_WAITING) {
        copy(meta->error, sizeof(meta->error), result_error(result->code));
        if (result->code == QUOTA_DIRECT_AUTH_REQUIRED) meta->auth = QUOTA_PORTABLE_AUTH_REAUTH;
        if (index >= 0) s_snapshot.accounts[index].status = result->code == QUOTA_DIRECT_AUTH_REQUIRED ? QUOTA_STATUS_EXPIRED : QUOTA_STATUS_ERROR;
        meta->retry_ms = millis() + (uint64_t)(result->retry_after_seconds ? result->retry_after_seconds : 30) * 1000;
        s_failed = true;
    }
    unlock(); changed();
}
static void process_command(quota_portable_command_t *command)
{
    const char *error = NULL; bool pending = false;
    clock_from_phone(command->phone_utc);
    quota_portable_config_t config = s_config;
    switch (command->op) {
        case QUOTA_PORTABLE_OP_NETWORK_SAVE: {
            if (s_candidate_pending) { error = "busy"; break; }
            unsigned index = command->network_index;
            if (!command->ssid[0]) {
                if (index >= config.network_count) { error = "invalid_request"; break; }
                s_candidate = config.networks[index];
            } else {
                if (index == UINT8_MAX) {
                    index = config.network_count;
                    for (unsigned i = 0; i < config.network_count; i++) if (!strcmp(config.networks[i].ssid, command->ssid)) index = i;
                }
                if (index > config.network_count || index >= QUOTA_PORTABLE_NETWORKS) { error = "network_limit"; break; }
                copy(s_candidate.ssid, sizeof(s_candidate.ssid), command->ssid); copy(s_candidate.password, sizeof(s_candidate.password), command->password);
            }
            s_candidate_index = (uint8_t)index; s_candidate_pending = true; copy(s_network_job, sizeof(s_network_job), command->request_id); pending = true; break;
        }
        case QUOTA_PORTABLE_OP_SETTINGS_SAVE: {
            bool timer_changed = config.refresh_seconds != command->refresh_seconds || config.auto_refresh != command->auto_refresh;
            config.refresh_seconds = command->refresh_seconds; config.auto_refresh = command->auto_refresh; config.screen_timeout_seconds = command->screen_timeout_seconds;
            if (!save_config(&config)) error = "storage_failed";
            else if (timer_changed) s_next_refresh = millis() + (uint64_t)config.refresh_seconds * 1000;
            break;
        }
        case QUOTA_PORTABLE_OP_MODE_SELECT:
            if (s_login || s_key || quota_direct_has_pending_persist(s_direct)) { error = "busy"; break; }
            config.mode = command->mode;
            if (!save_config(&config)) error = "storage_failed";
            else { s_refresh = true; s_next_refresh = millis() + 1000; }
            break;
        case QUOTA_PORTABLE_OP_SETUP_CLOSE: s_close_at = millis() + 500; break;
        case QUOTA_PORTABLE_OP_RECONNECT: s_close_at = millis() + 500; s_reconnect = true; break;
        case QUOTA_PORTABLE_OP_REFRESH:
            if (config.mode != QUOTA_MODE_DIRECT) error = "unsupported"; else s_refresh = true;
            break;
        case QUOTA_PORTABLE_OP_ACCOUNT_REMOVE: {
            if (config.mode != QUOTA_MODE_DIRECT || quota_direct_has_pending_persist(s_direct)) { error = "busy"; break; }
            int slot = slot_for(command->account_id);
            if (slot < 0) { error = "invalid_request"; break; }
            if (!quota_store_remove_credential((uint8_t)slot, s_accounts[slot].ref.id, s_accounts[slot].ref.generation)) { error = "storage_failed"; break; }
            if (s_login && !strcmp(s_login->id, command->account_id)) { quota_direct_login_cancel(s_direct); free_credential(&s_login); s_login_started = false; }
            if (s_key && !strcmp(s_key->id, command->account_id)) free_credential(&s_key);
            remove_snapshot(command->account_id); lock(); memset(&s_accounts[slot], 0, sizeof(s_accounts[slot])); unlock();
            break;
        }
        case QUOTA_PORTABLE_OP_CODEX_QUEUE: {
            if (config.mode != QUOTA_MODE_DIRECT || s_login || s_key || quota_direct_has_pending_persist(s_direct)) { error = "busy"; break; }
            int slot = command->account_id[0] ? slot_for(command->account_id) : free_slot();
            if (slot < 0 || (command->account_id[0] && s_accounts[slot].ref.provider != QUOTA_PROVIDER_CODEX)) { error = "invalid_request"; break; }
            s_login = calloc(1, sizeof(*s_login)); if (!s_login) { error = "no_memory"; break; }
            s_login_new = !command->account_id[0];
            if (s_login_new) initialize_account(s_login, QUOTA_PROVIDER_CODEX, slot, command->label);
            else if (!quota_store_load_credential((uint8_t)slot, s_login)) { free_credential(&s_login); error = "storage_failed"; break; }
            publish_credential(s_login); s_login_started = false; s_login_deadline = millis() + QUOTA_PORTABLE_LOGIN_MS;
            lock(); s_view.login_state = QUOTA_PORTABLE_LOGIN_QUEUED; s_view.auth_hold_awake = true; copy(s_view.login_account_id, sizeof(s_view.login_account_id), s_login->id); s_view.login_error[0] = 0; unlock();
            break;
        }
        case QUOTA_PORTABLE_OP_CODEX_LAUNCH:
            if (!s_login || s_login_started) error = "invalid_request";
            else { s_login_deadline = millis() + QUOTA_PORTABLE_LOGIN_MS; s_close_at = millis() + 500; copy(s_login_job, sizeof(s_login_job), command->request_id); pending = true; lock(); s_view.login_state = QUOTA_PORTABLE_LOGIN_CONNECTING; unlock(); }
            break;
        case QUOTA_PORTABLE_OP_DEEPSEEK_SAVE: {
            if (config.mode != QUOTA_MODE_DIRECT || s_key || s_login || quota_direct_has_pending_persist(s_direct)) { error = "busy"; break; }
            int slot = command->account_id[0] ? slot_for(command->account_id) : free_slot();
            if (slot < 0 || (command->account_id[0] && s_accounts[slot].ref.provider != QUOTA_PROVIDER_DEEPSEEK)) { error = "invalid_request"; break; }
            s_key = calloc(1, sizeof(*s_key)); if (!s_key) { error = "no_memory"; break; }
            s_key_new = !command->account_id[0];
            if (s_key_new) initialize_account(s_key, QUOTA_PROVIDER_DEEPSEEK, slot, command->label);
            else if (!quota_store_load_credential((uint8_t)slot, s_key)) { free_credential(&s_key); error = "storage_failed"; break; }
            copy(s_key->label, sizeof(s_key->label), command->label);
            if (!command->api_key[0]) { if (!persist(NULL, s_key)) error = "storage_failed"; free_credential(&s_key); break; }
            copy(s_key->api_key, sizeof(s_key->api_key), command->api_key);
            if (s_key_new) {
                if (!quota_store_save_credential(s_key->slot, s_key)) { free_credential(&s_key); error = "storage_failed"; break; }
                publish_credential(s_key);
            }
            copy(s_key_job, sizeof(s_key_job), command->request_id); pending = true; break;
        }
        default: error = "unsupported"; break;
    }
    if (!pending) finish_job(command->request_id, error);
    changed();
}
static void complete_login(quota_direct_result_code_t code)
{
    if (code == QUOTA_DIRECT_PERSIST_PENDING) return;
    bool ok = code == QUOTA_DIRECT_OK;
    lock(); s_view.login_state = ok ? QUOTA_PORTABLE_LOGIN_SUCCESS : code == QUOTA_DIRECT_EXPIRED ? QUOTA_PORTABLE_LOGIN_EXPIRED : QUOTA_PORTABLE_LOGIN_ERROR;
    copy(s_view.login_error, sizeof(s_view.login_error), ok ? "" : result_error(code)); s_view.auth_hold_awake = false; unlock();
    finish_job(s_login_job, ok ? NULL : result_error(code));
    if (!ok && s_login_new && s_login) { remove_snapshot(s_login->id); lock(); memset(&s_accounts[s_login->slot], 0, sizeof(s_accounts[0])); unlock(); }
    free_credential(&s_login); s_login_started = false;
    ESP_LOGI(TAG, "login result=%u free=%lu minimum=%lu largest=%lu", (unsigned)code,
             (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (ok) { lock(); s_refresh = true; unlock(); }
    changed();
}
static void trace_login(const char *phase, int code)
{
    ESP_LOGI(TAG, "login phase=%s result=%d free=%lu largest=%lu stack_low_water=%lu", phase, code,
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned long)uxTaskGetStackHighWaterMark(NULL));
}
static void login_tick(void)
{
    if (!s_login || s_view.login_state == QUOTA_PORTABLE_LOGIN_QUEUED) return;
    if (millis() >= s_login_deadline) { quota_direct_login_cancel(s_direct); complete_login(QUOTA_DIRECT_EXPIRED); return; }
    if (s_view.network_state != QUOTA_PORTABLE_NETWORK_READY) return;
    bool trace = millis() >= s_login_trace_at;
    const char *phase = !s_login_started ? "code" : s_view.login_state == QUOTA_PORTABLE_LOGIN_EXCHANGING ? "exchange" : "poll";
    if (trace) { s_login_trace_at = millis() + 15000; trace_login(phase, -1); }
    quota_direct_result_t result;
    if (!s_login_started) {
        lock(); s_view.login_state = QUOTA_PORTABLE_LOGIN_REQUESTING_CODE; unlock(); changed();
        result = quota_direct_login_begin(s_direct, s_login, millis(), epoch());
        s_login_started = result.code == QUOTA_DIRECT_WAITING || result.code == QUOTA_DIRECT_OK;
    } else result = quota_direct_login_step(s_direct, s_login, millis(), epoch());
    if (trace) trace_login(phase, result.code);
    if (result.code == QUOTA_DIRECT_DEFERRED) return;
    quota_direct_login_view_t view; quota_direct_login_view(s_direct, millis(), &view);
    bool transient = view.active && (result.code == QUOTA_DIRECT_RATE_LIMITED || result.code == QUOTA_DIRECT_NETWORK_ERROR || result.code == QUOTA_DIRECT_NO_MEMORY || result.code == QUOTA_DIRECT_TIME_REQUIRED);
    if (result.code != QUOTA_DIRECT_WAITING && result.code != QUOTA_DIRECT_PERSIST_PENDING && !transient) { complete_login(result.code); return; }
    lock(); s_view.login_state = view.exchanging ? QUOTA_PORTABLE_LOGIN_EXCHANGING : QUOTA_PORTABLE_LOGIN_WAITING;
    copy(s_view.login_url, sizeof(s_view.login_url), view.verification_url); copy(s_view.login_user_code, sizeof(s_view.login_user_code), view.user_code); unlock(); changed();
}
static void cache_tick(void)
{
    if (!s_cache_dirty || millis() < s_cache_at || !s_clock_ready) return;
    quota_portable_account_ref_t refs[QUOTA_MAX_ACCOUNTS]; size_t count = 0;
    for (unsigned i = 0; i < s_snapshot.account_count; i++) { int slot = slot_for(s_snapshot.accounts[i].id); if (slot >= 0) refs[count++] = s_accounts[slot].ref; }
    bool ok = count == s_snapshot.account_count && quota_store_save_snapshot(&s_snapshot, refs, count, epoch());
    s_cache_at = millis() + (ok ? CACHE_INTERVAL_MS : 30000);
    if (ok) {
        s_cache_dirty = false;
        quota_portable_config_t config = s_config; config.last_known_time = epoch();
        (void)save_config(&config);
    }
}
static void source_tick(void)
{
    if (s_view.network_state != QUOTA_PORTABLE_NETWORK_READY || !s_direct) return;
    if (s_key) {
        lock(); s_refreshing = true; unlock(); changed();
        quota_direct_result_t result = quota_direct_query(s_direct, s_key, epoch());
        lock(); s_refreshing = false; unlock(); changed();
        if (result.code == QUOTA_DIRECT_DEFERRED) return;
        bool ok = result.code == QUOTA_DIRECT_OK;
        if (ok) {
            s_key->auth_state = QUOTA_PORTABLE_AUTH_READY;
            if (!s_key_new) { s_key->generation++; if (!s_key->generation) s_key->generation = 1; }
            ok = quota_store_save_credential(s_key->slot, s_key);
            if (ok) publish_credential(s_key);
        }
        if (ok) apply_result(s_key->slot, &result);
        else if (s_key_new) apply_result(s_key->slot, &result);
        finish_job(s_key_job, ok ? NULL : result.code == QUOTA_DIRECT_OK ? "storage_failed" : result.code == QUOTA_DIRECT_AUTH_REQUIRED ? "invalid_key" : result_error(result.code));
        free_credential(&s_key); return;
    }
    lock();
    if (!s_cycle && (s_refresh || (s_config.auto_refresh && millis() >= s_next_refresh))) {
        s_cycle = true; s_refresh = false; s_refresh_slot = 0; s_failed = false;
    }
    unlock();
    if (!s_cycle) return;
    while (s_refresh_slot < QUOTA_MAX_ACCOUNTS && (!s_accounts[s_refresh_slot].used || s_accounts[s_refresh_slot].retry_ms > millis() || s_accounts[s_refresh_slot].auth == QUOTA_PORTABLE_AUTH_REAUTH)) s_refresh_slot++;
    if (s_refresh_slot >= QUOTA_MAX_ACCOUNTS) {
        lock(); s_cycle = false; s_refreshing = false; s_next_refresh = millis() + (uint64_t)s_config.refresh_seconds * 1000; unlock();
        cache_tick(); changed(); return;
    }
    int slot = s_refresh_slot; quota_direct_credential_t *credential = calloc(1, sizeof(*credential));
    if (!credential) { s_failed = true; return; }
    if (!quota_store_load_credential((uint8_t)slot, credential)) { free_credential(&credential); s_failed = true; s_refresh_slot++; return; }
    lock(); s_refreshing = true; unlock(); changed();
    quota_direct_result_t result;
    if (credential->provider == QUOTA_PROVIDER_CODEX && credential->expires_at <= epoch() + 300) {
        result = quota_direct_refresh(s_direct, credential, epoch());
        if (result.code == QUOTA_DIRECT_OK) result = quota_direct_query(s_direct, credential, epoch());
    } else {
        result = quota_direct_query(s_direct, credential, epoch());
        if (result.code == QUOTA_DIRECT_AUTH_REQUIRED && credential->provider == QUOTA_PROVIDER_CODEX && !credential->refresh_inflight) {
            result = quota_direct_refresh(s_direct, credential, epoch());
            if (result.code == QUOTA_DIRECT_OK) result = quota_direct_query(s_direct, credential, epoch());
        }
    }
    lock(); s_refreshing = false; unlock();
    if (result.code == QUOTA_DIRECT_OK && credential->auth_state != QUOTA_PORTABLE_AUTH_READY) {
        credential->auth_state = QUOTA_PORTABLE_AUTH_READY;
        if (!persist(NULL, credential)) result.code = QUOTA_DIRECT_STORAGE_ERROR;
    }
    if (result.code != QUOTA_DIRECT_DEFERRED) s_refresh_slot++;
    apply_result(slot, &result); free_credential(&credential);
    ESP_LOGI(TAG, "query result=%u free=%lu minimum=%lu largest=%lu", (unsigned)result.code,
             (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

bool quota_portable_service_init(const quota_device_config_t *legacy, const quota_portable_service_hooks_t *hooks)
{
    if (s_mutex) return true;
    s_mutex = xSemaphoreCreateMutex(); if (!s_mutex || !hooks) return false; s_hooks = *hooks;
    s_store_ready = quota_store_init();
    if (!quota_store_load_config(&s_config)) {
        s_config.mode = legacy ? QUOTA_MODE_COMPANION : QUOTA_MODE_DIRECT;
        s_config.refresh_seconds = legacy ? legacy->refresh_seconds : QUOTA_REFRESH_DEFAULT_SECONDS;
        s_config.auto_refresh = legacy ? legacy->auto_refresh : true; s_config.screen_timeout_seconds = QUOTA_SCREEN_TIMEOUT_DEFAULT_SECONDS;
        if (legacy) { s_config.network_count = 1; copy(s_config.networks[0].ssid, sizeof(s_config.networks[0].ssid), legacy->ssid); copy(s_config.networks[0].password, sizeof(s_config.networks[0].password), legacy->password); }
        if (s_store_ready) (void)quota_store_save_config(&s_config);
    }
    s_view.mode = s_config.mode;
    if (s_config.network_count) copy(s_view.network_ssid, sizeof(s_view.network_ssid), s_config.networks[s_config.selected_network].ssid);
    s_snapshot.refresh_seconds = s_config.refresh_seconds; s_snapshot.auto_refresh = s_config.auto_refresh;
    s_snapshot.has_screen_timeout_seconds = true; s_snapshot.screen_timeout_seconds = s_config.screen_timeout_seconds;
    quota_direct_hooks_t direct_hooks = {.admit = admit, .account_current = account_current, .persist = persist};
    s_direct = quota_direct_create(&direct_hooks, NULL, NULL); if (!s_direct) return false;
    quota_direct_credential_t *credential = calloc(1, sizeof(*credential)); if (!credential) return false;
    quota_portable_account_ref_t refs[QUOTA_MAX_ACCOUNTS]; size_t count = 0;
    for (uint8_t i = 0; i < QUOTA_MAX_ACCOUNTS; i++) if (quota_store_load_credential(i, credential) && !credential->tombstone) { publish_credential(credential); refs[count++] = s_accounts[i].ref; }
    free_credential(&credential);
    quota_snapshot_t *cached = calloc(1, sizeof(*cached));
    if (cached && quota_store_load_snapshot(refs, count, s_config.last_known_time, cached)) {
        for (unsigned i = 0; i < cached->account_count; i++) {
            int index = snapshot_index(cached->accounts[i].id);
            if (index >= 0) {
                s_snapshot.accounts[index] = cached->accounts[i]; s_snapshot.balances[index] = cached->balances[i];
                s_snapshot.codex_extras[index] = cached->codex_extras[i];
            }
        }
        s_snapshot.server_time = cached->server_time; s_snapshot.revision = cached->revision;
        for (unsigned i = 0; i < count; i++) { int index = snapshot_index(refs[i].id); int slot = slot_for(refs[i].id); if (index >= 0 && slot >= 0 && s_accounts[slot].auth == QUOTA_PORTABLE_AUTH_REAUTH) s_snapshot.accounts[index].status = QUOTA_STATUS_EXPIRED; }
    }
    free(cached);
    if (s_config.last_known_time >= 1704067200ULL) { struct timeval time = {.tv_sec = (time_t)s_config.last_known_time}; settimeofday(&time, NULL); }
    s_next_refresh = millis() + 1000;
    s_open = s_config.mode == QUOTA_MODE_DIRECT && s_config.network_count == 0;
    ESP_LOGI(TAG, "portable storage=%s mode=%s free=%lu largest=%lu", s_store_ready ? "ready" : "unavailable", s_config.mode == QUOTA_MODE_DIRECT ? "direct" : "companion", (unsigned long)esp_get_free_heap_size(), (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return true;
}
bool quota_portable_service_owns_network(void)
{
    if (!s_mutex) return false;
    lock();
    bool owns = s_config.mode == QUOTA_MODE_DIRECT || s_open || s_view.setup_active || s_login || s_candidate_pending || s_close_at || quota_direct_has_pending_persist(s_direct);
    unlock(); return owns;
}
void quota_portable_service_tick(bool sleeping, uint32_t generation)
{
    lock(); s_sleeping = sleeping; s_display_generation = generation;
    bool open = s_open, close = s_close, cancel = s_cancel, reconnect = s_reconnect;
    s_open = s_close = s_cancel = false; s_reconnect = false; unlock();
    /* Received token rotations must commit even after a display/mode change. */
    if (quota_direct_has_pending_persist(s_direct)) {
        quota_direct_result_t result = quota_direct_retry_persist(s_direct);
        if (millis() >= s_login_trace_at) { s_login_trace_at = millis() + 15000; trace_login("save", result.code); }
        if (result.code == QUOTA_DIRECT_OK && s_login) complete_login(QUOTA_DIRECT_OK);
    }
    bool persistence_pending = quota_direct_has_pending_persist(s_direct);
    if (cancel && persistence_pending) { lock(); s_cancel = true; unlock(); }
    if (cancel && s_login && !persistence_pending) {
        quota_direct_login_cancel(s_direct);
        if (s_login_new) { remove_snapshot(s_login->id); lock(); memset(&s_accounts[s_login->slot], 0, sizeof(s_accounts[0])); unlock(); }
        free_credential(&s_login); s_login_started = false; lock(); s_view.login_state = QUOTA_PORTABLE_LOGIN_CANCELED; s_view.auth_hold_awake = false; unlock();
        finish_job(s_login_job, "auth_expired");
    }
    if (sleeping) {
        if (s_view.setup_active) close_setup();
        stop_clock();
        if (s_wifi_active && s_hooks.wifi_stop()) s_wifi_active = false;
        s_connect_at = 0; lock(); s_view.network_state = QUOTA_PORTABLE_NETWORK_OFF; s_refreshing = false; unlock(); return;
    }
    if (s_login && !persistence_pending && millis() >= s_login_deadline) { quota_direct_login_cancel(s_direct); complete_login(QUOTA_DIRECT_EXPIRED); }
    if (close || (s_view.setup_active && millis() >= s_setup_deadline) || (s_close_at && millis() >= s_close_at)) { close_setup(); s_close_at = 0; }
    if (open && !s_view.setup_active) (void)open_setup();
    quota_portable_command_t command; bool have = false;
    lock(); if (s_count) { command = s_queue[s_head]; quota_portable_clear_secret(&s_queue[s_head], sizeof(s_queue[s_head])); s_head = (s_head + 1) % QUEUE_DEPTH; s_count--; have = true; for (unsigned i = 0; i < s_job_count; i++) if (!strcmp(s_jobs[i].id, command.request_id)) s_jobs[i].state = 1; } unlock();
    if (have) { process_command(&command); quota_portable_clear_secret(&command, sizeof(command)); }
    if (s_view.setup_active) return;
    if (reconnect) { (void)esp_wifi_disconnect(); s_connect_at = 0; }
    if (s_config.mode != QUOTA_MODE_DIRECT) {
        if (s_candidate_pending) maintain_network();
        return;
    }
    maintain_network();
    lock(); s_view.auth_hold_awake = s_login != NULL && millis() < s_login_deadline; unlock();
    if (s_login && !persistence_pending) login_tick();
    else if (!quota_direct_has_pending_persist(s_direct)) source_tick();
    if (s_selection_dirty) {
        quota_portable_config_t config = s_config; copy(config.selected_account_id, sizeof(config.selected_account_id), s_selected_pending);
        if (save_config(&config)) { lock(); s_selection_dirty = false; unlock(); }
    }
    cache_tick();
}
void quota_portable_service_overlay(quota_service_view_t *view)
{
    if (!s_mutex || !view) return;
    lock(); s_legacy_snapshot = view->snapshot; view->portable = s_view;
    uint64_t now = millis(); view->portable.setup_seconds_left = now < s_setup_deadline ? (uint32_t)((s_setup_deadline - now + 999) / 1000) : 0;
    view->portable.login_seconds_left = s_view.auth_hold_awake && now < s_login_deadline ? (uint32_t)((s_login_deadline - now + 999) / 1000) : 0;
    view->portable.auth_hold_awake = s_view.auth_hold_awake && now < s_login_deadline;
    if (s_config.mode == QUOTA_MODE_DIRECT) {
        view->snapshot = s_snapshot; view->snapshot.refresh_seconds = s_config.refresh_seconds; view->snapshot.auto_refresh = s_config.auto_refresh;
        view->snapshot_valid = true; view->configured = true;
        view->connected = s_view.network_state == QUOTA_PORTABLE_NETWORK_READY || s_view.network_state == QUOTA_PORTABLE_NETWORK_CONNECTED;
        view->refreshing = !s_sleeping && s_refreshing; view->request_failed = s_failed; view->clock_synchronized = s_clock_ready;
        view->refresh_seconds = s_config.refresh_seconds; view->auto_refresh = s_config.auto_refresh; view->screen_timeout_seconds = s_config.screen_timeout_seconds; view->now_epoch = epoch();
    }
    unlock();
}
void quota_portable_service_disconnected(uint8_t reason) { if (!s_mutex) return; lock(); s_disconnect_reason = reason; unlock(); }
void quota_portable_service_open(void) { lock(); s_open = true; unlock(); wake(); }
void quota_portable_service_close(void) { lock(); s_close = true; unlock(); wake(); }
void quota_portable_service_renew(void) { lock(); if (!s_view.setup_active) s_open = true; unlock(); wake(); }
void quota_portable_service_cancel_auth(void) { lock(); s_cancel = true; unlock(); wake(); }
void quota_portable_service_refresh(void) { lock(); s_refresh = true; unlock(); wake(); }
void quota_portable_service_reconnect(void) { lock(); s_reconnect = true; unlock(); wake(); }
void quota_portable_service_settings(uint16_t interval, bool automatic, uint16_t timeout)
{
    quota_portable_command_t command = {.op = QUOTA_PORTABLE_OP_SETTINGS_SAVE, .refresh_seconds = interval, .auto_refresh = automatic, .screen_timeout_seconds = timeout};
    lock(); if (s_count < QUEUE_DEPTH) { s_queue[(s_head + s_count) % QUEUE_DEPTH] = command; s_count++; } unlock(); wake();
}
void quota_portable_service_select(const char *id)
{
    if (!quota_id_is_valid(id)) return;
    lock(); copy(s_selected_pending, sizeof(s_selected_pending), id); s_selection_dirty = true; unlock(); wake();
}
bool quota_portable_service_selected(char id[QUOTA_ACCOUNT_ID_BYTES + 1])
{
    if (!s_mutex) return false;
    lock(); bool direct = s_config.mode == QUOTA_MODE_DIRECT;
    if (direct) copy(id, QUOTA_ACCOUNT_ID_BYTES + 1, s_selection_dirty ? s_selected_pending : s_config.selected_account_id);
    unlock(); return direct;
}
