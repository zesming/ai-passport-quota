#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define QUOTA_MAX_ACCOUNTS 8
#define QUOTA_MAX_PROVISION_FRAME_BYTES 4096
#define QUOTA_ACCOUNT_ID_BYTES 32
#define QUOTA_EMAIL_MAX_BYTES 128
#define QUOTA_PLAN_MAX_BYTES 32
#define QUOTA_SSID_MAX_BYTES 32
#define QUOTA_PASSWORD_MAX_BYTES 64
#define QUOTA_REFRESH_DEFAULT_SECONDS 300
#define QUOTA_SCREEN_TIMEOUT_DEFAULT_SECONDS 120
#define QUOTA_SCREEN_TIMEOUT_COUNT 6
#define QUOTA_BALANCE_AMOUNT_BYTES 20
#define QUOTA_BALANCE_CURRENCIES 2
#define QUOTA_CREDITS_BALANCE_BYTES 32
#define QUOTA_USB_WINDOW_MS 120000
/* A USB session opener silent this long has lost its page and may be replaced by a new opener. */
#define QUOTA_USB_OPENER_IDLE_MS 6000

/* Persisted values: never reuse 1. */
typedef enum {
    QUOTA_PROVIDER_CODEX = 0,
    /* 1 reserved: removed provider */
    QUOTA_PROVIDER_DEEPSEEK = 2,
} quota_provider_t;
_Static_assert(QUOTA_PROVIDER_CODEX == 0 && QUOTA_PROVIDER_DEEPSEEK == 2,
               "provider values are persisted");

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
    char currency[4];
    char total_balance[QUOTA_BALANCE_AMOUNT_BYTES + 1];
    char granted_balance[QUOTA_BALANCE_AMOUNT_BYTES + 1];
    char topped_up_balance[QUOTA_BALANCE_AMOUNT_BYTES + 1];
} quota_currency_balance_t;

/* Sidecar leaves the account and NVS quota-cache layouts unchanged. */
typedef struct {
    bool present;
    bool is_available;
    uint8_t currency_count;
    char label[QUOTA_PLAN_MAX_BYTES + 1];
    quota_currency_balance_t balance_infos[QUOTA_BALANCE_CURRENCIES];
} quota_balance_t;

/* Optional Codex metadata stays in RAM; the NVS accounts are unchanged. */
typedef struct {
    bool has_banked_reset;
    uint64_t available_resets;
    bool has_next_reset_expiry;
    uint64_t next_reset_expires_at;
    bool has_credits;
    bool unlimited_credits;
    char credits_balance[QUOTA_CREDITS_BALANCE_BYTES + 1];
} quota_codex_extras_t;

typedef struct {
    uint64_t server_time;
    uint64_t revision;
    uint16_t refresh_seconds;
    bool auto_refresh;
    bool has_screen_timeout_seconds;
    uint16_t screen_timeout_seconds;
    uint8_t account_count;
    quota_account_t accounts[QUOTA_MAX_ACCOUNTS];
    quota_balance_t balances[QUOTA_MAX_ACCOUNTS];
    quota_codex_extras_t codex_extras[QUOTA_MAX_ACCOUNTS];
} quota_snapshot_t;

typedef struct {
    uint16_t refresh_seconds;
    bool auto_refresh;
    bool has_screen_timeout_seconds;
    uint16_t screen_timeout_seconds;
} quota_settings_t;

typedef enum {
    QUOTA_METRIC_VALUE = 0,
    QUOTA_METRIC_UNAVAILABLE,
    QUOTA_METRIC_WAITING_FOR_SOURCE,
} quota_metric_state_t;

typedef enum {
    QUOTA_SCREEN_HOME = 0, /* account card, or the welcome screen while there is no account */
    QUOTA_SCREEN_MENU,
    QUOTA_SCREEN_HOTSPOT, /* three pages, then the validation result once it is closed */
    QUOTA_SCREEN_USB,
    QUOTA_SCREEN_REFRESH, /* option list */
    QUOTA_SCREEN_SLEEP,   /* option list */
    QUOTA_SCREEN_INFO,
    QUOTA_SCREEN_AUTH,
    QUOTA_SCREEN_CONFIRM,
} quota_screen_t;

#define QUOTA_MENU_ITEMS 5
#define QUOTA_REFRESH_OPTIONS 5 /* manual, then 1, 5, 15 and 30 minutes */
#define QUOTA_HOTSPOT_PAGES 3
#define QUOTA_NOTICE_MS 2500

typedef enum {
    QUOTA_CONFIRM_FACTORY_RESET = 0,
    QUOTA_CONFIRM_CANCEL_AUTH,
} quota_confirm_kind_t;

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
    QUOTA_ACTION_OPEN_HOTSPOT,
    QUOTA_ACTION_RENEW_HOTSPOT, /* OK on a closed hotspot screen: open it again */
    QUOTA_ACTION_CLOSE_HOTSPOT,
    QUOTA_ACTION_OPEN_USB,
    QUOTA_ACTION_CLOSE_USB,
    QUOTA_ACTION_CANCEL_AUTH,
    QUOTA_ACTION_FACTORY_RESET,
} quota_action_t;

