#include "quota_store.h"

#include "nvs.h"
#include "nvs_flash.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define STORE_PARTITION "portable"
#define STORE_NAMESPACE "quota_port"
#define STORE_VERSION 1U
#define CONFIG_MAGIC 0x41515043U
#define CREDENTIAL_MAGIC 0x41515041U
#define SNAPSHOT_MAGIC 0x41515053U
#define CACHE_MAX_AGE (30ULL * 24 * 60 * 60)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t bytes;
    quota_portable_config_t value;
    uint32_t crc;
} config_record_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t bytes;
    quota_portable_credential_t value;
    uint32_t crc;
} credential_record_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t bytes;
    uint64_t stored_at;
    quota_snapshot_t value;
    uint32_t generations[QUOTA_MAX_ACCOUNTS];
    uint32_t crc;
} snapshot_record_t;

_Static_assert(sizeof(credential_record_t) <= 16384, "credential record exceeds bound");
_Static_assert(sizeof(snapshot_record_t) <= 8192, "snapshot record exceeds bound");

static bool s_ready;
static credential_record_t s_credential_record;

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
    if (end == NULL || (!empty && end == text)) return false;
    size_t length = (size_t)(end - text);
    if (!quota_utf8_is_valid(text, length)) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 0x20 || ch == 0x7f || (ascii && ch > 0x7e)) return false;
    }
    return true;
}

static bool config_valid(const quota_portable_config_t *config)
{
    if (config == NULL || (config->mode != QUOTA_MODE_COMPANION && config->mode != QUOTA_MODE_DIRECT) ||
        config->network_count > QUOTA_PORTABLE_NETWORKS ||
        (config->network_count != 0 && config->selected_network >= config->network_count) ||
        (config->network_count == 0 && config->selected_network != 0) ||
        !quota_refresh_seconds_is_valid(config->refresh_seconds) ||
        !quota_screen_timeout_is_valid(config->screen_timeout_seconds) ||
        !text_valid(config->selected_account_id, sizeof(config->selected_account_id), true, true) ||
        (config->selected_account_id[0] != 0 && !quota_id_is_valid(config->selected_account_id)))
        return false;
    for (size_t i = 0; i < config->network_count; i++) {
        if (!text_valid(config->networks[i].ssid, sizeof(config->networks[i].ssid), false, false) ||
            !text_valid(config->networks[i].password, sizeof(config->networks[i].password), true, false))
            return false;
    }
    return true;
}

static bool credential_valid(const quota_portable_credential_t *credential, uint8_t slot)
{
    if (credential == NULL || slot >= QUOTA_MAX_ACCOUNTS || credential->slot != slot ||
        !text_valid(credential->id, sizeof(credential->id), false, true) ||
        !quota_id_is_valid(credential->id) || credential->generation == 0 ||
        (credential->provider != QUOTA_PROVIDER_CODEX && credential->provider != QUOTA_PROVIDER_DEEPSEEK) ||
        (unsigned)credential->auth_state > QUOTA_PORTABLE_AUTH_ERROR) return false;
    if (credential->tombstone) {
        return credential->access_token[0] == 0 && credential->refresh_token[0] == 0 &&
               credential->api_key[0] == 0 && !credential->refresh_inflight;
    }
    if (credential->refresh_inflight && credential->provider != QUOTA_PROVIDER_CODEX) return false;
    if (credential->auth_state == QUOTA_PORTABLE_AUTH_READY &&
        ((credential->provider == QUOTA_PROVIDER_CODEX &&
          (credential->access_token[0] == 0 || credential->refresh_token[0] == 0 ||
           credential->server_account_id[0] == 0)) ||
         (credential->provider == QUOTA_PROVIDER_DEEPSEEK && credential->api_key[0] == 0))) return false;
    return text_valid(credential->server_account_id, sizeof(credential->server_account_id), true, true) &&
           text_valid(credential->server_user_id, sizeof(credential->server_user_id), true, true) &&
           text_valid(credential->email, sizeof(credential->email), true, false) &&
           text_valid(credential->plan, sizeof(credential->plan), true, false) &&
           text_valid(credential->label, sizeof(credential->label), true, false) &&
           text_valid(credential->access_token, sizeof(credential->access_token), true, true) &&
           text_valid(credential->refresh_token, sizeof(credential->refresh_token), true, true) &&
           text_valid(credential->api_key, sizeof(credential->api_key), true, true);
}

