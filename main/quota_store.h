#pragma once

#include "quota_catalog.h"

/* All calls are serialized by the service owner. No automatic erase/recovery. */
typedef enum {
    QUOTA_STORE_READ_OK = 0,
    QUOTA_STORE_READ_MISSING,
    QUOTA_STORE_READ_INVALID,
    QUOTA_STORE_READ_IO_ERROR,
    QUOTA_STORE_READ_NO_MEMORY,
    QUOTA_STORE_READ_BUSY,
} quota_store_read_result_t;

bool quota_store_init(void);
quota_store_read_result_t quota_store_load_config_result(quota_portable_config_t *config);
bool quota_store_load_config(quota_portable_config_t *config);
bool quota_store_save_config(const quota_portable_config_t *config);
/* One exclusive heap record, allocated only while the owner needs credentials.
 * Acquire returns NULL when busy or out of memory. Only the serialized owner
 * releases it, after provider pending persistence is resolved. Never free the
 * value pointer directly. Alias load/save reuse the record; failed save retains
 * the complete bundle. A failed load clears it. Config/cache are independent.
 * External pointers use a temporary record and fail while one is acquired. */
quota_portable_credential_t *quota_store_credential_acquire(void);
void quota_store_credential_release(quota_portable_credential_t *credential);
quota_store_read_result_t quota_store_load_credential_result(
    uint8_t slot, quota_portable_credential_t *credential);
bool quota_store_load_credential(uint8_t slot, quota_portable_credential_t *credential);
bool quota_store_save_credential(uint8_t slot, const quota_portable_credential_t *credential);
bool quota_store_remove_credential(uint8_t slot, const char *id, uint32_t generation);
bool quota_store_save_snapshot(const quota_snapshot_t *snapshot,
                               const quota_portable_account_ref_t *accounts,
                               size_t account_count, uint64_t stored_at);
bool quota_store_load_snapshot(const quota_portable_account_ref_t *accounts,
                               size_t account_count, uint64_t now,
                               quota_snapshot_t *snapshot);

/* One complete model blob, verified after every attempted write. */
typedef enum { QUOTA_MODEL_APPLIED, QUOTA_MODEL_NOT_APPLIED,
               QUOTA_MODEL_WRITE_UNKNOWN } quota_model_write_result_t;
quota_store_read_result_t quota_store_load_model_result(quota_model_t *model,
                                                       uint64_t *sequence);
quota_model_write_result_t quota_store_save_model_verified(
    uint64_t expected_previous_sequence, const quota_model_t *candidate,
    uint64_t candidate_sequence);
/* Observation cache never supplies identity, aliases, settings or authority. */
bool quota_store_save_observations(const quota_model_t *model,
                                   const quota_snapshot_t *snapshot, uint64_t stored_at);
quota_store_read_result_t quota_store_load_observations(
    const quota_model_t *model, uint64_t now, quota_snapshot_t *snapshot);
