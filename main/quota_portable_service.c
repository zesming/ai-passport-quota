#include "quota_portable_service.h"
#include "quota_direct.h"
#include "quota_portal.h"
#include "quota_store.h"
#include "quota_usb.h"
#include "quota_wifi.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

_Static_assert(CONFIG_LWIP_SNTP_MAX_SERVERS >= 3,
               "clock sync configures three NTP servers; raise CONFIG_LWIP_SNTP_MAX_SERVERS in "
               "sdkconfig.defaults");
#define QUEUE_DEPTH 4
#define NETWORK_TIMEOUT_MS 25000ULL
#define CACHE_INTERVAL_MS 900000ULL
#define OPEN_RETRY_MS 5000ULL
#define OPEN_RETRY_MAX 3
typedef struct {
    quota_portable_auth_state_t auth;
    char error[QUOTA_PORTABLE_ERROR_BYTES + 1];
    uint64_t retry_ms;
} account_meta_t;
typedef struct {
    char id[9];
    quota_portable_op_t op;
    uint32_t hash;
    unsigned state;
    char error[QUOTA_PORTABLE_ERROR_BYTES + 1];
} job_t;
typedef enum { OP_NONE, OP_LOGIN, OP_KEY, OP_QUERY, OP_SAVE } operation_kind_t;
typedef struct {
    operation_kind_t kind, save_kind;
    quota_direct_credential_t *credential;
    bool created, started, received, cancel_requested;
    uint8_t save_attempt;
    uint64_t deadline, retry_at;
    char job_id[9], logical_id[QUOTA_ACCOUNT_ID_BYTES + 1];
    uint32_t row_generation;
} active_operation_t;
static bool s_initialized;
static quota_portable_service_hooks_t s_hooks;
static quota_model_t s_model;
static uint64_t s_sequence;
static quota_model_t *s_dirty_model;
static bool s_dirty_effective;
static char s_dirty_job[9];
static uint64_t s_storage_retry_at;
static bool s_model_ready, s_recovery_blocked, s_authority_retry;
static char s_storage_error[QUOTA_PORTABLE_ERROR_BYTES + 1];
#define s_view (s_hooks.view->portable)
#define s_snapshot (s_hooks.view->snapshot)
static account_meta_t s_accounts[QUOTA_MAX_ACCOUNTS];
static quota_portable_command_t *s_queue;
static unsigned s_head, s_count, s_job_count;
static job_t s_jobs[QUEUE_DEPTH];
static bool s_store_ready, s_clock_ready, s_sntp, s_sleeping, s_wifi_active;
static bool s_open, s_close, s_cancel, s_refresh, s_reconnect, s_selection_dirty, s_factory_reset,
    s_factory_reset_failed, s_setup_result;
static uint64_t s_open_retry_at;
static uint8_t s_open_failures;
static bool s_refreshing, s_failed, s_cache_dirty;
static uint32_t s_display_generation;
static uint64_t s_setup_deadline, s_close_at, s_usb_close_at, s_usb_close_window, s_next_refresh,
    s_cache_at;
static uint64_t s_connect_at, s_retry_at, s_login_deadline;
static uint8_t s_refresh_slot;
static bool s_cycle;
static esp_netif_t *s_ap;
static quota_direct_t *s_direct;
static active_operation_t s_operation;
static uint64_t s_init_retry_at, s_candidate_deadline;
static quota_portable_network_t s_candidate; /* The Wi-Fi network a validation pass tries. */
static uint8_t s_candidate_index;
static bool s_candidate_pending;
static uint8_t s_disconnect_reason;
static char s_selected_pending[QUOTA_ACCOUNT_ID_BYTES + 1];
static bool s_local_settings_pending;
static quota_settings_t s_local_settings;
static uint32_t s_local_settings_generation;

/* Validation of what the user saved. Wi-Fi rows keep it in RAM only (a restart reports saved
 * networks as "saved": neither waiting nor failed until the Passport connects or is refused); a
 * DeepSeek row is pending while its credential record is, and a failed one is remembered in RAM;
 * a ChatGPT account has no row until its authorization has succeeded. */
typedef quota_validation_t validation_t;
static validation_t s_network_validation[QUOTA_PORTABLE_NETWORKS];
static char s_network_error[QUOTA_PORTABLE_NETWORKS][QUOTA_PORTABLE_ERROR_BYTES + 1];
/* The ChatGPT authorization the user queued: a new account or the re-authorization of a row. It
 * is RAM only and starts, with its credential record, when validation reaches it. */
typedef struct {
    bool present, is_new;
    char id[QUOTA_ACCOUNT_ID_BYTES + 1], label[QUOTA_PLAN_MAX_BYTES + 1], job[9];
    char error[QUOTA_PORTABLE_ERROR_BYTES + 1]; /* set when the last attempt did not finish */
} login_queue_t;
static login_queue_t s_login_queue;
/* New credentials for the network in use. The stored ones stay until these have passed validation,
 * so a wrong password cannot take the device offline; a restart drops them. */
static struct {
    bool present;
    uint8_t index;
    quota_portable_network_t network;
} s_staged;
typedef enum { PHASE_WIFI, PHASE_KEYS, PHASE_LOGIN } validation_phase_t;
static struct {
    bool requested; /* validate accepted; refuse changes before the network task starts it */
    bool active, login_started;
    validation_phase_t phase;
    uint8_t tried_networks, tried_rows;
    uint64_t wait_deadline;
    char job[9];
} s_validation;
static bool s_validate_after_close;
static uint64_t s_setup_opened;
static unsigned s_validate_runs; /* passes started; host tests count entries through it */
#define KEYS_WAIT_MS 45000ULL

static uint64_t millis(void)
{
    return (uint64_t)esp_timer_get_time() / 1000;
}
static uint64_t epoch(void)
{
    struct timeval t;
    gettimeofday(&t, NULL);
    return t.tv_sec > 0 ? (uint64_t)t.tv_sec : 0;
}
static void lock(void)
{
    s_hooks.lock();
}
static void unlock(void)
{
    s_hooks.unlock();
}
static void copy(char *out, size_t capacity, const char *in)
{
    snprintf(out, capacity, "%s", in ? in : "");
}
static void changed(void)
{
    if (s_hooks.notify)
        s_hooks.notify();
}
static void wake(void)
{
    if (s_hooks.wake)
        s_hooks.wake();
}
static uint32_t increment(uint32_t n)
{
    return n == UINT32_MAX ? 0 : n + 1;
}
static int snapshot_index(const char *id)
{
    return quota_find_account_by_id(&s_snapshot, id);
}
static void random_text(char *out, size_t length, const char *alphabet);
static void finish_job(const char *id, const char *error);
static void sync_public_locked(void);
static void finish_operation(const char *error);
static void cancel_candidate(const char *error);
static void abort_validation(void);
static bool submit_model(quota_model_t *candidate, bool effective, const char *job);
static bool ensure_direct(void);
static bool usb_blocked(void)
{
    return quota_usb_blocked();
}
static bool usb_active(void)
{
    return quota_usb_active();
}
static bool storage_barrier(void)
{
    return !s_model_ready || s_authority_retry || s_dirty_model || s_recovery_blocked ||
           s_operation.kind == OP_SAVE ||
           (s_model.intent.kind != QUOTA_INTENT_NONE && s_operation.kind == OP_NONE);
}
static const char *read_error(quota_store_read_result_t result)
{
    static const char *names[] = {
        "", "storage_missing", "storage_invalid", "storage_io_error", "no_memory", "storage_busy"};
    return (unsigned)result < sizeof(names) / sizeof(names[0]) ? names[result] : "storage_io_error";
}
static void storage_failed(quota_store_read_result_t result)
{
    copy(s_storage_error, sizeof(s_storage_error), read_error(result));
    s_storage_retry_at = millis() + 5000;
    s_authority_retry = true;
    if (result == QUOTA_STORE_READ_INVALID)
        s_recovery_blocked = true;
}
static int native_row(const char *id, uint32_t generation)
{
    for (unsigned i = 0; i < s_model.entry_count; i++) {
        const quota_catalog_entry_t *e = &s_model.entries[i];
        if (e->binding.native.credential_generation == generation &&
            !strcmp(e->binding.native.credential_id, id))
            return (int)i;
    }
    return -1;
}
static bool intent_target(const char *id, uint32_t generation)
{
    const quota_model_intent_t *intent = &s_model.intent;
    if (intent->kind != QUOTA_INTENT_UPSERT_NATIVE || !s_operation.credential ||
        s_operation.credential->slot != intent->slot ||
        generation != intent->target_credential_generation ||
        strcmp(id, intent->target_credential_id))
        return false;
    int row = quota_catalog_find(&s_model, intent->logical_id);
    return intent->new_row
               ? row < 0
               : row >= 0 && s_model.entries[row].row_generation == intent->expected_row_generation;
}
static bool account_current(void *context, const char *id, uint32_t generation)
{
    (void)context;
    lock();
    bool ok = native_row(id, generation) >= 0 || intent_target(id, generation);
    unlock();
    return ok;
}
bool quota_portable_service_http_allowed(void)
{
    if (!s_initialized)
        return false;
    lock();
    bool ok = !s_sleeping && !s_open && !s_view.setup_active && !storage_barrier() &&
              s_view.network_state == QUOTA_PORTABLE_NETWORK_READY;
    uint32_t generation = s_display_generation;
    unlock();
    return ok && !usb_blocked() && s_hooks.display_current(generation);
}
static bool admit(void *context, const char *id, uint32_t generation)
{
    (void)context;
    return quota_portable_service_http_allowed() && account_current(NULL, id, generation);
}

/* Build identity outside the shared lock. Temporary observation scratch has a single owner. */
static bool publish_model(const quota_model_t *previous)
{
    quota_snapshot_t *next = calloc(1, sizeof(*next));
    if (!next) {
        storage_failed(QUOTA_STORE_READ_NO_MEMORY);
        return false;
    }
    account_meta_t metadata[QUOTA_MAX_ACCOUNTS] = {0};
    quota_portable_credential_t *scratch = NULL;
    for (unsigned row = 0; row < s_model.entry_count; row++) {
        const quota_catalog_entry_t *entry = &s_model.entries[row];
        int old = previous ? quota_catalog_find(previous, entry->logical_id) : -1;
        bool unchanged = old >= 0 && quota_catalog_binding_equal(entry, &previous->entries[old]) &&
                         entry->row_generation == previous->entries[old].row_generation;
        if (unchanged)
            metadata[row] = s_accounts[old];
        int old_index = snapshot_index(entry->logical_id);
        const quota_portable_credential_t *credential = NULL;
        if (s_operation.credential) {
            if (quota_catalog_native_matches(entry, s_operation.credential))
                credential = s_operation.credential;
            else if (!unchanged) {
                free(next);
                copy(s_storage_error, sizeof(s_storage_error), "storage_busy");
                return false;
            }
        } else {
            if (!scratch)
                scratch = quota_store_credential_acquire();
            if (!scratch) {
                free(next);
                storage_failed(QUOTA_STORE_READ_NO_MEMORY);
                return false;
            }
            quota_store_read_result_t result =
                quota_store_load_credential_result(entry->binding.native.slot, scratch);
            if (result != QUOTA_STORE_READ_OK || !quota_catalog_native_matches(entry, scratch)) {
                quota_store_credential_release(scratch);
                free(next);
                if (result == QUOTA_STORE_READ_OK) {
                    copy(s_storage_error, sizeof(s_storage_error), "recovery_conflict");
                    s_recovery_blocked = true;
                } else
                    storage_failed(result);
                return false;
            }
            credential = scratch;
        }
        if (credential) {
            quota_portable_auth_state_t stored =
                credential->refresh_inflight ? QUOTA_PORTABLE_AUTH_REAUTH : credential->auth_state;
            /* A failed validation is remembered in RAM; the stored key stays pending. */
            if (!(unchanged && metadata[row].auth == QUOTA_PORTABLE_AUTH_ERROR &&
                  stored == QUOTA_PORTABLE_AUTH_PENDING))
                metadata[row].auth = stored;
        }
        unsigned index = next->account_count++;
        quota_account_t *account = &next->accounts[index];
        copy(account->id, sizeof(account->id), entry->logical_id);
        account->provider = entry->provider;
        account->status = QUOTA_STATUS_WAITING;
        if (unchanged && old_index >= 0) {
            quota_catalog_copy_observation(next, index, &s_snapshot, (size_t)old_index);
            copy(account->email, sizeof(account->email), s_snapshot.accounts[old_index].email);
            copy(account->plan, sizeof(account->plan), s_snapshot.accounts[old_index].plan);
        }
        if (credential) {
            copy(account->email, sizeof(account->email), credential->email);
            copy(account->plan, sizeof(account->plan), credential->plan);
        }
        if (metadata[row].auth == QUOTA_PORTABLE_AUTH_REAUTH)
            account->status = QUOTA_STATUS_EXPIRED;
        copy(next->balances[index].label, sizeof(next->balances[index].label), entry->label);
    }
    if (scratch)
        quota_store_credential_release(scratch);
    next->refresh_seconds = s_model.refresh_seconds;
    next->auto_refresh = s_model.auto_refresh;
    next->has_screen_timeout_seconds = true;
    next->screen_timeout_seconds = s_model.screen_timeout_seconds;
    next->server_time = s_snapshot.server_time;
    next->revision = s_snapshot.revision + 1;
    lock();
    s_snapshot = *next;
    memcpy(s_accounts, metadata, sizeof(metadata));
    sync_public_locked();
    unlock();
    free(next);
    return true;
}
/* A network that is new or whose name or password changed is pending; the others keep their
 * state when the list shifts. Called under the lock, before the new model replaces the old. */
