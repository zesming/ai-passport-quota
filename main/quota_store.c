#include "quota_store.h"

#include "nvs.h"
#include "nvs_flash.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define STORE_PARTITION "portable"
#define STORE_NAMESPACE "quota_port"
/* Retired formats: namespace of the default partition and records beside the model. */
#define RETIRED_NAMESPACE "ai_quota"
#define STORE_VERSION 1U
#define CREDENTIAL_MAGIC 0x41515041U
#define CACHE_MAX_AGE (30ULL * 24 * 60 * 60)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t bytes;
    quota_portable_credential_t value;
    uint32_t crc;
} credential_record_t;

_Static_assert(sizeof(credential_record_t) <= 16384, "credential record exceeds bound");

static bool s_ready;
static credential_record_t *s_credential_record;

static uint32_t store_crc(const void *data, size_t length)
{
    const unsigned char *bytes = data;
    uint32_t crc = UINT32_MAX;
    while (length-- != 0) {
        crc ^= *bytes++;
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

static bool text_valid(const char *text, size_t capacity, bool empty, bool ascii)
{
    const char *end = memchr(text, 0, capacity);
    if (end == NULL || (!empty && end == text))
        return false;
    size_t length = (size_t)(end - text);
    if (!quota_utf8_is_valid(text, length))
        return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 0x20 || ch == 0x7f || (ascii && ch > 0x7e))
            return false;
    }
    return true;
}

static bool credential_valid(const quota_portable_credential_t *credential, uint8_t slot)
{
    if (credential == NULL || slot >= QUOTA_MAX_ACCOUNTS || credential->slot != slot ||
        !text_valid(credential->id, sizeof(credential->id), false, true) ||
        !quota_id_is_valid(credential->id) || credential->generation == 0 ||
        (credential->provider != QUOTA_PROVIDER_CODEX &&
         credential->provider != QUOTA_PROVIDER_DEEPSEEK) ||
        (unsigned)credential->auth_state > QUOTA_PORTABLE_AUTH_ERROR)
        return false;
    if (credential->tombstone) {
        return credential->access_token[0] == 0 && credential->refresh_token[0] == 0 &&
               credential->api_key[0] == 0 && !credential->refresh_inflight;
    }
    if (credential->refresh_inflight && credential->provider != QUOTA_PROVIDER_CODEX)
        return false;
    if (credential->auth_state == QUOTA_PORTABLE_AUTH_READY &&
        ((credential->provider == QUOTA_PROVIDER_CODEX &&
          (credential->access_token[0] == 0 || credential->refresh_token[0] == 0 ||
           credential->server_account_id[0] == 0)) ||
         (credential->provider == QUOTA_PROVIDER_DEEPSEEK && credential->api_key[0] == 0)))
        return false;
    return text_valid(credential->server_account_id, sizeof(credential->server_account_id), true,
                      true) &&
           text_valid(credential->server_user_id, sizeof(credential->server_user_id), true, true) &&
           text_valid(credential->email, sizeof(credential->email), true, false) &&
           text_valid(credential->plan, sizeof(credential->plan), true, false) &&
           text_valid(credential->label, sizeof(credential->label), true, false) &&
           text_valid(credential->access_token, sizeof(credential->access_token), true, true) &&
           text_valid(credential->refresh_token, sizeof(credential->refresh_token), true, true) &&
           text_valid(credential->api_key, sizeof(credential->api_key), true, true);
}

static quota_store_read_result_t nvs_read_error(esp_err_t error)
{
    if (error == ESP_ERR_NVS_NOT_FOUND)
        return QUOTA_STORE_READ_MISSING;
    if (error == ESP_ERR_NO_MEM)
        return QUOTA_STORE_READ_NO_MEMORY;
    if (error == ESP_ERR_NVS_TYPE_MISMATCH)
        return QUOTA_STORE_READ_INVALID;
    return QUOTA_STORE_READ_IO_ERROR;
}

