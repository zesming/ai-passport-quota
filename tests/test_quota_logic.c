#include "quota_logic.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_utf8_and_identifiers(void)
{
    assert(quota_utf8_is_valid("额度", strlen("额度")));
    assert(quota_utf8_is_valid("a\xf0\x9f\x98\x80", 5));
    assert(!quota_utf8_is_valid("\xc0\xaf", 2));
    assert(!quota_utf8_is_valid("\xed\xa0\x80", 3));
    assert(!quota_utf8_is_valid("\xf0\x80\x80\x80", 4));
    assert(!quota_utf8_is_valid("\xe4\xb8", 2));
    assert(quota_id_is_valid("0123456789abcdef0123456789abcdef"));
    assert(!quota_id_is_valid("0123456789ABCDEF0123456789abcdef"));
    assert(!quota_id_is_valid("0123456789abcdef0123456789abcde"));
}

static void test_display_text(void)
{
    char display[32];
    quota_copy_display_plan("pro", display, sizeof(display));
    assert(strcmp(display, "Pro") == 0);
    quota_copy_display_plan("max", display, 2);
    assert(strcmp(display, "M") == 0);
    quota_copy_display_ascii("m你好@example.com", display, sizeof(display));
    assert(strcmp(display, "m??@example.com") == 0);
    quota_copy_display_ascii("你", display, 2);
    assert(strcmp(display, "?") == 0);
    quota_copy_display_ascii("name\n@example.com", display, sizeof(display));
    assert(strcmp(display, "name?@example.com") == 0);
}

static void test_account_lookup(void)
{
    quota_snapshot_t snapshot = {0};
    snapshot.account_count = QUOTA_MAX_ACCOUNTS;
    for (int i = 0; i < QUOTA_MAX_ACCOUNTS; i++) {
        snprintf(snapshot.accounts[i].id, sizeof(snapshot.accounts[i].id),
                 "%08x0123456789abcdef01234567", (unsigned)i);
    }
    assert(quota_find_account_by_id(&snapshot, "000000070123456789abcdef01234567") == 7);
    assert(quota_find_account_by_id(&snapshot, "abcdefabcdefabcdefabcdefabcdefab") == -1);
}

static void test_serial_framing_recovers_after_overlong_line(void)
{
    quota_frame_decoder_t decoder;
    quota_frame_decoder_init(&decoder);
    const char *frame = NULL;
    size_t length = 0;
    for (size_t i = 0; i < QUOTA_MAX_PROVISION_FRAME_BYTES + 1; i++) {
        assert(quota_frame_decoder_feed(&decoder, 'x', &frame, &length) == QUOTA_FRAME_PENDING);
    }
    assert(quota_frame_decoder_feed(&decoder, '\n', &frame, &length) == QUOTA_FRAME_TOO_LONG);
    static const char next_frame[] = "@AIQ:{}\r\n";
    quota_frame_result_t result = QUOTA_FRAME_PENDING;
    for (size_t i = 0; i < sizeof(next_frame) - 1; i++) {
        result = quota_frame_decoder_feed(&decoder, next_frame[i], &frame, &length);
    }
    assert(result == QUOTA_FRAME_COMPLETE);
    assert(length == strlen("@AIQ:{}"));
    assert(memcmp(frame, "@AIQ:{}", length) == 0);
}

static void test_freshness_and_reset_states(void)
{
    assert(quota_data_is_stale(1000, false, 0, 60));
    assert(!quota_data_is_stale(1900, true, 1000, 60));
    assert(quota_data_is_stale(1901, true, 1000, 60));
    assert(!quota_data_is_stale(4599, true, 1000, 1800));
    assert(quota_data_is_stale(4601, true, 1000, 1800));
    assert(quota_data_is_stale(999, true, 1000, 300));
    assert(quota_usb_window_active(true, 1000, 1000));
    assert(quota_usb_window_active(true, 120999, 1000));
    assert(!quota_usb_window_active(true, 121000, 1000));
    assert(!quota_usb_window_active(false, 2000, 1000));

    quota_window_t window = {
        .present = true, .remaining_percent = 0, .has_resets_at = true, .resets_at = 1500};
    assert(quota_metric_state(&window, 1499) == QUOTA_METRIC_VALUE);
    assert(quota_metric_state(&window, 1500) == QUOTA_METRIC_WAITING_FOR_SOURCE);
    window.present = false;
    assert(quota_metric_state(&window, 1500) == QUOTA_METRIC_UNAVAILABLE);
}

