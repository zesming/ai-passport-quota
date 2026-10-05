#pragma once

#include "quota_portable.h"

#define QUOTA_CATALOG_CAPACITY (2 * QUOTA_MAX_ACCOUNTS)

typedef enum { QUOTA_ACCOUNT_ACTIVE, QUOTA_ACCOUNT_PENDING } quota_account_activity_t;

typedef union {
    struct {
        uint8_t slot;
        char credential_id[QUOTA_ACCOUNT_ID_BYTES + 1];
        uint32_t credential_generation;
    } native;
    struct {
        uint32_t endpoint_epoch;
        char remote_id[QUOTA_ACCOUNT_ID_BYTES + 1];
    } legacy;
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

typedef struct {
    bool enabled;
    uint32_t epoch;
    char base_url[QUOTA_BASE_URL_MAX_BYTES + 1];
    char pair_token[QUOTA_PAIR_TOKEN_BYTES + 1];
    char server_cert_pem[QUOTA_CERT_MAX_BYTES + 1];
    uint64_t trusted_time;
} quota_legacy_endpoint_t;

typedef enum { QUOTA_INTENT_NONE, QUOTA_INTENT_UPSERT_NATIVE,
               QUOTA_INTENT_DELETE_NATIVE } quota_intent_kind_t;
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

typedef struct {
    uint8_t network_count, selected_network;
    quota_portable_network_t networks[QUOTA_PORTABLE_NETWORKS];
    bool pending_network_present;
    quota_portable_network_t pending_network;
    uint16_t refresh_seconds, screen_timeout_seconds;
    bool auto_refresh;
    char selected_account_id[QUOTA_ACCOUNT_ID_BYTES + 1];
    uint64_t last_known_time;
    quota_legacy_endpoint_t legacy;
    uint8_t entry_count;
    quota_catalog_entry_t entries[QUOTA_CATALOG_CAPACITY];
    quota_model_intent_t intent;
} quota_model_t;

/* Pure bounded helpers; persistence and ownership stay in the service. */
bool quota_catalog_valid(const quota_model_t *model);
int quota_catalog_find(const quota_model_t *model, const char *logical_id);
size_t quota_catalog_count(const quota_model_t *model, quota_account_activity_t activity);
bool quota_catalog_binding_equal(const quota_catalog_entry_t *a,
                                 const quota_catalog_entry_t *b);
bool quota_catalog_native_matches(const quota_catalog_entry_t *entry,
                                  const quota_portable_credential_t *credential);
/* Preserve identity/alias; only attach source observations to the active row. */
void quota_catalog_copy_observation(quota_snapshot_t *target, size_t target_index,
                                   const quota_snapshot_t *source, size_t source_index);
