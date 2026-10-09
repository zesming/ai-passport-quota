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

const quota_currency_balance_t *quota_balance_primary(const quota_balance_t *balance)
{
    const quota_currency_balance_t *cny = quota_balance_cny(balance);
    if (cny != NULL)
        return cny;
    if (balance == NULL || !balance->present || balance->currency_count == 0 ||
        balance->currency_count > QUOTA_BALANCE_CURRENCIES)
        return NULL;
    return &balance->balance_infos[0];
}

void quota_money_parts(const char *currency, const char *amount, quota_money_t *money)
{
    if (money == NULL)
        return;
    memset(money, 0, sizeof(*money));
    if (amount == NULL)
        amount = "";
    const char *sign = "";
    if (amount[0] == '-') {
        sign = "-";
        amount++;
    }
    snprintf(money->number, sizeof(money->number), "%s", amount);
    if (currency != NULL && strcmp(currency, "CNY") == 0) {
        snprintf(money->prefix, sizeof(money->prefix), "%s¥", sign);
    } else if (currency != NULL && strcmp(currency, "USD") == 0) {
        snprintf(money->prefix, sizeof(money->prefix), "%s$", sign);
    } else {
        snprintf(money->prefix, sizeof(money->prefix), "%s", sign);
        if (currency != NULL && currency[0] != '\0') {
            char code[4]; /* ISO 4217 codes are three letters */
            quota_copy_display_ascii(currency, code, sizeof(code));
            snprintf(money->suffix, sizeof(money->suffix), " %s", code);
        }
    }
}

void quota_format_money(const char *currency, const char *amount, char *output, size_t capacity)
{
    if (output == NULL || capacity == 0)
        return;
    quota_money_t money;
    quota_money_parts(currency, amount, &money);
    snprintf(output, capacity, "%s%s%s", money.prefix, money.number, money.suffix);
}

void quota_format_credits(const char *balance, char *output, size_t capacity)
{
    if (output == NULL || capacity == 0)
        return;
    if (balance == NULL)
        balance = "";
    const char *p = balance;
    bool negative = *p == '-';
    if (negative)
        p++;
    const char *integer = p;
    while (*p >= '0' && *p <= '9')
        p++;
    size_t integer_length = (size_t)(p - integer);
    bool fraction_nonzero = false, plain = integer_length > 0;
    if (plain && *p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            fraction_nonzero = fraction_nonzero || *p != '0';
            p++;
        }
    }
    if (!plain || *p != '\0' || integer_length > QUOTA_CREDITS_BALANCE_BYTES) {
        quota_copy_display_ascii(balance, output, capacity);
        return;
    }
    while (integer_length > 1 && *integer == '0') {
        integer++;
        integer_length--;
    }
    char digits[QUOTA_CREDITS_BALANCE_BYTES + 3];
    memcpy(digits + 1, integer, integer_length);
    digits[0] = '0';
    digits[integer_length + 1] = '\0';
    /* A negative value with a fraction rounds away from zero, which is the lower number. */
    if (negative && fraction_nonzero) {
        size_t at = integer_length;
        for (; at > 0 && digits[at] == '9'; at--)
            digits[at] = '0';
        digits[at]++;
    }
    const char *start = digits;
    while (start[0] == '0' && start[1] != '\0')
        start++;
    bool zero = start[0] == '0' && start[1] == '\0';
    snprintf(output, capacity, "%s%s", negative && !zero ? "-" : "", start);
}

quota_battery_icon_t quota_battery_icon(int percent, bool usb_powered, unsigned max_fill)
{
    quota_battery_icon_t icon = {QUOTA_BATTERY_UNAVAILABLE, 0};
    if (percent < 0 || percent > 100)
        return icon;
    icon.fill = (uint8_t)(((unsigned)percent * max_fill + 50) / 100);
    if (percent > 0 && icon.fill == 0)
        icon.fill = 1;
    if (usb_powered)
        icon.tone = QUOTA_BATTERY_USB;
    else if (percent <= QUOTA_BATTERY_LOW_PERCENT)
        icon.tone = QUOTA_BATTERY_LOW;
    else
        icon.tone = QUOTA_BATTERY_NORMAL;
    return icon;
}