static quota_store_read_result_t read_record(const char *key, void *record, size_t bytes)
{
    if (!s_ready)
        return QUOTA_STORE_READ_IO_ERROR;
    nvs_handle_t handle;
    esp_err_t error =
        nvs_open_from_partition(STORE_PARTITION, STORE_NAMESPACE, NVS_READONLY, &handle);
    if (error != ESP_OK)
        return nvs_read_error(error);
    size_t length = 0;
    error = nvs_get_blob(handle, key, NULL, &length);
    if (error != ESP_OK) {
        nvs_close(handle);
        return nvs_read_error(error);
    }
    if (length != bytes) {
        nvs_close(handle);
        return QUOTA_STORE_READ_INVALID;
    }
    length = bytes;
    error = nvs_get_blob(handle, key, record, &length);
    nvs_close(handle);
    if (error != ESP_OK)
        return nvs_read_error(error);
    if (length != bytes)
        return QUOTA_STORE_READ_INVALID;
    return QUOTA_STORE_READ_OK;
}

static bool write_record(const char *key, const void *record, size_t bytes)
{
    if (!s_ready)
        return false;
    nvs_handle_t handle;
    if (nvs_open_from_partition(STORE_PARTITION, STORE_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
        return false;
    bool ok = nvs_set_blob(handle, key, record, bytes) == ESP_OK;
    if (ok)
        ok = nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
}

bool quota_store_init(void)
{
    if (!s_ready)
        s_ready = nvs_flash_init_partition(STORE_PARTITION) == ESP_OK;
    return s_ready;
}

/* A namespace that does not exist is left alone, so a clean device is never written. */
bool quota_store_erase_retired(bool default_partition_ready)
{
    bool ok = true;
    nvs_handle_t handle;
    if (default_partition_ready && nvs_open(RETIRED_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_close(handle);
        ok = nvs_open(RETIRED_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK;
        if (ok) {
            ok = nvs_erase_all(handle) == ESP_OK && nvs_commit(handle) == ESP_OK;
            nvs_close(handle);
        }
    }
    if (s_ready && nvs_open_from_partition(STORE_PARTITION, STORE_NAMESPACE, NVS_READONLY,
                                           &handle) == ESP_OK) {
        nvs_close(handle);
        if (nvs_open_from_partition(STORE_PARTITION, STORE_NAMESPACE, NVS_READWRITE, &handle) !=
            ESP_OK)
            return false;
        static const char *const keys[] = {"config", "snapshot"};
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            esp_err_t error = nvs_erase_key(handle, keys[i]);
            ok = ok && (error == ESP_OK || error == ESP_ERR_NVS_NOT_FOUND);
        }
        ok = nvs_commit(handle) == ESP_OK && ok;
        nvs_close(handle);
    }
    return ok;
}

quota_factory_reset_result_t quota_store_factory_reset(void)
{
    /* Without an initialized store the catalog was never deleted, so a failed erase leaves the
       data untouched and must be reported as a failure rather than a restart-worthy result. */
    bool catalog_deleted = s_ready;
    if (s_ready) {
        nvs_handle_t handle;
        if (nvs_open_from_partition(STORE_PARTITION, STORE_NAMESPACE, NVS_READWRITE, &handle) !=
            ESP_OK)
            return QUOTA_FACTORY_RESET_FAILED;
        esp_err_t error = nvs_erase_key(handle, "model_v2");
        if (error == ESP_OK || error == ESP_ERR_NVS_NOT_FOUND)
            error = nvs_commit(handle);
        nvs_close(handle);
        if (error != ESP_OK)
            return QUOTA_FACTORY_RESET_FAILED;
    }
    bool ok = quota_store_erase_retired(true);
    if (s_ready) {
        s_ready = false;
        ok = nvs_flash_deinit_partition(STORE_PARTITION) == ESP_OK && ok;
    }
    ok = nvs_flash_erase_partition(STORE_PARTITION) == ESP_OK && ok;
    s_ready = nvs_flash_init_partition(STORE_PARTITION) == ESP_OK;
    if (ok && s_ready)
        return QUOTA_FACTORY_RESET_OK;
    return catalog_deleted ? QUOTA_FACTORY_RESET_CATALOG_GONE : QUOTA_FACTORY_RESET_FAILED;
}

quota_portable_credential_t *quota_store_credential_acquire(void)
{
    if (s_credential_record)
        return NULL;
    s_credential_record = calloc(1, sizeof(*s_credential_record));
    return s_credential_record ? &s_credential_record->value : NULL;
}

void quota_store_credential_release(quota_portable_credential_t *credential)
{
    if (!s_credential_record || credential != &s_credential_record->value)
        return;
    quota_portable_clear_secret(s_credential_record, sizeof(*s_credential_record));
    free(s_credential_record);
    s_credential_record = NULL;
}

quota_store_read_result_t
quota_store_load_credential_result(uint8_t slot, quota_portable_credential_t *credential)
{
    if (!credential)
        return QUOTA_STORE_READ_INVALID;
    bool workspace = s_credential_record && credential == &s_credential_record->value;
    if (!workspace && s_credential_record)
        return QUOTA_STORE_READ_BUSY;
    memset(credential, 0, sizeof(*credential));
    if (slot >= QUOTA_MAX_ACCOUNTS) {
        if (workspace)
            quota_portable_clear_secret(s_credential_record, sizeof(*s_credential_record));
        return QUOTA_STORE_READ_INVALID;
    }
    if (!workspace && !quota_store_credential_acquire()) {
        return s_credential_record ? QUOTA_STORE_READ_BUSY : QUOTA_STORE_READ_NO_MEMORY;
    }
    credential_record_t *record = s_credential_record;
    quota_portable_clear_secret(record, sizeof(*record));
    char key[] = "account0";
    key[7] = (char)('0' + slot);
    quota_store_read_result_t result = read_record(key, record, sizeof(*record));
    if (result == QUOTA_STORE_READ_OK &&
        (record->magic != CREDENTIAL_MAGIC || record->version != STORE_VERSION ||
         record->bytes != sizeof(*record) ||
         record->crc != store_crc(record, offsetof(credential_record_t, crc)) ||
         !credential_valid(&record->value, slot))) {
        result = QUOTA_STORE_READ_INVALID;
    }
    if (result == QUOTA_STORE_READ_OK && !workspace)
        *credential = record->value;
    if (result != QUOTA_STORE_READ_OK)
        quota_portable_clear_secret(record, sizeof(*record));
    if (!workspace)
        quota_store_credential_release(&record->value);
    return result;
}

bool quota_store_save_credential(uint8_t slot, const quota_portable_credential_t *credential)
{
    if (!credential_valid(credential, slot))
        return false;
    bool workspace = s_credential_record && credential == &s_credential_record->value;
    if (!workspace && (s_credential_record || !quota_store_credential_acquire()))
        return false;
    credential_record_t *record = s_credential_record;
    if (!workspace)
        record->value = *credential;
    record->magic = CREDENTIAL_MAGIC;
    record->version = STORE_VERSION;
    record->bytes = sizeof(*record);
    record->crc = store_crc(record, offsetof(credential_record_t, crc));
    char key[] = "account0";
    key[7] = (char)('0' + slot);
    bool ok = write_record(key, record, sizeof(*record));
    if (!workspace)
        quota_store_credential_release(&record->value);
    return ok;
}

bool quota_store_remove_credential(uint8_t slot, const char *id, uint32_t generation)
{
    if (!id || !quota_id_is_valid(id) || !generation || slot >= QUOTA_MAX_ACCOUNTS ||
        s_credential_record)
        return false;
    quota_portable_credential_t *credential = quota_store_credential_acquire();
    if (!credential)
        return false;
    memcpy(credential->id, id, sizeof(credential->id));
    credential->slot = slot;
    credential->generation = generation;
    credential->provider = QUOTA_PROVIDER_CODEX;
    credential->tombstone = true;
    bool ok = quota_store_save_credential(slot, credential);
    quota_store_credential_release(credential);
    return ok;
}

#define MODEL_MAGIC 0x41514D32U
#define OBSERVATION_MAGIC 0x41514F32U
#define MODEL_VERSION 2U

typedef struct {
    uint32_t magic;
    uint16_t version, bytes;
    uint64_t sequence;
    quota_model_t value;
    uint32_t crc;
} model_record_t;

typedef struct {
    char logical_id[QUOTA_ACCOUNT_ID_BYTES + 1];
    quota_provider_t provider;
    quota_account_source_t source;
    uint32_t row_generation;
    quota_account_binding_t binding;
    quota_source_status_t status;
    bool has_observed_at;
    uint64_t observed_at;
    quota_window_t five_hour, seven_day;
    quota_balance_t balance;
    quota_codex_extras_t extras;
} observation_t;

typedef struct {
    uint32_t magic;
    uint16_t version, bytes;
    uint64_t stored_at;
    uint8_t count;
    observation_t rows[QUOTA_MAX_ACCOUNTS];
    uint32_t crc;
} observation_record_t;

_Static_assert(sizeof(model_record_t) <= 8192, "model record exceeds bound");
_Static_assert(sizeof(observation_record_t) <= 8192, "observation record exceeds bound");
/* Stored layouts are frozen; removed fields stay as reserved bytes. */
_Static_assert(sizeof(model_record_t) == 4424, "model record layout changed");
_Static_assert(offsetof(model_record_t, value) == 16, "model record layout changed");
_Static_assert(sizeof(observation_t) == 384, "observation layout changed");
_Static_assert(offsetof(observation_t, binding) == 48, "observation layout changed");
_Static_assert(sizeof(observation_record_t) == 3104, "observation record layout changed");
_Static_assert(offsetof(observation_record_t, rows) == 24, "observation record layout changed");

/* The checksum covers the stored bytes, so the retired content is removed only after it. Cleanup
 * must precede quota_catalog_valid: one dropped row must not invalidate the whole catalog. */
static quota_store_read_result_t read_model_record(model_record_t *record,
                                                   quota_catalog_removed_t *removed, bool *scrubbed)
{
    quota_store_read_result_t result = read_record("model_v2", record, sizeof(*record));
    if (scrubbed)
        *scrubbed = false;
    if (result != QUOTA_STORE_READ_OK)
        return result;
    if (record->magic != MODEL_MAGIC || record->version != MODEL_VERSION ||
        record->bytes != sizeof(*record) || !record->sequence ||
        record->crc != store_crc(record, offsetof(model_record_t, crc)))
        return QUOTA_STORE_READ_INVALID;
    bool changed = quota_catalog_scrub_removed(&record->value, removed);
    if (scrubbed)
        *scrubbed = changed;
    return quota_catalog_valid(&record->value) ? QUOTA_STORE_READ_OK : QUOTA_STORE_READ_INVALID;
}

/* Replace the slot's credential with a tombstone; a slot that is already free or a tombstone is
 * left alone. The tombstone keeps the generation so later reuse stays monotonic. */
static quota_store_read_result_t release_slot(uint8_t slot, const char *fallback_id,
                                              uint32_t fallback_generation)
{
    quota_portable_credential_t *credential = quota_store_credential_acquire();
    if (!credential)
        return s_credential_record ? QUOTA_STORE_READ_BUSY : QUOTA_STORE_READ_NO_MEMORY;
    quota_store_read_result_t result = quota_store_load_credential_result(slot, credential);
    char id[QUOTA_ACCOUNT_ID_BYTES + 1] = {0};
    uint32_t generation = 0;
    if (result == QUOTA_STORE_READ_OK && !credential->tombstone) {
        memcpy(id, credential->id, sizeof(id));
        generation = credential->generation;
    } else if (result == QUOTA_STORE_READ_INVALID && quota_id_is_valid(fallback_id)) {
        memcpy(id, fallback_id, sizeof(id));
        generation = fallback_generation; /* Unreadable: overwrite. */
    }
    quota_store_credential_release(credential);
    if (result != QUOTA_STORE_READ_OK && result != QUOTA_STORE_READ_MISSING &&
        result != QUOTA_STORE_READ_INVALID)
        return result;
    if (!generation)
        return QUOTA_STORE_READ_OK; /* Free, already a tombstone, or unknowable. */
    return quota_store_remove_credential(slot, id, generation) ? QUOTA_STORE_READ_OK
                                                               : QUOTA_STORE_READ_IO_ERROR;
}

/* A row of a provider or source the product no longer has. */
static bool observation_retired_kind(const observation_t *row)
{
    return (row->provider != QUOTA_PROVIDER_CODEX && row->provider != QUOTA_PROVIDER_DEEPSEEK) ||
           row->source != QUOTA_ACCOUNT_DEVICE;
}

static bool observation_row_retired(const observation_t *row,
                                    const quota_catalog_removed_t *removed)
{
    if (observation_retired_kind(row))
        return true;
    for (unsigned i = 0; i < removed->count; i++)
        if (!strncmp(row->logical_id, removed->rows[i].logical_id, sizeof(row->logical_id)) &&
            removed->rows[i].logical_id[0])
            return true;
    return false;
}

/* Zero the cached rows of dropped accounts and rewrite the cache only if one was present. A cache
 * that cannot be read as a whole is ignored by the loader and replaced by the next save. */
static quota_store_read_result_t scrub_observations(const quota_catalog_removed_t *removed)
{
    observation_record_t *record = calloc(1, sizeof(*record));
    if (!record)
        return QUOTA_STORE_READ_NO_MEMORY;
    quota_store_read_result_t result = read_record("observations_v2", record, sizeof(*record));
    if (result == QUOTA_STORE_READ_OK &&
        (record->magic != OBSERVATION_MAGIC || record->version != MODEL_VERSION ||
         record->bytes != sizeof(*record) || record->count > QUOTA_MAX_ACCOUNTS ||
         record->crc != store_crc(record, offsetof(observation_record_t, crc))))
        result = QUOTA_STORE_READ_INVALID;
    if (result == QUOTA_STORE_READ_OK) {
        unsigned kept = 0;
        for (unsigned i = 0; i < record->count; i++) {
            if (observation_row_retired(&record->rows[i], removed))
                continue;
            if (kept != i)
                record->rows[kept] = record->rows[i];
            kept++;
        }
        if (kept != record->count) {
            memset(&record->rows[kept], 0, (record->count - kept) * sizeof(record->rows[0]));
            record->count = (uint8_t)kept;
            record->crc = store_crc(record, offsetof(observation_record_t, crc));
            if (!write_record("observations_v2", record, sizeof(*record)))
                result = QUOTA_STORE_READ_IO_ERROR;
        }
    }
    free(record);
    return result == QUOTA_STORE_READ_MISSING || result == QUOTA_STORE_READ_INVALID
               ? QUOTA_STORE_READ_OK
               : result;
}

/* Upgrade from a catalog that still holds removed content. Every step is idempotent and the
 * model commit is last: it is the only thing that stops the next boot from running them again,
 * so a power cut at any point simply repeats the cleanup. No write happens when nothing was
 * removed. */
static quota_store_read_result_t commit_scrubbed_model(model_record_t *record,
                                                       const quota_catalog_removed_t *removed)
{
    for (unsigned i = 0; i < removed->count; i++) {
        bool repeated = !removed->rows[i].owns_slot;
        for (unsigned j = 0; j < i; j++)
            repeated = repeated || (removed->rows[j].owns_slot &&
                                    removed->rows[j].slot == removed->rows[i].slot);
        if (repeated)
            continue;
        quota_store_read_result_t result =
            release_slot(removed->rows[i].slot, removed->rows[i].credential_id,
                         removed->rows[i].credential_generation);
        if (result != QUOTA_STORE_READ_OK)
            return result;
    }
    quota_store_read_result_t result = scrub_observations(removed);
    if (result != QUOTA_STORE_READ_OK)
        return result;
    quota_model_t *scrubbed = malloc(sizeof(*scrubbed));
    if (!scrubbed)
        return QUOTA_STORE_READ_NO_MEMORY;
    *scrubbed = record->value;
    uint64_t sequence = record->sequence;
    quota_model_write_result_t written =
        quota_store_save_model_verified(sequence, scrubbed, sequence + 1);
    quota_portable_clear_secret(scrubbed, sizeof(*scrubbed));
    free(scrubbed);
    if (written != QUOTA_MODEL_APPLIED)
        return QUOTA_STORE_READ_IO_ERROR;
    record->sequence = sequence + 1;
    return QUOTA_STORE_READ_OK;
}

/* Without a catalog no credential belongs to an account: free every slot so stale keys neither
 * linger nor use up account capacity. Idempotent; unreadable slots are left alone. */
quota_store_read_result_t quota_store_release_orphan_credentials(void)
{
    for (uint8_t slot = 0; slot < QUOTA_MAX_ACCOUNTS; slot++) {
        quota_store_read_result_t result = release_slot(slot, "", 0);
        if (result != QUOTA_STORE_READ_OK)
            return result;
    }
    return QUOTA_STORE_READ_OK;
}

quota_store_read_result_t quota_store_load_model_result(quota_model_t *model, uint64_t *sequence)
{
    if (!model || !sequence)
        return QUOTA_STORE_READ_INVALID;
    memset(model, 0, sizeof(*model));
    *sequence = 0;
    model_record_t *record = calloc(1, sizeof(*record));
    quota_catalog_removed_t *removed = calloc(1, sizeof(*removed));
    if (!record || !removed) {
        free(record);
        free(removed);
        return QUOTA_STORE_READ_NO_MEMORY;
    }
    bool scrubbed = false;
    quota_store_read_result_t result = read_model_record(record, removed, &scrubbed);
    if (result == QUOTA_STORE_READ_OK && scrubbed)
        result = commit_scrubbed_model(record, removed);
    if (result == QUOTA_STORE_READ_OK) {
        *model = record->value;
        *sequence = record->sequence;
    }
    quota_portable_clear_secret(record, sizeof(*record));
    free(record);
    free(removed);
    return result;
}

quota_model_write_result_t quota_store_save_model_verified(uint64_t expected_previous_sequence,
                                                           const quota_model_t *candidate,
                                                           uint64_t candidate_sequence)
{
    if (!quota_catalog_valid(candidate) || expected_previous_sequence == UINT64_MAX ||
        candidate_sequence != expected_previous_sequence + 1)
        return QUOTA_MODEL_NOT_APPLIED;
    model_record_t *record = calloc(1, sizeof(*record));
    if (!record)
        return QUOTA_MODEL_WRITE_UNKNOWN; /* A prior attempt may be durable. */
    quota_store_read_result_t before = read_model_record(record, NULL, NULL);
    if (before == QUOTA_STORE_READ_OK && record->sequence == candidate_sequence &&
        !memcmp(&record->value, candidate, sizeof(*candidate))) {
        quota_portable_clear_secret(record, sizeof(*record));
        free(record);
        return QUOTA_MODEL_APPLIED;
    }
    if (!((before == QUOTA_STORE_READ_OK && record->sequence == expected_previous_sequence) ||
          (before == QUOTA_STORE_READ_MISSING && expected_previous_sequence == 0))) {
        quota_portable_clear_secret(record, sizeof(*record));
        free(record);
        return QUOTA_MODEL_WRITE_UNKNOWN;
    }
    memset(record, 0, sizeof(*record));
    record->magic = MODEL_MAGIC;
    record->version = MODEL_VERSION;
    record->bytes = sizeof(*record);
    record->sequence = candidate_sequence;
    record->value = *candidate;
    record->crc = store_crc(record, offsetof(model_record_t, crc));
    /* A failed write/commit may still be durable. Read back before deciding. */
    (void)write_record("model_v2", record, sizeof(*record));
    quota_portable_clear_secret(record, sizeof(*record));
    quota_store_read_result_t result = read_model_record(record, NULL, NULL);
    quota_model_write_result_t outcome = QUOTA_MODEL_WRITE_UNKNOWN;
    if (result == QUOTA_STORE_READ_OK) {
        if (record->sequence == candidate_sequence &&
            !memcmp(&record->value, candidate, sizeof(*candidate)))
            outcome = QUOTA_MODEL_APPLIED;
        else if (record->sequence == expected_previous_sequence)
            outcome = QUOTA_MODEL_NOT_APPLIED;
    } else if (result == QUOTA_STORE_READ_MISSING && expected_previous_sequence == 0)
        outcome = QUOTA_MODEL_NOT_APPLIED;
    quota_portable_clear_secret(record, sizeof(*record));
    free(record);
    return outcome;
}

static bool window_valid(const quota_window_t *window)
{
    return (!window->present || window->remaining_percent <= 100) &&
           (!window->has_resets_at || (window->present && window->resets_at != 0));
}

static bool observation_valid(const observation_t *row)
{
    if (!text_valid(row->logical_id, sizeof(row->logical_id), false, true) ||
        !quota_id_is_valid(row->logical_id) ||
        (row->provider != QUOTA_PROVIDER_CODEX && row->provider != QUOTA_PROVIDER_DEEPSEEK) ||
        row->source != QUOTA_ACCOUNT_DEVICE || !row->row_generation ||
        (unsigned)row->status > QUOTA_STATUS_UNSUPPORTED ||
        (row->has_observed_at && !row->observed_at) || !window_valid(&row->five_hour) ||
        !window_valid(&row->seven_day) || !quota_balance_is_valid(&row->balance) ||
        !text_valid(row->extras.credits_balance, sizeof(row->extras.credits_balance), true, true))
        return false;
    return row->binding.native.slot < QUOTA_MAX_ACCOUNTS &&
           row->binding.native.credential_generation &&
           text_valid(row->binding.native.credential_id, sizeof(row->binding.native.credential_id),
                      false, true) &&
           quota_id_is_valid(row->binding.native.credential_id);
}

bool quota_store_save_observations(const quota_model_t *model, const quota_snapshot_t *snapshot,
                                   uint64_t stored_at)
{
    if (!quota_catalog_valid(model) || !snapshot || snapshot->account_count > QUOTA_MAX_ACCOUNTS ||
        !stored_at)
        return false;
    observation_record_t *record = calloc(1, sizeof(*record));
    if (!record)
        return false;
    record->magic = OBSERVATION_MAGIC;
    record->version = MODEL_VERSION;
    record->bytes = sizeof(*record);
    record->stored_at = stored_at;
    bool valid = true;
    for (unsigned i = 0; i < snapshot->account_count; i++) {
        int found = quota_catalog_find(model, snapshot->accounts[i].id);
        if (found < 0) {
            valid = false;
            break;
        }
        const quota_catalog_entry_t *entry = &model->entries[found];
        if (entry->provider != snapshot->accounts[i].provider) {
            valid = false;
            break;
        }
        observation_t *row = &record->rows[record->count++];
        memcpy(row->logical_id, entry->logical_id, sizeof(row->logical_id));
        row->provider = entry->provider;
        row->source = entry->source;
        row->row_generation = entry->row_generation;
        row->binding = entry->binding;
        row->status = snapshot->accounts[i].status;
        row->has_observed_at = snapshot->accounts[i].has_observed_at;
        row->observed_at = snapshot->accounts[i].observed_at;
        row->five_hour = snapshot->accounts[i].five_hour;
        row->seven_day = snapshot->accounts[i].seven_day;
        row->balance = snapshot->balances[i];
        memset(row->balance.label, 0, sizeof(row->balance.label));
        row->extras = snapshot->codex_extras[i];
        if (!observation_valid(row)) {
            valid = false;
            break;
        }
    }
    if (valid) {
        record->crc = store_crc(record, offsetof(observation_record_t, crc));
        valid = write_record("observations_v2", record, sizeof(*record));
    }
    free(record);
    return valid;
}

quota_store_read_result_t quota_store_load_observations(const quota_model_t *model, uint64_t now,
                                                        quota_snapshot_t *snapshot)
{
    if (!quota_catalog_valid(model) || !snapshot || snapshot->account_count > QUOTA_MAX_ACCOUNTS)
        return QUOTA_STORE_READ_INVALID;
    observation_record_t *record = calloc(1, sizeof(*record));
    if (!record)
        return QUOTA_STORE_READ_NO_MEMORY;
    quota_store_read_result_t result = read_record("observations_v2", record, sizeof(*record));
    if (result == QUOTA_STORE_READ_OK) {
        if (record->magic != OBSERVATION_MAGIC || record->version != MODEL_VERSION ||
            record->bytes != sizeof(*record) || !record->stored_at ||
            record->count > QUOTA_MAX_ACCOUNTS ||
            record->crc != store_crc(record, offsetof(observation_record_t, crc)))
            result = QUOTA_STORE_READ_INVALID;
        /* Rows of removed providers or sources are ignored; they must not discard the others. */
        for (unsigned i = 0; result == QUOTA_STORE_READ_OK && i < record->count; i++) {
            if (observation_retired_kind(&record->rows[i]))
                continue;
            if (!observation_valid(&record->rows[i]))
                result = QUOTA_STORE_READ_INVALID;
            for (unsigned j = 0; result == QUOTA_STORE_READ_OK && j < i; j++)
                if (!observation_retired_kind(&record->rows[j]) &&
                    !strcmp(record->rows[j].logical_id, record->rows[i].logical_id))
                    result = QUOTA_STORE_READ_INVALID;
        }
    }
    if (result == QUOTA_STORE_READ_OK &&
        !(now > record->stored_at && now - record->stored_at > CACHE_MAX_AGE)) {
        for (unsigned i = 0; i < record->count; i++) {
            const observation_t *row = &record->rows[i];
            if (observation_retired_kind(row))
                continue;
            int found = quota_catalog_find(model, row->logical_id);
            int output = quota_find_account_by_id(snapshot, row->logical_id);
            if (found < 0 || output < 0)
                continue;
            const quota_catalog_entry_t *entry = &model->entries[found];
            quota_catalog_entry_t cached = {0};
            memcpy(cached.logical_id, row->logical_id, sizeof(cached.logical_id));
            cached.provider = row->provider;
            cached.source = row->source;
            cached.row_generation = row->row_generation;
            cached.binding = row->binding;
            if (!quota_catalog_binding_equal(entry, &cached))
                continue;
            quota_account_t *account = &snapshot->accounts[output];
            account->status = row->status;
            account->has_observed_at = row->has_observed_at;
            account->observed_at = row->observed_at;
            account->five_hour = row->five_hour;
            account->seven_day = row->seven_day;
            char label[sizeof(snapshot->balances[output].label)];
            memcpy(label, snapshot->balances[output].label, sizeof(label));
            snapshot->balances[output] = row->balance;
            memcpy(snapshot->balances[output].label, label, sizeof(label));
            snapshot->codex_extras[output] = row->extras;
        }
    }
    free(record);
    return result;
}
