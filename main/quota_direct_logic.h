#pragma once

#include "quota_logic.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define QUOTA_DIRECT_ACCESS_BYTES 8192
#define QUOTA_DIRECT_REFRESH_BYTES 4096
#define QUOTA_DIRECT_ID_TOKEN_BYTES 8192
#define QUOTA_DIRECT_PROVIDER_ID_BYTES 128
#define QUOTA_DIRECT_AUTH_ID_BYTES 256
#define QUOTA_DIRECT_CODE_BYTES 4096
#define QUOTA_DIRECT_VERIFIER_BYTES 128
#define QUOTA_DIRECT_USER_CODE_BYTES 64
#define QUOTA_DIRECT_BODY_BYTES 32768

typedef struct {
    char account_id[QUOTA_DIRECT_PROVIDER_ID_BYTES + 1];
    char user_id[QUOTA_DIRECT_PROVIDER_ID_BYTES + 1];
    char email[QUOTA_EMAIL_MAX_BYTES + 1];
    char plan[QUOTA_PLAN_MAX_BYTES + 1];
    uint64_t expires_at;
} quota_direct_identity_t;

typedef struct {
    char device_auth_id[QUOTA_DIRECT_AUTH_ID_BYTES + 1];
    char user_code[QUOTA_DIRECT_USER_CODE_BYTES + 1];
    uint32_t interval_seconds;
} quota_direct_device_code_t;

typedef struct {
    char authorization_code[QUOTA_DIRECT_CODE_BYTES + 1];
    char code_verifier[QUOTA_DIRECT_VERIFIER_BYTES + 1];
} quota_direct_authorization_t;

/* Metadata decoding only. This does not verify a JWT signature. Input tokens
 * must come exclusively from the fixed, certificate-verified auth endpoint. */
bool quota_direct_parse_identity(const char *jwt, quota_direct_identity_t *identity);
typedef struct quota_direct_tokens quota_direct_tokens_t;
/* Prepare owns detached token strings, never the input body. The HTTP owner
 * releases its body between stages, after the envelope DOM has been deleted.
 * Finish parses one JWT at a time and writes outputs only after all checks.
 * Destroy wipes every detached secret, including after failed validation. */
bool quota_direct_tokens_prepare(const char *body, size_t length, quota_direct_tokens_t **tokens);
bool quota_direct_tokens_finish(const quota_direct_tokens_t *tokens,
                               const quota_direct_identity_t *expected,
                               char *access, size_t access_capacity,
                               char *refresh, size_t refresh_capacity,
                               quota_direct_identity_t *identity);
void quota_direct_tokens_destroy(quota_direct_tokens_t *tokens);
bool quota_direct_parse_device_code(const char *body, size_t length,
                                    quota_direct_device_code_t *code);
bool quota_direct_parse_authorization(const char *body, size_t length,
                                      quota_direct_authorization_t *code);
bool quota_direct_parse_codex_usage(const char *body, size_t length,
                                    const char *expected_account_id,
                                    quota_account_t *account,
                                    quota_codex_extras_t *extras);
bool quota_direct_parse_reset_details(const char *body, size_t length,
                                      quota_codex_extras_t *extras);
bool quota_direct_parse_deepseek(const char *body, size_t length,
                                quota_balance_t *balance);
bool quota_direct_form_encode(const char *value, char *output, size_t capacity);
uint64_t quota_direct_parse_date(const char *date);
uint32_t quota_direct_retry_after(const char *header, uint64_t now);
bool quota_direct_token_is_safe(const char *value, size_t maximum);
void quota_direct_secure_clear(void *memory, size_t size);
