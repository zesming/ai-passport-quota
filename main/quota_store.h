#pragma once

#include "quota_portable.h"

/* All calls are serialized by the service owner. No automatic erase/recovery. */
bool quota_store_init(void);
bool quota_store_load_config(quota_portable_config_t *config);
bool quota_store_save_config(const quota_portable_config_t *config);
bool quota_store_load_credential(uint8_t slot, quota_portable_credential_t *credential);
bool quota_store_save_credential(uint8_t slot, const quota_portable_credential_t *credential);
bool quota_store_remove_credential(uint8_t slot, const char *id, uint32_t generation);
bool quota_store_save_snapshot(const quota_snapshot_t *snapshot,
                               const quota_portable_account_ref_t *accounts,
                               size_t account_count, uint64_t stored_at);
bool quota_store_load_snapshot(const quota_portable_account_ref_t *accounts,
                               size_t account_count, uint64_t now,
                               quota_snapshot_t *snapshot);
