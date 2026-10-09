#pragma once

#include "quota_logic.h"

#define QUOTA_PORTABLE_NETWORKS 3
/* Setup protocol version. The page and the firmware must agree on it. */
#define QUOTA_PROTOCOL_VERSION 3
#define QUOTA_FIRMWARE_VERSION_BYTES 32
/* Base session length: hotspot 10 minutes, USB 2 minutes (QUOTA_USB_WINDOW_MS). Every command that
 * changes something tops the remaining time up to at least QUOTA_SESSION_TOPUP_MS; a session never
 * lasts longer than QUOTA_SESSION_MAX_MS in total. */
#define QUOTA_PORTABLE_SETUP_MS 600000ULL
#define QUOTA_SESSION_TOPUP_MS 300000ULL
#define QUOTA_SESSION_MAX_MS 1200000ULL
#define QUOTA_PORTABLE_LOGIN_MS 900000ULL
#define QUOTA_PORTABLE_ACCESS_BYTES 8192
#define QUOTA_PORTABLE_REFRESH_BYTES 4096
#define QUOTA_PORTABLE_KEY_BYTES 512
#define QUOTA_PORTABLE_IDENTITY_BYTES 128
/* Hotspot access code: 16 Crockford Base32 characters shown as XXXX-XXXX-XXXX-XXXX. */
#define QUOTA_PORTABLE_ACCESS_CODE_BYTES 19
/* Wrong access codes in a row after which the hotspot refuses every request. */
#define QUOTA_PORTABLE_ACCESS_FAILURES 5
#define QUOTA_PORTABLE_ERROR_BYTES 48
#define QUOTA_PORTABLE_URL_BYTES 128
#define QUOTA_PORTABLE_USER_CODE_BYTES 64
#define QUOTA_PORTABLE_COMMAND_BYTES 2048
#define QUOTA_PORTABLE_STATE_BYTES 16384

/* Validation result of a saved Wi-Fi network or account. A network that was saved before the last
 * restart is only SAVED: it is neither waiting nor failed until the Passport connects or fails. */
typedef enum {
    QUOTA_VALIDATION_SAVED = 0,
    QUOTA_VALIDATION_OK,
    QUOTA_VALIDATION_PENDING,
    QUOTA_VALIDATION_FAILED,
} quota_validation_t;

/* Persisted values: never reuse 1. */
typedef enum {
    QUOTA_ACCOUNT_DEVICE = 0,
    /* 1 reserved: removed source */
} quota_account_source_t;

typedef enum {
    QUOTA_PORTABLE_NETWORK_OFF = 0,
    QUOTA_PORTABLE_NETWORK_AP,
    QUOTA_PORTABLE_NETWORK_CONNECTING,
    QUOTA_PORTABLE_NETWORK_CONNECTED,
    QUOTA_PORTABLE_NETWORK_TIME_REQUIRED,
    QUOTA_PORTABLE_NETWORK_READY,
    QUOTA_PORTABLE_NETWORK_ERROR,
} quota_portable_network_state_t;

typedef enum {
    QUOTA_PORTABLE_LOGIN_IDLE = 0,
    QUOTA_PORTABLE_LOGIN_QUEUED,
    QUOTA_PORTABLE_LOGIN_CONNECTING,
    QUOTA_PORTABLE_LOGIN_REQUESTING_CODE,
    QUOTA_PORTABLE_LOGIN_WAITING,
    QUOTA_PORTABLE_LOGIN_EXCHANGING,
    QUOTA_PORTABLE_LOGIN_SUCCESS,
    QUOTA_PORTABLE_LOGIN_ERROR,
    QUOTA_PORTABLE_LOGIN_CANCELED,
    QUOTA_PORTABLE_LOGIN_EXPIRED,
} quota_portable_login_state_t;

typedef enum {
    QUOTA_PORTABLE_AUTH_PENDING = 0,
    QUOTA_PORTABLE_AUTH_READY,
    QUOTA_PORTABLE_AUTH_REAUTH,
    QUOTA_PORTABLE_AUTH_ERROR,
} quota_portable_auth_state_t;