static void reconcile_networks_locked(const quota_model_t *old, const quota_model_t *next)
{
    validation_t validation[QUOTA_PORTABLE_NETWORKS] = {QUOTA_VALIDATION_SAVED};
    char error[QUOTA_PORTABLE_NETWORKS][QUOTA_PORTABLE_ERROR_BYTES + 1] = {{0}};
    for (unsigned i = 0; i < next->network_count; i++) {
        validation[i] = QUOTA_VALIDATION_PENDING;
        for (unsigned j = 0; j < old->network_count; j++)
            if (!strcmp(old->networks[j].ssid, next->networks[i].ssid) &&
                !strcmp(old->networks[j].password, next->networks[i].password)) {
                validation[i] = s_network_validation[j];
                memcpy(error[i], s_network_error[j], sizeof(error[i]));
                break;
            }
    }
    memcpy(s_network_validation, validation, sizeof(validation));
    memcpy(s_network_error, error, sizeof(error));
}
static void apply_model(quota_model_t *candidate, bool effective, quota_model_t *previous)
{
    bool cadence_changed = s_model.refresh_seconds != candidate->refresh_seconds ||
                           s_model.auto_refresh != candidate->auto_refresh;
    lock();
    reconcile_networks_locked(&s_model, candidate);
    s_model = *candidate;
    s_sequence++;
    s_model_ready = true;
    if (effective)
        s_hooks.config_changed_locked();
    unlock();
    if (!publish_model(previous))
        s_model_ready = false;
    else {
        s_storage_error[0] = 0;
        s_authority_retry = false;
    }
    if (cadence_changed)
        s_next_refresh = millis() + (uint64_t)s_model.refresh_seconds * 1000;
    free(previous);
    changed();
}
/* UNKNOWN retains the exact candidate and bars every HTTP source until readback resolves. */
static bool submit_model(quota_model_t *candidate, bool effective, const char *job)
{
    if (s_dirty_model || s_recovery_blocked || s_sequence == UINT64_MAX)
        return false;
    quota_model_t *previous = malloc(sizeof(*previous));
    if (!previous) {
        storage_failed(QUOTA_STORE_READ_NO_MEMORY);
        return false;
    }
    *previous = s_model;
    quota_model_write_result_t result =
        quota_store_save_model_verified(s_sequence, candidate, s_sequence + 1);
    if (result == QUOTA_MODEL_APPLIED) {
        apply_model(candidate, effective, previous);
        return true;
    }
    free(previous);
    if (result == QUOTA_MODEL_WRITE_UNKNOWN) {
        s_dirty_model = candidate;
        s_dirty_effective = effective;
        copy(s_dirty_job, sizeof(s_dirty_job), job);
        copy(s_storage_error, sizeof(s_storage_error), "storage_write_unknown");
        s_storage_retry_at = millis() + 1000;
    } else
        copy(s_storage_error, sizeof(s_storage_error), "storage_failed");
    return false;
}
static void retry_dirty(void)
{
    if (!s_dirty_model || s_recovery_blocked || millis() < s_storage_retry_at)
        return;
    quota_model_t *previous = malloc(sizeof(*previous));
    if (!previous) {
        copy(s_storage_error, sizeof(s_storage_error), "no_memory");
        s_storage_retry_at = millis() + 5000;
        return;
    }
    *previous = s_model;
    quota_model_write_result_t result =
        quota_store_save_model_verified(s_sequence, s_dirty_model, s_sequence + 1);
    if (result == QUOTA_MODEL_WRITE_UNKNOWN) {
        uint64_t durable_sequence = 0;
        quota_store_read_result_t read = quota_store_load_model_result(previous, &durable_sequence);
        if (read == QUOTA_STORE_READ_OK && durable_sequence == s_sequence + 1 &&
            !memcmp(previous, s_dirty_model, sizeof(*previous)))
            result = QUOTA_MODEL_APPLIED;
        else if (read == QUOTA_STORE_READ_INVALID) {
            copy(s_storage_error, sizeof(s_storage_error), "storage_invalid");
            s_recovery_blocked = true;
        } else if (read == QUOTA_STORE_READ_OK && (durable_sequence != s_sequence ||
                                                   memcmp(previous, &s_model, sizeof(*previous)))) {
            copy(s_storage_error, sizeof(s_storage_error), "recovery_conflict");
            s_recovery_blocked = true;
        } else if (read == QUOTA_STORE_READ_MISSING && s_sequence != 0) {
            copy(s_storage_error, sizeof(s_storage_error), "recovery_conflict");
            s_recovery_blocked = true;
        } else if (read != QUOTA_STORE_READ_OK && read != QUOTA_STORE_READ_MISSING)
            copy(s_storage_error, sizeof(s_storage_error), read_error(read));
        *previous = s_model;
    }
    if (result == QUOTA_MODEL_APPLIED) {
        quota_model_t *candidate = s_dirty_model;
        s_dirty_model = NULL;
        apply_model(candidate, s_dirty_effective, previous);
        quota_portable_clear_secret(candidate, sizeof(*candidate));
        free(candidate);
        if (s_dirty_job[0] &&
            !(s_operation.cancel_requested && !strcmp(s_dirty_job, s_operation.job_id)))
            finish_job(s_dirty_job, NULL);
        s_dirty_job[0] = 0;
    } else {
        free(previous);
        s_storage_retry_at = millis() + 5000;
    }
}
static void dispose_candidate(quota_model_t *candidate)
{
    if (candidate && candidate != s_dirty_model) {
        quota_portable_clear_secret(candidate, sizeof(*candidate));
        free(candidate);
    }
}
/* An authorized ChatGPT credential, or a DeepSeek key that waits for its validation. */
static bool finalizable(const quota_portable_credential_t *credential)
{
    return !credential->refresh_inflight &&
           (credential->auth_state == QUOTA_PORTABLE_AUTH_READY ||
            (credential->provider == QUOTA_PROVIDER_DEEPSEEK &&
             credential->auth_state == QUOTA_PORTABLE_AUTH_PENDING));
}
static bool finalize_native(const quota_direct_credential_t *credential)
{
    if (s_model.intent.kind != QUOTA_INTENT_UPSERT_NATIVE)
        return native_row(credential->id, credential->generation) >= 0;
    const quota_model_intent_t *intent = &s_model.intent;
    if (!intent_target(credential->id, credential->generation) || !finalizable(credential))
        return false;
    quota_model_t *candidate = malloc(sizeof(*candidate));
    if (!candidate) {
        storage_failed(QUOTA_STORE_READ_NO_MEMORY);
        return false;
    }
    *candidate = s_model;
    int row = quota_catalog_find(candidate, intent->logical_id);
    if (row < 0)
        row = candidate->entry_count++;
    quota_catalog_entry_t *entry = &candidate->entries[row];
    uint32_t row_generation = intent->new_row ? 1 : increment(entry->row_generation);
    memset(entry, 0, sizeof(*entry));
    copy(entry->logical_id, sizeof(entry->logical_id), intent->logical_id);
    entry->provider = credential->provider;
    entry->row_generation = row_generation;
    copy(entry->label, sizeof(entry->label), intent->desired_label);
    entry->binding.native.slot = credential->slot;
    copy(entry->binding.native.credential_id, sizeof(entry->binding.native.credential_id),
         credential->id);
    entry->binding.native.credential_generation = credential->generation;
    memset(&candidate->intent, 0, sizeof(candidate->intent));
    bool ok = submit_model(candidate, true, NULL);
    dispose_candidate(candidate);
    return ok;
}
static bool persist(void *context, const quota_direct_credential_t *credential)
{
    (void)context;
    if (s_dirty_model || !account_current(NULL, credential->id, credential->generation) ||
        !quota_store_save_credential(credential->slot, credential))
        return false;
    if (intent_target(credential->id, credential->generation) && finalizable(credential)) {
        s_operation.received = true;
        return finalize_native(credential);
    }
    int row = native_row(credential->id, credential->generation);
    if (row >= 0) {
        lock();
        s_accounts[row].auth =
            credential->refresh_inflight ? QUOTA_PORTABLE_AUTH_REAUTH : credential->auth_state;
        sync_public_locked();
        unlock();
    }
    return true;
}
static void release_operation(void)
{
    if (quota_direct_has_pending_persist(s_direct))
        return;
    if (s_operation.credential)
        quota_store_credential_release(s_operation.credential);
    memset(&s_operation, 0, sizeof(s_operation));
}

static void publish_operation(void)
{
    changed();
}
static void finish_job(const char *id, const char *error)
{
    lock();
    for (unsigned i = 0; i < s_job_count; i++)
        if (!strcmp(s_jobs[i].id, id)) {
            if (s_jobs[i].state < 2) { /* The first result stands. */
                s_jobs[i].state = error ? 3 : 2;
                copy(s_jobs[i].error, sizeof(s_jobs[i].error), error);
            }
            break;
        }
    unlock();
    changed();
}
static uint32_t command_hash(const quota_portable_command_t *command)
{
    const unsigned char *bytes = (const unsigned char *)command;
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < offsetof(quota_portable_command_t, accepted_config_generation); i++)
        hash = (hash ^ bytes[i]) * 16777619U;
    return hash;
}
static bool session_active(void *context)
{
    (void)context;
    lock();
    bool active = s_view.setup_ready && !s_sleeping && millis() < s_setup_deadline;
    unlock();
    return active;
}
/* Called under the common lock; read-only state queries never evict receipts. */
static quota_portable_submit_result_t reserve_job(const char *id, quota_portable_op_t op,
                                                  uint32_t hash, bool *created)
{
    *created = false;
    for (unsigned i = 0; i < s_job_count; i++)
        if (!strcmp(s_jobs[i].id, id))
            return s_jobs[i].op == op && s_jobs[i].hash == hash ? QUOTA_PORTABLE_SUBMIT_ACCEPTED
                                                                : QUOTA_PORTABLE_SUBMIT_CONFLICT;
    if (s_job_count == QUEUE_DEPTH) {
        unsigned completed = 0;
        while (completed < s_job_count && s_jobs[completed].state < 2)
            completed++;
        if (completed == s_job_count)
            return QUOTA_PORTABLE_SUBMIT_BUSY;
        memmove(&s_jobs[completed], &s_jobs[completed + 1],
                sizeof(s_jobs[0]) * (s_job_count - completed - 1));
        s_job_count--;
    }
    job_t *job = &s_jobs[s_job_count++];
    memset(job, 0, sizeof(*job));
    copy(job->id, sizeof(job->id), id);
    job->op = op;
    job->hash = hash;
    *created = true;
    return QUOTA_PORTABLE_SUBMIT_ACCEPTED;
}
/* Called under the common lock. */
static void top_up_setup_locked(void)
{
    uint64_t now = millis(), cap = s_setup_opened + QUOTA_SESSION_MAX_MS;
    uint64_t wanted = now + QUOTA_SESSION_TOPUP_MS;
    if (wanted > cap)
        wanted = cap;
    if (wanted > s_setup_deadline)
        s_setup_deadline = wanted;
}
/* While a validation pass runs only cancel and setup_close are taken; the rest waits. */
static bool validating_locked(void)
{
    return s_validation.requested || s_validation.active;
}
static void validation_not_started(void)
{
    lock();
    s_validation.requested = false;
    unlock();
}
quota_portable_submit_result_t
quota_portable_service_submit(const quota_portable_command_t *command,
                              quota_setup_transport_t transport)
{
    if (!command || command->op == QUOTA_PORTABLE_OP_INVALID ||
        (command->op == QUOTA_PORTABLE_OP_VALIDATE && transport != QUOTA_SETUP_USB))
        return QUOTA_PORTABLE_SUBMIT_INVALID;
    quota_portable_command_t *fresh =
        transport == QUOTA_SETUP_USB ? calloc(QUEUE_DEPTH, sizeof(*fresh)) : NULL;
    if (!s_hooks.try_lock()) {
        free(fresh);
        return QUOTA_PORTABLE_SUBMIT_BUSY;
    }
    bool active = transport == QUOTA_SETUP_USB ? usb_active()
                                               : s_view.setup_ready && millis() < s_setup_deadline;
    quota_portable_submit_result_t result = QUOTA_PORTABLE_SUBMIT_CLOSED;
    bool created = false;
    if (active && !s_sleeping) {
        for (unsigned i = 0; i < s_job_count; i++)
            if (!strcmp(s_jobs[i].id, command->request_id)) {
                result = s_jobs[i].op == command->op && s_jobs[i].hash == command_hash(command)
                             ? QUOTA_PORTABLE_SUBMIT_ACCEPTED
                             : QUOTA_PORTABLE_SUBMIT_CONFLICT;
                unlock();
                free(fresh);
                return result;
            }
        if (validating_locked() && command->op != QUOTA_PORTABLE_OP_OPERATION_CANCEL &&
            command->op != QUOTA_PORTABLE_OP_SETUP_CLOSE) {
            unlock();
            free(fresh);
            return QUOTA_PORTABLE_SUBMIT_BUSY;
        }
        if (!s_queue && fresh) {
            s_queue = fresh;
            fresh = NULL;
            s_head = 0;
        }
        if (s_queue && s_count < QUEUE_DEPTH) {
            result = reserve_job(command->request_id, command->op, command_hash(command), &created);
            if (created) {
                quota_portable_command_t *slot = &s_queue[(s_head + s_count) % QUEUE_DEPTH];
                *slot = *command;
                slot->accepted_config_generation = s_hooks.config_generation_locked();
                slot->accepted_transport = transport;
                slot->accepted_usb_window =
                    transport == QUOTA_SETUP_USB ? quota_usb_window_id() : 0;
                s_count++;
                if (command->op == QUOTA_PORTABLE_OP_VALIDATE)
                    s_validation.requested = true;
                if (transport == QUOTA_SETUP_AP && quota_portable_op_changes_setup(command->op))
                    top_up_setup_locked();
            }
        } else
            result = QUOTA_PORTABLE_SUBMIT_BUSY;
    }
    unlock();
    free(fresh);
    if (created)
        wake();
    return result;
}
static quota_portable_submit_result_t submit(const quota_portable_command_t *command, void *context)
{
    (void)context;
    return quota_portable_service_submit(command, QUOTA_SETUP_AP);
}
static void random_text(char *out, size_t length, const char *alphabet)
{
    size_t count = strlen(alphabet);
    for (size_t i = 0; i < length; i++)
        out[i] = alphabet[esp_random() % count];
    out[length] = 0;
}
static cJSON *window_json(const quota_window_t *window)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "present", window->present);
    if (window->present) {
        cJSON_AddNumberToObject(json, "remaining_percent", window->remaining_percent);
        if (window->has_resets_at)
            cJSON_AddNumberToObject(json, "resets_at", (double)window->resets_at);
    }
    return json;
}
static void clock_from_phone(uint64_t utc)
{
    if (utc < 1704067200ULL || utc > 4102444800ULL)
        return;
    struct timeval time = {.tv_sec = (time_t)utc};
    settimeofday(&time, NULL);
    lock();
    s_clock_ready = true;
    unlock();
}
static void stop_clock(void)
{
    if (s_sntp) {
        esp_netif_sntp_deinit();
        s_sntp = false;
    }
}
static void model_defaults(quota_model_t *model)
{
    memset(model, 0, sizeof(*model));
    model->refresh_seconds = QUOTA_REFRESH_DEFAULT_SECONDS;
    model->auto_refresh = true;
    model->screen_timeout_seconds = QUOTA_SCREEN_TIMEOUT_DEFAULT_SECONDS;
}
static bool previous_tuple_matches(const quota_model_intent_t *intent,
                                   quota_store_read_result_t result,
                                   const quota_portable_credential_t *credential)
{
    if (result == QUOTA_STORE_READ_MISSING)
        return intent->previous_missing;
    return result == QUOTA_STORE_READ_OK && !intent->previous_missing &&
           credential->tombstone == intent->previous_tombstone &&
           credential->generation == intent->previous_credential_generation &&
           !strcmp(credential->id, intent->previous_credential_id) &&
           (credential->tombstone || credential->provider == intent->provider);
}
static void recover_intent(void)
{
    if (!s_model_ready || s_dirty_model || s_recovery_blocked || s_operation.kind != OP_NONE ||
        s_model.intent.kind == QUOTA_INTENT_NONE || millis() < s_storage_retry_at)
        return;
    quota_model_intent_t intent = s_model.intent;
    quota_portable_credential_t *credential = quota_store_credential_acquire();
    if (!credential) {
        storage_failed(QUOTA_STORE_READ_NO_MEMORY);
        return;
    }
    quota_store_read_result_t result = quota_store_load_credential_result(intent.slot, credential);
    if (result != QUOTA_STORE_READ_OK && result != QUOTA_STORE_READ_MISSING) {
        quota_store_credential_release(credential);
        storage_failed(result);
        return;
    }
    bool target = result == QUOTA_STORE_READ_OK && credential->slot == intent.slot &&
                  credential->generation == intent.target_credential_generation &&
                  !strcmp(credential->id, intent.target_credential_id);
    if (intent.kind == QUOTA_INTENT_DELETE_NATIVE) {
        if (target &&
            credential->tombstone) { /* Already cleaned; model intent can now be removed. */
        } else if (previous_tuple_matches(&intent, result, credential) &&
                   result == QUOTA_STORE_READ_OK && !credential->tombstone) {
            quota_store_credential_release(credential);
            credential = NULL;
            if (!quota_store_remove_credential(intent.slot, intent.target_credential_id,
                                               intent.target_credential_generation)) {
                copy(s_storage_error, sizeof(s_storage_error), "storage_failed");
                s_storage_retry_at = millis() + 5000;
                return;
            }
            credential = quota_store_credential_acquire();
            if (!credential) {
                storage_failed(QUOTA_STORE_READ_NO_MEMORY);
                return;
            }
            result = quota_store_load_credential_result(intent.slot, credential);
            target = result == QUOTA_STORE_READ_OK && credential->tombstone &&
                     credential->generation == intent.target_credential_generation &&
                     !strcmp(credential->id, intent.target_credential_id);
            if (!target) {
                quota_store_credential_release(credential);
                if (result != QUOTA_STORE_READ_OK)
                    storage_failed(result);
                else {
                    copy(s_storage_error, sizeof(s_storage_error), "recovery_conflict");
                    s_recovery_blocked = true;
                }
                return;
            }
        } else {
            quota_store_credential_release(credential);
            copy(s_storage_error, sizeof(s_storage_error), "recovery_conflict");
            s_recovery_blocked = true;
            return;
        }
    } else if (target && !credential->tombstone && credential->provider == intent.provider &&
               finalizable(credential)) {
        s_operation.credential = credential;
        s_operation.kind = OP_SAVE;
        s_operation.received = true;
        copy(s_operation.logical_id, sizeof(s_operation.logical_id), intent.logical_id);
        s_operation.row_generation = intent.expected_row_generation;
        if (finalize_native(credential))
            release_operation();
        else {
            s_operation.retry_at = millis() + 1000;
        }
        return;
    } else if (!previous_tuple_matches(&intent, result, credential)) {
        quota_store_credential_release(credential);
        copy(s_storage_error, sizeof(s_storage_error), "recovery_conflict");
        s_recovery_blocked = true;
        return;
    }
    quota_store_credential_release(credential);
    quota_model_t *candidate = malloc(sizeof(*candidate));
    if (!candidate) {
        storage_failed(QUOTA_STORE_READ_NO_MEMORY);
        return;
    }
    *candidate = s_model;
    memset(&candidate->intent, 0, sizeof(candidate->intent));
    if (!submit_model(candidate, false, NULL) && !s_dirty_model)
        s_storage_retry_at = millis() + 5000;
    dispose_candidate(candidate);
}
static const char *provider_name(quota_provider_t provider)
{
    return provider == QUOTA_PROVIDER_CODEX ? "codex" : "deepseek";
}
static const char *op_name(quota_portable_op_t op)
{
    static const char *names[] = {"invalid",       "network_save",     "network_remove",
                                  "deepseek_save", "codex_queue",      "account_remove",
                                  "settings_save", "setup_close",      "refresh",
                                  "reconnect",     "operation_cancel", "validate"};
    return (unsigned)op < sizeof(names) / sizeof(names[0]) ? names[op] : "invalid";
}
const char *quota_portable_service_firmware(void)
{
    static char version[QUOTA_FIRMWARE_VERSION_BYTES + 1];
    if (!version[0]) {
        const char *text = esp_app_get_description()->version;
        size_t length = 0;
        /* The version goes into JSON frames: keep it to plain characters. */
        for (; text && text[length] && length < QUOTA_FIRMWARE_VERSION_BYTES; length++) {
            char ch = text[length];
            bool plain = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
                         (ch >= 'a' && ch <= 'z') || ch == '.' || ch == '-' || ch == '+' ||
                         ch == '_';
            version[length] = plain ? ch : '-';
        }
        if (!length)
            copy(version, sizeof(version), "unknown");
    }
    return version;
}
static const char *validation_name(validation_t validation)
{
    return validation == QUOTA_VALIDATION_SAVED     ? "saved"
           : validation == QUOTA_VALIDATION_OK      ? "ok"
           : validation == QUOTA_VALIDATION_PENDING ? "pending"
                                                    : "failed";
}
/* A stored key or ChatGPT authorization decides the row: ready is ok, a new one is pending, and
 * an expired, rejected or failed one has failed. */
