#include "quota_logic.h"

#include <stdio.h>
#include <string.h>

static bool is_hex_lower(char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
}

static bool ascii_has_control(const char *text, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 0x20 || ch == 0x7f)
            return true;
    }
    return false;
}

bool quota_utf8_is_valid(const char *text, size_t length)
{
    if (text == NULL)
        return false;
    const unsigned char *bytes = (const unsigned char *)text;
    size_t i = 0;
    while (i < length) {
        uint32_t codepoint;
        unsigned char first = bytes[i++];
        if (first <= 0x7f)
            continue;
        if (first >= 0xc2 && first <= 0xdf) {
            if (i >= length || (bytes[i] & 0xc0) != 0x80)
                return false;
            codepoint = ((uint32_t)(first & 0x1f) << 6) | (bytes[i++] & 0x3f);
        } else if (first >= 0xe0 && first <= 0xef) {
            if (length - i < 2 || (bytes[i] & 0xc0) != 0x80 || (bytes[i + 1] & 0xc0) != 0x80)
                return false;
            if ((first == 0xe0 && bytes[i] < 0xa0) || (first == 0xed && bytes[i] >= 0xa0))
                return false;
            codepoint = ((uint32_t)(first & 0x0f) << 12) | ((uint32_t)(bytes[i] & 0x3f) << 6) |
                        (bytes[i + 1] & 0x3f);
            i += 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            if (length - i < 3 || (bytes[i] & 0xc0) != 0x80 || (bytes[i + 1] & 0xc0) != 0x80 ||
                (bytes[i + 2] & 0xc0) != 0x80)
                return false;
            if ((first == 0xf0 && bytes[i] < 0x90) || (first == 0xf4 && bytes[i] >= 0x90))
                return false;
            codepoint = ((uint32_t)(first & 0x07) << 18) | ((uint32_t)(bytes[i] & 0x3f) << 12) |
                        ((uint32_t)(bytes[i + 1] & 0x3f) << 6) | (bytes[i + 2] & 0x3f);
            i += 3;
        } else {
            return false;
        }
        if (codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

bool quota_id_is_valid(const char *id)
{
    if (id == NULL || strlen(id) != QUOTA_ACCOUNT_ID_BYTES)
        return false;
    for (size_t i = 0; i < QUOTA_ACCOUNT_ID_BYTES; i++) {
        if (!is_hex_lower(id[i]))
            return false;
    }
    return true;
}
void quota_copy_display_ascii(const char *source, char *destination, size_t capacity)
{
    if (destination == NULL || capacity == 0)
        return;
    destination[0] = '\0';
    if (source == NULL)
        return;

    const unsigned char *input = (const unsigned char *)source;
    size_t out = 0;
    for (size_t i = 0; input[i] != '\0' && out + 1 < capacity;) {
        unsigned char first = input[i];
        if (first >= 0x20 && first <= 0x7e) {
            destination[out++] = (char)first;
            i++;
        } else if (first < 0x80) {
            destination[out++] = '?';
            i++;
        } else {
            size_t sequence = first >= 0xf0 && first <= 0xf4   ? 4
                              : first >= 0xe0 && first <= 0xef ? 3
                              : first >= 0xc2 && first <= 0xdf ? 2
                                                               : 1;
            bool valid_sequence = true;
            for (size_t j = 1; j < sequence; j++) {
                if (input[i + j] == '\0' || (input[i + j] & 0xc0) != 0x80) {
                    valid_sequence = false;
                    break;
                }
            }
            destination[out++] = '?';
            i += valid_sequence ? sequence : 1;
        }
    }
    destination[out] = '\0';
}

void quota_frame_decoder_init(quota_frame_decoder_t *decoder)
{
    if (decoder != NULL)
        memset(decoder, 0, sizeof(*decoder));
}

quota_frame_result_t quota_frame_decoder_feed(quota_frame_decoder_t *decoder, char byte,
                                              const char **frame_out, size_t *frame_length_out)
{
    if (frame_out != NULL)
        *frame_out = NULL;
    if (frame_length_out != NULL)
        *frame_length_out = 0;
    if (decoder == NULL)
        return QUOTA_FRAME_PENDING;

    if (byte == '\n') {
        if (decoder->discarding_overlong_line) {
            decoder->discarding_overlong_line = false;
            decoder->length = 0;
            decoder->bytes[0] = '\0';
            return QUOTA_FRAME_TOO_LONG;
        }
        if (decoder->length > 0 && decoder->bytes[decoder->length - 1] == '\r') {
            decoder->length--;
        }
        if (decoder->length == 0)
            return QUOTA_FRAME_PENDING;
        decoder->bytes[decoder->length] = '\0';
        if (frame_out != NULL)
            *frame_out = decoder->bytes;
        if (frame_length_out != NULL)
            *frame_length_out = decoder->length;
        decoder->length = 0;
        return QUOTA_FRAME_COMPLETE;
    }
    if (decoder->discarding_overlong_line)
        return QUOTA_FRAME_PENDING;
    if (decoder->length >= QUOTA_MAX_PROVISION_FRAME_BYTES) {
        decoder->discarding_overlong_line = true;
        decoder->length = 0;
        decoder->bytes[0] = '\0';
        return QUOTA_FRAME_PENDING;
    }
    decoder->bytes[decoder->length++] = byte;
    return QUOTA_FRAME_PENDING;
}
static bool valid_balance_amount(const char *text)
{
    if (text == NULL)
        return false;
    const char *end = memchr(text, '\0', QUOTA_BALANCE_AMOUNT_BYTES + 1);
    if (end == NULL || end == text)
        return false;
    const char *p = text;
    if (*p == '-')
        p++;
    if (*p < '0' || *p > '9')
        return false;
    while (*p >= '0' && *p <= '9')
        p++;
    if (*p == '.') {
        p++;
        if (*p < '0' || *p > '9')
            return false;
        while (*p >= '0' && *p <= '9')
            p++;
    }
    return *p == '\0';
}

bool quota_balance_is_valid(const quota_balance_t *balance)
{
    if (balance == NULL || balance->currency_count > QUOTA_BALANCE_CURRENCIES ||
        memchr(balance->label, '\0', sizeof(balance->label)) == NULL ||
        !quota_utf8_is_valid(balance->label, strlen(balance->label)) ||
        ascii_has_control(balance->label, strlen(balance->label)))
        return false;
    if (!balance->present)
        return balance->currency_count == 0;
    for (size_t i = 0; i < balance->currency_count; i++) {
        const quota_currency_balance_t *entry = &balance->balance_infos[i];
        if ((memcmp(entry->currency, "CNY", 4) != 0 && memcmp(entry->currency, "USD", 4) != 0) ||
            !valid_balance_amount(entry->total_balance) ||
            !valid_balance_amount(entry->granted_balance) ||
            !valid_balance_amount(entry->topped_up_balance) ||
            (i > 0 && memcmp(entry->currency, balance->balance_infos[0].currency, 4) == 0))
            return false;
    }
    return true;
}

const quota_currency_balance_t *quota_balance_cny(const quota_balance_t *balance)
{
    if (balance == NULL || !balance->present || balance->currency_count > QUOTA_BALANCE_CURRENCIES)
        return NULL;
    for (size_t i = 0; i < balance->currency_count; i++) {
        if (memcmp(balance->balance_infos[i].currency, "CNY", 4) == 0) {
            return &balance->balance_infos[i];
        }
    }
    return NULL;
}
void quota_format_duration(uint64_t seconds, char *output, size_t capacity)
{
    if (output == NULL || capacity == 0)
        return;
    uint64_t hours = seconds / 3600;
    if (hours == 0) {
        snprintf(output, capacity, "<1h");
    } else {
        snprintf(output, capacity, "%llud %lluh", (unsigned long long)(hours / 24),
                 (unsigned long long)(hours % 24));
    }
}

void quota_format_reset_time(const quota_window_t *window, uint64_t now, bool clock_synchronized,
                             char *output, size_t capacity)
{
    if (output == NULL || capacity == 0)
        return;
    if (window == NULL || !window->present || !window->has_resets_at) {
        snprintf(output, capacity, "重置时间未知");
    } else if (!clock_synchronized) {
        snprintf(output, capacity, "时间待同步");
    } else if (now >= window->resets_at) {
        snprintf(output, capacity, "等待新数据");
    } else {
        char remaining[32];
        quota_format_duration(window->resets_at - now, remaining, sizeof(remaining));
        /* U+F021 is the refresh glyph in the built-in font fallback. */
        snprintf(output, capacity, "\xEF\x80\xA1 %s", remaining);
    }
}

bool quota_refresh_seconds_is_valid(uint64_t seconds)
{
    return seconds == 60 || seconds == 300 || seconds == 900 || seconds == 1800;
}

const uint16_t quota_screen_timeouts[QUOTA_SCREEN_TIMEOUT_COUNT] = {0, 30, 60, 120, 300, 600};

bool quota_screen_timeout_is_valid(uint64_t seconds)
{
    for (size_t i = 0; i < QUOTA_SCREEN_TIMEOUT_COUNT; i++) {
        if (seconds == quota_screen_timeouts[i])
            return true;
    }
    return false;
}

void quota_copy_display_plan(const char *source, char *destination, size_t capacity)
{
    quota_copy_display_ascii(source, destination, capacity);
    if (destination != NULL && capacity > 0 && destination[0] >= 'a' && destination[0] <= 'z') {
        destination[0] = (char)(destination[0] - 'a' + 'A');
    }
}
bool quota_data_is_stale(uint64_t now, bool has_observed_at, uint64_t observed_at,
                         uint16_t refresh_seconds)
{
    if (!has_observed_at || now < observed_at)
        return true;
    uint64_t threshold = (uint64_t)refresh_seconds * 2;
    if (threshold < 900)
        threshold = 900;
    return now - observed_at > threshold;
}

bool quota_usb_window_active(bool setup_screen_open, uint64_t now_ms, uint64_t opened_at_ms)
{
    return setup_screen_open && now_ms >= opened_at_ms &&
           now_ms - opened_at_ms < QUOTA_USB_WINDOW_MS;
}

quota_metric_state_t quota_metric_state(const quota_window_t *window, uint64_t now)
{
    if (window == NULL || !window->present)
        return QUOTA_METRIC_UNAVAILABLE;
    if (window->has_resets_at && now >= window->resets_at) {
        return QUOTA_METRIC_WAITING_FOR_SOURCE;
    }
    return QUOTA_METRIC_VALUE;
}

void quota_display_tick(quota_display_state_t *display, uint64_t now_ms, uint16_t timeout_seconds,
                        bool usb_window_active)
{
    if (display == NULL)
        return;
    /* Start a fresh idle period when the USB window closes, including on clock rollback. */
    if (usb_window_active || now_ms < display->last_input_ms)
        display->last_input_ms = now_ms;
    if (!usb_window_active && timeout_seconds != 0 &&
        quota_screen_timeout_is_valid(timeout_seconds) &&
        now_ms - display->last_input_ms >= (uint64_t)timeout_seconds * 1000) {
        display->sleeping = true;
    }
}

bool quota_display_handle_key(quota_display_state_t *display, uint64_t now_ms,
                              quota_key_event_t event, bool down_key)
{
    if (display == NULL)
        return false;
    display->last_input_ms = now_ms;
    if (display->sleeping) {
        display->sleeping = false;
        display->consume_wake_gesture = event == QUOTA_KEY_PRESS;
        return false;
    }
    if (display->consume_wake_gesture) {
        if (event != QUOTA_KEY_PRESS)
            display->consume_wake_gesture = false;
        return false;
    }
    if (event == QUOTA_KEY_LONG && down_key) {
        display->sleeping = true;
        return false;
    }
    return event != QUOTA_KEY_PRESS;
}

void quota_navigation_init(quota_navigation_t *navigation, bool configured,
                           uint16_t refresh_seconds, bool auto_refresh,
                           uint16_t screen_timeout_seconds, uint8_t account_count)
{
    if (navigation == NULL)
        return;
    memset(navigation, 0, sizeof(*navigation));
    navigation->configured = configured;
    navigation->refresh_seconds = quota_refresh_seconds_is_valid(refresh_seconds)
                                      ? refresh_seconds
                                      : QUOTA_REFRESH_DEFAULT_SECONDS;
    navigation->auto_refresh = auto_refresh;
    navigation->screen_timeout_seconds = quota_screen_timeout_is_valid(screen_timeout_seconds)
                                             ? screen_timeout_seconds
                                             : QUOTA_SCREEN_TIMEOUT_DEFAULT_SECONDS;
    navigation->screen = configured ? QUOTA_SCREEN_HOME : QUOTA_SCREEN_PHONE;
    navigation->setup_return_screen = QUOTA_SCREEN_HOME;
    if (account_count == 0)
        navigation->selected_account = 0;
}

void quota_navigation_sync_settings(quota_navigation_t *navigation, uint16_t refresh_seconds,
                                    bool auto_refresh, uint16_t screen_timeout_seconds)
{
    if (navigation == NULL || !quota_refresh_seconds_is_valid(refresh_seconds))
        return;
    navigation->refresh_seconds = refresh_seconds;
    navigation->auto_refresh = auto_refresh;
    if (quota_screen_timeout_is_valid(screen_timeout_seconds)) {
        navigation->screen_timeout_seconds = screen_timeout_seconds;
    }
}

static const uint16_t quota_refresh_intervals[] = {60, 300, 900, 1800};

static uint8_t wrap_index(uint8_t current, int direction, uint8_t count)
{
    if (count == 0)
        return 0;
    if (direction < 0)
        return current == 0 ? (uint8_t)(count - 1) : (uint8_t)(current - 1);
    return (uint8_t)((current + 1) % count);
}

quota_action_t quota_navigation_handle(quota_navigation_t *navigation, quota_input_t input,
                                       uint8_t account_count)
{
    if (navigation == NULL || account_count > QUOTA_MAX_ACCOUNTS)
        return QUOTA_ACTION_NONE;
    if (input == QUOTA_INPUT_UP || input == QUOTA_INPUT_DOWN) {
        int direction = input == QUOTA_INPUT_UP ? -1 : 1;
        if (navigation->screen == QUOTA_SCREEN_HOME && account_count > 1) {
            navigation->selected_account =
                wrap_index(navigation->selected_account, direction, account_count);
            return QUOTA_ACTION_PERSIST_SELECTION;
        }
        if (navigation->screen == QUOTA_SCREEN_SETTINGS) {
            navigation->settings_focus = wrap_index(navigation->settings_focus, direction, 6);
        } else if (navigation->screen == QUOTA_SCREEN_ACCOUNTS) {
            navigation->account_focus =
                wrap_index(navigation->account_focus, direction, (uint8_t)(account_count + 1));
        } else if (navigation->screen == QUOTA_SCREEN_INTERVAL) {
            navigation->interval_focus = wrap_index(navigation->interval_focus, direction, 5);
        } else if (navigation->screen == QUOTA_SCREEN_SLEEP) {
            navigation->sleep_focus =
                wrap_index(navigation->sleep_focus, direction, QUOTA_SCREEN_TIMEOUT_COUNT);
        } else if (navigation->screen == QUOTA_SCREEN_PHONE) {
            navigation->phone_step = wrap_index(navigation->phone_step, direction, 3);
        } else if (navigation->screen == QUOTA_SCREEN_DEVICE_SETTINGS) {
            navigation->device_settings_focus =
                wrap_index(navigation->device_settings_focus, direction, 2);
        }
        return QUOTA_ACTION_NONE;
    }

    if (input == QUOTA_INPUT_OK_LONG) {
        if (navigation->screen == QUOTA_SCREEN_HOME) {
            navigation->screen = QUOTA_SCREEN_SETTINGS;
        } else if (navigation->screen == QUOTA_SCREEN_SETUP) {
            navigation->screen = navigation->setup_return_screen;
        } else if (navigation->screen == QUOTA_SCREEN_PHONE) {
            navigation->screen = navigation->setup_return_screen;
            return QUOTA_ACTION_CLOSE_PHONE;
        } else if (navigation->screen == QUOTA_SCREEN_AUTH) {
            navigation->screen = QUOTA_SCREEN_ACCOUNTS;
            return QUOTA_ACTION_CANCEL_AUTH;
        } else if (navigation->screen == QUOTA_SCREEN_INTERVAL ||
                   navigation->screen == QUOTA_SCREEN_SLEEP ||
                   navigation->screen == QUOTA_SCREEN_NETWORK ||
                   navigation->screen == QUOTA_SCREEN_ACCOUNTS) {
            navigation->screen = QUOTA_SCREEN_SETTINGS;
        } else if (navigation->screen == QUOTA_SCREEN_DEVICE_SETTINGS) {
            navigation->screen = QUOTA_SCREEN_SETTINGS;
        } else {
            navigation->screen = QUOTA_SCREEN_HOME;
        }
        return QUOTA_ACTION_NONE;
    }

    if (input != QUOTA_INPUT_OK_SHORT)
        return QUOTA_ACTION_NONE;
    switch (navigation->screen) {
    case QUOTA_SCREEN_HOME:
        return navigation->configured ? QUOTA_ACTION_REFRESH : QUOTA_ACTION_NONE;
    case QUOTA_SCREEN_SETTINGS:
        if (navigation->settings_focus == 0) {
            navigation->screen = QUOTA_SCREEN_ACCOUNTS;
            navigation->account_focus = 0;
        } else if (navigation->settings_focus == 1) {
            navigation->screen = QUOTA_SCREEN_INTERVAL;
            navigation->interval_focus = 0;
            for (size_t i = 0; navigation->auto_refresh && i < 4; i++) {
                if (quota_refresh_intervals[i] == navigation->refresh_seconds) {
                    navigation->interval_focus = (uint8_t)(i + 1);
                    break;
                }
            }
        } else if (navigation->settings_focus == 2) {
            return QUOTA_ACTION_REFRESH;
        } else if (navigation->settings_focus == 3) {
            navigation->screen = QUOTA_SCREEN_SLEEP;
            navigation->sleep_focus = 0;
            for (size_t i = 0; i < QUOTA_SCREEN_TIMEOUT_COUNT; i++) {
                if (quota_screen_timeouts[i] == navigation->screen_timeout_seconds) {
                    navigation->sleep_focus = (uint8_t)i;
                    break;
                }
            }
        } else if (navigation->settings_focus == 4) {
            navigation->screen = QUOTA_SCREEN_NETWORK;
        } else {
            navigation->device_settings_focus = 0;
            navigation->screen = QUOTA_SCREEN_DEVICE_SETTINGS;
        }
        return QUOTA_ACTION_NONE;
    case QUOTA_SCREEN_ACCOUNTS:
        if (navigation->account_focus < account_count) {
            navigation->selected_account = navigation->account_focus;
            navigation->screen = QUOTA_SCREEN_HOME;
            return QUOTA_ACTION_PERSIST_SELECTION;
        }
        navigation->setup_return_screen = QUOTA_SCREEN_ACCOUNTS;
        navigation->screen = QUOTA_SCREEN_PHONE;
        navigation->phone_step = 0;
        return QUOTA_ACTION_OPEN_PHONE;
    case QUOTA_SCREEN_INTERVAL: {
        if (navigation->interval_focus == 0) {
            navigation->auto_refresh = !navigation->auto_refresh;
        } else {
            navigation->refresh_seconds = quota_refresh_intervals[navigation->interval_focus - 1];
            navigation->auto_refresh = true;
        }
        navigation->screen = QUOTA_SCREEN_SETTINGS;
        return QUOTA_ACTION_APPLY_SETTINGS;
    }
    case QUOTA_SCREEN_SETUP:
        return QUOTA_ACTION_NONE;
    case QUOTA_SCREEN_NETWORK:
        return QUOTA_ACTION_NONE;
    case QUOTA_SCREEN_DEVICE_SETTINGS:
        navigation->phone_step = 0;
        navigation->setup_return_screen = QUOTA_SCREEN_DEVICE_SETTINGS;
        if (navigation->device_settings_focus == 0) {
            navigation->screen = QUOTA_SCREEN_PHONE;
            return QUOTA_ACTION_OPEN_PHONE;
        }
        navigation->screen = QUOTA_SCREEN_SETUP;
        return QUOTA_ACTION_NONE;
    case QUOTA_SCREEN_PHONE:
        navigation->phone_step = wrap_index(navigation->phone_step, 1, 3);
        return QUOTA_ACTION_RENEW_PHONE;
    case QUOTA_SCREEN_AUTH:
        navigation->setup_return_screen = QUOTA_SCREEN_AUTH;
        navigation->screen = QUOTA_SCREEN_SETUP;
        return QUOTA_ACTION_NONE;
    case QUOTA_SCREEN_SLEEP:
        if (navigation->sleep_focus >= QUOTA_SCREEN_TIMEOUT_COUNT)
            return QUOTA_ACTION_NONE;
        navigation->screen_timeout_seconds = quota_screen_timeouts[navigation->sleep_focus];
        navigation->screen = QUOTA_SCREEN_SETTINGS;
        return QUOTA_ACTION_APPLY_SETTINGS;
    default:
        return QUOTA_ACTION_NONE;
    }
}

int quota_find_account_by_id(const quota_snapshot_t *snapshot, const char *id)
{
    if (snapshot == NULL || id == NULL || !quota_id_is_valid(id))
        return -1;
    for (size_t i = 0; i < snapshot->account_count && i < QUOTA_MAX_ACCOUNTS; i++) {
        if (strcmp(snapshot->accounts[i].id, id) == 0)
            return (int)i;
    }
    return -1;
}
