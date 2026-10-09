#pragma once

#include "quota_portable.h"

#include <stddef.h>

/* The persisted array keeps its original size. Only the first QUOTA_MAX_ACCOUNTS rows are used;
 * the rest are reserved and always zero. */
#define QUOTA_CATALOG_STORED_ROWS (2 * QUOTA_MAX_ACCOUNTS)

/* Persisted values: never reuse 1. */
typedef enum {
    QUOTA_ACCOUNT_ACTIVE = 0,
    /* 1 reserved: removed pending state */
} quota_account_activity_t;

typedef union {
    struct {
        uint8_t slot;
        char credential_id[QUOTA_ACCOUNT_ID_BYTES + 1];
        uint32_t credential_generation;
    } native;
    uint8_t reserved_legacy[40]; /* Was the binding of a removed source. */
} quota_account_binding_t;

typedef struct {
    char logical_id[QUOTA_ACCOUNT_ID_BYTES + 1];
    quota_provider_t provider;
    quota_account_source_t source;
    quota_account_activity_t activity;
    uint32_t row_generation;
    char label[QUOTA_PLAN_MAX_BYTES + 1];
    quota_account_binding_t binding;
} quota_catalog_entry_t;

typedef enum {
    QUOTA_INTENT_NONE,
    QUOTA_INTENT_UPSERT_NATIVE,
    QUOTA_INTENT_DELETE_NATIVE
} quota_intent_kind_t;
typedef struct {
    quota_intent_kind_t kind;
    char request_id[9];
    char logical_id[QUOTA_ACCOUNT_ID_BYTES + 1];
    uint32_t expected_row_generation;
    quota_provider_t provider;
    uint8_t slot;
    bool previous_missing, previous_tombstone;
    char previous_credential_id[QUOTA_ACCOUNT_ID_BYTES + 1];
    uint32_t previous_credential_generation;
    char target_credential_id[QUOTA_ACCOUNT_ID_BYTES + 1];
    uint32_t target_credential_generation;
    char desired_label[QUOTA_PLAN_MAX_BYTES + 1];
    bool new_row;
} quota_model_intent_t;

/* Same size and offsets as the stored record. The reserved ranges held the pending network
 * (present flag and network) and the legacy endpoint, which are always zero now. */
typedef struct {
    uint8_t network_count, selected_network;
    quota_portable_network_t networks[QUOTA_PORTABLE_NETWORKS];
    uint8_t reserved_pending_network[99];
    uint16_t refresh_seconds, screen_timeout_seconds;
    bool auto_refresh;
    char selected_account_id[QUOTA_ACCOUNT_ID_BYTES + 1];
    uint64_t last_known_time;
    uint8_t reserved_legacy_endpoint[1728];
    uint8_t entry_count;
    quota_catalog_entry_t entries[QUOTA_CATALOG_STORED_ROWS];
    quota_model_intent_t intent;
} quota_model_t;

_Static_assert(sizeof(quota_account_binding_t) == 40, "binding layout changed");
_Static_assert(sizeof(quota_catalog_entry_t) == 128, "catalog entry layout changed");
_Static_assert(offsetof(quota_catalog_entry_t, binding) == 88, "catalog entry layout changed");
_Static_assert(offsetof(quota_model_t, networks) == 2, "model layout changed");
_Static_assert(offsetof(quota_model_t, reserved_pending_network) == 296, "model layout changed");
_Static_assert(offsetof(quota_model_t, refresh_seconds) == 396, "model layout changed");
_Static_assert(offsetof(quota_model_t, selected_account_id) == 401, "model layout changed");
_Static_assert(offsetof(quota_model_t, last_known_time) == 440, "model layout changed");
_Static_assert(offsetof(quota_model_t, reserved_legacy_endpoint) == 448, "model layout changed");
_Static_assert(offsetof(quota_model_t, entry_count) == 2176, "model layout changed");
_Static_assert(offsetof(quota_model_t, entries) == 2180, "model layout changed");
_Static_assert(offsetof(quota_model_t, intent) == 4228, "model layout changed");
_Static_assert(sizeof(quota_model_t) == 4400, "model layout changed");

/* Rows dropped by quota_catalog_scrub_removed. The store frees their credential slots and
 * cached observations. A row from an interrupted upsert has no logical row to drop but still
 * names its slot. */
typedef struct {
    uint8_t count;
    struct {
        char logical_id[QUOTA_ACCOUNT_ID_BYTES + 1];
        bool owns_slot; /* Native rows and interrupted upserts; the legacy bytes alias a slot. */
        uint8_t slot;
        char credential_id[QUOTA_ACCOUNT_ID_BYTES + 1];
        uint32_t credential_generation;
    } rows[QUOTA_CATALOG_STORED_ROWS + 1];
} quota_catalog_removed_t;

/* Pure bounded helpers; persistence and ownership stay in the service. */
bool quota_catalog_valid(const quota_model_t *model);
/* Upgrade cleanup, run before quota_catalog_valid. Zeroes and compacts rows that the product
 * no longer has (other providers, other sources, pending rows), the pending network and the
 * legacy endpoint. Returns true when anything changed. Idempotent. */
bool quota_catalog_scrub_removed(quota_model_t *model, quota_catalog_removed_t *removed);
int quota_catalog_find(const quota_model_t *model, const char *logical_id);
bool quota_catalog_binding_equal(const quota_catalog_entry_t *a, const quota_catalog_entry_t *b);
bool quota_catalog_native_matches(const quota_catalog_entry_t *entry,
                                  const quota_portable_credential_t *credential);
/* Preserve identity/alias; only attach source observations to the active row. */
void quota_catalog_copy_observation(quota_snapshot_t *target, size_t target_index,
                                    const quota_snapshot_t *source, size_t source_index);