typedef struct {
    quota_screen_t screen;
    quota_screen_t return_screen; /* where a long OK leaves hotspot, USB and the confirm box to */
    uint8_t selected_account;
    uint8_t menu_focus; /* remembered while there is an account, until restart */
    uint8_t option_focus;
    uint8_t hotspot_page;
    quota_confirm_kind_t confirm_kind;
    uint8_t confirm_focus;  /* 0 is always the harmless choice */
    bool factory_resetting; /* the erase is under way: every key is ignored */
    bool factory_failed;    /* the erase failed: the box says so and only a long OK works */
    bool sleep_notice;      /* the footer says why a long DOWN did not switch the screen off */
    uint64_t sleep_notice_until_ms;
    uint16_t refresh_seconds;
    bool auto_refresh;
    uint16_t screen_timeout_seconds;
    bool configured;
} quota_navigation_t;

/* What the hotspot screen is doing, and so what OK does there. */
typedef enum {
    QUOTA_HOTSPOT_CLOSED = 0, /* closed (with or without a result): OK opens it again */
    QUOTA_HOTSPOT_SHOWING,    /* the three pages are shown: OK turns the page */
    QUOTA_HOTSPOT_BUSY,       /* opening, validating or in error: OK does nothing */
} quota_hotspot_state_t;

/* What the service knows that changes what a key does. The zero value is a plain idle device. */
typedef struct {
    uint8_t account_count;
    bool auth_active; /* ChatGPT authorization is running: OK on the home screen shows it */
    quota_hotspot_state_t hotspot;
    bool usb_window_open; /* the USB window is open or being prepared */
} quota_navigation_context_t;

typedef enum {
    QUOTA_KEY_PRESS = 0,
    QUOTA_KEY_CLICK,
    QUOTA_KEY_DOUBLE,
    QUOTA_KEY_LONG,
} quota_key_event_t;

typedef struct {
    uint64_t last_input_ms;
    bool sleeping;
    bool consume_wake_gesture;
    /* A setup session (hotspot, USB, authorization) keeps the screen on; set by the tick. */
    bool session_open;
    /* A long DOWN was refused because of the session: the interface says why, then clears it. */
    bool sleep_blocked;
} quota_display_state_t;

/* The status line of an account card shows the first of these that applies. */
typedef enum {
    QUOTA_STATUS_LINE_STORAGE_ERROR = 0,
    QUOTA_STATUS_LINE_AUTHORIZING,
    QUOTA_STATUS_LINE_REAUTH,
    QUOTA_STATUS_LINE_UNVERIFIED,
    QUOTA_STATUS_LINE_WIFI_FAILED,
    QUOTA_STATUS_LINE_RATE_LIMITED,
    QUOTA_STATUS_LINE_UPDATE_FAILED,
    QUOTA_STATUS_LINE_REFRESHING,
    QUOTA_STATUS_LINE_UPDATED,
    QUOTA_STATUS_LINE_NO_DATA,
} quota_status_line_t;

typedef struct {
    bool storage_error;
    bool authorizing;
    bool reauth_needed;
    uint8_t unverified_items; /* saved, never validated */
    bool wifi_failed;
    bool rate_limited;
    bool update_failed;
    bool refreshing;
    bool has_observed_at;
} quota_status_input_t;

extern const uint16_t quota_screen_timeouts[QUOTA_SCREEN_TIMEOUT_COUNT];

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
void quota_copy_display_ascii(const char *source, char *destination, size_t capacity);
void quota_copy_display_plan(const char *source, char *destination, size_t capacity);
/* "3 天 5 小时", "2 小时 14 分", "9 分钟"; under a minute is "不到 1 分钟". */
void quota_format_duration(uint64_t seconds, char *output, size_t capacity);
void quota_format_reset_time(const quota_window_t *window, uint64_t now, bool clock_synchronized,
                             char *output, size_t capacity);
/* The banked-reset note of the ChatGPT card: "可用重置 2 次 · 3 天 5 小时后过期" (the duration is
 * the one of window resets). Only the count when no expiry is known, "待校时" in place of the
 * duration while the clock is not synchronized, "等待新数据" once the expiry has passed. Empty when
 * there is no banked reset. */
void quota_format_banked_resets(const quota_codex_extras_t *extras, uint64_t now,
                                bool clock_synchronized, char *output, size_t capacity);
bool quota_refresh_seconds_is_valid(uint64_t seconds);
bool quota_screen_timeout_is_valid(uint64_t seconds);
bool quota_balance_is_valid(const quota_balance_t *balance);
const quota_currency_balance_t *quota_balance_cny(const quota_balance_t *balance);
/* The entry the home card shows: CNY when present, otherwise the first one. NULL when none. */
const quota_currency_balance_t *quota_balance_primary(const quota_balance_t *balance);

/* A money amount split for drawing: "-¥12.30" is prefix "-¥" and number "12.30"; an unknown
 * currency has no symbol and appends its code instead ("123.45" and " EUR"). The amount stays the
 * provider's decimal string; it is never converted to a number. */