static void test_navigation(void)
{
    quota_navigation_t navigation;
    quota_navigation_init(&navigation, true, 300, true, 120, 3);
    assert(navigation.screen == QUOTA_SCREEN_HOME);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_UP, 3) ==
           QUOTA_ACTION_PERSIST_SELECTION);
    assert(navigation.selected_account == 2);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_DOWN, 3) ==
           QUOTA_ACTION_PERSIST_SELECTION);
    assert(navigation.selected_account == 0);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 3) == QUOTA_ACTION_REFRESH);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_LONG, 3) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_SETTINGS);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_UP, 3) == QUOTA_ACTION_NONE);
    assert(navigation.settings_focus == 5);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 3) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_DEVICE_SETTINGS);
    assert(navigation.device_settings_focus == 0);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_LONG, 3) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_SETTINGS);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_DOWN, 3) == QUOTA_ACTION_NONE);
    assert(navigation.settings_focus == 0);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 3) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_ACCOUNTS);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_UP, 3) == QUOTA_ACTION_NONE);
    assert(navigation.account_focus == 3);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 3) ==
           QUOTA_ACTION_OPEN_PHONE);
    assert(navigation.screen == QUOTA_SCREEN_PHONE);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_LONG, 3) ==
           QUOTA_ACTION_CLOSE_PHONE);
    assert(navigation.screen == QUOTA_SCREEN_ACCOUNTS);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_LONG, 3) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_SETTINGS);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_LONG, 3) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_HOME);

    quota_navigation_init(&navigation, true, 300, true, 120, 1);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_LONG, 1) == QUOTA_ACTION_NONE);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_DOWN, 1) == QUOTA_ACTION_NONE);
    assert(navigation.settings_focus == 1);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 1) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_INTERVAL);
    assert(navigation.interval_focus == 2); /* focus starts on the current 5 minute setting */
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_UP, 1) == QUOTA_ACTION_NONE);
    assert(navigation.interval_focus == 1);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 1) ==
           QUOTA_ACTION_APPLY_SETTINGS);
    assert(navigation.screen == QUOTA_SCREEN_SETTINGS && navigation.refresh_seconds == 60);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 1) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_INTERVAL && navigation.interval_focus == 1);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_UP, 1) == QUOTA_ACTION_NONE);
    assert(navigation.interval_focus == 0);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 1) ==
           QUOTA_ACTION_APPLY_SETTINGS);
    assert(!navigation.auto_refresh);
    /* With auto refresh off, focus starts on the toggle; choosing an interval turns it back on. */
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 1) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_INTERVAL && navigation.interval_focus == 0);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_DOWN, 1) == QUOTA_ACTION_NONE);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_DOWN, 1) == QUOTA_ACTION_NONE);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 1) ==
           QUOTA_ACTION_APPLY_SETTINGS);
    assert(navigation.auto_refresh && navigation.refresh_seconds == 300);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 1) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_INTERVAL && navigation.interval_focus == 2);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_UP, 1) == QUOTA_ACTION_NONE);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_UP, 1) == QUOTA_ACTION_NONE);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 1) ==
           QUOTA_ACTION_APPLY_SETTINGS);
    assert(!navigation.auto_refresh);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_LONG, 1) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_HOME);

    quota_navigation_init(&navigation, false, 300, true, 120, 0);
    assert(navigation.screen == QUOTA_SCREEN_PHONE);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_LONG, 0) ==
           QUOTA_ACTION_CLOSE_PHONE);
    assert(navigation.screen == QUOTA_SCREEN_HOME);
}