static bool read_record(const char *key, void *record, size_t bytes)
{
    if (!s_ready) return false;
    nvs_handle_t handle;
    if (nvs_open_from_partition(STORE_PARTITION, STORE_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
        return false;
    size_t length = 0;
    bool ok = nvs_get_blob(handle, key, NULL, &length) == ESP_OK && length == bytes;
    if (ok) ok = nvs_get_blob(handle, key, record, &length) == ESP_OK && length == bytes;
    nvs_close(handle);
    return ok;
}

static bool write_record(const char *key, const void *record, size_t bytes)
{
    if (!s_ready) return false;
    nvs_handle_t handle;
    if (nvs_open_from_partition(STORE_PARTITION, STORE_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
        return false;
    bool ok = nvs_set_blob(handle, key, record, bytes) == ESP_OK;
    if (ok) ok = nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
}

bool quota_store_init(void)
{
    if (!s_ready) s_ready = nvs_flash_init_partition(STORE_PARTITION) == ESP_OK;
    return s_ready;
}

bool quota_store_load_config(quota_portable_config_t *config)
{
    if (config == NULL) return false;
    config_record_t record = {0};
    bool ok = read_record("config", &record, sizeof(record)) && record.magic == CONFIG_MAGIC &&
              record.version == STORE_VERSION && record.bytes == sizeof(record) &&
              record.crc == store_crc(&record, offsetof(config_record_t, crc)) &&
              config_valid(&record.value);
    if (ok) *config = record.value;
    else memset(config, 0, sizeof(*config));
    quota_portable_clear_secret(&record, sizeof(record));
    return ok;
}

bool quota_store_save_config(const quota_portable_config_t *config)
{
    if (!config_valid(config)) return false;
    config_record_t record = {0};
    record.magic = CONFIG_MAGIC;
    record.version = STORE_VERSION;
    record.bytes = sizeof(record);
    record.value = *config;
    record.crc = store_crc(&record, offsetof(config_record_t, crc));
    bool ok = write_record("config", &record, sizeof(record));
    quota_portable_clear_secret(&record, sizeof(record));
    return ok;
}

quota_portable_credential_t *quota_store_credential_buffer(void)
{
    return &s_credential_record.value;
}

bool quota_store_load_credential(uint8_t slot, quota_portable_credential_t *credential)
{
    if (credential == NULL) return false;
    credential_record_t *record = &s_credential_record;
    bool workspace = credential == &record->value;
    quota_portable_clear_secret(record, sizeof(*record));
    memset(credential, 0, sizeof(*credential));
    if (slot >= QUOTA_MAX_ACCOUNTS) return false;
    char key[] = "account0";
    key[7] = (char)('0' + slot);
    bool ok = read_record(key, record, sizeof(*record)) && record->magic == CREDENTIAL_MAGIC &&
              record->version == STORE_VERSION && record->bytes == sizeof(*record) &&
              record->crc == store_crc(record, offsetof(credential_record_t, crc)) &&
              credential_valid(&record->value, slot);
    if (ok && !workspace) *credential = record->value;
    if (!ok || !workspace) quota_portable_clear_secret(record, sizeof(*record));
    return ok;
}

bool quota_store_save_credential(uint8_t slot, const quota_portable_credential_t *credential)
{
    if (!credential_valid(credential, slot)) return false;
    credential_record_t *record = &s_credential_record;
    bool workspace = credential == &record->value;
    if (!workspace) {
        quota_portable_clear_secret(record, sizeof(*record));
        record->value = *credential;
    }
    record->magic = CREDENTIAL_MAGIC;
    record->version = STORE_VERSION;
    record->bytes = sizeof(*record);
    record->crc = store_crc(record, offsetof(credential_record_t, crc));
    char key[] = "account0";
    key[7] = (char)('0' + slot);
    bool ok = write_record(key, record, sizeof(*record));
    if (!workspace) quota_portable_clear_secret(record, sizeof(*record));
    return ok;
}

bool quota_store_remove_credential(uint8_t slot, const char *id, uint32_t generation)
{
    if (id == NULL || !quota_id_is_valid(id) || generation == 0 || slot >= QUOTA_MAX_ACCOUNTS)
        return false;
    /* id can itself belong to the workspace being replaced. */
    char copied_id[QUOTA_ACCOUNT_ID_BYTES + 1];
    memcpy(copied_id, id, sizeof(copied_id));
    quota_portable_clear_secret(&s_credential_record, sizeof(s_credential_record));
    quota_portable_credential_t *credential = quota_store_credential_buffer();
    memcpy(credential->id, copied_id, sizeof(credential->id));
    credential->slot = slot;
    credential->generation = generation;
    credential->provider = QUOTA_PROVIDER_CODEX;
    credential->tombstone = true;
    bool ok = quota_store_save_credential(slot, credential);
    quota_portable_clear_secret(&s_credential_record, sizeof(s_credential_record));
    return ok;
}

static int find_reference(const quota_portable_account_ref_t *accounts, size_t count,
                          const quota_account_t *account)
{
    for (size_t i = 0; i < count; i++) {
        if (accounts[i].generation != 0 && accounts[i].provider == account->provider &&
            memcmp(accounts[i].id, account->id, sizeof(account->id)) == 0) return (int)i;
    }
    return -1;
}

static bool snapshot_valid(const quota_snapshot_t *snapshot)
{
    if (snapshot->account_count > QUOTA_MAX_ACCOUNTS ||
        !quota_refresh_seconds_is_valid(snapshot->refresh_seconds) ||
        (snapshot->has_screen_timeout_seconds &&
         !quota_screen_timeout_is_valid(snapshot->screen_timeout_seconds))) return false;
    for (size_t i = 0; i < snapshot->account_count; i++) {
        const quota_account_t *account = &snapshot->accounts[i];
        if (!text_valid(account->id, sizeof(account->id), false, true) || !quota_id_is_valid(account->id) ||
            (account->provider != QUOTA_PROVIDER_CODEX && account->provider != QUOTA_PROVIDER_DEEPSEEK) ||
            account->status > QUOTA_STATUS_UNSUPPORTED ||
            !text_valid(account->email, sizeof(account->email), true, false) ||
            !text_valid(account->plan, sizeof(account->plan), true, false) ||
            (account->has_observed_at && account->observed_at == 0) ||
            (account->five_hour.present && account->five_hour.remaining_percent > 100) ||
            (account->seven_day.present && account->seven_day.remaining_percent > 100) ||
            (account->five_hour.has_resets_at && account->five_hour.resets_at == 0) ||
            (account->seven_day.has_resets_at && account->seven_day.resets_at == 0) ||
            !quota_balance_is_valid(&snapshot->balances[i])) return false;
        for (size_t j = 0; j < i; j++)
            if (strcmp(account->id, snapshot->accounts[j].id) == 0) return false;
    }
    return true;
}

bool quota_store_save_snapshot(const quota_snapshot_t *snapshot,
                               const quota_portable_account_ref_t *accounts,
                               size_t account_count, uint64_t stored_at)
{
    if (snapshot == NULL || account_count > QUOTA_MAX_ACCOUNTS ||
        (account_count != 0 && accounts == NULL) || stored_at == 0 || !snapshot_valid(snapshot)) return false;
    snapshot_record_t *record = calloc(1, sizeof(*record));
    if (record == NULL) return false;
    record->magic = SNAPSHOT_MAGIC;
    record->version = STORE_VERSION;
    record->bytes = sizeof(*record);
    record->stored_at = stored_at;
    record->value = *snapshot;
    /* Optional Codex extras have always been RAM-only. */
    memset(record->value.codex_extras, 0, sizeof(record->value.codex_extras));
    bool ok = true;
    for (size_t i = 0; i < snapshot->account_count; i++) {
        int found = find_reference(accounts, account_count, &snapshot->accounts[i]);
        if (found < 0) { ok = false; break; }
        record->generations[i] = accounts[found].generation;
    }
    if (ok) {
        record->crc = store_crc(record, offsetof(snapshot_record_t, crc));
        ok = write_record("snapshot", record, sizeof(*record));
    }
    free(record);
    return ok;
}

bool quota_store_load_snapshot(const quota_portable_account_ref_t *accounts,
                               size_t account_count, uint64_t now,
                               quota_snapshot_t *snapshot)
{
    if (snapshot == NULL) return false;
    memset(snapshot, 0, sizeof(*snapshot));
    if (account_count > QUOTA_MAX_ACCOUNTS || (account_count != 0 && accounts == NULL)) return false;
    snapshot_record_t *record = calloc(1, sizeof(*record));
    if (record == NULL) return false;
    bool ok = read_record("snapshot", record, sizeof(*record)) && record->magic == SNAPSHOT_MAGIC &&
              record->version == STORE_VERSION && record->bytes == sizeof(*record) &&
              record->crc == store_crc(record, offsetof(snapshot_record_t, crc)) &&
              record->stored_at != 0 &&
              !(now > record->stored_at && now - record->stored_at > CACHE_MAX_AGE) &&
              snapshot_valid(&record->value);
    if (ok) {
        snapshot->server_time = record->value.server_time;
        snapshot->revision = record->value.revision;
        snapshot->refresh_seconds = record->value.refresh_seconds;
        snapshot->auto_refresh = record->value.auto_refresh;
        snapshot->has_screen_timeout_seconds = record->value.has_screen_timeout_seconds;
        snapshot->screen_timeout_seconds = record->value.screen_timeout_seconds;
        for (size_t i = 0; i < record->value.account_count; i++) {
            int found = find_reference(accounts, account_count, &record->value.accounts[i]);
            if (found < 0 || accounts[found].generation != record->generations[i]) continue;
            size_t output = snapshot->account_count++;
            snapshot->accounts[output] = record->value.accounts[i];
            snapshot->balances[output] = record->value.balances[i];
        }
    }
    free(record);
    return ok;
}