/* Ephemeral AP secrets are for the physical device QR only, never public JSON. */
typedef struct {
    bool setup_active;
    bool setup_ready;
    /* A validation pass is running; the display stays on until it ends. */
    bool validating;
    /* A hotspot open is requested and has not started yet. */
    bool setup_opening;
    /* This hotspot session was closed with 完成设置: the validation result is on show. */
    bool setup_result;
    /* A device-side write is waiting for storage or has just been started (not a login state). */
    bool saving;
    /* The last factory reset request failed to erase; cleared by the next request. */
    bool factory_reset_failed;
    uint32_t setup_seconds_left;
    char setup_ssid[QUOTA_SSID_MAX_BYTES + 1];
    char setup_password[QUOTA_PASSWORD_MAX_BYTES + 1];
    char setup_secret[QUOTA_PORTABLE_ACCESS_CODE_BYTES + 1]; /* The access code. */
    char setup_page_url[QUOTA_PORTABLE_URL_BYTES + 1];
    quota_portable_network_state_t network_state;
    char network_ssid[QUOTA_SSID_MAX_BYTES + 1];
    char network_ip[16];
    char network_error[QUOTA_PORTABLE_ERROR_BYTES + 1];
    char storage_error[32]; /* Sanitized owner error; never a credential or storage record. */
    quota_portable_login_state_t login_state;
    char login_account_id[QUOTA_ACCOUNT_ID_BYTES + 1];
    char login_url[QUOTA_PORTABLE_URL_BYTES + 1];
    char login_user_code[QUOTA_PORTABLE_USER_CODE_BYTES + 1];
    uint32_t login_seconds_left;
    char login_error[QUOTA_PORTABLE_ERROR_BYTES + 1];
    bool auth_hold_awake;
    /* Indexed like the active snapshot; transport errors are not Wi-Fi state. */
    char account_errors[QUOTA_MAX_ACCOUNTS][QUOTA_PORTABLE_ERROR_BYTES + 1];
    uint64_t account_retry_at[QUOTA_MAX_ACCOUNTS];
    /* Password-free snapshot of the committed saved network inventory. */
    uint8_t saved_network_count;
    uint8_t selected_saved_network;
    char saved_network_ssids[QUOTA_PORTABLE_NETWORKS][QUOTA_SSID_MAX_BYTES + 1];
    /* Validation result per saved network (quota_validation_t) with its error code, and how many
     * networks, accounts and queued authorizations wait or failed: the device screen shows them. */
    uint8_t saved_network_validation[QUOTA_PORTABLE_NETWORKS];
    char saved_network_errors[QUOTA_PORTABLE_NETWORKS][QUOTA_PORTABLE_ERROR_BYTES + 1];
    uint8_t pending_items, failed_items;
    /* Validation result of each account, indexed like the active snapshot (quota_validation_t). */
    uint8_t account_validation[QUOTA_MAX_ACCOUNTS];
    char firmware[QUOTA_FIRMWARE_VERSION_BYTES + 1];
} quota_portable_view_t;

typedef struct {
    char ssid[QUOTA_SSID_MAX_BYTES + 1];
    char password[QUOTA_PASSWORD_MAX_BYTES + 1];
} quota_portable_network_t;

/* Allocate/load one at a time; never keep an array of eight token bundles. */
typedef struct {
    char id[QUOTA_ACCOUNT_ID_BYTES + 1];
    quota_provider_t provider;
    uint8_t slot;
    uint32_t generation;
    bool tombstone;
    quota_portable_auth_state_t auth_state;
    char server_account_id[QUOTA_PORTABLE_IDENTITY_BYTES + 1];
    char server_user_id[QUOTA_PORTABLE_IDENTITY_BYTES + 1];
    char email[QUOTA_EMAIL_MAX_BYTES + 1];
    char plan[QUOTA_PLAN_MAX_BYTES + 1];
    char label[QUOTA_PLAN_MAX_BYTES + 1];
    uint64_t expires_at;
    bool refresh_inflight;
    char access_token[QUOTA_PORTABLE_ACCESS_BYTES + 1];
    char refresh_token[QUOTA_PORTABLE_REFRESH_BYTES + 1];
    char api_key[QUOTA_PORTABLE_KEY_BYTES + 1];
} quota_portable_credential_t;