static void test_portable_navigation(void)
{
    quota_navigation_t navigation;
    quota_navigation_init(&navigation, true, 300, true, 120, 2);
    navigation.screen = QUOTA_SCREEN_SETTINGS;
    navigation.settings_focus = 4;
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 2) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_NETWORK);
    /* Network details are passive: button input causes no action or page change. */
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_UP, 2) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_NETWORK);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_DOWN, 2) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_NETWORK);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 2) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_NETWORK);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_LONG, 2) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_SETTINGS);

    navigation.settings_focus = 5;
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 2) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_DEVICE_SETTINGS);
    assert(navigation.device_settings_focus == 0); /* Hotspot is the default choice. */
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 2) ==
           QUOTA_ACTION_OPEN_PHONE);
    assert(navigation.screen == QUOTA_SCREEN_PHONE && navigation.phone_step == 0);
    assert(navigation.setup_return_screen == QUOTA_SCREEN_DEVICE_SETTINGS);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 2) ==
           QUOTA_ACTION_RENEW_PHONE);
    assert(navigation.phone_step == 1);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 2) ==
           QUOTA_ACTION_RENEW_PHONE);
    assert(navigation.phone_step == 2); /* Manual address and full session secret. */
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 2) ==
           QUOTA_ACTION_RENEW_PHONE);
    assert(navigation.phone_step == 0);
    quota_navigation_handle(&navigation, QUOTA_INPUT_UP, 2);
    assert(navigation.phone_step == 2);
    quota_navigation_handle(&navigation, QUOTA_INPUT_DOWN, 2);
    assert(navigation.phone_step == 0);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_LONG, 2) ==
           QUOTA_ACTION_CLOSE_PHONE);
    assert(navigation.screen == QUOTA_SCREEN_DEVICE_SETTINGS);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_LONG, 2) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_SETTINGS);

    navigation.settings_focus = 5;
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 2) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_DEVICE_SETTINGS &&
           navigation.device_settings_focus == 0);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_UP, 2) == QUOTA_ACTION_NONE);
    assert(navigation.device_settings_focus == 1);
    navigation.configured = false;
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 2) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_SETUP);
    assert(navigation.setup_return_screen == QUOTA_SCREEN_DEVICE_SETTINGS);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_LONG, 2) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_DEVICE_SETTINGS);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_LONG, 2) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_SETTINGS);

    navigation.screen = QUOTA_SCREEN_AUTH;
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 0) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_SETUP &&
           navigation.setup_return_screen == QUOTA_SCREEN_AUTH);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_LONG, 0) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_AUTH);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_LONG, 2) ==
           QUOTA_ACTION_CANCEL_AUTH);
    assert(navigation.screen == QUOTA_SCREEN_ACCOUNTS);
    /* Appended screens/actions must not renumber the legacy USB state contract. */
    assert(QUOTA_SCREEN_SETUP == 5 && QUOTA_SCREEN_NETWORK == 6 && QUOTA_SCREEN_PHONE == 7 &&
           QUOTA_SCREEN_AUTH == 8 && QUOTA_SCREEN_DEVICE_SETTINGS == 9 &&
           QUOTA_ACTION_PERSIST_SELECTION == 3);
}

static void test_navigation_after_external_settings_change(void)
{
    quota_navigation_t navigation;
    quota_navigation_init(&navigation, true, 300, true, 120, 1);
    navigation.screen = QUOTA_SCREEN_INTERVAL;
    navigation.interval_focus = 0;

    quota_navigation_sync_settings(&navigation, 900, false, 120);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 1) ==
           QUOTA_ACTION_APPLY_SETTINGS);
    assert(navigation.auto_refresh && navigation.refresh_seconds == 900);

    /* A later remote change must survive editing just the interval. */
    quota_navigation_sync_settings(&navigation, 1800, false, 120);
    navigation.screen = QUOTA_SCREEN_INTERVAL;
    navigation.interval_focus = 2;
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 1) ==
           QUOTA_ACTION_APPLY_SETTINGS);
    assert(navigation.auto_refresh && navigation.refresh_seconds == 300);

    quota_navigation_sync_settings(&navigation, 61, false, 120);
    assert(navigation.auto_refresh && navigation.refresh_seconds == 300);
}

