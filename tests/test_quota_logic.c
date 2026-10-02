#include "quota_logic.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *valid_account =
    "{\"id\":\"0123456789abcdef0123456789abcdef\",\"provider\":\"codex\","
    "\"email\":\"alex@example.com\",\"plan\":\"Plus\",\"status\":\"ok\","
    "\"observed_at\":1700000000,\"five_hour\":{\"remaining_percent\":0,"
    "\"resets_at\":1700000100},\"seven_day\":null}";

static void appendf(char *buffer, size_t capacity, size_t *length, const char *format, ...)
{
    assert(*length < capacity);
    va_list arguments;
    va_start(arguments, format);
    int count = vsnprintf(buffer + *length, capacity - *length, format, arguments);
    va_end(arguments);
    assert(count >= 0 && (size_t)count < capacity - *length);
    *length += (size_t)count;
}

static size_t make_snapshot(char *buffer, size_t capacity, const char *accounts,
                            const char *settings)
{
    int count = snprintf(buffer, capacity,
        "{\"v\":1,\"server_time\":1700000000,\"revision\":12,"
        "\"settings\":%s,\"accounts\":[%s]}", settings, accounts);
    assert(count >= 0 && (size_t)count < capacity);
    return (size_t)count;
}

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

static void test_urls_tokens_and_display(void)
{
    char host[16];
    assert(quota_url_is_private_ipv4("https://192.168.1.20:4318", host));
    assert(strcmp(host, "192.168.1.20") == 0);
    assert(quota_url_is_private_ipv4("https://10.0.0.3:4318", host));
    assert(quota_url_is_private_ipv4("https://172.31.255.254:4318", host));
    assert(!quota_url_is_private_ipv4("https://172.32.0.1:4318", host));
    assert(!quota_url_is_private_ipv4("https://8.8.8.8:4318", host));
    assert(!quota_url_is_private_ipv4("https://192.168.1.20:443", host));
    assert(!quota_url_is_private_ipv4("https://192.168.1.20:4318/", host));
    assert(!quota_url_is_private_ipv4("https://user@192.168.1.20:4318", host));
    assert(!quota_url_is_private_ipv4("https://192.168.001.20:4318", host));
    assert(!quota_url_is_private_ipv4("https://[fd00::1]:4318", host));
    assert(quota_pair_token_is_valid("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq"));
    assert(!quota_pair_token_is_valid("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq="));
    assert(!quota_pair_token_is_valid("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq/"));

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

static void test_snapshot_validation_and_null_semantics(void)
{
    char json[1024];
    size_t length = make_snapshot(json, sizeof(json), valid_account,
                                  "{\"refresh_seconds\":300,\"auto_refresh\":true}");
    quota_snapshot_t snapshot = {0};
    assert(quota_parse_snapshot(json, length, &snapshot));
    assert(snapshot.account_count == 1 && snapshot.refresh_seconds == 300);
    assert(snapshot.accounts[0].five_hour.present);
    assert(snapshot.accounts[0].five_hour.remaining_percent == 0);
    assert(snapshot.accounts[0].five_hour.has_resets_at);
    assert(snapshot.accounts[0].five_hour.resets_at == 1700000100);
    assert(!snapshot.accounts[0].seven_day.present);

    char absent_metric[] =
        "{\"v\":1,\"server_time\":1,\"revision\":0,"
        "\"settings\":{\"refresh_seconds\":60,\"auto_refresh\":false},"
        "\"accounts\":[{\"id\":\"0123456789abcdef0123456789abcdef\","
        "\"provider\":\"claude\",\"email\":\"\",\"plan\":\"\","
        "\"status\":\"waiting\",\"observed_at\":null,\"five_hour\":null,"
        "\"seven_day\":{\"remaining_percent\":100,\"resets_at\":null}}]}";
    assert(quota_parse_snapshot(absent_metric, strlen(absent_metric), &snapshot));
    assert(!snapshot.accounts[0].has_observed_at);
    assert(!snapshot.accounts[0].five_hour.present);
    assert(snapshot.accounts[0].seven_day.present);
    assert(snapshot.accounts[0].seven_day.remaining_percent == 100);
    assert(!snapshot.accounts[0].seven_day.has_resets_at);

    char bad[1100];
    size_t bad_length = make_snapshot(bad, sizeof(bad), valid_account,
                                      "{\"refresh_seconds\":61,\"auto_refresh\":true}");
    assert(!quota_parse_snapshot(bad, bad_length, &snapshot));
    bad_length = make_snapshot(bad, sizeof(bad), valid_account,
                               "{\"refresh_seconds\":300,\"auto_refresh\":1}");
    assert(!quota_parse_snapshot(bad, bad_length, &snapshot));
    bad_length = make_snapshot(bad, sizeof(bad), valid_account,
                               "{\"refresh_seconds\":300,\"auto_refresh\":true,"
                               "\"auto_refresh\":false}");
    assert(!quota_parse_snapshot(bad, bad_length, &snapshot));
    bad_length = make_snapshot(bad, sizeof(bad), valid_account,
                               "{\"refresh_seconds\":300,\"auto_refresh\":true}");
    bad[bad_length++] = 'x';
    assert(!quota_parse_snapshot(bad, bad_length, &snapshot));

    char invalid_percent[] =
        "{\"v\":1,\"server_time\":1,\"revision\":0,"
        "\"settings\":{\"refresh_seconds\":300,\"auto_refresh\":true},"
        "\"accounts\":[{\"id\":\"0123456789abcdef0123456789abcdef\","
        "\"provider\":\"codex\",\"email\":\"x@example.com\",\"plan\":\"P\","
        "\"status\":\"ok\",\"observed_at\":1,\"five_hour\":{"
        "\"remaining_percent\":101,\"resets_at\":null},\"seven_day\":null}]}";
    assert(!quota_parse_snapshot(invalid_percent, strlen(invalid_percent), &snapshot));
    char upper_id[sizeof(invalid_percent)];
    memcpy(upper_id, invalid_percent, sizeof(invalid_percent));
    char *id = strstr(upper_id, "0123456789abcdef");
    assert(id != NULL);
    id[0] = 'A';
    assert(!quota_parse_snapshot(upper_id, strlen(upper_id), &snapshot));
}

static void test_account_limit_and_identity(void)
{
    char accounts[8000] = {0};
    size_t length = 0;
    for (int i = 0; i < 9; i++) {
        appendf(accounts, sizeof(accounts), &length,
            "%s{\"id\":\"%08x0123456789abcdef01234567\","
            "\"provider\":\"codex\",\"email\":\"x@example.com\","
            "\"plan\":\"Plus\",\"status\":\"ok\",\"observed_at\":1,"
            "\"five_hour\":null,\"seven_day\":null}", i == 0 ? "" : ",", (unsigned)i);
    }
    char json[QUOTA_MAX_SNAPSHOT_BYTES + 1];
    size_t json_length = make_snapshot(json, sizeof(json), accounts,
                                       "{\"refresh_seconds\":300,\"auto_refresh\":true}");
    quota_snapshot_t snapshot;
    assert(!quota_parse_snapshot(json, json_length, &snapshot));

    length = 0;
    for (int i = 0; i < 8; i++) {
        appendf(accounts, sizeof(accounts), &length,
            "%s{\"id\":\"%08x0123456789abcdef01234567\","
            "\"provider\":\"codex\",\"email\":\"x@example.com\","
            "\"plan\":\"Plus\",\"status\":\"ok\",\"observed_at\":1,"
            "\"five_hour\":null,\"seven_day\":null}", i == 0 ? "" : ",", (unsigned)i);
    }
    json_length = make_snapshot(json, sizeof(json), accounts,
                                "{\"refresh_seconds\":1800,\"auto_refresh\":false}");
    assert(quota_parse_snapshot(json, json_length, &snapshot));
    assert(snapshot.account_count == 8);
    assert(quota_find_account_by_id(&snapshot, "000000070123456789abcdef01234567") == 7);
    assert(quota_find_account_by_id(&snapshot, "abcdefabcdefabcdefabcdefabcdefab") == -1);
}

static void test_utf8_field_byte_boundaries(void)
{
    char email[131];
    for (size_t i = 0; i < 64; i++) {
        email[i * 2] = (char)0xc3;
        email[i * 2 + 1] = (char)0xa9;
    }
    email[128] = '\0';
    char account[1024];
    int account_length = snprintf(account, sizeof(account),
        "{\"id\":\"0123456789abcdef0123456789abcdef\",\"provider\":\"codex\","
        "\"email\":\"%s\",\"plan\":\"Plus\",\"status\":\"ok\","
        "\"observed_at\":1,\"five_hour\":null,\"seven_day\":null}", email);
    assert(account_length > 0 && (size_t)account_length < sizeof(account));
    char json[1200];
    size_t json_length = make_snapshot(json, sizeof(json), account,
                                       "{\"refresh_seconds\":300,\"auto_refresh\":true}");
    quota_snapshot_t snapshot;
    assert(quota_parse_snapshot(json, json_length, &snapshot));
    assert(strlen(snapshot.accounts[0].email) == QUOTA_EMAIL_MAX_BYTES);

    email[128] = (char)0xc3;
    email[129] = (char)0xa9;
    email[130] = '\0';
    account_length = snprintf(account, sizeof(account),
        "{\"id\":\"0123456789abcdef0123456789abcdef\",\"provider\":\"codex\","
        "\"email\":\"%s\",\"plan\":\"Plus\",\"status\":\"ok\","
        "\"observed_at\":1,\"five_hour\":null,\"seven_day\":null}", email);
    assert(account_length > 0 && (size_t)account_length < sizeof(account));
    json_length = make_snapshot(json, sizeof(json), account,
                                "{\"refresh_seconds\":300,\"auto_refresh\":true}");
    assert(!quota_parse_snapshot(json, json_length, &snapshot));
}

static void test_provision_frame(void)
{
    static const char token[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq";
    char frame[1024];
    int count = snprintf(frame, sizeof(frame),
        "@AIQ:{\"v\":1,\"op\":\"configure\",\"request_id\":\"a1b2c3d4\","
        "\"ssid\":\"Office\",\"password\":\"p\\\"ass\\\\word\","
        "\"base_url\":\"https://192.168.1.20:4318\",\"pair_token\":\"%s\","
        "\"server_cert_pem\":\"-----BEGIN CERTIFICATE-----\\nabc\\n"
        "-----END CERTIFICATE-----\\n\",\"server_time\":1790899200}", token);
    assert(count > 0 && (size_t)count < sizeof(frame));
    quota_device_config_t config = {0};
    char request_id[9];
    const char *error = NULL;
    assert(quota_parse_provision_frame(frame, (size_t)count, &config, request_id, &error));
    assert(strcmp(request_id, "a1b2c3d4") == 0);
    assert(strcmp(config.ssid, "Office") == 0);
    assert(strcmp(config.password, "p\"ass\\word") == 0);
    assert(strcmp(config.base_url, "https://192.168.1.20:4318") == 0);
    assert(config.refresh_seconds == 300 && config.auto_refresh);
    assert(config.server_time == 1790899200 && error == NULL);

    quota_device_config_t preserved = config;
    int changed = snprintf(frame, sizeof(frame),
        "@AIQ:{\"v\":1,\"op\":\"configure\",\"request_id\":\"a1b2c3d4\","
        "\"ssid\":\"%s\",\"password\":\"\","
        "\"base_url\":\"https://192.168.1.20:4318\",\"pair_token\":\"%s\","
        "\"server_cert_pem\":\"-----BEGIN CERTIFICATE-----x"
        "-----END CERTIFICATE-----\",\"server_time\":1790899200}",
        "123456789012345678901234567890123", token);
    assert(changed > 0 && (size_t)changed < sizeof(frame));
    assert(!quota_parse_provision_frame(frame, (size_t)changed, &config,
                                        request_id, &error));
    assert(strcmp(error, "invalid_config") == 0);
    assert(memcmp(&config, &preserved, sizeof(config)) == 0);

    size_t too_long = QUOTA_MAX_PROVISION_FRAME_BYTES + 1;
    char *overlong = (char *)malloc(too_long);
    assert(overlong != NULL);
    memset(overlong, 'x', too_long);
    assert(!quota_parse_provision_frame(overlong, too_long, &config, request_id, &error));
    assert(strcmp(error, "invalid_frame") == 0);
    free(overlong);

    const char malformed[] = "@AIQ:{bad}";
    assert(!quota_parse_provision_frame(malformed, sizeof(malformed) - 1,
                                        &config, request_id, &error));
    assert(strcmp(error, "invalid_json") == 0);
    const char bad_version[] = "@AIQ:{\"v\":2,\"op\":\"configure\",\"request_id\":\"a1b2c3d4\"}";
    assert(!quota_parse_provision_frame(bad_version, sizeof(bad_version) - 1,
                                        &config, request_id, &error));
    assert(strcmp(error, "unsupported_version") == 0);
}

static void test_serial_framing_recovers_after_overlong_line(void)
{
    quota_frame_decoder_t decoder;
    quota_frame_decoder_init(&decoder);
    const char *frame = NULL;
    size_t length = 0;
    for (size_t i = 0; i < QUOTA_MAX_PROVISION_FRAME_BYTES + 1; i++) {
        assert(quota_frame_decoder_feed(&decoder, 'x', &frame, &length) ==
               QUOTA_FRAME_PENDING);
    }
    assert(quota_frame_decoder_feed(&decoder, '\n', &frame, &length) ==
           QUOTA_FRAME_TOO_LONG);
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
    assert(quota_pairing_window_active(true, 1000, 1000));
    assert(quota_pairing_window_active(true, 120999, 1000));
    assert(!quota_pairing_window_active(true, 121000, 1000));
    assert(!quota_pairing_window_active(false, 2000, 1000));

    quota_window_t window = {.present = true, .remaining_percent = 0,
                             .has_resets_at = true, .resets_at = 1500};
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
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 3) ==
           QUOTA_ACTION_REFRESH);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_LONG, 3) ==
           QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_SETTINGS);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_UP, 3) == QUOTA_ACTION_NONE);
    assert(navigation.settings_focus == 4);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 3) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_SETUP);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_LONG, 3) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_SETTINGS);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_DOWN, 3) == QUOTA_ACTION_NONE);
    assert(navigation.settings_focus == 0);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 3) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_ACCOUNTS);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_UP, 3) == QUOTA_ACTION_NONE);
    assert(navigation.account_focus == 3);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 3) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_SETUP);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_LONG, 3) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_ACCOUNTS);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_LONG, 3) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_HOME);

    quota_navigation_init(&navigation, true, 300, true, 120, 1);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_LONG, 1) == QUOTA_ACTION_NONE);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_DOWN, 1) == QUOTA_ACTION_NONE);
    assert(navigation.settings_focus == 1);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 1) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_INTERVAL);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_DOWN, 1) == QUOTA_ACTION_NONE);
    assert(navigation.interval_focus == 1);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 1) ==
           QUOTA_ACTION_APPLY_SETTINGS);
    assert(navigation.screen == QUOTA_SCREEN_SETTINGS && navigation.refresh_seconds == 60);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 1) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_INTERVAL);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 1) ==
           QUOTA_ACTION_APPLY_SETTINGS);
    assert(!navigation.auto_refresh);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_LONG, 1) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_HOME);

    quota_navigation_init(&navigation, false, 300, true, 120, 0);
    assert(navigation.screen == QUOTA_SCREEN_SETUP);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_LONG, 0) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_HOME);
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
    assert(!navigation.auto_refresh && navigation.refresh_seconds == 300);

    quota_navigation_sync_settings(&navigation, 61, true, 120);
    assert(!navigation.auto_refresh && navigation.refresh_seconds == 300);
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