static const char *account_validation_name(quota_portable_auth_state_t auth)
{
    return auth == QUOTA_PORTABLE_AUTH_READY     ? "ok"
           : auth == QUOTA_PORTABLE_AUTH_PENDING ? "pending"
                                                 : "failed";
}
typedef struct {
    quota_snapshot_t snapshot;
    quota_catalog_entry_t entries[QUOTA_MAX_ACCOUNTS];
    account_meta_t metadata[QUOTA_MAX_ACCOUNTS];
    job_t jobs[QUEUE_DEPTH];
    uint8_t entry_count, network_count, selected_network;
    unsigned job_count;
    bool clock_ready;
    uint16_t refresh_seconds, screen_timeout_seconds;
    bool auto_refresh, connected;
    char ssids[QUOTA_PORTABLE_NETWORKS][QUOTA_SSID_MAX_BYTES + 1];
    char network_ssid[QUOTA_SSID_MAX_BYTES + 1], network_ip[16],
        storage_error[QUOTA_PORTABLE_ERROR_BYTES + 1];
    uint64_t setup_deadline, operation_deadline;
    operation_kind_t operation_kind;
    validation_t network_validation[QUOTA_PORTABLE_NETWORKS];
    char network_errors[QUOTA_PORTABLE_NETWORKS][QUOTA_PORTABLE_ERROR_BYTES + 1];
    bool validating;
    const char *validation_step;
    login_queue_t queue;
    char operation_job[9];
    quota_portable_login_state_t login_state;
    char login_code[QUOTA_PORTABLE_USER_CODE_BYTES + 1],
        login_error[QUOTA_PORTABLE_ERROR_BYTES + 1];
    uint64_t login_deadline;
} public_state_t;
bool quota_portable_service_state_json(char *buffer, size_t capacity, size_t *length,
                                       quota_setup_transport_t transport)
{
    public_state_t *state = calloc(1, sizeof(*state));
    if (!state)
        return false;
    lock();
    state->snapshot = s_snapshot;
    memcpy(state->entries, s_model.entries, sizeof(state->entries));
    memcpy(state->metadata, s_accounts, sizeof(state->metadata));
    memcpy(state->jobs, s_jobs, sizeof(state->jobs));
    state->entry_count = s_model.entry_count;
    state->job_count = s_job_count;
    state->network_count = s_model.network_count;
    state->selected_network = s_model.selected_network;
    for (unsigned i = 0; i < s_model.network_count; i++)
        copy(state->ssids[i], sizeof(state->ssids[i]),
             s_staged.present && s_staged.index == i ? s_staged.network.ssid
                                                     : s_model.networks[i].ssid);
    state->refresh_seconds = s_model.refresh_seconds;
    state->auto_refresh = s_model.auto_refresh;
    state->screen_timeout_seconds = s_model.screen_timeout_seconds;
    state->clock_ready = s_clock_ready;
    state->connected = s_view.network_state == QUOTA_PORTABLE_NETWORK_READY ||
                       s_view.network_state == QUOTA_PORTABLE_NETWORK_CONNECTED;
    copy(state->network_ssid, sizeof(state->network_ssid), s_view.network_ssid);
    copy(state->network_ip, sizeof(state->network_ip), s_view.network_ip);
    copy(state->storage_error, sizeof(state->storage_error), s_storage_error);
    state->setup_deadline = s_setup_deadline;
    state->operation_kind = s_operation.kind;
    state->operation_deadline = s_operation.deadline;
    copy(state->operation_job, sizeof(state->operation_job), s_operation.job_id);
    memcpy(state->network_validation, s_network_validation, sizeof(s_network_validation));
    memcpy(state->network_errors, s_network_error, sizeof(s_network_error));
    state->validating = validating_locked();
    state->validation_step = !s_validation.active               ? ""
                             : s_validation.phase == PHASE_WIFI ? "wifi"
                             : s_validation.phase == PHASE_KEYS ? "deepseek"
                                                                : "chatgpt";
    state->queue = s_login_queue;
    state->login_state = s_view.login_state;
    if (transport == QUOTA_SETUP_USB) {
        state->setup_deadline = quota_usb_deadline_ms();
        state->login_deadline = s_login_deadline;
        copy(state->login_code, sizeof(state->login_code), s_view.login_user_code);
        copy(state->login_error, sizeof(state->login_error), s_view.login_error);
    }
    unlock();
    cJSON *json = cJSON_CreateObject();
    if (!json) {
        free(state);
        return false;
    }
    cJSON_AddNumberToObject(json, "protocol", QUOTA_PROTOCOL_VERSION);
    cJSON_AddStringToObject(json, "firmware", quota_portable_service_firmware());
    cJSON_AddStringToObject(json, "storage_error", state->storage_error);
    cJSON_AddBoolToObject(json, "validating", state->validating);
    cJSON_AddStringToObject(json, "validation_step", state->validation_step);
    cJSON *session = cJSON_AddObjectToObject(json, "session");
    cJSON_AddNumberToObject(session, "remaining_seconds",
                            millis() < state->setup_deadline
                                ? (double)((state->setup_deadline - millis() + 999) / 1000)
                                : 0);
    cJSON *network = cJSON_AddObjectToObject(json, "network");
    cJSON_AddBoolToObject(network, "connected", state->connected);
    cJSON_AddStringToObject(network, "state", state->connected ? "connected" : "disconnected");
    cJSON_AddStringToObject(network, "ssid", state->network_ssid);
    cJSON_AddStringToObject(network, "ip", state->network_ip);
    cJSON *networks = cJSON_AddArrayToObject(network, "saved_networks");
    for (unsigned i = 0; i < state->network_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index", i);
        cJSON_AddStringToObject(item, "ssid", state->ssids[i]);
        cJSON_AddBoolToObject(item, "selected", i == state->selected_network);
        cJSON_AddStringToObject(item, "validation", validation_name(state->network_validation[i]));
        cJSON_AddStringToObject(item, "error_code", state->network_errors[i]);
        cJSON_AddItemToArray(networks, item);
    }
    cJSON *clock = cJSON_AddObjectToObject(json, "clock");
    cJSON_AddNumberToObject(clock, "epoch", (double)epoch());
    cJSON_AddBoolToObject(clock, "synchronized", state->clock_ready);
    cJSON *settings = cJSON_AddObjectToObject(json, "settings");
    cJSON_AddNumberToObject(settings, "refresh_seconds", state->refresh_seconds);
    cJSON_AddBoolToObject(settings, "auto_refresh", state->auto_refresh);
    cJSON_AddNumberToObject(settings, "screen_timeout_seconds", state->screen_timeout_seconds);
    cJSON *accounts = cJSON_AddArrayToObject(json, "accounts");
    static const char *statuses[] = {"ok", "waiting", "expired", "error", "unsupported"},
                      *auth_names[] = {"pending", "ready", "expired", "error"};
    for (unsigned row = 0; row < state->entry_count; row++) {
        const quota_catalog_entry_t *entry = &state->entries[row];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", entry->logical_id);
        cJSON_AddStringToObject(item, "provider", provider_name(entry->provider));
        cJSON_AddStringToObject(item, "label", entry->label);
        bool queued = state->queue.present && !strcmp(state->queue.id, entry->logical_id);
        cJSON_AddStringToObject(item, "error_code",
                                queued && state->queue.error[0] ? state->queue.error
                                                                : state->metadata[row].error);
        cJSON_AddStringToObject(item, "validation",
                                queued ? (state->queue.error[0] ? "failed" : "pending")
                                       : account_validation_name(state->metadata[row].auth));
        int index = quota_find_account_by_id(&state->snapshot, entry->logical_id);
        const quota_account_t *account = index >= 0 ? &state->snapshot.accounts[index] : NULL;
        cJSON_AddStringToObject(item, "status",
                                account
                                    ? statuses[(unsigned)account->status <= QUOTA_STATUS_UNSUPPORTED
                                                   ? account->status
                                                   : QUOTA_STATUS_ERROR]
                                    : "waiting");
        cJSON_AddStringToObject(
            item, "auth_state",
            auth_names[(unsigned)state->metadata[row].auth <= QUOTA_PORTABLE_AUTH_ERROR
                           ? state->metadata[row].auth
                           : QUOTA_PORTABLE_AUTH_ERROR]);
        uint64_t remaining = state->metadata[row].retry_ms > millis()
                                 ? (state->metadata[row].retry_ms - millis() + 999) / 1000
                                 : 0;
        cJSON_AddNumberToObject(item, "retry_after_seconds", (double)remaining);
        if (state->clock_ready && remaining)
            cJSON_AddNumberToObject(item, "retry_at", (double)(epoch() + remaining));
        if (account) {
            cJSON_AddStringToObject(item, "email", account->email);
            cJSON_AddStringToObject(item, "plan", account->plan);
            if (account->has_observed_at)
                cJSON_AddNumberToObject(item, "observed_at", (double)account->observed_at);
            cJSON_AddItemToObject(item, "five_hour", window_json(&account->five_hour));
            cJSON_AddItemToObject(item, "seven_day", window_json(&account->seven_day));
            cJSON *balance = cJSON_AddObjectToObject(item, "balance"),
                  *infos = cJSON_AddArrayToObject(balance, "balance_infos");
            const quota_currency_balance_t *cny =
                quota_balance_cny(&state->snapshot.balances[index]);
            if (cny) {
                cJSON *info = cJSON_CreateObject();
                cJSON_AddStringToObject(info, "currency", "CNY");
                cJSON_AddStringToObject(info, "total_balance", cny->total_balance);
                cJSON_AddItemToArray(infos, info);
            }
        }
        cJSON_AddItemToArray(accounts, item);
    }
    /* A new ChatGPT account has no row until it is authorized; it is listed as pending. */
    bool listed = false;
    for (unsigned row = 0; row < state->entry_count; row++)
        listed = listed || !strcmp(state->entries[row].logical_id, state->queue.id);
    if (state->queue.present && !listed) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", state->queue.id);
        cJSON_AddStringToObject(item, "provider", provider_name(QUOTA_PROVIDER_CODEX));
        cJSON_AddStringToObject(item, "label", state->queue.label);
        cJSON_AddStringToObject(item, "error_code", state->queue.error);
        cJSON_AddStringToObject(item, "status", "waiting");
        cJSON_AddStringToObject(item, "auth_state", "pending");
        cJSON_AddStringToObject(item, "validation", state->queue.error[0] ? "failed" : "pending");
        cJSON_AddNumberToObject(item, "retry_after_seconds", 0);
        cJSON_AddItemToArray(accounts, item);
    }
    cJSON *operation = cJSON_AddObjectToObject(json, "operation");
    static const char *operations[] = {"none", "login", "key", "query", "save"};
    cJSON_AddStringToObject(operation, "kind", operations[state->operation_kind]);
    cJSON_AddStringToObject(operation, "request_id", state->operation_job);
    cJSON_AddStringToObject(operation, "state",
                            state->operation_kind == OP_SAVE                        ? "saving"
                            : state->login_state == QUOTA_PORTABLE_LOGIN_QUEUED     ? "queued"
                            : state->login_state == QUOTA_PORTABLE_LOGIN_EXCHANGING ? "exchanging"
                                                                                    : "waiting");
    cJSON_AddNumberToObject(operation, "seconds_left",
                            state->operation_deadline > millis()
                                ? (double)((state->operation_deadline - millis() + 999) / 1000)
                                : 0);
    cJSON_AddBoolToObject(operation, "cancelable",
                          state->operation_kind != OP_NONE && state->operation_kind != OP_SAVE);
    if (transport == QUOTA_SETUP_USB) {
        static const char *login_names[] = {
            "idle",       "queued",    "connecting", "requesting_code", "waiting",
            "exchanging", "succeeded", "failed",     "canceled",        "expired"};
        cJSON *login = cJSON_AddObjectToObject(json, "login");
        cJSON_AddStringToObject(login, "state",
                                (unsigned)state->login_state <
                                        sizeof(login_names) / sizeof(login_names[0])
                                    ? login_names[state->login_state]
                                    : "failed");
        bool awaiting =
            state->operation_kind == OP_LOGIN && state->login_state == QUOTA_PORTABLE_LOGIN_WAITING;
        cJSON_AddStringToObject(login, "verification_url",
                                awaiting && state->login_code[0] ? QUOTA_DIRECT_VERIFICATION_URL
                                                                 : "");
        cJSON_AddStringToObject(login, "user_code", awaiting ? state->login_code : "");
        cJSON_AddStringToObject(login, "error_code", state->login_error);
        cJSON_AddStringToObject(login, "account_id", state->queue.present ? state->queue.id : "");
        cJSON_AddNumberToObject(login, "seconds_left",
                                awaiting && state->login_deadline > millis()
                                    ? (double)((state->login_deadline - millis() + 999) / 1000)
                                    : 0);
    }
    cJSON *jobs = cJSON_AddArrayToObject(json, "jobs");
    static const char *job_states[] = {"queued", "running", "succeeded", "failed"};
    for (unsigned i = 0; i < state->job_count; i++) {
        cJSON *job = cJSON_CreateObject();
        cJSON_AddStringToObject(job, "request_id", state->jobs[i].id);
        cJSON_AddStringToObject(job, "op", op_name(state->jobs[i].op));
        cJSON_AddStringToObject(job, "status", job_states[state->jobs[i].state]);
        cJSON_AddStringToObject(job, "error_code", state->jobs[i].error);
        cJSON_AddItemToArray(jobs, job);
    }
    bool ok = cJSON_PrintPreallocated(json, buffer, (int)capacity, false);
    cJSON_Delete(json);
    free(state);
    if (ok)
        *length = strlen(buffer);
    return ok;
}
static bool state_json(char *buffer, size_t capacity, size_t *length, void *context)
{
    (void)context;
    return quota_portable_service_state_json(buffer, capacity, length, QUOTA_SETUP_AP);
}