typedef struct {
    char prefix[8];
    char number[QUOTA_BALANCE_AMOUNT_BYTES + 1];
    char suffix[8];
} quota_money_t;
void quota_money_parts(const char *currency, const char *amount, quota_money_t *money);
/* The parts joined: "¥123.45", "$12.30", "123.45 EUR". */
void quota_format_money(const char *currency, const char *amount, char *output, size_t capacity);
/* A credits balance as a whole number for display: the fraction is dropped, never rounded up, so
 * the shown value is never above the real one ("120.75" is "120", "-0.5" is "-1"). Text that is
 * not a plain decimal is shown as it is. */
void quota_format_credits(const char *balance, char *output, size_t capacity);

/* Status bar battery. The fill is `max_fill` pixels wide at 100%; any charge above 0 shows at
 * least one pixel. USB power (a USB host is connected; see the application) turns the fill green,
 * otherwise 20% or less is red. */
typedef enum {
    QUOTA_BATTERY_UNAVAILABLE = 0, /* no reading: outline with a slash, distinct from 0% */
    QUOTA_BATTERY_NORMAL,
    QUOTA_BATTERY_LOW,
    QUOTA_BATTERY_USB,
} quota_battery_tone_t;
#define QUOTA_BATTERY_LOW_PERCENT 20
typedef struct {
    quota_battery_tone_t tone;
    uint8_t fill;
} quota_battery_icon_t;
quota_battery_icon_t quota_battery_icon(int percent, bool usb_powered, unsigned max_fill);

/* Status bar Wi-Fi glyph: hidden without a saved network, dim while connecting or disconnected,
 * dim with a slash after a failure, otherwise 1 to 3 lit arcs from the signal strength. */
#define QUOTA_WIFI_SIGNAL_3_DBM (-60) /* this strong or stronger lights all three arcs */
#define QUOTA_WIFI_SIGNAL_2_DBM (-72)
#define QUOTA_WIFI_HYSTERESIS_DB 3
typedef enum {
    QUOTA_WIFI_ICON_HIDDEN = 0,
    QUOTA_WIFI_ICON_OFFLINE,
    QUOTA_WIFI_ICON_FAILED,
    QUOTA_WIFI_ICON_SIGNAL_1,
    QUOTA_WIFI_ICON_SIGNAL_2,
    QUOTA_WIFI_ICON_SIGNAL_3,
} quota_wifi_icon_t;
/* rssi_dbm is 0 when unknown (a connected link of unknown strength shows full). `previous` is the
 * icon drawn last: a level is kept until the signal is 3 dB below the level's threshold, so a
 * reading that hovers on a threshold does not flicker. */
quota_wifi_icon_t quota_wifi_icon(bool has_network, bool connected, bool failed, int rssi_dbm,
                                  quota_wifi_icon_t previous);
void quota_frame_decoder_init(quota_frame_decoder_t *decoder);
quota_frame_result_t quota_frame_decoder_feed(quota_frame_decoder_t *decoder, char byte,
                                              const char **frame_out, size_t *frame_length_out);

bool quota_data_is_stale(uint64_t now, bool has_observed_at, uint64_t observed_at,
                         uint16_t refresh_seconds);
bool quota_usb_window_active(bool setup_screen_open, uint64_t now_ms, uint64_t deadline_ms);
void quota_display_tick(quota_display_state_t *display, uint64_t now_ms, uint16_t timeout_seconds,
                        bool usb_window_active);
/* True permits navigation; the entire first waking gesture is consumed. */
bool quota_display_handle_key(quota_display_state_t *display, uint64_t now_ms,
                              quota_key_event_t event, bool down_key);
quota_metric_state_t quota_metric_state(const quota_window_t *window, uint64_t now);
quota_action_t quota_navigation_handle(quota_navigation_t *navigation, quota_input_t input,
                                       quota_navigation_context_t context);
void quota_navigation_init(quota_navigation_t *navigation, bool configured,
                           uint16_t refresh_seconds, bool auto_refresh,
                           uint16_t screen_timeout_seconds);
quota_status_line_t quota_status_line_select(const quota_status_input_t *input);
quota_hotspot_state_t quota_hotspot_state(bool storage_error, bool validating, bool active,
                                          bool ready, bool opening);
/* True when valid UTF-8 text without control characters has a glyph for every character:
 * has_glyph answers for code points above 0x7e (printable ASCII is always drawn). A name with a
 * character the font lacks is shown as a numbered stand-in instead. */
bool quota_text_is_displayable(const char *text, bool (*has_glyph)(uint32_t codepoint));
/* Raises the sleep notice for QUOTA_NOTICE_MS, or drops it once that has passed. */
void quota_navigation_notice(quota_navigation_t *navigation, uint64_t now_ms, bool raise);
void quota_navigation_sync_settings(quota_navigation_t *navigation, uint16_t refresh_seconds,
                                    bool auto_refresh, uint16_t screen_timeout_seconds);
int quota_find_account_by_id(const quota_snapshot_t *snapshot, const char *id);