typedef quota_portable_credential_t quota_direct_credential_t;

typedef enum {
    QUOTA_PORTABLE_OP_INVALID = 0,
    QUOTA_PORTABLE_OP_NETWORK_SAVE,
    QUOTA_PORTABLE_OP_NETWORK_REMOVE,
    QUOTA_PORTABLE_OP_DEEPSEEK_SAVE,
    QUOTA_PORTABLE_OP_CODEX_QUEUE,
    QUOTA_PORTABLE_OP_ACCOUNT_REMOVE,
    QUOTA_PORTABLE_OP_SETTINGS_SAVE,
    QUOTA_PORTABLE_OP_SETUP_CLOSE,
    QUOTA_PORTABLE_OP_REFRESH,
    QUOTA_PORTABLE_OP_RECONNECT,
    QUOTA_PORTABLE_OP_OPERATION_CANCEL,
    QUOTA_PORTABLE_OP_VALIDATE, /* USB only; the hotspot validates after setup_close */
} quota_portable_op_t;

typedef enum { QUOTA_SETUP_AP = 0, QUOTA_SETUP_USB } quota_setup_transport_t;

/* Commands that change something. They top the session up, and they are refused (busy) while a
 * validation pass runs. State reads, refresh, reconnect, cancel and setup_close are not. */
static inline bool quota_portable_op_changes_setup(quota_portable_op_t op)
{
    switch (op) {
    case QUOTA_PORTABLE_OP_NETWORK_SAVE:
    case QUOTA_PORTABLE_OP_NETWORK_REMOVE:
    case QUOTA_PORTABLE_OP_DEEPSEEK_SAVE:
    case QUOTA_PORTABLE_OP_CODEX_QUEUE:
    case QUOTA_PORTABLE_OP_ACCOUNT_REMOVE:
    case QUOTA_PORTABLE_OP_SETTINGS_SAVE:
    case QUOTA_PORTABLE_OP_VALIDATE:
        return true;
    default:
        return false;
    }
}

/* This private queue payload may hold secrets; wipe after consumption. */
typedef struct {
    char request_id[9];
    quota_portable_op_t op;
    char account_id[QUOTA_ACCOUNT_ID_BYTES + 1];
    uint8_t network_index; /* network_save: UINT8_MAX inserts or updates by SSID. */
    char ssid[QUOTA_SSID_MAX_BYTES + 1];
    char password[QUOTA_PASSWORD_MAX_BYTES + 1];
    bool open_network;
    char api_key[QUOTA_PORTABLE_KEY_BYTES + 1];
    char label[QUOTA_PLAN_MAX_BYTES + 1];
    uint64_t phone_utc;
    uint16_t refresh_seconds;
    bool auto_refresh;
    uint16_t screen_timeout_seconds;
    char target_request_id[9];
    /* Captured by the service, never accepted from a phone request. */
    uint32_t accepted_config_generation;
    quota_setup_transport_t accepted_transport;
    uint64_t accepted_usb_window; /* Identifies the USB window; see quota_usb_window_id(). */
} quota_portable_command_t;

typedef enum {
    QUOTA_PORTABLE_SUBMIT_ACCEPTED = 0,
    QUOTA_PORTABLE_SUBMIT_BUSY,
    QUOTA_PORTABLE_SUBMIT_CLOSED,
    QUOTA_PORTABLE_SUBMIT_INVALID,
    QUOTA_PORTABLE_SUBMIT_CONFLICT,
} quota_portable_submit_result_t;

/* Volatile writes prevent credential clearing from being optimized away. */
static inline void quota_portable_clear_secret(void *memory, size_t length)
{
    volatile unsigned char *bytes = (volatile unsigned char *)memory;
    while (length-- != 0)
        *bytes++ = 0;
}