static void close_setup(void)
{
    /* Stop waits for handlers before releasing their heap queue. Accepted
     * commands stay queued until the owner consumes them. */
    quota_portal_stop();
    lock();
    s_view.setup_active = false;
    s_view.setup_ready = false;
    quota_portable_clear_secret(s_view.setup_password, sizeof(s_view.setup_password));
    quota_portable_clear_secret(s_view.setup_secret, sizeof(s_view.setup_secret));
    s_view.setup_page_url[0] = 0;
    s_setup_deadline = 0;
    unlock();
    publish_operation();
    if (s_wifi_active) {
        (void)esp_wifi_set_mode(WIFI_MODE_STA);
        (void)esp_wifi_disconnect();
    }
    s_connect_at = 0;
    s_retry_at = 0;
    changed();
}
static bool open_setup(void)
{
    if (s_operation.kind == OP_SAVE || quota_direct_has_pending_persist(s_direct) ||
        (s_operation.kind == OP_LOGIN && s_operation.started))
        return false;
    if (!s_queue) {
        s_queue = calloc(QUEUE_DEPTH, sizeof(*s_queue));
        if (!s_queue) {
            lock();
            copy(s_view.login_error, sizeof(s_view.login_error), "no_memory");
            unlock();
            changed();
            return false;
        }
    }
    if (!quota_wifi_start())
        return false;
    s_wifi_active = true;
    if (!s_ap)
        s_ap = esp_netif_create_default_wifi_ap();
    if (!s_ap)
        return false;
    wifi_config_t ap = {0};
    char suffix[5], password[17], secret[QUOTA_PORTABLE_ACCESS_CODE_BYTES + 1], digits[17];
    random_text(suffix, 4, "0123456789ABCDEF");
    random_text(password, 16, "ABCDEFGHJKLMNPQRSTUVWXYZ23456789");
    /* 16 Crockford Base32 characters (80 bits) as XXXX-XXXX-XXXX-XXXX. */
    random_text(digits, 16, "0123456789ABCDEFGHJKMNPQRSTVWXYZ");
    snprintf(secret, sizeof(secret), "%.4s-%.4s-%.4s-%.4s", digits, digits + 4, digits + 8,
             digits + 12);
    quota_portable_clear_secret(digits, sizeof(digits));
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "Passport-%s", suffix);
    copy((char *)ap.ap.password, sizeof(ap.ap.password), password);
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 1;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    bool ok = quota_wifi_stop();
    s_wifi_active = false;
    if (ok)
        ok = esp_wifi_set_mode(WIFI_MODE_APSTA) == ESP_OK &&
             esp_wifi_set_config(WIFI_IF_AP, &ap) == ESP_OK && quota_wifi_start();
    if (ok) {
        s_wifi_active = true;
        (void)esp_wifi_disconnect();
        stop_clock();
        s_connect_at = 0;
        lock();
        copy(s_view.setup_ssid, sizeof(s_view.setup_ssid), (char *)ap.ap.ssid);
        copy(s_view.setup_password, sizeof(s_view.setup_password), password);
        copy(s_view.setup_secret, sizeof(s_view.setup_secret), secret);
        snprintf(s_view.setup_page_url, sizeof(s_view.setup_page_url),
                 "http://192.168.4.1/#code=%s", secret);
        s_view.setup_active = s_view.setup_ready = true;
        s_view.network_state = QUOTA_PORTABLE_NETWORK_AP;
        s_setup_opened = millis();
        s_setup_deadline = s_setup_opened + QUOTA_PORTABLE_SETUP_MS;
        unlock();
        quota_portal_callbacks_t callbacks = {
            .submit = submit, .state_json = state_json, .session_active = session_active};
        ok = quota_portal_start(secret, &callbacks);
    }
    quota_portable_clear_secret(password, sizeof(password));
    quota_portable_clear_secret(secret, sizeof(secret));
    quota_portable_clear_secret(&ap, sizeof(ap));
    if (!ok)
        close_setup();
    changed();
    return ok;
}
/* A failed open retries after a backoff, then gives up; OK on the closed-window screen requests it
 * again. */
static void open_attempt(void)
{
    lock();
    s_open = false;
    unlock();
    abort_validation();
    bool ok = open_setup();
    lock();
    if (ok || ++s_open_failures >= OPEN_RETRY_MAX) {
        s_open_failures = 0;
        s_open_retry_at = 0;
    } else {
        s_open = true;
        s_open_retry_at = millis() + OPEN_RETRY_MS;
    }
    unlock();
}
static bool connected_ip(char ip[16])
{
    wifi_ap_record_t ap;
    esp_netif_ip_info_t info;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta || esp_wifi_sta_get_ap_info(&ap) != ESP_OK ||
        esp_netif_get_ip_info(sta, &info) != ESP_OK || !info.ip.addr)
        return false;
    snprintf(ip, 16, IPSTR, IP2STR(&info.ip));
    return true;
}
static bool connect_network(const quota_portable_network_t *network)
{
    if (!network->ssid[0] || !quota_wifi_start())
        return false;
    s_wifi_active = true;
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, network->ssid, strlen(network->ssid));
    memcpy(config.sta.password, network->password, strlen(network->password));
    config.sta.threshold.authmode = network->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    (void)esp_wifi_disconnect();
    bool ok = esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK &&
              esp_wifi_set_config(WIFI_IF_STA, &config) == ESP_OK && esp_wifi_connect() == ESP_OK;
    quota_portable_clear_secret(&config, sizeof(config));
    s_connect_at = millis();
    s_retry_at = millis() + NETWORK_TIMEOUT_MS;
    lock();
    s_view.network_state = ok ? QUOTA_PORTABLE_NETWORK_CONNECTING : QUOTA_PORTABLE_NETWORK_ERROR;
    copy(s_view.network_ssid, sizeof(s_view.network_ssid), network->ssid);
    s_view.network_ip[0] = 0;
    copy(s_view.network_error, sizeof(s_view.network_error), ok ? "" : "network_unavailable");
    s_disconnect_reason = 0;
    unlock();
    changed();
    return ok;
}
static const char *result_error(quota_direct_result_code_t code)
{
    switch (code) {
    case QUOTA_DIRECT_AUTH_REQUIRED:
        return "auth_required";
    case QUOTA_DIRECT_LOGIN_DISABLED:
        return "login_disabled";
    case QUOTA_DIRECT_EXPIRED:
        return "codex_expired";
    case QUOTA_DIRECT_RATE_LIMITED:
        return "rate_limited";
    case QUOTA_DIRECT_STORAGE_ERROR:
    case QUOTA_DIRECT_PERSIST_PENDING:
        return "storage_failed";
    case QUOTA_DIRECT_TIME_REQUIRED:
        return "time_required";
    case QUOTA_DIRECT_PROTOCOL_ERROR:
        return "provider_response_invalid";
    case QUOTA_DIRECT_NO_MEMORY:
        return "no_memory";
    case QUOTA_DIRECT_TLS_ERROR:
        return "tls_error";
    case QUOTA_DIRECT_RESOURCE_ERROR:
        return "resource_error";
    case QUOTA_DIRECT_RESPONSE_TOO_LARGE:
        return "response_too_large";
    case QUOTA_DIRECT_CANCELED:
        return "canceled";
    default:
        return "network_unavailable";
    }
}