quota_wifi_icon_t quota_wifi_icon(bool has_network, bool connected, bool failed, int rssi_dbm,
                                  quota_wifi_icon_t previous)
{
    if (!has_network)
        return QUOTA_WIFI_ICON_HIDDEN;
    if (!connected)
        return failed ? QUOTA_WIFI_ICON_FAILED : QUOTA_WIFI_ICON_OFFLINE;
    if (rssi_dbm == 0)
        return QUOTA_WIFI_ICON_SIGNAL_3;
    /* A level already shown is kept down to 3 dB below the threshold that earned it. */
    int hold3 = previous == QUOTA_WIFI_ICON_SIGNAL_3 ? QUOTA_WIFI_HYSTERESIS_DB : 0;
    int hold2 = previous == QUOTA_WIFI_ICON_SIGNAL_3 || previous == QUOTA_WIFI_ICON_SIGNAL_2
                    ? QUOTA_WIFI_HYSTERESIS_DB
                    : 0;
    if (rssi_dbm >= QUOTA_WIFI_SIGNAL_3_DBM - hold3)
        return QUOTA_WIFI_ICON_SIGNAL_3;
    return rssi_dbm >= QUOTA_WIFI_SIGNAL_2_DBM - hold2 ? QUOTA_WIFI_ICON_SIGNAL_2
                                                       : QUOTA_WIFI_ICON_SIGNAL_1;
}

void quota_format_duration(uint64_t seconds, char *output, size_t capacity)
{
    if (output == NULL || capacity == 0)
        return;
    uint64_t days = seconds / 86400, hours = seconds % 86400 / 3600, minutes = seconds % 3600 / 60;
    if (days > 0) {
        snprintf(output, capacity, "%llu 天 %llu 小时", (unsigned long long)days,
                 (unsigned long long)hours);
    } else if (hours > 0) {
        snprintf(output, capacity, "%llu 小时 %llu 分", (unsigned long long)hours,
                 (unsigned long long)minutes);
    } else if (minutes > 0) {
        snprintf(output, capacity, "%llu 分钟", (unsigned long long)minutes);
    } else {
        snprintf(output, capacity, "不到 1 分钟");
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
        snprintf(output, capacity, "待校时");
    } else if (now >= window->resets_at) {
        snprintf(output, capacity, "等待新数据");
    } else {
        char remaining[32];
        quota_format_duration(window->resets_at - now, remaining, sizeof(remaining));
        snprintf(output, capacity, "%s后重置", remaining);
    }
}

void quota_format_banked_resets(const quota_codex_extras_t *extras, uint64_t now,
                                bool clock_synchronized, char *output, size_t capacity)
{
    if (output == NULL || capacity == 0)
        return;
    output[0] = '\0';
    if (extras == NULL || !extras->has_banked_reset || extras->available_resets == 0)
        return;
    int used = snprintf(output, capacity, "可用重置 %llu 次",
                        (unsigned long long)extras->available_resets);
    if (used < 0 || (size_t)used >= capacity || !extras->has_next_reset_expiry)
        return;
    if (!clock_synchronized) {
        snprintf(output + used, capacity - (size_t)used, " · 待校时");
    } else if (now >= extras->next_reset_expires_at) {
        snprintf(output + used, capacity - (size_t)used, " · 等待新数据");
    } else {
        char remaining[32];
        quota_format_duration(extras->next_reset_expires_at - now, remaining, sizeof(remaining));
        snprintf(output + used, capacity - (size_t)used, " · %s后过期", remaining);
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

bool quota_usb_window_active(bool setup_screen_open, uint64_t now_ms, uint64_t deadline_ms)
{
    return setup_screen_open && now_ms < deadline_ms;
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
    display->session_open = usb_window_active;
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
        if (display->session_open)
            display->sleep_blocked = true;
        else
            display->sleeping = true;
        return false;
    }
    return event != QUOTA_KEY_PRESS;
}

void quota_navigation_init(quota_navigation_t *navigation, bool configured,
                           uint16_t refresh_seconds, bool auto_refresh,
                           uint16_t screen_timeout_seconds)
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
    navigation->screen = QUOTA_SCREEN_HOME;
    navigation->return_screen = QUOTA_SCREEN_HOME;
}