static void test_screen_timeout_settings_compatibility(void)
{
    char json[1024];
    quota_snapshot_t snapshot;
    quota_settings_t settings;
    size_t length = make_snapshot(json, sizeof(json), valid_account,
        "{\"refresh_seconds\":300,\"auto_refresh\":true}");
    assert(quota_parse_snapshot(json, length, &snapshot));
    assert(!snapshot.has_screen_timeout_seconds);
    for (size_t i = 0; i < QUOTA_SCREEN_TIMEOUT_COUNT; i++) {
        char fields[128];
        snprintf(fields, sizeof(fields), "{\"refresh_seconds\":300,\"auto_refresh\":true,"
                 "\"screen_timeout_seconds\":%u}", (unsigned)quota_screen_timeouts[i]);
        length = make_snapshot(json, sizeof(json), valid_account, fields);
        assert(quota_parse_snapshot(json, length, &snapshot));
        assert(snapshot.has_screen_timeout_seconds);
        assert(snapshot.screen_timeout_seconds == quota_screen_timeouts[i]);
        snprintf(json, sizeof(json), "{\"v\":1,\"settings\":%s}", fields);
        assert(quota_parse_settings_ack(json, strlen(json), &settings));
        assert(settings.has_screen_timeout_seconds);
        assert(settings.screen_timeout_seconds == quota_screen_timeouts[i]);
    }
    const char *invalid[] = {"null", "true", "\"120\"", "-1", "31", "601", "30.5"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        char fields[128];
        snprintf(fields, sizeof(fields), "{\"refresh_seconds\":300,\"auto_refresh\":true,"
                 "\"screen_timeout_seconds\":%s}", invalid[i]);
        length = make_snapshot(json, sizeof(json), valid_account, fields);
        assert(!quota_parse_snapshot(json, length, &snapshot));
        snprintf(json, sizeof(json), "{\"v\":1,\"settings\":%s}", fields);
        assert(!quota_parse_settings_ack(json, strlen(json), &settings));
    }
    const char old_ack[] = "{\"v\":1,\"settings\":{\"refresh_seconds\":900,\"auto_refresh\":false}}";
    assert(quota_parse_settings_ack(old_ack, strlen(old_ack), &settings));
    assert(!settings.has_screen_timeout_seconds && settings.refresh_seconds == 900);
    const char duplicate[] = "{\"v\":1,\"settings\":{\"refresh_seconds\":300,\"auto_refresh\":true,"
        "\"screen_timeout_seconds\":30,\"screen_timeout_seconds\":60}}";
    assert(!quota_parse_settings_ack(duplicate, strlen(duplicate), &settings));
    assert(!quota_parse_settings_ack("{}junk", 6, &settings));

    quota_navigation_t navigation;
    quota_navigation_init(&navigation, true, 300, false, 120, 1);
    navigation.screen = QUOTA_SCREEN_SETTINGS;
    navigation.settings_focus = 3;
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 1) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_SLEEP && navigation.sleep_focus == 3);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_UP, 1) == QUOTA_ACTION_NONE);
    quota_navigation_sync_settings(&navigation, 900, true, 120);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 1) == QUOTA_ACTION_APPLY_SETTINGS);
    assert(navigation.screen_timeout_seconds == 60 && navigation.refresh_seconds == 900 && navigation.auto_refresh);
    navigation.screen = QUOTA_SCREEN_SLEEP;
    navigation.sleep_focus = 0;
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_UP, 1) == QUOTA_ACTION_NONE);
    assert(navigation.sleep_focus == 5);
}