static void sync_public_locked(void)
{
    quota_service_view_t *view = s_hooks.view;
    view->configured = s_model_ready;
    view->snapshot_valid = s_model_ready;
    view->connected = s_view.network_state == QUOTA_PORTABLE_NETWORK_READY ||
                      s_view.network_state == QUOTA_PORTABLE_NETWORK_CONNECTED;
    view->refreshing = !s_sleeping && s_refreshing;
    s_view.validating = s_validation.active;
    s_view.setup_opening = s_open && !s_view.setup_active;
    s_view.setup_result = s_setup_result;
    s_view.saving = s_dirty_model || s_operation.kind == OP_SAVE;
    s_view.factory_reset_failed = s_factory_reset_failed;
    view->request_failed = s_failed || s_storage_error[0];
    view->refresh_seconds = s_model.refresh_seconds;
    view->auto_refresh = s_model.auto_refresh;
    view->screen_timeout_seconds = s_model.screen_timeout_seconds;
    view->clock_synchronized = s_clock_ready;
    view->now_epoch = epoch();
    copy(s_view.storage_error, sizeof(s_view.storage_error), s_storage_error);
    s_view.saved_network_count = s_model.network_count <= QUOTA_PORTABLE_NETWORKS
                                     ? s_model.network_count
                                     : QUOTA_PORTABLE_NETWORKS;
    s_view.selected_saved_network = s_model.selected_network;
    memset(s_view.saved_network_ssids, 0, sizeof(s_view.saved_network_ssids));
    for (unsigned i = 0; i < s_view.saved_network_count; i++) {
        copy(s_view.saved_network_ssids[i], sizeof(s_view.saved_network_ssids[i]),
             s_model.networks[i].ssid);
    }
    unsigned pending = 0, failed = 0;
    memset(s_view.saved_network_validation, 0, sizeof(s_view.saved_network_validation));
    memset(s_view.saved_network_errors, 0, sizeof(s_view.saved_network_errors));
    for (unsigned i = 0; i < s_view.saved_network_count; i++) {
        s_view.saved_network_validation[i] = (uint8_t)s_network_validation[i];
        copy(s_view.saved_network_errors[i], sizeof(s_view.saved_network_errors[i]),
             s_network_error[i]);
        pending += s_network_validation[i] == QUOTA_VALIDATION_PENDING;
        failed += s_network_validation[i] == QUOTA_VALIDATION_FAILED;
    }
    for (unsigned row = 0; row < s_model.entry_count; row++) {
        pending += s_accounts[row].auth == QUOTA_PORTABLE_AUTH_PENDING;
        failed += s_accounts[row].auth == QUOTA_PORTABLE_AUTH_REAUTH ||
                  s_accounts[row].auth == QUOTA_PORTABLE_AUTH_ERROR;
    }
    if (s_login_queue.present) {
        pending += s_login_queue.error[0] == 0;
        failed += s_login_queue.error[0] != 0;
    }
    copy(s_view.firmware, sizeof(s_view.firmware), quota_portable_service_firmware());
    memset(s_view.account_validation, 0, sizeof(s_view.account_validation));
    for (unsigned i = 0; i < s_snapshot.account_count; i++) {
        int row = quota_catalog_find(&s_model, s_snapshot.accounts[i].id);
        if (row < 0)
            continue;
        s_view.account_validation[i] =
            s_accounts[row].auth == QUOTA_PORTABLE_AUTH_READY     ? QUOTA_VALIDATION_OK
            : s_accounts[row].auth == QUOTA_PORTABLE_AUTH_PENDING ? QUOTA_VALIDATION_PENDING
                                                                  : QUOTA_VALIDATION_FAILED;
    }
    s_view.pending_items = pending > UINT8_MAX ? UINT8_MAX : (uint8_t)pending;
    s_view.failed_items = failed > UINT8_MAX ? UINT8_MAX : (uint8_t)failed;
    memset(s_view.account_errors, 0, sizeof(s_view.account_errors));
    memset(s_view.account_retry_at, 0, sizeof(s_view.account_retry_at));
    for (unsigned i = 0; i < s_snapshot.account_count; i++) {
        int row = quota_catalog_find(&s_model, s_snapshot.accounts[i].id);
        if (row < 0)
            continue;
        copy(s_view.account_errors[i], sizeof(s_view.account_errors[i]), s_accounts[row].error);
        uint64_t retry = s_accounts[row].retry_ms;
        s_view.account_retry_at[i] =
            s_clock_ready && retry > millis() ? epoch() + (retry - millis() + 999) / 1000 : 0;
    }
}
/* The candidate network failed its validation. */
static void cancel_candidate(const char *error)
{
    if (!s_candidate_pending)
        return;
    lock();
    if (s_candidate_index < QUOTA_PORTABLE_NETWORKS) {
        s_network_validation[s_candidate_index] = QUOTA_VALIDATION_FAILED;
        copy(s_network_error[s_candidate_index], sizeof(s_network_error[0]), error);
    }
    unlock();
    s_candidate_pending = false;
    s_candidate_deadline = 0;
    quota_portable_clear_secret(&s_candidate, sizeof(s_candidate));
    s_connect_at = s_retry_at = 0;
    (void)esp_wifi_disconnect();
    lock();
    copy(s_view.network_ssid, sizeof(s_view.network_ssid),
         s_model.network_count ? s_model.networks[s_model.selected_network].ssid : "");
    unlock();
    changed();
}
/* New credentials wait for the network in use: what the stored ones do says nothing about them. */
static bool staged_for_selected(void)
{
    return s_staged.present && s_staged.index == s_model.selected_network;
}
static bool wifi_auth_failure(uint8_t reason)
{
    return reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT;
}
static const char *wifi_failure_code(void)
{
    lock();
    uint8_t reason = s_disconnect_reason;
    unlock();
    return wifi_auth_failure(reason) ? "wifi_auth_failed" : "wifi_not_found";
}
static void maintain_network(void)
{
    uint64_t now = millis();
    char ip[16];
    const quota_portable_network_t *target = s_candidate_pending ? &s_candidate
                                             : s_model.network_count
                                                 ? &s_model.networks[s_model.selected_network]
                                                 : NULL;
    if (!s_connect_at && target) {
        (void)connect_network(target);
        return;
    }
    wifi_ap_record_t association;
    bool matching =
        target && esp_wifi_sta_get_ap_info(&association) == ESP_OK &&
        strnlen((char *)association.ssid, sizeof(association.ssid)) == strlen(target->ssid) &&
        !memcmp(association.ssid, target->ssid, strlen(target->ssid));
    if (matching && connected_ip(ip)) {
        if (s_candidate_pending) {
            /* The validated network becomes the one in use, with the credentials that worked. */
            bool staged = s_staged.present && s_staged.index == s_candidate_index;
            /* Credentials staged for the network that stops being in use are stored now: only
             * the network in use is protected. */
            bool promote = s_staged.present && s_staged.index != s_candidate_index;
            if (staged || promote || s_model.selected_network != s_candidate_index) {
                quota_model_t *candidate = malloc(sizeof(*candidate));
                if (!candidate) {
                    storage_failed(QUOTA_STORE_READ_NO_MEMORY);
                    return;
                }
                *candidate = s_model;
                validation_t promoted_state =
                    promote ? s_network_validation[s_staged.index] : QUOTA_VALIDATION_SAVED;
                char promoted_error[QUOTA_PORTABLE_ERROR_BYTES + 1] = {0};
                if (promote)
                    memcpy(promoted_error, s_network_error[s_staged.index], sizeof(promoted_error));
                if (staged)
                    candidate->networks[s_candidate_index] = s_staged.network;
                if (promote && s_staged.index < candidate->network_count)
                    candidate->networks[s_staged.index] = s_staged.network;
                candidate->selected_network = s_candidate_index;
                bool ok = submit_model(candidate, true, NULL);
                dispose_candidate(candidate);
                if (!ok) {
                    if (!s_dirty_model)
                        cancel_candidate("storage_failed");
                    return;
                }
                if (staged || promote) {
                    lock();
                    if (promote) { /* storing it reset its state; it keeps what validation found */
                        s_network_validation[s_staged.index] = promoted_state;
                        memcpy(s_network_error[s_staged.index], promoted_error,
                               sizeof(promoted_error));
                    }
                    s_staged.present = false;
                    quota_portable_clear_secret(&s_staged.network, sizeof(s_staged.network));
                    unlock();
                }
            }
            lock();
            s_network_validation[s_candidate_index] = QUOTA_VALIDATION_OK;
            s_network_error[s_candidate_index][0] = 0;
            unlock();
            s_candidate_pending = false;
            s_candidate_deadline = 0;
            quota_portable_clear_secret(&s_candidate, sizeof(s_candidate));
            changed();
        }
        /* lwIP falls through the list on timeout. Server Date headers are not a clock source: TLS
         * certificate checks need time first. */
        if (!s_sntp) {
            esp_sntp_config_t ntp = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
                3, ESP_SNTP_SERVER_LIST("time.cloudflare.com", "ntp.aliyun.com", "pool.ntp.org"));
            ntp.start = true;
            s_sntp = esp_netif_sntp_init(&ntp) == ESP_OK;
        }
        if (s_sntp && esp_netif_sntp_sync_wait(0) == ESP_OK) {
            lock();
            s_clock_ready = epoch() >= 1704067200ULL;
            unlock();
        }
        lock();
        /* Connected for real: a network that was only saved, or had been refused, is fine. */
        if (s_model.network_count && !s_candidate_pending && !staged_for_selected() &&
            (s_network_validation[s_model.selected_network] == QUOTA_VALIDATION_SAVED ||
             s_network_validation[s_model.selected_network] == QUOTA_VALIDATION_FAILED)) {
            s_network_validation[s_model.selected_network] = QUOTA_VALIDATION_OK;
            s_network_error[s_model.selected_network][0] = 0;
        }
        s_view.network_state =
            s_clock_ready ? QUOTA_PORTABLE_NETWORK_READY : QUOTA_PORTABLE_NETWORK_CONNECTED;
        copy(s_view.network_ip, sizeof(s_view.network_ip), ip);
        copy(s_view.network_error, sizeof(s_view.network_error),
             s_clock_ready ? "" : "time_required");
        unlock();
        return;
    }
    stop_clock();
    /* A wrong password is known long before the timeout. */
    if (s_candidate_pending && s_connect_at && !strcmp(wifi_failure_code(), "wifi_auth_failed")) {
        cancel_candidate("wifi_auth_failed");
        return;
    }
    if (!s_candidate_pending && s_connect_at && s_model.network_count && !staged_for_selected() &&
        !strcmp(wifi_failure_code(), "wifi_auth_failed")) {
        /* The network in use refuses its stored password. */
        lock();
        validation_t *state = &s_network_validation[s_model.selected_network];
        if (*state == QUOTA_VALIDATION_SAVED || *state == QUOTA_VALIDATION_OK) {
            *state = QUOTA_VALIDATION_FAILED;
            copy(s_network_error[s_model.selected_network], sizeof(s_network_error[0]),
                 "wifi_auth_failed");
            sync_public_locked();
        }
        unlock();
    }
    if (s_connect_at && now < s_retry_at) {
        lock();
        s_view.network_state = QUOTA_PORTABLE_NETWORK_CONNECTING;
        s_view.network_ip[0] = 0;
        unlock();
        return;
    }
    if (s_candidate_pending && s_connect_at) {
        cancel_candidate(wifi_failure_code());
        return;
    }
    if (s_connect_at && now < s_retry_at + 30000) {
        lock();
        s_view.network_state = QUOTA_PORTABLE_NETWORK_ERROR;
        copy(s_view.network_error, sizeof(s_view.network_error), "network_unavailable");
        s_view.network_ip[0] = 0;
        unlock();
        return;
    }
    if (target)
        (void)connect_network(target);
    else {
        lock();
        s_view.network_state = QUOTA_PORTABLE_NETWORK_OFF;
        unlock();
    }
}
static void finish_operation(const char *error)
{
    if (quota_direct_has_pending_persist(s_direct) ||
        (s_operation.received && s_model.intent.kind != QUOTA_INTENT_NONE) || s_dirty_model)
        return;
    operation_kind_t kind = s_operation.kind == OP_SAVE ? s_operation.save_kind : s_operation.kind;
    if (s_model.intent.kind == QUOTA_INTENT_UPSERT_NATIVE) {
        quota_model_t *candidate = malloc(sizeof(*candidate));
        if (!candidate) {
            storage_failed(QUOTA_STORE_READ_NO_MEMORY);
            return;
        }
        *candidate = s_model;
        memset(&candidate->intent, 0, sizeof(candidate->intent));
        bool cleared = submit_model(candidate, false, NULL);
        dispose_candidate(candidate);
        if (!cleared) {
            s_operation.cancel_requested = true;
            return;
        }
    }
    if (kind == OP_LOGIN) {
        quota_direct_login_cancel(s_direct);
        lock();
        s_view.login_state = !error                            ? QUOTA_PORTABLE_LOGIN_SUCCESS
                             : !strcmp(error, "canceled")      ? QUOTA_PORTABLE_LOGIN_CANCELED
                             : !strcmp(error, "codex_expired") ? QUOTA_PORTABLE_LOGIN_EXPIRED
                                                               : QUOTA_PORTABLE_LOGIN_ERROR;
        copy(s_view.login_error, sizeof(s_view.login_error), error);
        s_view.auth_hold_awake = false;
        if (!error)
            s_login_queue.present = false;
        else /* the account stays queued, failed, for the next 完成设置 */
            copy(s_login_queue.error, sizeof(s_login_queue.error), error);
        unlock();
    }
    finish_job(s_operation.job_id, error);
    release_operation();
    lock();
    s_refreshing = false;
    if (!error && kind == OP_LOGIN)
        s_refresh = true;
    sync_public_locked();
    unlock();
    changed();
}
static void enter_save(void)
{
    if (s_operation.kind != OP_SAVE) {
        s_operation.save_kind = s_operation.kind;
        s_operation.kind = OP_SAVE;
        s_operation.save_attempt = 0;
        s_operation.retry_at = millis() + 1000;
    }
    if (s_operation.save_kind == OP_LOGIN) {
        lock();
        s_view.login_state = QUOTA_PORTABLE_LOGIN_EXCHANGING;
        copy(s_view.login_error, sizeof(s_view.login_error), "storage_failed");
        unlock();
    }
    changed();
}
static void save_tick(void)
{
    if (s_operation.kind != OP_SAVE || s_dirty_model || s_recovery_blocked ||
        millis() < s_operation.retry_at)
        return;
    bool success;
    if (quota_direct_has_pending_persist(s_direct))
        success = quota_direct_retry_persist(s_direct).code == QUOTA_DIRECT_OK;
    else
        success = s_operation.credential && persist(NULL, s_operation.credential);
    if (success) {
        finish_operation(NULL);
        return;
    }
    s_operation.save_attempt++;
    s_operation.retry_at = millis() + (s_operation.save_attempt == 1 ? 5000 : 30000);
}
static bool ensure_direct(void)
{
    if (!s_model_ready || s_recovery_blocked || (!s_direct && millis() < s_init_retry_at))
        return false;
    if (!s_direct) {
        quota_direct_hooks_t hooks = {
            .admit = admit, .account_current = account_current, .persist = persist};
        s_direct = quota_direct_create(&hooks, NULL, NULL);
    }
    if (!s_direct) {
        s_init_retry_at = millis() + 5000;
        copy(s_storage_error, sizeof(s_storage_error), "no_memory");
        return false;
    }
    return true;
}
/* Prepare the credential record and the catalog intent of a DeepSeek key (kind OP_KEY) or a
 * ChatGPT authorization (OP_LOGIN). new_id names a new ChatGPT account. */
static const char *start_account(const quota_portable_command_t *command, operation_kind_t kind,
                                 const char *new_id)
{
    if (storage_barrier() || s_operation.kind != OP_NONE || !ensure_direct())
        return "busy";
    quota_provider_t provider = kind == OP_LOGIN ? QUOTA_PROVIDER_CODEX : QUOTA_PROVIDER_DEEPSEEK;
    int row = command->account_id[0] ? quota_catalog_find(&s_model, command->account_id) : -1;
    if (command->account_id[0] && (row < 0 || s_model.entries[row].provider != provider))
        return "invalid_request";
    if (kind == OP_KEY && !command->api_key[0]) {
        if (row < 0)
            return "invalid_request";
        quota_model_t *candidate = malloc(sizeof(*candidate));
        if (!candidate)
            return "no_memory";
        *candidate = s_model;
        copy(candidate->entries[row].label, sizeof(candidate->entries[row].label), command->label);
        bool saved = submit_model(candidate, true, command->request_id);
        dispose_candidate(candidate);
        return saved || s_dirty_model ? NULL : "storage_failed";
    }
    /* A new ChatGPT account the user queued will take a row too. */
    unsigned rows = s_model.entry_count +
                    (kind == OP_KEY && s_login_queue.present && s_login_queue.is_new ? 1 : 0);
    if (row < 0 && rows >= QUOTA_MAX_ACCOUNTS)
        return "account_limit";
    if (row >= 0 && s_model.entries[row].row_generation == UINT32_MAX)
        return "generation_exhausted";
    quota_direct_credential_t *credential = quota_store_credential_acquire();
    if (!credential)
        return "no_memory";
    int slot = -1;
    quota_store_read_result_t previous = QUOTA_STORE_READ_MISSING;
    if (row >= 0) {
        slot = s_model.entries[row].binding.native.slot;
        previous = quota_store_load_credential_result((uint8_t)slot, credential);
        if (previous != QUOTA_STORE_READ_OK ||
            !quota_catalog_native_matches(&s_model.entries[row], credential)) {
            quota_store_credential_release(credential);
            if (previous != QUOTA_STORE_READ_OK) {
                storage_failed(previous);
                return read_error(previous);
            }
            return "recovery_conflict";
        }
    } else
        for (uint8_t i = 0; i < QUOTA_MAX_ACCOUNTS; i++) {
            bool occupied = false;
            for (unsigned j = 0; j < s_model.entry_count; j++)
                if (s_model.entries[j].binding.native.slot == i)
                    occupied = true;
            if (occupied)
                continue;
            quota_store_read_result_t result = quota_store_load_credential_result(i, credential);
            if (result != QUOTA_STORE_READ_OK && result != QUOTA_STORE_READ_MISSING) {
                quota_store_credential_release(credential);
                storage_failed(result);
                return read_error(result);
            }
            if (result == QUOTA_STORE_READ_MISSING ||
                (credential->tombstone && credential->generation < UINT32_MAX)) {
                slot = i;
                previous = result;
                break;
            }
        }
    if (slot < 0) {
        quota_store_credential_release(credential);
        return "native_credential_limit";
    }
    if (previous == QUOTA_STORE_READ_OK && credential->generation == UINT32_MAX) {
        quota_store_credential_release(credential);
        return "generation_exhausted";
    }
    quota_model_t *candidate = malloc(sizeof(*candidate));
    if (!candidate) {
        quota_store_credential_release(credential);
        return "no_memory";
    }
    *candidate = s_model;
    quota_model_intent_t *intent = &candidate->intent;
    memset(intent, 0, sizeof(*intent));
    intent->kind = QUOTA_INTENT_UPSERT_NATIVE;
    intent->provider = provider;
    intent->slot = (uint8_t)slot;
    intent->new_row = row < 0;
    intent->expected_row_generation = row < 0 ? 0 : s_model.entries[row].row_generation;
    if (row >= 0)
        copy(intent->logical_id, sizeof(intent->logical_id), s_model.entries[row].logical_id);
    else if (new_id)
        copy(intent->logical_id, sizeof(intent->logical_id), new_id);
    else
        random_text(intent->logical_id, QUOTA_ACCOUNT_ID_BYTES, "0123456789abcdef");
    intent->previous_missing = previous == QUOTA_STORE_READ_MISSING;
    if (!intent->previous_missing) {
        intent->previous_tombstone = credential->tombstone;
        copy(intent->previous_credential_id, sizeof(intent->previous_credential_id),
             credential->id);
        intent->previous_credential_generation = credential->generation;
    }
    intent->target_credential_generation =
        intent->previous_missing ? 1 : credential->generation + 1;
    if (row >= 0)
        copy(intent->target_credential_id, sizeof(intent->target_credential_id), credential->id);
    else
        random_text(intent->target_credential_id, QUOTA_ACCOUNT_ID_BYTES, "0123456789abcdef");
    copy(intent->desired_label, sizeof(intent->desired_label),
         command->label[0] ? command->label
         : row >= 0        ? s_model.entries[row].label
                           : "");
    copy(intent->request_id, sizeof(intent->request_id), command->request_id);
    if (intent->previous_missing || intent->previous_tombstone || row < 0)
        memset(credential, 0, sizeof(*credential));
    credential->slot = (uint8_t)slot;
    credential->provider = provider;
    credential->tombstone = false;
    credential->generation = intent->target_credential_generation;
    copy(credential->id, sizeof(credential->id), intent->target_credential_id);
    copy(credential->label, sizeof(credential->label), intent->desired_label);
    credential->auth_state = QUOTA_PORTABLE_AUTH_PENDING;
    credential->refresh_inflight = false;
    s_operation.kind = kind;
    s_operation.credential = credential;
    s_operation.created = row < 0;
    s_operation.row_generation = intent->expected_row_generation;
    copy(s_operation.logical_id, sizeof(s_operation.logical_id), intent->logical_id);
    copy(s_operation.job_id, sizeof(s_operation.job_id), command->request_id);
    bool prepared = submit_model(candidate, false, kind == OP_LOGIN ? command->request_id : NULL);
    dispose_candidate(candidate);
    if (!prepared && !s_dirty_model) {
        release_operation();
        return "storage_failed";
    }
    s_operation.deadline = millis() + (kind == OP_LOGIN ? QUOTA_PORTABLE_LOGIN_MS : 60000);
    if (kind == OP_LOGIN) {
        lock();
        s_view.login_state = QUOTA_PORTABLE_LOGIN_QUEUED;
        s_view.auth_hold_awake = true;
        s_login_deadline = s_operation.deadline;
        copy(s_view.login_account_id, sizeof(s_view.login_account_id), s_operation.logical_id);
        s_view.login_error[0] = 0;
        unlock();
    } else {
        copy(credential->api_key, sizeof(credential->api_key), command->api_key);
        if (!command->api_key[0]) {
            finish_operation("invalid_request");
            return "invalid_request";
        }
    }
    return NULL;
}
static const char *catalog_command(const quota_portable_command_t *command)
{
    quota_model_t *candidate = malloc(sizeof(*candidate));
    if (!candidate)
        return "no_memory";
    *candidate = s_model;
    const char *error = NULL;
    int row = quota_catalog_find(candidate, command->account_id);
    if (row < 0)
        error = "invalid_request";
    else {
        quota_catalog_entry_t entry = candidate->entries[row];
        if (entry.binding.native.credential_generation == UINT32_MAX)
            error = "generation_exhausted";
        else {
            quota_model_intent_t *intent = &candidate->intent;
            memset(intent, 0, sizeof(*intent));
            intent->kind = QUOTA_INTENT_DELETE_NATIVE;
            intent->provider = entry.provider;
            intent->slot = entry.binding.native.slot;
            copy(intent->logical_id, sizeof(intent->logical_id), entry.logical_id);
            intent->expected_row_generation = entry.row_generation;
            copy(intent->previous_credential_id, sizeof(intent->previous_credential_id),
                 entry.binding.native.credential_id);
            intent->previous_credential_generation = entry.binding.native.credential_generation;
            copy(intent->target_credential_id, sizeof(intent->target_credential_id),
                 entry.binding.native.credential_id);
            intent->target_credential_generation = entry.binding.native.credential_generation + 1;
            copy(intent->request_id, sizeof(intent->request_id), command->request_id);
        }
        if (!error) {
            memmove(&candidate->entries[row], &candidate->entries[row + 1],
                    (candidate->entry_count - (unsigned)row - 1) * sizeof(candidate->entries[0]));
            memset(&candidate->entries[--candidate->entry_count], 0, sizeof(candidate->entries[0]));
        }
    }
    int selected = quota_catalog_find(candidate, candidate->selected_account_id);
    if (selected < 0)
        candidate->selected_account_id[0] = 0;
    if (!error && !submit_model(candidate, true, command->request_id))
        error = s_dirty_model ? "storage_pending" : "storage_failed";
    dispose_candidate(candidate);
    return error;
}
static void cancel_operation(const char *target)
{
    if (s_operation.kind != OP_NONE && (!target[0] || !strcmp(target, s_operation.job_id)) &&
        s_operation.kind != OP_SAVE && !s_operation.received &&
        !quota_direct_has_pending_persist(s_direct)) {
        s_operation.cancel_requested = true;
        finish_operation("canceled");
    }
    lock();
    if (s_login_queue.present && s_operation.kind != OP_LOGIN &&
        (!target[0] || !strcmp(target, s_login_queue.job)))
        s_login_queue.present = false;
    if (s_queue && target[0])
        for (unsigned i = 0; i < s_count; i++)
            if (!strcmp(s_queue[(s_head + i) % QUEUE_DEPTH].request_id, target))
                s_queue[(s_head + i) % QUEUE_DEPTH].op = QUOTA_PORTABLE_OP_INVALID;
    unlock();
}
/* Queue a ChatGPT authorization: a new account, or the re-authorization of a row. It starts when
 * validation reaches it, so the credential record is not held while the user keeps editing. */
