#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define QUOTA_MAX_ACCOUNTS 8
#define QUOTA_MAX_SNAPSHOT_BYTES 8192
#define QUOTA_MAX_PROVISION_FRAME_BYTES 4096
#define QUOTA_ACCOUNT_ID_BYTES 32
#define QUOTA_EMAIL_MAX_BYTES 128
#define QUOTA_PLAN_MAX_BYTES 32
#define QUOTA_SSID_MAX_BYTES 32
#define QUOTA_PASSWORD_MAX_BYTES 64
#define QUOTA_BASE_URL_MAX_BYTES 128
#define QUOTA_PAIR_TOKEN_BYTES 43
#define QUOTA_CERT_MAX_BYTES 1536
#define QUOTA_REFRESH_DEFAULT_SECONDS 300
#define QUOTA_PAIRING_WINDOW_MS 120000

typedef enum {
    QUOTA_PROVIDER_CODEX = 0,
    QUOTA_PROVIDER_CLAUDE,
} quota_provider_t;

typedef enum {
    QUOTA_STATUS_OK = 0,
    QUOTA_STATUS_WAITING,
    QUOTA_STATUS_EXPIRED,
    QUOTA_STATUS_ERROR,
    QUOTA_STATUS_UNSUPPORTED,
} quota_source_status_t;

typedef struct {
    bool present;
    uint8_t remaining_percent;
    bool has_resets_at;
    uint64_t resets_at;
} quota_window_t;

typedef struct {
    char id[QUOTA_ACCOUNT_ID_BYTES + 1];
    quota_provider_t provider;
    char email[QUOTA_EMAIL_MAX_BYTES + 1];
    char plan[QUOTA_PLAN_MAX_BYTES + 1];
    quota_source_status_t status;
    bool has_observed_at;
    uint64_t observed_at;
    quota_window_t five_hour;
    quota_window_t seven_day;
} quota_account_t;

typedef struct {
    uint64_t server_time;
    uint64_t revision;
    uint16_t refresh_seconds;
    bool auto_refresh;
    uint8_t account_count;
    quota_account_t accounts[QUOTA_MAX_ACCOUNTS];
} quota_snapshot_t;

typedef struct {
    char request_id[9];
    char ssid[QUOTA_SSID_MAX_BYTES + 1];
    char password[QUOTA_PASSWORD_MAX_BYTES + 1];
    char base_url[QUOTA_BASE_URL_MAX_BYTES + 1];
    char pair_token[QUOTA_PAIR_TOKEN_BYTES + 1];
    char server_cert_pem[QUOTA_CERT_MAX_BYTES + 1];
    uint64_t server_time;
    uint16_t refresh_seconds;
    bool auto_refresh;
    char selected_account_id[QUOTA_ACCOUNT_ID_BYTES + 1];
} quota_device_config_t;

typedef enum {
    QUOTA_METRIC_VALUE = 0,
    QUOTA_METRIC_UNAVAILABLE,
    QUOTA_METRIC_WAITING_FOR_SOURCE,
} quota_metric_state_t;

typedef enum {
    QUOTA_SCREEN_HOME = 0,
    QUOTA_SCREEN_SETTINGS,
    QUOTA_SCREEN_ACCOUNTS,
    QUOTA_SCREEN_INTERVAL,
    QUOTA_SCREEN_SETUP,
} quota_screen_t;

typedef enum {
    QUOTA_INPUT_UP = 0,
    QUOTA_INPUT_DOWN,
    QUOTA_INPUT_OK_SHORT,
    QUOTA_INPUT_OK_LONG,
    QUOTA_INPUT_OTHER,
} quota_input_t;

typedef enum {
    QUOTA_ACTION_NONE = 0,
    QUOTA_ACTION_REFRESH,
    QUOTA_ACTION_APPLY_SETTINGS,
    QUOTA_ACTION_PERSIST_SELECTION,
} quota_action_t;

typedef struct {
    quota_screen_t screen;
    quota_screen_t setup_return_screen;
    uint8_t selected_account;
    uint8_t account_focus;
    uint8_t settings_focus;
    uint8_t interval_focus;
    uint16_t refresh_seconds;
    bool auto_refresh;
    bool configured;
} quota_navigation_t;

typedef struct {
    char bytes[QUOTA_MAX_PROVISION_FRAME_BYTES + 1];
    size_t length;
    bool discarding_overlong_line;
} quota_frame_decoder_t;

typedef enum {
    QUOTA_FRAME_PENDING = 0,
    QUOTA_FRAME_COMPLETE,
    QUOTA_FRAME_TOO_LONG,
} quota_frame_result_t;

bool quota_utf8_is_valid(const char *text, size_t length);
bool quota_id_is_valid(const char *id);
bool quota_url_is_private_ipv4(const char *url, char host_out[16]);
bool quota_pair_token_is_valid(const char *token);
void quota_copy_display_ascii(const char *source, char *destination, size_t capacity);
void quota_frame_decoder_init(quota_frame_decoder_t *decoder);
quota_frame_result_t quota_frame_decoder_feed(quota_frame_decoder_t *decoder, char byte,
                                              const char **frame_out,
                                              size_t *frame_length_out);

bool quota_parse_snapshot(const char *json, size_t json_length, quota_snapshot_t *snapshot);
bool quota_parse_provision_frame(const char *frame, size_t frame_length,
                                 quota_device_config_t *config,
                                 char request_id_out[9], const char **error_code_out);

bool quota_data_is_stale(uint64_t now, bool has_observed_at, uint64_t observed_at,
                         uint16_t refresh_seconds);
bool quota_pairing_window_active(bool setup_screen_open, uint64_t now_ms,
                                 uint64_t opened_at_ms);
bool quota_display_should_dim(uint64_t now_ms, uint64_t last_input_ms,
                              bool pairing_active);
quota_metric_state_t quota_metric_state(const quota_window_t *window, uint64_t now);
quota_action_t quota_navigation_handle(quota_navigation_t *navigation,
                                       quota_input_t input, uint8_t account_count);
void quota_navigation_init(quota_navigation_t *navigation, bool configured,
                           uint16_t refresh_seconds, bool auto_refresh,
                           uint8_t account_count);
void quota_navigation_sync_settings(quota_navigation_t *navigation,
                                     uint16_t refresh_seconds, bool auto_refresh);
int quota_find_account_by_id(const quota_snapshot_t *snapshot, const char *id);