static void test_deepseek_balance_contract(void)
{
    const char *account =
        "{\"id\":\"0123456789abcdef0123456789abcdef\",\"provider\":\"deepseek\","
        "\"email\":\"\",\"label\":\"My API\",\"plan\":\"API\",\"status\":\"ok\","
        "\"observed_at\":1700000000,\"five_hour\":null,\"seven_day\":null,"
        "\"balance\":{\"is_available\":false,\"balance_infos\":["
        "{\"currency\":\"CNY\",\"total_balance\":\"-0.12345678\",\"granted_balance\":\"0\",\"topped_up_balance\":\"-0.12345678\"},"
        "{\"currency\":\"USD\",\"total_balance\":\"1.23456789\",\"granted_balance\":\"1.23456789\",\"topped_up_balance\":\"0.00\"}]}}";
    char json[8192];
    size_t length = make_snapshot(json, sizeof(json), account,
        "{\"refresh_seconds\":300,\"auto_refresh\":true,\"screen_timeout_seconds\":120}");
    quota_snapshot_t snapshot;
    assert(quota_parse_snapshot(json, length, &snapshot));
    assert(snapshot.accounts[0].provider == QUOTA_PROVIDER_DEEPSEEK);
    assert(!snapshot.accounts[0].five_hour.present && !snapshot.accounts[0].seven_day.present);
    const quota_balance_t *balance = &snapshot.balances[0];
    assert(balance->present && !balance->is_available && balance->currency_count == 2);
    assert(strcmp(balance->label, "My API") == 0);
    assert(strcmp(balance->balance_infos[0].total_balance, "-0.12345678") == 0);
    assert(strcmp(balance->balance_infos[1].total_balance, "1.23456789") == 0);
    assert(quota_balance_is_valid(balance));
    assert(quota_balance_cny(balance) == &balance->balance_infos[0]);
    quota_balance_t reordered = *balance;
    reordered.balance_infos[0] = balance->balance_infos[1];
    reordered.balance_infos[1] = balance->balance_infos[0];
    assert(quota_balance_cny(&reordered) == &reordered.balance_infos[1]);
    reordered.currency_count = 1;  /* USD alone must not become an RMB amount. */
    assert(quota_balance_cny(&reordered) == NULL);
    assert(quota_balance_cny(NULL) == NULL);
    quota_balance_t invalid = *balance;
    memcpy(invalid.balance_infos[1].currency, "CNY", 4);
    assert(!quota_balance_is_valid(&invalid));
    invalid = *balance;
    strcpy(invalid.balance_infos[0].total_balance, "NaN");
    assert(!quota_balance_is_valid(&invalid));
    invalid = *balance;
    memset(invalid.balance_infos[0].total_balance, '1', sizeof(invalid.balance_infos[0].total_balance));
    assert(!quota_balance_is_valid(&invalid));
    char *amount = strstr(json, "-0.12345678");
    assert(amount != NULL); amount[0] = 'e';
    assert(!quota_parse_snapshot(json, length, &snapshot));
    const char *unknown =
        "{\"id\":\"0123456789abcdef0123456789abcdef\",\"provider\":\"deepseek\","
        "\"email\":\"\",\"plan\":\"API\",\"status\":\"waiting\","
        "\"observed_at\":null,\"five_hour\":null,\"seven_day\":null,\"balance\":null}";
    length = make_snapshot(json, sizeof(json), unknown,
        "{\"refresh_seconds\":300,\"auto_refresh\":true}");
    assert(quota_parse_snapshot(json, length, &snapshot));
    assert(!snapshot.balances[0].present && snapshot.balances[0].currency_count == 0);
    /* Old providers retain their quota windows and never gain a fabricated wallet. */
    length = make_snapshot(json, sizeof(json), valid_account,
        "{\"refresh_seconds\":300,\"auto_refresh\":true}");
    assert(quota_parse_snapshot(json, length, &snapshot));
    assert(snapshot.accounts[0].five_hour.present && !snapshot.balances[0].present);
}

int main(void)
{
    test_utf8_and_identifiers();
    test_urls_tokens_and_display();
    test_snapshot_validation_and_null_semantics();
    test_account_limit_and_identity();
    test_utf8_field_byte_boundaries();
    test_provision_frame();
    test_serial_framing_recovers_after_overlong_line();
    test_freshness_and_reset_states();
    test_navigation();
    test_navigation_after_external_settings_change();
    test_display_sleep_and_wake_gestures();
    test_screen_timeout_settings_compatibility();
    test_deepseek_balance_contract();
    puts("quota logic tests passed");
    return 0;
}