static const char *queue_login(const quota_portable_command_t *command)
{
    if (s_login_queue.present) {
        /* A failed authorization is armed again by asking for the same account. */
        bool same = command->account_id[0] ? !strcmp(command->account_id, s_login_queue.id)
                                           : s_login_queue.is_new;
        if (!s_login_queue.error[0] || !same || s_operation.kind == OP_LOGIN)
            return "login_pending";
        lock();
        s_login_queue.error[0] = 0;
        if (command->label[0])
            copy(s_login_queue.label, sizeof(s_login_queue.label), command->label);
        copy(s_login_queue.job, sizeof(s_login_queue.job), command->request_id);
        sync_public_locked();
        unlock();
        return NULL;
    }
    int row = command->account_id[0] ? quota_catalog_find(&s_model, command->account_id) : -1;
    if (command->account_id[0] &&
        (row < 0 || s_model.entries[row].provider != QUOTA_PROVIDER_CODEX))
        return "invalid_request";
    if (row < 0 && s_model.entry_count >= QUOTA_MAX_ACCOUNTS)
        return "account_limit";
    login_queue_t queued = {.present = true, .is_new = row < 0};
    if (row >= 0)
        copy(queued.id, sizeof(queued.id), s_model.entries[row].logical_id);
    else
        random_text(queued.id, QUOTA_ACCOUNT_ID_BYTES, "0123456789abcdef");
    copy(queued.label, sizeof(queued.label),
         command->label[0] ? command->label
         : row >= 0        ? s_model.entries[row].label
                           : "");
    copy(queued.job, sizeof(queued.job), command->request_id);
    lock();
    s_login_queue = queued;
    sync_public_locked();
    unlock();
    return NULL;
}
/* Start the queued authorization. The result lands on the validate job. */
static const char *launch_login(void)
{
    quota_portable_command_t command = {0};
    copy(command.request_id, sizeof(command.request_id), s_validation.job);
    if (!s_login_queue.is_new)
        copy(command.account_id, sizeof(command.account_id), s_login_queue.id);
    copy(command.label, sizeof(command.label), s_login_queue.label);
    const char *error =
        start_account(&command, OP_LOGIN, s_login_queue.is_new ? s_login_queue.id : NULL);
    if (error)
        return error;
    s_operation.deadline = s_login_deadline = millis() + QUOTA_PORTABLE_LOGIN_MS;
    lock();
    s_view.login_state = QUOTA_PORTABLE_LOGIN_CONNECTING;
    s_login_queue.error[0] = 0;
    unlock();
    return NULL;
}
static const char *save_key(const quota_portable_command_t *command)
{
    const char *error = start_account(command, OP_KEY, NULL);
    if (error || s_operation.kind != OP_KEY)
        return error;
    /* The key is stored as it is and waits for validation; no request goes to DeepSeek yet. */
    s_operation.credential->auth_state = QUOTA_PORTABLE_AUTH_PENDING;
    s_operation.received = true;
    if (persist(NULL, s_operation.credential))
        finish_operation(NULL);
    else
        enter_save();
    return NULL;
}
static const char *save_network(const quota_portable_command_t *command)
{
    unsigned index = command->network_index;
    if (index == UINT8_MAX) {
        index = s_model.network_count;
        unsigned matches = 0;
        for (unsigned i = 0; i < s_model.network_count; i++)
            if (!strcmp(s_model.networks[i].ssid, command->ssid)) {
                index = i;
                matches++;
            }
        if (matches > 1)
            return "network_ambiguous";
    }
    if (index >= QUOTA_PORTABLE_NETWORKS)
        return "network_limit";
    if (index > s_model.network_count)
        return "invalid_request";
    if (index < s_model.network_count && index == s_model.selected_network) {
        /* The network in use keeps its working credentials until the new ones validate. */
        lock();
        s_staged.present = true;
        s_staged.index = (uint8_t)index;
        copy(s_staged.network.ssid, sizeof(s_staged.network.ssid), command->ssid);
        copy(s_staged.network.password, sizeof(s_staged.network.password), command->password);
        s_network_validation[index] = QUOTA_VALIDATION_PENDING;
        s_network_error[index][0] = 0;
        sync_public_locked();
        unlock();
        return NULL;
    }
    quota_model_t *candidate = malloc(sizeof(*candidate));
    if (!candidate)
        return "no_memory";
    *candidate = s_model;
    copy(candidate->networks[index].ssid, sizeof(candidate->networks[index].ssid), command->ssid);
    copy(candidate->networks[index].password, sizeof(candidate->networks[index].password),
         command->password);
    if (index == candidate->network_count)
        candidate->network_count++;
    bool saved = submit_model(candidate, true, command->request_id);
    dispose_candidate(candidate);
    if ((saved || s_dirty_model) && s_staged.present && s_staged.index == index) {
        lock(); /* stored through the normal path: it replaces what was staged */
        s_staged.present = false;
        quota_portable_clear_secret(&s_staged.network, sizeof(s_staged.network));
        unlock();
    }
    return saved || s_dirty_model ? NULL : "storage_failed";
}
static const char *remove_network(const quota_portable_command_t *command)
{
    unsigned index = command->network_index;
    if (index >= s_model.network_count)
        return "invalid_request";
    quota_model_t *candidate = malloc(sizeof(*candidate));
    if (!candidate)
        return "no_memory";
    *candidate = s_model;
    memmove(&candidate->networks[index], &candidate->networks[index + 1],
            (candidate->network_count - index - 1) * sizeof(candidate->networks[0]));
    quota_portable_clear_secret(&candidate->networks[--candidate->network_count],
                                sizeof(candidate->networks[0]));
    /* The list shifts: the selection follows its network, or falls back to the first. */
    if (candidate->network_count == 0 || index == s_model.selected_network)
        candidate->selected_network = 0;
    else if (index < s_model.selected_network)
        candidate->selected_network--;
    bool in_use = index == s_model.selected_network;
    bool saved = submit_model(candidate, true, command->request_id);
    dispose_candidate(candidate);
    if (saved && s_staged.present) {
        lock();
        if (s_staged.index == index) {
            s_staged.present = false;
            quota_portable_clear_secret(&s_staged.network, sizeof(s_staged.network));
        } else if (s_staged.index > index)
            s_staged.index--;
        unlock();
    }
    if (saved && in_use) {
        (void)esp_wifi_disconnect();
        s_connect_at = 0;
    }
    return saved || s_dirty_model ? NULL : "storage_failed";
}
static void process_command(quota_portable_command_t *command)
{
    lock();
    bool current = command->accepted_config_generation == s_hooks.config_generation_locked();
    unlock();
    const char *error = NULL;
    bool pending = false;
    if (!current && command->op != QUOTA_PORTABLE_OP_SETUP_CLOSE &&
        command->op != QUOTA_PORTABLE_OP_OPERATION_CANCEL) {
        if (command->op == QUOTA_PORTABLE_OP_VALIDATE)
            validation_not_started();
        finish_job(command->request_id, "configuration_changed");
        return;
    }
    if (storage_barrier() && command->op != QUOTA_PORTABLE_OP_SETUP_CLOSE &&
        command->op != QUOTA_PORTABLE_OP_OPERATION_CANCEL) {
        if (command->op == QUOTA_PORTABLE_OP_VALIDATE)
            validation_not_started();
        finish_job(command->request_id, s_storage_error[0] ? s_storage_error : "busy");
        return;
    }
    /* Commands accepted before the pass began ran first; none changes anything while it runs. */
    if (s_validation.active && command->op != QUOTA_PORTABLE_OP_SETUP_CLOSE &&
        command->op != QUOTA_PORTABLE_OP_OPERATION_CANCEL) {
        finish_job(command->request_id, "busy");
        return;
    }
    clock_from_phone(command->phone_utc);
    switch (command->op) {
    case QUOTA_PORTABLE_OP_NETWORK_SAVE:
        error = save_network(command);
        pending = !error && s_dirty_model;
        break;
    case QUOTA_PORTABLE_OP_NETWORK_REMOVE:
        error = remove_network(command);
        pending = !error && s_dirty_model;
        break;
    case QUOTA_PORTABLE_OP_SETTINGS_SAVE: {
        if (!quota_refresh_seconds_is_valid(command->refresh_seconds) ||
            !quota_screen_timeout_is_valid(command->screen_timeout_seconds)) {
            error = "invalid_request";
            break;
        }
        quota_model_t *candidate = malloc(sizeof(*candidate));
        if (!candidate) {
            error = "no_memory";
            break;
        }
        *candidate = s_model;
        bool timer_changed = candidate->refresh_seconds != command->refresh_seconds ||
                             candidate->auto_refresh != command->auto_refresh;
        candidate->refresh_seconds = command->refresh_seconds;
        candidate->auto_refresh = command->auto_refresh;
        candidate->screen_timeout_seconds = command->screen_timeout_seconds;
        if (!submit_model(candidate, true, command->request_id)) {
            if (s_dirty_model)
                pending = true;
            else
                error = "storage_failed";
        } else if (timer_changed)
            s_next_refresh = millis() + (uint64_t)s_model.refresh_seconds * 1000;
        dispose_candidate(candidate);
        break;
    }
    case QUOTA_PORTABLE_OP_SETUP_CLOSE:
        if (command->accepted_transport == QUOTA_SETUP_USB) {
            s_usb_close_at = millis() + 500;
            s_usb_close_window = command->accepted_usb_window;
        } else {
            /* Answer first, close the access point, then validate: a Wi-Fi check moves the radio
             * to the station and the phone would lose the access point anyway. */
            s_close_at = millis() + 500;
            s_validate_after_close = true;
        }
        break;
    case QUOTA_PORTABLE_OP_RECONNECT:
        if (command->accepted_transport == QUOTA_SETUP_AP)
            s_close_at = millis() + 500;
        s_reconnect = true;
        break;
    case QUOTA_PORTABLE_OP_REFRESH:
        s_refresh = true;
        break;
    case QUOTA_PORTABLE_OP_ACCOUNT_REMOVE:
        if (s_login_queue.present && !strcmp(s_login_queue.id, command->account_id) &&
            s_login_queue.is_new) {
            lock();
            s_login_queue.present = false;
            sync_public_locked();
            unlock();
        } else if (s_operation.kind != OP_NONE)
            error = "busy";
        else {
            if (s_login_queue.present && !strcmp(s_login_queue.id, command->account_id)) {
                lock();
                s_login_queue.present = false;
                unlock();
            }
            error = catalog_command(command);
            if (error && !strcmp(error, "storage_pending")) {
                error = NULL;
                pending = true;
            }
        }
        break;
    case QUOTA_PORTABLE_OP_CODEX_QUEUE:
        error = queue_login(command);
        break;
    case QUOTA_PORTABLE_OP_DEEPSEEK_SAVE:
        error = save_key(command);
        pending = !error && (s_operation.kind != OP_NONE || s_dirty_model);
        break;
    case QUOTA_PORTABLE_OP_VALIDATE:
        copy(s_validation.job, sizeof(s_validation.job), command->request_id);
        if (quota_portable_validate_pending())
            pending = true; /* the job ends with the pass */
        else
            s_validation.job[0] = 0;
        break;
    case QUOTA_PORTABLE_OP_OPERATION_CANCEL:
        cancel_operation(command->target_request_id);
        break;
    default:
        error = "unsupported";
        break;
    }
    if (!pending)
        finish_job(command->request_id, error);
    changed();
}
static void apply_result(int row, const quota_direct_result_t *result)
{
    if (result->code == QUOTA_DIRECT_DEFERRED || result->code == QUOTA_DIRECT_WAITING)
        return;
    const quota_catalog_entry_t *entry = &s_model.entries[row];
    if (strcmp(entry->logical_id, s_operation.logical_id) ||
        entry->row_generation != s_operation.row_generation ||
        !quota_catalog_native_matches(entry, s_operation.credential))
        return;
    if (result->code == QUOTA_DIRECT_OK && result->source_valid &&
        result->source_epoch >= 1704067200ULL && result->source_epoch <= 4102444800ULL)
        clock_from_phone(result->source_epoch);
    lock();
    int index = snapshot_index(entry->logical_id);
    account_meta_t *meta = &s_accounts[row];
    if (result->code == QUOTA_DIRECT_OK && result->source_valid && index >= 0) {
        s_snapshot.accounts[index] = result->account;
        copy(s_snapshot.accounts[index].id, sizeof(s_snapshot.accounts[index].id),
             entry->logical_id);
        s_snapshot.balances[index] = result->balance;
        copy(s_snapshot.balances[index].label, sizeof(s_snapshot.balances[index].label),
             entry->label);
        s_snapshot.codex_extras[index] = result->extras;
        s_snapshot.server_time = result->source_epoch;
        s_snapshot.revision++;
        meta->auth = QUOTA_PORTABLE_AUTH_READY;
        meta->error[0] = 0;
        meta->retry_ms = 0;
        s_cache_dirty = true;
    } else {
        copy(meta->error, sizeof(meta->error),
             result->code == QUOTA_DIRECT_AUTH_REQUIRED &&
                     entry->provider == QUOTA_PROVIDER_DEEPSEEK
                 ? "deepseek_invalid_key"
                 : result_error(result->code));
        if (result->code == QUOTA_DIRECT_AUTH_REQUIRED)
            meta->auth = QUOTA_PORTABLE_AUTH_REAUTH;
        if (index >= 0)
            s_snapshot.accounts[index].status = result->code == QUOTA_DIRECT_AUTH_REQUIRED
                                                    ? QUOTA_STATUS_EXPIRED
                                                    : QUOTA_STATUS_ERROR;
        meta->retry_ms =
            millis() +
            (uint64_t)(result->retry_after_seconds ? result->retry_after_seconds : 30) * 1000;
        s_failed = true;
    }
    sync_public_locked();
    unlock();
    changed();
}
static void login_tick(void)
{
    if (s_operation.kind != OP_LOGIN || s_view.login_state == QUOTA_PORTABLE_LOGIN_QUEUED ||
        !quota_portable_service_http_allowed())
        return;
    quota_direct_result_t result;
    if (!s_operation.started) {
        lock();
        s_view.login_state = QUOTA_PORTABLE_LOGIN_REQUESTING_CODE;
        unlock();
        changed();
        result = quota_direct_login_begin(s_direct, s_operation.credential, millis(), epoch());
        s_operation.started = result.code == QUOTA_DIRECT_WAITING || result.code == QUOTA_DIRECT_OK;
    } else
        result = quota_direct_login_step(s_direct, s_operation.credential, millis(), epoch());
    if (result.code == QUOTA_DIRECT_PERSIST_PENDING) {
        enter_save();
        return;
    }
    if (result.code == QUOTA_DIRECT_DEFERRED)
        return;
    quota_direct_login_view_t view;
    quota_direct_login_view(s_direct, millis(), &view);
    bool transient = view.active && (result.code == QUOTA_DIRECT_RATE_LIMITED ||
                                     result.code == QUOTA_DIRECT_NETWORK_ERROR ||
                                     result.code == QUOTA_DIRECT_TIME_REQUIRED);
    if (result.code != QUOTA_DIRECT_WAITING && !transient) {
        finish_operation(result.code == QUOTA_DIRECT_OK ? NULL : result_error(result.code));
        return;
    }
    lock();
    s_view.login_state =
        view.exchanging ? QUOTA_PORTABLE_LOGIN_EXCHANGING : QUOTA_PORTABLE_LOGIN_WAITING;
    copy(s_view.login_url, sizeof(s_view.login_url), view.verification_url);
    copy(s_view.login_user_code, sizeof(s_view.login_user_code), view.user_code);
    unlock();
    changed();
}
/* Ask one row's provider. ChatGPT refreshes an expiring token first. While validating, a DeepSeek
 * key that answers is stored as ready, and any other answer leaves the row failed. */