static void test_display_sleep_and_wake_gestures(void)
{
    quota_display_state_t display = {.last_input_ms = 10000};
    quota_display_tick(&display, 129999, 120, false);
    assert(!display.sleeping);
    quota_display_tick(&display, 130000, 120, false);
    assert(display.sleeping);
    /* Wake PRESS plus CLICK must not trigger a quota refresh/navigation. */
    assert(!quota_display_handle_key(&display, 130001, QUOTA_KEY_PRESS, false));
    assert(!display.sleeping);
    assert(!quota_display_handle_key(&display, 130010, QUOTA_KEY_CLICK, false));
    assert(quota_display_handle_key(&display, 130100, QUOTA_KEY_CLICK, false));
    assert(!quota_display_handle_key(&display, 130200, QUOTA_KEY_LONG, true));
    assert(display.sleeping);
    /* A held key wakes only: long DOWN cannot immediately switch it off again. */
    assert(!quota_display_handle_key(&display, 130300, QUOTA_KEY_PRESS, true));
    assert(!quota_display_handle_key(&display, 130900, QUOTA_KEY_LONG, true));
    assert(!display.sleeping);
    assert(!quota_display_handle_key(&display, 131000, QUOTA_KEY_PRESS, false));
    assert(quota_display_handle_key(&display, 131600, QUOTA_KEY_LONG, false));

    display = (quota_display_state_t){.last_input_ms = 10000};
    quota_display_tick(&display, 1000000, 0, false);
    assert(!display.sleeping);
    quota_display_tick(&display, 1000000, 30, true);
    assert(!display.sleeping && display.last_input_ms == 1000000);
    quota_display_tick(&display, 1029999, 30, false);
    assert(!display.sleeping);
    quota_display_tick(&display, 1030000, 30, false);
    assert(display.sleeping);
    /* Terminal events still wake safely if their PRESS event was dropped. */
    assert(!quota_display_handle_key(&display, 1030100, QUOTA_KEY_CLICK, false));
    assert(!display.sleeping);
    quota_display_tick(&display, 1, 30, false);
    assert(!display.sleeping && display.last_input_ms == 1);

    display.sleeping = true;
    assert(!quota_display_handle_key(&display, 2, QUOTA_KEY_PRESS, false));
    assert(!quota_display_handle_key(&display, 3, QUOTA_KEY_PRESS, false));
    assert(!quota_display_handle_key(&display, 4, QUOTA_KEY_DOUBLE, false));
    assert(!display.sleeping && !display.consume_wake_gesture);
}

static void test_screen_timeout_settings(void)
{
    for (size_t i = 0; i < QUOTA_SCREEN_TIMEOUT_COUNT; i++)
        assert(quota_screen_timeout_is_valid(quota_screen_timeouts[i]));
    assert(!quota_screen_timeout_is_valid(31) && !quota_screen_timeout_is_valid(601));

    quota_navigation_t navigation;
    quota_navigation_init(&navigation, true, 300, false, 120, 1);
    navigation.screen = QUOTA_SCREEN_SETTINGS;
    navigation.settings_focus = 3;
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 1) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_SLEEP && navigation.sleep_focus == 3);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_UP, 1) == QUOTA_ACTION_NONE);
    quota_navigation_sync_settings(&navigation, 900, true, 120);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 1) ==
           QUOTA_ACTION_APPLY_SETTINGS);
    assert(navigation.screen_timeout_seconds == 60 && navigation.refresh_seconds == 900 &&
           navigation.auto_refresh);
    navigation.screen = QUOTA_SCREEN_SLEEP;
    navigation.sleep_focus = 0;
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_UP, 1) == QUOTA_ACTION_NONE);
    assert(navigation.sleep_focus == 5);
}

