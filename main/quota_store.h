#pragma once

#include "quota_portable.h"

/* All calls are serialized by the service owner. No automatic erase/recovery. */
bool quota_store_init(void);
bool quota_store_load_config(quota_portable_config_t *config);
bool quota_store_save_config(const quota_portable_config_t *config);
/* One record-backed workspace, owned by the serialized service worker. Never
 * free it. The entire borrowing lifetime is exclusive: no other credential
 * load/save/remove may run while a login, key check or pending token commit
 * retains this pointer. Config/cache operations do not touch it.
 * Load/save with this pointer retain its value (also on save failure); the
 * owner clears it after the final consumer. A failed load clears it. External
 * pointers remain supported via copying and wiping the record scratch. */
quota_portable_credential_t *quota_store_credential_buffer(void);
bool quota_store_load_credential(uint8_t slot, quota_portable_credential_t *credential);
bool quota_store_save_credential(uint8_t slot, const quota_portable_credential_t *credential);
bool quota_store_remove_credential(uint8_t slot, const char *id, uint32_t generation);
bool quota_store_save_snapshot(const quota_snapshot_t *snapshot,
                               const quota_portable_account_ref_t *accounts,
                               size_t account_count, uint64_t stored_at);
bool quota_store_load_snapshot(const quota_portable_account_ref_t *accounts,
                               size_t account_count, uint64_t now,
                               quota_snapshot_t *snapshot);