static quota_direct_result_code_t query_row(int row, bool validating)
{
    const quota_catalog_entry_t *entry = &s_model.entries[row];
    quota_direct_credential_t *credential = quota_store_credential_acquire();
    if (!credential) {
        storage_failed(QUOTA_STORE_READ_NO_MEMORY);
        return QUOTA_DIRECT_NO_MEMORY;
    }
    quota_store_read_result_t loaded =
        quota_store_load_credential_result(entry->binding.native.slot, credential);
    if (loaded != QUOTA_STORE_READ_OK || !quota_catalog_native_matches(entry, credential)) {
        quota_store_credential_release(credential);
        if (loaded != QUOTA_STORE_READ_OK)
            storage_failed(loaded);
        else {
            s_recovery_blocked = true;
            copy(s_storage_error, sizeof(s_storage_error), "recovery_conflict");
        }
        return QUOTA_DIRECT_STORAGE_ERROR;
    }
    s_operation.kind = OP_QUERY;
    s_operation.credential = credential;
    s_operation.row_generation = entry->row_generation;
    copy(s_operation.logical_id, sizeof(s_operation.logical_id), entry->logical_id);
    lock();
    s_refreshing = true;
    sync_public_locked();
    unlock();
    changed();
    quota_direct_result_t result;
    if (credential->provider == QUOTA_PROVIDER_CODEX && credential->expires_at <= epoch() + 300) {
        result = quota_direct_refresh(s_direct, credential, epoch());
        if (result.code == QUOTA_DIRECT_OK)
            result = quota_direct_query(s_direct, credential, epoch());
    } else {
        result = quota_direct_query(s_direct, credential, epoch());
        if (result.code == QUOTA_DIRECT_AUTH_REQUIRED &&
            credential->provider == QUOTA_PROVIDER_CODEX && !credential->refresh_inflight) {
            result = quota_direct_refresh(s_direct, credential, epoch());
            if (result.code == QUOTA_DIRECT_OK)
                result = quota_direct_query(s_direct, credential, epoch());
        }
    }
    if (validating && result.code == QUOTA_DIRECT_OK &&
        credential->provider == QUOTA_PROVIDER_DEEPSEEK) {
        credential->auth_state = QUOTA_PORTABLE_AUTH_READY;
        if (!persist(NULL, credential))
            result.code = QUOTA_DIRECT_STORAGE_ERROR;
    }
    lock();
    s_refreshing = false;
    sync_public_locked();
    unlock();
    if (!validating && result.code != QUOTA_DIRECT_DEFERRED)
        s_refresh_slot++;
    apply_result(row, &result);
    if (validating && result.code != QUOTA_DIRECT_OK && result.code != QUOTA_DIRECT_DEFERRED) {
        lock();
        s_accounts[row].auth = QUOTA_PORTABLE_AUTH_ERROR;
        sync_public_locked();
        unlock();
    }
    quota_direct_result_code_t code = result.code;
    if (code == QUOTA_DIRECT_PERSIST_PENDING)
        enter_save();
    else
        release_operation();
    return code;
}
static void source_tick(void)
{
    if (!quota_portable_service_http_allowed() || !ensure_direct() || s_operation.kind != OP_NONE)
        return;
    /* An open USB session owns the serial link: scheduled polling waits. An explicit refresh still
     * runs. */
    bool scheduled = !usb_active();
    if (!s_cycle &&
        (s_refresh || (scheduled && s_model.auto_refresh && millis() >= s_next_refresh))) {
        s_cycle = true;
        s_refresh = false;
        s_refresh_slot = 0;
        s_failed = false;
    }
    if (!s_cycle)
        return;
    /* A key that waits for validation, or failed it, is not asked in the background. */
    while (s_refresh_slot < s_model.entry_count &&
           (s_accounts[s_refresh_slot].retry_ms > millis() ||
            s_accounts[s_refresh_slot].auth != QUOTA_PORTABLE_AUTH_READY))
        s_refresh_slot++;
    if (s_refresh_slot >= s_model.entry_count) {
        s_cycle = false;
        s_next_refresh = millis() + (uint64_t)s_model.refresh_seconds * 1000;
        return;
    }
    (void)query_row(s_refresh_slot, false);
}
/* Validation: Wi-Fi networks first, then DeepSeek keys, then the queued ChatGPT authorization.
 * One pass at a time, driven by the network task; nothing here blocks. */
static bool network_needs_validation(unsigned index)
{
    return s_network_validation[index] == QUOTA_VALIDATION_PENDING ||
           s_network_validation[index] == QUOTA_VALIDATION_FAILED;
}
static int next_network_to_validate(void)
{
    for (unsigned i = 0; i < s_model.network_count; i++)
        if (network_needs_validation(i) && !(s_validation.tried_networks & (1u << i)))
            return (int)i;
    return -1;
}
static int next_key_to_validate(void)
{
    for (unsigned row = 0; row < s_model.entry_count; row++)
        if (s_model.entries[row].provider == QUOTA_PROVIDER_DEEPSEEK &&
            s_accounts[row].auth != QUOTA_PORTABLE_AUTH_READY &&
            !(s_validation.tried_rows & (1u << row)))
            return (int)row;
    return -1;
}
static void validation_finish(const char *error)
{
    lock();
    s_validation.active = s_validation.requested = false;
    sync_public_locked();
    unlock();
    finish_job(s_validation.job, error);
    s_validation.job[0] = 0;
    changed();
}
/* A physical hotspot request ends the pass; what it had not reached stays pending, including a
 * ChatGPT authorization that has not started asking yet. */
static void abort_validation(void)
{
    if (!s_validation.active)
        return;
    if (s_candidate_pending) {
        s_candidate_pending = false;
        s_candidate_deadline = 0;
        quota_portable_clear_secret(&s_candidate, sizeof(s_candidate));
        s_connect_at = s_retry_at = 0;
    }
    validation_finish("canceled");
    if (s_operation.kind == OP_LOGIN && !s_operation.started) {
        finish_operation("canceled");
        lock();
        s_login_queue.error[0] = 0; /* not the user's doing: it simply waits for the next pass */
        unlock();
    }
}
/* Why the network is not there: no link, or no time yet. */
static const char *network_failure_code(void)
{
    return s_view.network_state == QUOTA_PORTABLE_NETWORK_CONNECTED ? "time_required"
                                                                    : "network_unavailable";
}
/* No network came up in time: the keys that were waiting for it fail. */
static void fail_remaining_keys(void)
{
    const char *code = network_failure_code();
    for (int row; (row = next_key_to_validate()) >= 0;) {
        s_validation.tried_rows |= 1u << row;
        lock();
        s_accounts[row].auth = QUOTA_PORTABLE_AUTH_ERROR;
        copy(s_accounts[row].error, sizeof(s_accounts[row].error), code);
        sync_public_locked();
        unlock();
    }
}
static void validate_tick(void)
{
    if (!s_validation.active)
        return;
    if (s_validation.phase == PHASE_WIFI) {
        if (s_candidate_pending)
            return; /* maintain_network settles it */
        int index = next_network_to_validate();
        if (index >= 0) {
            s_candidate = s_staged.present && s_staged.index == index ? s_staged.network
                                                                      : s_model.networks[index];
            s_candidate_index = (uint8_t)index;
            s_candidate_pending = true;
            s_candidate_deadline = millis() + NETWORK_TIMEOUT_MS;
            s_connect_at = 0;
            s_validation.tried_networks |= 1u << index;
            changed();
            return;
        }
        lock();
        s_validation.phase = PHASE_KEYS;
        unlock();
        s_validation.wait_deadline = millis() + KEYS_WAIT_MS;
    }
    if (s_validation.phase == PHASE_KEYS) {
        int row = next_key_to_validate();
        if (row >= 0) {
            if (s_operation.kind != OP_NONE)
                return;
            if (!quota_portable_service_http_allowed()) { /* wait for the network and the clock */
                if (millis() >= s_validation.wait_deadline) {
                    fail_remaining_keys();
                    changed();
                }
                return;
            }
            if (query_row(row, true) != QUOTA_DIRECT_DEFERRED)
                s_validation.tried_rows |= 1u << row;
            changed();
            return;
        }
        lock();
        s_validation.phase = PHASE_LOGIN;
        unlock();
        s_validation.wait_deadline = millis() + KEYS_WAIT_MS;
    }
    if (!s_validation.login_started) {
        if (!s_login_queue.present) {
            validation_finish(NULL);
            return;
        }
        if (s_operation.kind != OP_NONE)
            return;
        if (!quota_portable_service_http_allowed()) {
            /* Authorization needs the network and the clock; wait a little, then give up without
             * dropping the queued account. */
            if (millis() >= s_validation.wait_deadline) {
                const char *code = network_failure_code();
                lock();
                copy(s_login_queue.error, sizeof(s_login_queue.error), code);
                unlock();
                validation_finish(code);
            }
            return;
        }
        const char *error = launch_login();
        if (error && !strcmp(error, "busy") && millis() < s_validation.wait_deadline)
            return; /* the direct client is not ready yet; try again shortly */
        if (error) {
            lock();
            copy(s_view.login_error, sizeof(s_view.login_error), error);
            s_view.login_state = QUOTA_PORTABLE_LOGIN_ERROR;
            copy(s_login_queue.error, sizeof(s_login_queue.error), error);
            unlock();
            validation_finish(error);
            return;
        }
        s_validation.login_started = true;
        return;
    }
    if (s_operation.kind == OP_NONE) /* the authorization ended, whatever its result */
        validation_finish(NULL);
}
bool quota_portable_validate_pending(void)
{
    s_validate_runs++;
    if (s_validation.active)
        return true;
    bool work = s_login_queue.present;
    lock();
    for (unsigned i = 0; i < s_model.network_count; i++)
        work = work || network_needs_validation(i);
    for (unsigned row = 0; row < s_model.entry_count; row++)
        work = work || (s_model.entries[row].provider == QUOTA_PROVIDER_DEEPSEEK &&
                        s_accounts[row].auth != QUOTA_PORTABLE_AUTH_READY);
    s_validation.requested = false;
    if (work) {
        s_validation.active = true;
        s_validation.login_started = false;
        s_validation.phase = PHASE_WIFI;
        s_validation.tried_networks = s_validation.tried_rows = 0;
        sync_public_locked();
    }
    unlock();
    if (work) {
        changed();
        wake();
    }
    return work;
}
static void apply_authentication_overlay(quota_snapshot_t *snapshot)
{
    for (unsigned i = 0; i < snapshot->account_count; i++) {
        int row = quota_catalog_find(&s_model, snapshot->accounts[i].id);
        if (row >= 0 && s_accounts[row].auth == QUOTA_PORTABLE_AUTH_REAUTH) {
            snapshot->accounts[i].status = QUOTA_STATUS_EXPIRED;
            copy(s_accounts[row].error, sizeof(s_accounts[row].error), "auth_required");
        }
    }
}
static void load_authority(void)
{
    if (s_dirty_model || s_recovery_blocked || s_operation.kind != OP_NONE ||
        millis() < s_init_retry_at)
        return;
    quota_model_t *loaded = malloc(sizeof(*loaded));
    if (!loaded) {
        storage_failed(QUOTA_STORE_READ_NO_MEMORY);
        s_init_retry_at = millis() + 5000;
        return;
    }
    uint64_t sequence = 0;
    quota_store_read_result_t result = quota_store_load_model_result(loaded, &sequence);
    if (result == QUOTA_STORE_READ_OK) {
        lock();
        s_model = *loaded;
        s_sequence = sequence;
        s_model_ready = true;
        unlock();
        if (s_model.intent.kind != QUOTA_INTENT_NONE)
            recover_intent();
        if (s_model.intent.kind == QUOTA_INTENT_NONE && !s_dirty_model) {
            if (publish_model(NULL)) {
                s_storage_error[0] = 0;
                s_authority_retry = false;
                quota_snapshot_t *cached = malloc(sizeof(*cached));
                if (cached) {
                    lock();
                    *cached = s_snapshot;
                    unlock();
                    quota_store_read_result_t observations =
                        quota_store_load_observations(&s_model, s_model.last_known_time, cached);
                    lock();
                    apply_authentication_overlay(cached);
                    s_snapshot = *cached;
                    sync_public_locked();
                    unlock();
                    free(cached);
                    if (observations == QUOTA_STORE_READ_IO_ERROR ||
                        observations == QUOTA_STORE_READ_NO_MEMORY ||
                        observations == QUOTA_STORE_READ_BUSY)
                        storage_failed(observations);
                } else
                    storage_failed(QUOTA_STORE_READ_NO_MEMORY);
            } else
                s_model_ready = false;
        }
    } else if (result == QUOTA_STORE_READ_MISSING) {
        /* No catalog: a new device starts empty, without accounts or Wi-Fi, and stale credentials
         * are freed. */
        quota_store_read_result_t orphans = quota_store_release_orphan_credentials();
        quota_model_t *initial = orphans == QUOTA_STORE_READ_OK ? malloc(sizeof(*initial)) : NULL;
        if (orphans != QUOTA_STORE_READ_OK) {
            storage_failed(orphans);
            s_model_ready = false;
        } else if (!initial) {
            storage_failed(QUOTA_STORE_READ_NO_MEMORY);
            s_model_ready = false;
        } else {
            model_defaults(initial);
            if (!submit_model(initial, true, NULL) && !s_dirty_model)
                s_model_ready = false;
            dispose_candidate(initial);
        }
    } else {
        storage_failed(result);
        s_model_ready = false;
    }
    quota_portable_clear_secret(loaded, sizeof(*loaded));
    free(loaded);
    s_init_retry_at = s_model_ready && !s_authority_retry ? 0 : millis() + 5000;
}
static void cache_tick(void)
{
    if (!s_cache_dirty || millis() < s_cache_at || !s_clock_ready || storage_barrier())
        return;
    bool ok = quota_store_save_observations(&s_model, &s_snapshot, epoch());
    s_cache_at = millis() + (ok ? CACHE_INTERVAL_MS : 30000);
    if (ok) {
        s_cache_dirty = false;
        quota_model_t *candidate = malloc(sizeof(*candidate));
        if (candidate) {
            *candidate = s_model;
            candidate->last_known_time = epoch();
            (void)submit_model(candidate, false, NULL);
            dispose_candidate(candidate);
        }
    }
}
bool quota_portable_service_init(const quota_portable_service_hooks_t *hooks)
{
    if (s_initialized)
        return true;
    if (!hooks || !hooks->view || !hooks->lock || !hooks->unlock || !hooks->try_lock ||
        !hooks->config_generation_locked || !hooks->config_changed_locked ||
        !hooks->display_current)
        return false;
    s_hooks = *hooks;
    s_store_ready = quota_store_init();
    model_defaults(&s_model);
    s_initialized = true;
    if (s_store_ready)
        load_authority();
    else
        storage_failed(QUOTA_STORE_READ_IO_ERROR);
    if (s_model.last_known_time >= 1704067200ULL) {
        struct timeval time = {.tv_sec = (time_t)s_model.last_known_time};
        settimeofday(&time, NULL);
    }
    (void)ensure_direct();
    s_next_refresh = millis() + 1000;
    lock();
    sync_public_locked();
    unlock();
    return true;
}
bool quota_portable_service_prepare_usb(void)
{
    /* Entry waits for storage-only recovery, never cancels a received grant or
     * a started login. The device remains on STA after closing an AP session. */
    lock();
    s_open = false;
    unlock();
    if (storage_barrier() || quota_direct_has_pending_persist(s_direct))
        return false;
    if (s_view.setup_active)
        close_setup();
    return !s_view.setup_active;
}
/* Between ticks nothing else touches storage, so this is the one place an erase is safe. Setup
 * and USB end first; whatever is still queued dies with the restart. Returns true when the device
 * is restarting (a host test build returns), false when the erase failed and was reported. */
