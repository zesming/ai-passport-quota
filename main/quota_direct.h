#pragma once

#include "quota_portable.h"
#include "quota_direct_logic.h"

#define QUOTA_DIRECT_VERIFICATION_URL "https://auth.openai.com/codex/device"
/* Experimental reproduction of the official Codex client. This identifier is
 * not a registered OAuth client for this product. Override only deliberately. */
#ifndef QUOTA_DIRECT_CODEX_CLIENT_ID
#define QUOTA_DIRECT_CODEX_CLIENT_ID "app_EMoamEEZ73f0CkXaXp7hrann"
#endif

typedef enum {
    QUOTA_DIRECT_OK = 0,
    QUOTA_DIRECT_WAITING,
    QUOTA_DIRECT_DEFERRED,
    QUOTA_DIRECT_AUTH_REQUIRED,
    QUOTA_DIRECT_LOGIN_DISABLED,
    QUOTA_DIRECT_EXPIRED,
    QUOTA_DIRECT_CANCELED,
    QUOTA_DIRECT_RATE_LIMITED,
    QUOTA_DIRECT_NETWORK_ERROR,
    QUOTA_DIRECT_PROTOCOL_ERROR,
    QUOTA_DIRECT_TIME_REQUIRED,
    QUOTA_DIRECT_STORAGE_ERROR,
    QUOTA_DIRECT_PERSIST_PENDING,
    QUOTA_DIRECT_NO_MEMORY,
    QUOTA_DIRECT_UNSUPPORTED,
    QUOTA_DIRECT_TLS_ERROR,
    QUOTA_DIRECT_RESOURCE_ERROR,
    QUOTA_DIRECT_RESPONSE_TOO_LARGE,
} quota_direct_result_code_t;

typedef struct {
    quota_direct_result_code_t code;
    int http_status;
    int esp_error;
    int tls_error;
    uint32_t retry_after_seconds;
    uint64_t source_epoch;
    bool source_valid;
    bool details_available;
    quota_account_t account;
    quota_balance_t balance;
    quota_codex_extras_t extras;
} quota_direct_result_t;

typedef struct {
    void *context;
    /* Before each HTTP admission: includes awake, network/mode and identity. */
    bool (*admit)(void *context, const char *id, uint32_t generation);
    /* After authentication: identity only; must NOT depend on screen state. */
    bool (*account_current)(void *context, const char *id, uint32_t generation);
    /* Atomically commit the entire same-account bundle including inflight. */
    bool (*persist)(void *context, const quota_direct_credential_t *credential);
} quota_direct_hooks_t;

typedef struct {
    const char *url;
    bool post;
    const char *content_type;
    /* Sole-owned heap POST body, transferred into perform; transport may clear
     * it after response starts. Borrowed/injected string literals are invalid. */
    char *body;
    const char *bearer;
    const char *account_id;
    const char *logical_id;
    uint32_t generation;
} quota_direct_http_request_t;

typedef struct {
    char *body;
    size_t capacity;
    size_t length;
    int status;
    bool admitted;
    bool overflow;
    bool allocation_failed;
    bool redirected;
    bool resource_limited;
    int esp_error;
    int tls_error;
    char date[65];
    char retry_after[65];
} quota_direct_http_response_t;

/* Injection for deterministic host tests. Production passes NULL and uses the
 * fixed-origin ESP HTTPS transport with certificate bundle verification. */
typedef bool (*quota_direct_transport_t)(void *context, quota_direct_http_request_t *request,
                                         quota_direct_http_response_t *response);

typedef struct quota_direct quota_direct_t;
typedef struct {
    bool active;
    bool exchanging;
    char account_id[QUOTA_ACCOUNT_ID_BYTES + 1];
    char verification_url[QUOTA_PORTABLE_URL_BYTES + 1];
    char user_code[QUOTA_DIRECT_USER_CODE_BYTES + 1];
    uint32_t remaining_seconds;
    uint32_t poll_after_seconds;
} quota_direct_login_view_t;

quota_direct_t *quota_direct_create(const quota_direct_hooks_t *hooks,
                                    quota_direct_transport_t transport, void *transport_context);
/* Each operation is synchronous and bounded, called by the ONE network owner.
 * Login steps perform at most one HTTP phase; no fifteen-minute blocking loop.
 * Credentials are caller-owned. A successful token response updates them only
 * after complete validation. PERSIST_PENDING borrows that SAME credential
 * pointer: retain it, unmodified, until has_pending_persist becomes false.
 * No other operation may reuse its backing storage while pending. The
 * provider never frees or wipes caller-owned credentials, including on retry
 * completion and account cancellation. */
quota_direct_result_t quota_direct_login_begin(quota_direct_t *direct,
                                               const quota_direct_credential_t *credential,
                                               uint64_t monotonic_ms, uint64_t valid_utc);
quota_direct_result_t quota_direct_login_step(quota_direct_t *direct,
                                              quota_direct_credential_t *credential,
                                              uint64_t monotonic_ms, uint64_t valid_utc);
void quota_direct_login_cancel(quota_direct_t *direct);
void quota_direct_login_view(const quota_direct_t *direct, uint64_t monotonic_ms,
                             quota_direct_login_view_t *view);
quota_direct_result_t quota_direct_query(quota_direct_t *direct,
                                         quota_direct_credential_t *credential, uint64_t valid_utc);
quota_direct_result_t quota_direct_refresh(quota_direct_t *direct,
                                           quota_direct_credential_t *credential,
                                           uint64_t valid_utc);
/* When persistence fails after a successful exchange, this is the ONLY retry.
 * It performs no HTTP and retains the received bundle until committed/canceled
 * by account deletion. Mode/display changes are not account deletion. */
quota_direct_result_t quota_direct_retry_persist(quota_direct_t *direct);
bool quota_direct_has_pending_persist(const quota_direct_t *direct);