static void test_deepseek_balance_contract(void)
{
    quota_balance_t balance = {.present = true, .is_available = false, .currency_count = 2};
    strcpy(balance.label, "My API");
    strcpy(balance.balance_infos[0].currency, "CNY");
    strcpy(balance.balance_infos[0].total_balance, "-0.12345678");
    strcpy(balance.balance_infos[0].granted_balance, "0");
    strcpy(balance.balance_infos[0].topped_up_balance, "-0.12345678");
    strcpy(balance.balance_infos[1].currency, "USD");
    strcpy(balance.balance_infos[1].total_balance, "1.23456789");
    strcpy(balance.balance_infos[1].granted_balance, "1.23456789");
    strcpy(balance.balance_infos[1].topped_up_balance, "0.00");
    assert(quota_balance_is_valid(&balance));
    assert(quota_balance_cny(&balance) == &balance.balance_infos[0]);
    quota_balance_t reordered = balance;
    reordered.balance_infos[0] = balance.balance_infos[1];
    reordered.balance_infos[1] = balance.balance_infos[0];
    assert(quota_balance_cny(&reordered) == &reordered.balance_infos[1]);
    reordered.currency_count = 1; /* USD alone must not become an RMB amount. */
    assert(quota_balance_cny(&reordered) == NULL);
    assert(quota_balance_cny(NULL) == NULL);
    quota_balance_t invalid = balance;
    memcpy(invalid.balance_infos[1].currency, "CNY", 4);
    assert(!quota_balance_is_valid(&invalid));
    invalid = balance;
    strcpy(invalid.balance_infos[0].total_balance, "NaN");
    assert(!quota_balance_is_valid(&invalid));
    invalid = balance;
    memset(invalid.balance_infos[0].total_balance, '1',
           sizeof(invalid.balance_infos[0].total_balance));
    assert(!quota_balance_is_valid(&invalid));
    quota_balance_t unknown = {0};
    assert(quota_balance_is_valid(&unknown) && quota_balance_cny(&unknown) == NULL);
}

static void test_remaining_duration(void)
{
    const struct {
        uint64_t seconds;
        const char *text;
    } cases[] = {
        {1, "<1h"},        {3599, "<1h"},    {3600, "0d 1h"},  {8100, "0d 2h"},
        {86399, "0d 23h"}, {86400, "1d 0h"}, {97200, "1d 3h"}, {604800, "7d 0h"},
    };
    char output[48], expected[48];
    quota_window_t window = {.present = true, .has_resets_at = true};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        window.resets_at = 1700000000 + cases[i].seconds;
        quota_format_reset_time(&window, 1700000000, true, output, sizeof(output));
        snprintf(expected, sizeof(expected), "\xEF\x80\xA1 %s", cases[i].text);
        assert(strcmp(output, expected) == 0);
    }
    /* Cached boot time is not a current-clock observation. */
    quota_format_reset_time(&window, 1700000000, false, output, sizeof(output));
    assert(strcmp(output, "时间待同步") == 0);
    quota_format_reset_time(&window, window.resets_at, true, output, sizeof(output));
    assert(strcmp(output, "等待新数据") == 0);
    window.has_resets_at = false;
    quota_format_reset_time(&window, 1700000000, true, output, sizeof(output));
    assert(strcmp(output, "重置时间未知") == 0);
}

int main(void)
{
    test_utf8_and_identifiers();
    test_display_text();
    test_account_lookup();
    test_serial_framing_recovers_after_overlong_line();
    test_freshness_and_reset_states();
    test_navigation();
    test_portable_navigation();
    test_navigation_after_external_settings_change();
    test_display_sleep_and_wake_gestures();
    test_screen_timeout_settings();
    test_deepseek_balance_contract();
    test_remaining_duration();
    puts("quota logic tests passed");
    return 0;
}