static bool factory_reset(void)
{
    quota_usb_close_window();
    close_setup();
    quota_factory_reset_result_t result = quota_store_factory_reset();
    if (result != QUOTA_FACTORY_RESET_FAILED) {
        /* A catalog that is gone makes this a new device even if the rest could not be erased:
         * restart before anything saves the catalog in memory back. */
        esp_restart();
        return true;
    }
    lock();
    s_factory_reset_failed = true;
    sync_public_locked();
    unlock();
    return false;
}
void quota_portable_service_tick(bool sleeping, uint32_t generation)
{
    if (!s_initialized)
        return;
    lock();
    s_sleeping = sleeping;
    s_display_generation = generation;
    bool open = s_open, open_due = s_open && millis() >= s_open_retry_at, close = s_close,
         cancel = s_cancel, reconnect = s_reconnect, reset = s_factory_reset;
    s_close = s_cancel = s_reconnect = s_factory_reset = false;
    unlock();
    if (reset && factory_reset())
        return;
    retry_dirty();
    save_tick();
    if ((!s_model_ready || s_authority_retry) && !s_dirty_model && s_operation.kind == OP_NONE) {
        if (!s_store_ready)
            s_store_ready = quota_store_init();
        if (s_store_ready)
            load_authority();
    }
    recover_intent();
    if (s_operation.cancel_requested && !s_dirty_model)
        finish_operation("canceled");
    if (s_candidate_pending && !s_dirty_model && millis() >= s_candidate_deadline)
        cancel_candidate(wifi_failure_code());
    if (s_operation.kind != OP_NONE && s_operation.kind != OP_SAVE && !s_operation.received &&
        s_operation.deadline && millis() >= s_operation.deadline)
        finish_operation(s_operation.kind == OP_LOGIN ? "codex_expired" : "operation_expired");
    if (cancel)
        cancel_operation("");
    if (close || (s_view.setup_active && millis() >= s_setup_deadline) ||
        (s_close_at && millis() >= s_close_at) || (sleeping && s_view.setup_active)) {
        bool validate = s_validate_after_close && !sleeping;
        close_setup();
        s_close_at = 0;
        s_validate_after_close = false;
        if (validate) {
            lock();
            s_setup_result = true; /* this session's 完成设置: its result goes on the screen */
            unlock();
            (void)quota_portable_validate_pending();
        }
    }
    if (s_usb_close_at && millis() >= s_usb_close_at) {
        s_usb_close_at = 0;
        if (quota_usb_window_id() == s_usb_close_window)
            quota_usb_close_window();
        s_usb_close_window = 0;
    }
    if (usb_blocked()) {
        lock();
        sync_public_locked();
        unlock();
        return;
    }
    if (open && s_view.setup_active) {
        lock();
        s_open = false;
        unlock();
    } /* Already open: the existing session is what the user sees. */
    else if (!sleeping && open_due && !storage_barrier()) {
        if (s_operation.kind == OP_LOGIN && s_operation.started && !s_operation.received)
            finish_operation("canceled");
        if (!storage_barrier() && s_operation.kind != OP_LOGIN)
            open_attempt();
        else if (!s_operation.started)
            open_attempt();
    }
    quota_portable_command_t command;
    bool have = false;
    lock();
    if (s_queue && s_count) {
        command = s_queue[s_head];
        quota_portable_clear_secret(&s_queue[s_head], sizeof(s_queue[0]));
        s_head = (s_head + 1) % QUEUE_DEPTH;
        s_count--;
        have = true;
        for (unsigned i = 0; i < s_job_count; i++)
            if (!strcmp(s_jobs[i].id, command.request_id))
                s_jobs[i].state = 1;
    }
    unlock();
    if (have) {
        process_command(&command);
        quota_portable_clear_secret(&command, sizeof(command));
    }
    lock();
    bool free_queue = !s_view.setup_active && !s_count;
    quota_portable_command_t *finished_queue = free_queue ? s_queue : NULL;
    if (free_queue) {
        s_queue = NULL;
        s_head = 0;
    }
    unlock();
    if (finished_queue) {
        quota_portable_clear_secret(finished_queue, QUEUE_DEPTH * sizeof(*finished_queue));
        free(finished_queue);
    }
    if (s_local_settings_pending && !storage_barrier()) {
        quota_portable_command_t local = {
            .op = QUOTA_PORTABLE_OP_SETTINGS_SAVE,
            .refresh_seconds = s_local_settings.refresh_seconds,
            .auto_refresh = s_local_settings.auto_refresh,
            .screen_timeout_seconds = s_local_settings.screen_timeout_seconds,
            .accepted_config_generation = s_local_settings_generation};
        s_local_settings_pending = false;
        process_command(&local);
    }
    if (sleeping) {
        stop_clock();
        if (s_wifi_active && quota_wifi_stop())
            s_wifi_active = false;
        s_connect_at = 0;
        lock();
        s_view.network_state = QUOTA_PORTABLE_NETWORK_OFF;
        s_refreshing = false;
        sync_public_locked();
        unlock();
        return;
    }
    if (s_view.setup_active || storage_barrier()) {
        lock();
        sync_public_locked();
        unlock();
        return;
    }
    if (reconnect) {
        (void)esp_wifi_disconnect();
        s_connect_at = 0;
    }
    maintain_network();
    if (!storage_barrier()) {
        (void)ensure_direct();
        validate_tick();
        if (s_operation.kind == OP_LOGIN)
            login_tick();
        else if (!s_validation.active)
            source_tick();
        if (s_selection_dirty && s_operation.kind == OP_NONE) {
            quota_model_t *candidate = malloc(sizeof(*candidate));
            if (candidate) {
                *candidate = s_model;
                int row = quota_catalog_find(candidate, s_selected_pending);
                if (row >= 0 && candidate->entries[row].activity == QUOTA_ACCOUNT_ACTIVE) {
                    copy(candidate->selected_account_id, sizeof(candidate->selected_account_id),
                         s_selected_pending);
                    if (submit_model(candidate, true, NULL))
                        s_selection_dirty = false;
                } else
                    s_selection_dirty = false;
                dispose_candidate(candidate);
            }
        }
        cache_tick();
    }
    lock();
    s_view.auth_hold_awake =
        (s_operation.kind == OP_LOGIN && millis() < s_operation.deadline) || s_validation.active;
    sync_public_locked();
    unlock();
}
void quota_portable_service_countdown_overlay_locked(quota_service_view_t *view)
{
    if (!s_initialized || !view)
        return;
    uint64_t now = millis();
    view->portable.setup_seconds_left =
        now < s_setup_deadline ? (uint32_t)((s_setup_deadline - now + 999) / 1000) : 0;
    view->portable.login_seconds_left = view->portable.auth_hold_awake && now < s_login_deadline
                                            ? (uint32_t)((s_login_deadline - now + 999) / 1000)
                                            : 0;
    view->portable.auth_hold_awake =
        view->portable.auth_hold_awake && (view->portable.validating || now < s_login_deadline);
}
static void sooner(uint64_t *deadline, uint64_t candidate)
{
    if (candidate && candidate < *deadline)
        *deadline = candidate;
}
/* Under the lock: the latched open request exactly as tick can act on it, so a request it cannot
 * consume never zeroes the wait. */
static bool open_actionable(void)
{
    return s_open && !s_view.setup_active &&
           !(s_operation.kind == OP_LOGIN && s_operation.started && s_operation.received);
}
uint64_t quota_portable_service_next_deadline_ms(bool sleeping)
{
    if (!s_initialized)
        return UINT64_MAX;
    lock();
    uint64_t deadline = UINT64_MAX;
    bool usb_busy = usb_blocked();
    if (s_close || s_cancel ||
        (!usb_busy && !storage_barrier() &&
         ((!sleeping && ((open_actionable() && millis() >= s_open_retry_at) || s_reconnect)) ||
          s_local_settings_pending || s_count)))
        deadline = 0;
    if (!sleeping && !usb_busy && !storage_barrier() && open_actionable())
        sooner(&deadline, s_open_retry_at);
    if (s_view.setup_active)
        sooner(&deadline, s_setup_deadline);
    sooner(&deadline, s_close_at);
    sooner(&deadline, s_usb_close_at);
    if (s_dirty_model && !s_recovery_blocked)
        sooner(&deadline, s_storage_retry_at);
    if ((!s_model_ready || s_authority_retry) && !s_recovery_blocked)
        sooner(&deadline, s_init_retry_at);
    if (s_model.intent.kind != QUOTA_INTENT_NONE && s_operation.kind == OP_NONE &&
        !s_recovery_blocked)
        sooner(&deadline, s_storage_retry_at ? s_storage_retry_at : millis() + 1000);
    if (!s_recovery_blocked && !s_dirty_model) {
        if (s_operation.kind == OP_SAVE)
            sooner(&deadline, s_operation.retry_at);
        else if (s_operation.kind != OP_NONE)
            sooner(&deadline, s_operation.deadline);
    }
    if (s_candidate_pending && !s_dirty_model && !s_recovery_blocked)
        sooner(&deadline, s_candidate_deadline);
    if (!sleeping && !usb_busy && !s_view.setup_active && !storage_barrier()) {
        if (!s_direct)
            sooner(&deadline, s_init_retry_at);
        if (s_model.auto_refresh && s_operation.kind == OP_NONE)
            sooner(&deadline, s_next_refresh > millis() ? s_next_refresh : millis() + 500);
        if (s_refresh || s_cycle || s_operation.kind == OP_LOGIN)
            sooner(&deadline, millis() + 500);
        if (s_validation.active)
            sooner(&deadline, millis() + 200);
        if (s_cache_dirty && s_clock_ready)
            sooner(&deadline, s_cache_at <= millis() ? millis() + 500 : s_cache_at);
    }
    unlock();
    return deadline;
}
void quota_portable_service_disconnected(uint8_t reason)
{
    if (!s_initialized)
        return;
    lock();
    s_disconnect_reason = reason;
    unlock();
}
void quota_portable_service_open(void)
{
    if (!s_initialized)
        return;
    lock();
    if (!s_view.setup_active) {
        s_setup_result = false;
        s_open = true;
        s_open_failures = 0;
        s_open_retry_at = 0;
    }
    unlock();
    wake();
}
void quota_portable_service_close(void)
{
    if (!s_initialized)
        return;
    lock();
    s_close = true;
    unlock();
    wake();
}
void quota_portable_service_renew(void)
{
    if (!s_initialized)
        return;
    lock();
    if (!s_view.setup_active) {
        s_setup_result = false;
        s_open = true;
        s_open_failures = 0;
        s_open_retry_at = 0;
    }
    unlock();
    wake();
}
void quota_portable_service_cancel_auth(void)
{
    if (!s_initialized)
        return;
    lock();
    s_cancel = true;
    unlock();
    wake();
}
void quota_portable_service_factory_reset(void)
{
    if (!s_initialized)
        return;
    lock();
    s_factory_reset = true;
    s_factory_reset_failed = false;
    sync_public_locked();
    unlock();
    wake();
}
void quota_portable_service_refresh(void)
{
    if (!s_initialized)
        return;
    lock();
    s_refresh = true;
    unlock();
    wake();
}
void quota_portable_service_reconnect(void)
{
    if (!s_initialized)
        return;
    lock();
    s_reconnect = true;
    unlock();
    wake();
}
void quota_portable_service_settings(uint16_t interval, bool automatic, uint16_t timeout)
{
    if (!s_initialized || !quota_refresh_seconds_is_valid(interval) ||
        !quota_screen_timeout_is_valid(timeout))
        return;
    lock();
    s_local_settings.refresh_seconds = interval;
    s_local_settings.auto_refresh = automatic;
    s_local_settings.screen_timeout_seconds = timeout;
    s_local_settings_generation = s_hooks.config_generation_locked();
    s_local_settings_pending = true;
    unlock();
    wake();
}
void quota_portable_service_select(const char *id)
{
    if (!s_initialized || !quota_id_is_valid(id))
        return;
    lock();
    copy(s_selected_pending, sizeof(s_selected_pending), id);
    s_selection_dirty = true;
    unlock();
    wake();
}
bool quota_portable_service_selected(char id[QUOTA_ACCOUNT_ID_BYTES + 1])
{
    if (!s_initialized)
        return false;
    lock();
    copy(id, QUOTA_ACCOUNT_ID_BYTES + 1,
         s_selection_dirty ? s_selected_pending : s_model.selected_account_id);
    unlock();
    return true;
}
