#pragma once

#include "quota_logic.h"

#define QUOTA_PORTABLE_NETWORKS 3
#define QUOTA_PORTABLE_SETUP_MS 600000ULL
#define QUOTA_PORTABLE_LOGIN_MS 900000ULL
#define QUOTA_PORTABLE_ACCESS_BYTES 8192
#define QUOTA_PORTABLE_REFRESH_BYTES 4096
#define QUOTA_PORTABLE_KEY_BYTES 512
#define QUOTA_PORTABLE_IDENTITY_BYTES 128
#define QUOTA_PORTABLE_SESSION_BYTES 43
#define QUOTA_PORTABLE_ERROR_BYTES 48
#define QUOTA_PORTABLE_URL_BYTES 128
#define QUOTA_PORTABLE_USER_CODE_BYTES 64
#define QUOTA_PORTABLE_COMMAND_BYTES 2048
#define QUOTA_PORTABLE_STATE_BYTES 16384

typedef enum { QUOTA_ACCOUNT_DEVICE, QUOTA_ACCOUNT_LEGACY } quota_account_source_t;

/* v1 storage compatibility only; no product-wide mode switching. */
typedef enum {
    QUOTA_MODE_COMPANION = 0,
    QUOTA_MODE_DIRECT = 1,
} quota_mode_t;

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
    quota_mode_t mode;
    bool setup_active;
    bool setup_ready;
    uint32_t setup_seconds_left;
    char setup_ssid[QUOTA_SSID_MAX_BYTES + 1];
    char setup_password[QUOTA_PASSWORD_MAX_BYTES + 1];
    char setup_secret[QUOTA_PORTABLE_SESSION_BYTES + 1];
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
    quota_account_source_t account_sources[QUOTA_MAX_ACCOUNTS];
    /* Password-free snapshot of the committed saved network inventory. */
    uint8_t saved_network_count;
    uint8_t selected_saved_network;
    char saved_network_ssids[QUOTA_PORTABLE_NETWORKS][QUOTA_SSID_MAX_BYTES + 1];
    bool pending_saved_network_present;
    char pending_saved_network_ssid[QUOTA_SSID_MAX_BYTES + 1];
} quota_portable_view_t;

typedef struct {
    char ssid[QUOTA_SSID_MAX_BYTES + 1];
    char password[QUOTA_PASSWORD_MAX_BYTES + 1];
} quota_portable_network_t;

typedef struct {
    quota_mode_t mode;
    uint8_t network_count;
    uint8_t selected_network;
    quota_portable_network_t networks[QUOTA_PORTABLE_NETWORKS];
    uint16_t refresh_seconds;
    bool auto_refresh;
    uint16_t screen_timeout_seconds;
    char selected_account_id[QUOTA_ACCOUNT_ID_BYTES + 1];
    uint64_t last_known_time;
} quota_portable_config_t;

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

typedef struct {
    char id[QUOTA_ACCOUNT_ID_BYTES + 1];
    quota_provider_t provider;
    uint32_t generation;
} quota_portable_account_ref_t;

typedef enum {
    QUOTA_PORTABLE_OP_INVALID = 0,
    QUOTA_PORTABLE_OP_NETWORK_SAVE,
    QUOTA_PORTABLE_OP_NETWORK_SCAN,
    QUOTA_PORTABLE_OP_DEEPSEEK_SAVE,
    QUOTA_PORTABLE_OP_CODEX_QUEUE,
    QUOTA_PORTABLE_OP_CODEX_LAUNCH,
    QUOTA_PORTABLE_OP_ACCOUNT_REMOVE,
    QUOTA_PORTABLE_OP_SETTINGS_SAVE,
    QUOTA_PORTABLE_OP_MODE_SELECT,
    QUOTA_PORTABLE_OP_SETUP_CLOSE,
    QUOTA_PORTABLE_OP_REFRESH,
    QUOTA_PORTABLE_OP_RECONNECT,
    QUOTA_PORTABLE_OP_OPERATION_CANCEL,
    QUOTA_PORTABLE_OP_ACCOUNT_ACTIVATE,
    QUOTA_PORTABLE_OP_ACCOUNT_DEACTIVATE,
    QUOTA_PORTABLE_OP_EXTERNAL_IMPORT,
    QUOTA_PORTABLE_OP_NETWORK_ACTIVATE,
    QUOTA_PORTABLE_OP_COLLECTOR_CONFIGURE, /* Owner-only USB endpoint operation. */
} quota_portable_op_t;

typedef enum { QUOTA_SETUP_AP = 0, QUOTA_SETUP_USB } quota_setup_transport_t;

/* This private queue payload may hold secrets; wipe after consumption. */
typedef struct {
    char request_id[9];
    quota_portable_op_t op;
    char account_id[QUOTA_ACCOUNT_ID_BYTES + 1];
    uint8_t network_index; /* UINT8_MAX means insert/update by SSID. */
    char ssid[QUOTA_SSID_MAX_BYTES + 1];
    char password[QUOTA_PASSWORD_MAX_BYTES + 1];
    bool open_network;
    char api_key[QUOTA_PORTABLE_KEY_BYTES + 1];
    char label[QUOTA_PLAN_MAX_BYTES + 1];
    uint64_t phone_utc;
    uint16_t refresh_seconds;
    bool auto_refresh;
    uint16_t screen_timeout_seconds;
    quota_mode_t mode;
    char target_request_id[9];
    char replace_active_id[QUOTA_ACCOUNT_ID_BYTES + 1];
    char remote_account_id[QUOTA_ACCOUNT_ID_BYTES + 1];
    uint8_t replace_index;
    /* Captured by the service, never accepted from a phone request. */
    quota_mode_t accepted_mode;
    uint32_t accepted_config_generation;
    quota_setup_transport_t accepted_transport;
    uint64_t accepted_usb_deadline;
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
    while (length-- != 0) *bytes++ = 0;
}