void quota_navigation_notice(quota_navigation_t *navigation, uint64_t now_ms, bool raise)
{
    if (navigation == NULL)
        return;
    if (raise) {
        navigation->sleep_notice = true;
        navigation->sleep_notice_until_ms = now_ms + QUOTA_NOTICE_MS;
    } else if (navigation->sleep_notice && now_ms >= navigation->sleep_notice_until_ms) {
        navigation->sleep_notice = false;
    }
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

static const uint16_t quota_refresh_intervals[QUOTA_REFRESH_OPTIONS - 1] = {60, 300, 900, 1800};

static uint8_t wrap_index(uint8_t current, int direction, uint8_t count)
{
    if (count == 0)
        return 0;
    if (direction < 0)
        return current == 0 ? (uint8_t)(count - 1) : (uint8_t)(current - 1);
    return (uint8_t)((current + 1) % count);
}

/* Without an account the menu always opens on its first item, so "long OK, DOWN, OK" reaches USB
 * from the welcome screen. With an account it keeps the item that was last in use. */
static void enter_menu(quota_navigation_t *navigation, uint8_t account_count)
{
    navigation->screen = QUOTA_SCREEN_MENU;
    if (account_count == 0 || navigation->menu_focus >= QUOTA_MENU_ITEMS)
        navigation->menu_focus = 0;
}

/* Show the hotspot screen. Only a closed hotspot is opened: while one is showing, opening or
 * validating, a second open would restart the session or abort the validation. */
static quota_action_t enter_hotspot(quota_navigation_t *navigation, quota_screen_t from,
                                    quota_navigation_context_t context)
{
    navigation->return_screen = from;
    navigation->hotspot_page = 0;
    navigation->screen = QUOTA_SCREEN_HOTSPOT;
    return context.hotspot == QUOTA_HOTSPOT_CLOSED ? QUOTA_ACTION_OPEN_HOTSPOT : QUOTA_ACTION_NONE;
}

static void enter_option_list(quota_navigation_t *navigation, quota_screen_t screen)
{
    navigation->screen = screen;
    navigation->option_focus = 0;
    if (screen == QUOTA_SCREEN_REFRESH) {
        for (size_t i = 0; navigation->auto_refresh && i < QUOTA_REFRESH_OPTIONS - 1; i++) {
            if (quota_refresh_intervals[i] == navigation->refresh_seconds)
                navigation->option_focus = (uint8_t)(i + 1);
        }
    } else {
        for (size_t i = 0; i < QUOTA_SCREEN_TIMEOUT_COUNT; i++) {
            if (quota_screen_timeouts[i] == navigation->screen_timeout_seconds)
                navigation->option_focus = (uint8_t)i;
        }
    }
}

static void enter_confirm(quota_navigation_t *navigation, quota_confirm_kind_t kind)
{
    navigation->return_screen = navigation->screen;
    navigation->confirm_kind = kind;
    navigation->confirm_focus = 0;
    navigation->factory_resetting = false;
    navigation->factory_failed = false;
    navigation->screen = QUOTA_SCREEN_CONFIRM;
}

static quota_action_t handle_move(quota_navigation_t *navigation, int direction,
                                  quota_navigation_context_t context)
{
    switch (navigation->screen) {
    case QUOTA_SCREEN_HOME:
        if (context.account_count > 1) {
            navigation->selected_account =
                wrap_index(navigation->selected_account, direction, context.account_count);
            return QUOTA_ACTION_PERSIST_SELECTION;
        }
        break;
    case QUOTA_SCREEN_MENU:
        navigation->menu_focus = wrap_index(navigation->menu_focus, direction, QUOTA_MENU_ITEMS);
        break;
    case QUOTA_SCREEN_REFRESH:
        navigation->option_focus =
            wrap_index(navigation->option_focus, direction, QUOTA_REFRESH_OPTIONS);
        break;
    case QUOTA_SCREEN_SLEEP:
        navigation->option_focus =
            wrap_index(navigation->option_focus, direction, QUOTA_SCREEN_TIMEOUT_COUNT);
        break;
    case QUOTA_SCREEN_HOTSPOT:
        navigation->hotspot_page =
            wrap_index(navigation->hotspot_page, direction, QUOTA_HOTSPOT_PAGES);
        break;
    case QUOTA_SCREEN_CONFIRM:
        if (!navigation->factory_failed)
            navigation->confirm_focus = wrap_index(navigation->confirm_focus, direction, 2);
        break;
    default:
        break;
    }
    return QUOTA_ACTION_NONE;
}

/* Long OK always goes back one level and never does anything destructive. */
static quota_action_t handle_back(quota_navigation_t *navigation,
                                  quota_navigation_context_t context)
{
    switch (navigation->screen) {
    case QUOTA_SCREEN_HOME:
        enter_menu(navigation, context.account_count);
        break;
    case QUOTA_SCREEN_MENU:
        navigation->screen = QUOTA_SCREEN_HOME;
        break;
    case QUOTA_SCREEN_HOTSPOT:
        navigation->screen = navigation->return_screen;
        if (navigation->screen == QUOTA_SCREEN_MENU)
            enter_menu(navigation, context.account_count);
        return QUOTA_ACTION_CLOSE_HOTSPOT;
    case QUOTA_SCREEN_USB:
        navigation->screen = navigation->return_screen;
        if (navigation->screen == QUOTA_SCREEN_MENU)
            enter_menu(navigation, context.account_count);
        return QUOTA_ACTION_CLOSE_USB;
    case QUOTA_SCREEN_AUTH:
        navigation->screen = QUOTA_SCREEN_HOME; /* authorization carries on in the background */
        break;
    case QUOTA_SCREEN_CONFIRM:
        navigation->factory_failed = false;
        navigation->screen = navigation->return_screen;
        break;
    default: /* option lists and device information */
        enter_menu(navigation, context.account_count);
        break;
    }
    return QUOTA_ACTION_NONE;
}

static quota_action_t handle_ok(quota_navigation_t *navigation, quota_navigation_context_t context)
{
    switch (navigation->screen) {
    case QUOTA_SCREEN_HOME:
        if (context.auth_active) {
            navigation->screen = QUOTA_SCREEN_AUTH;
        } else if (context.account_count == 0) {
            return enter_hotspot(navigation, QUOTA_SCREEN_HOME, context);
        } else if (navigation->configured) {
            return QUOTA_ACTION_REFRESH;
        }
        break;
    case QUOTA_SCREEN_MENU:
        switch (navigation->menu_focus) {
        case 0:
            return enter_hotspot(navigation, QUOTA_SCREEN_MENU, context);
        case 1:
            navigation->return_screen = QUOTA_SCREEN_MENU;
            navigation->screen = QUOTA_SCREEN_USB;
            /* An open window keeps its session and its clock: opening again would replace both. */
            return context.usb_window_open ? QUOTA_ACTION_NONE : QUOTA_ACTION_OPEN_USB;
        case 2:
            enter_option_list(navigation, QUOTA_SCREEN_REFRESH);
            break;
        case 3:
            enter_option_list(navigation, QUOTA_SCREEN_SLEEP);
            break;
        default:
            navigation->screen = QUOTA_SCREEN_INFO;
            break;
        }
        break;
    case QUOTA_SCREEN_REFRESH:
        if (navigation->option_focus == 0) {
            navigation->auto_refresh = false;
        } else if (navigation->option_focus < QUOTA_REFRESH_OPTIONS) {
            navigation->refresh_seconds = quota_refresh_intervals[navigation->option_focus - 1];
            navigation->auto_refresh = true;
        }
        enter_menu(navigation, context.account_count);
        return QUOTA_ACTION_APPLY_SETTINGS;
    case QUOTA_SCREEN_SLEEP:
        if (navigation->option_focus < QUOTA_SCREEN_TIMEOUT_COUNT)
            navigation->screen_timeout_seconds = quota_screen_timeouts[navigation->option_focus];
        enter_menu(navigation, context.account_count);
        return QUOTA_ACTION_APPLY_SETTINGS;
    case QUOTA_SCREEN_HOTSPOT:
        if (context.hotspot == QUOTA_HOTSPOT_SHOWING) {
            navigation->hotspot_page = wrap_index(navigation->hotspot_page, 1, QUOTA_HOTSPOT_PAGES);
        } else if (context.hotspot == QUOTA_HOTSPOT_CLOSED) {
            navigation->hotspot_page = 0;
            return QUOTA_ACTION_RENEW_HOTSPOT;
        }
        break; /* busy: nothing, so nothing can restart a validation */
    case QUOTA_SCREEN_USB:
        return context.usb_window_open ? QUOTA_ACTION_NONE : QUOTA_ACTION_OPEN_USB;
    case QUOTA_SCREEN_INFO:
        enter_confirm(navigation, QUOTA_CONFIRM_FACTORY_RESET);
        break;
    case QUOTA_SCREEN_AUTH:
        if (context.auth_active)
            enter_confirm(navigation, QUOTA_CONFIRM_CANCEL_AUTH);
        else
            navigation->screen = QUOTA_SCREEN_HOME;
        break;
    case QUOTA_SCREEN_CONFIRM:
        if (navigation->factory_failed)
            break;
        if (navigation->confirm_focus == 0) {
            navigation->screen = navigation->return_screen;
            break;
        }
        if (navigation->confirm_kind == QUOTA_CONFIRM_CANCEL_AUTH) {
            navigation->screen = QUOTA_SCREEN_HOME;
            return QUOTA_ACTION_CANCEL_AUTH;
        }
        navigation->factory_resetting = true;
        return QUOTA_ACTION_FACTORY_RESET;
    default:
        break;
    }
    return QUOTA_ACTION_NONE;
}

quota_action_t quota_navigation_handle(quota_navigation_t *navigation, quota_input_t input,
                                       quota_navigation_context_t context)
{
    if (navigation == NULL || context.account_count > QUOTA_MAX_ACCOUNTS)
        return QUOTA_ACTION_NONE;
    if (navigation->factory_resetting)
        return QUOTA_ACTION_NONE; /* the erase is under way: the device restarts, or says it failed
                                   */
    if (input == QUOTA_INPUT_UP)
        return handle_move(navigation, -1, context);
    if (input == QUOTA_INPUT_DOWN)
        return handle_move(navigation, 1, context);
    if (input == QUOTA_INPUT_OK_LONG)
        return handle_back(navigation, context);
    if (input == QUOTA_INPUT_OK_SHORT)
        return handle_ok(navigation, context);
    return QUOTA_ACTION_NONE;
}

quota_status_line_t quota_status_line_select(const quota_status_input_t *input)
{
    if (input == NULL)
        return QUOTA_STATUS_LINE_NO_DATA;
    if (input->storage_error)
        return QUOTA_STATUS_LINE_STORAGE_ERROR;
    if (input->authorizing)
        return QUOTA_STATUS_LINE_AUTHORIZING;
    if (input->reauth_needed)
        return QUOTA_STATUS_LINE_REAUTH;
    if (input->unverified_items > 0)
        return QUOTA_STATUS_LINE_UNVERIFIED;
    if (input->wifi_failed)
        return QUOTA_STATUS_LINE_WIFI_FAILED;
    if (input->rate_limited)
        return QUOTA_STATUS_LINE_RATE_LIMITED;
    if (input->update_failed)
        return QUOTA_STATUS_LINE_UPDATE_FAILED;
    if (input->refreshing)
        return QUOTA_STATUS_LINE_REFRESHING;
    return input->has_observed_at ? QUOTA_STATUS_LINE_UPDATED : QUOTA_STATUS_LINE_NO_DATA;
}

quota_hotspot_state_t quota_hotspot_state(bool storage_error, bool validating, bool active,
                                          bool ready, bool opening)
{
    if (storage_error || validating)
        return QUOTA_HOTSPOT_BUSY;
    if (active)
        return ready ? QUOTA_HOTSPOT_SHOWING : QUOTA_HOTSPOT_BUSY;
    return opening ? QUOTA_HOTSPOT_BUSY : QUOTA_HOTSPOT_CLOSED;
}

bool quota_text_is_displayable(const char *text, bool (*has_glyph)(uint32_t codepoint))
{
    if (text == NULL || !quota_utf8_is_valid(text, strlen(text)))
        return false;
    const unsigned char *p = (const unsigned char *)text;
    while (*p != '\0') {
        uint32_t codepoint = *p;
        size_t length = 1;
        if (codepoint >= 0xf0) {
            codepoint &= 0x07;
            length = 4;
        } else if (codepoint >= 0xe0) {
            codepoint &= 0x0f;
            length = 3;
        } else if (codepoint >= 0xc0) {
            codepoint &= 0x1f;
            length = 2;
        }
        for (size_t i = 1; i < length; i++)
            codepoint = (codepoint << 6) | (p[i] & 0x3f);
        p += length;
        if (codepoint < 0x20 || codepoint == 0x7f)
            return false;
        if (codepoint > 0x7e && (has_glyph == NULL || !has_glyph(codepoint)))
            return false;
    }
    return true;
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
