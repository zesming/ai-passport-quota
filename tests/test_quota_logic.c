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
    quota_navigation_init(&navigation, true, 300, true, 3);
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
    assert(navigation.settings_focus == 3);
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

    quota_navigation_init(&navigation, true, 300, true, 1);
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

    quota_navigation_init(&navigation, false, 300, true, 0);
    assert(navigation.screen == QUOTA_SCREEN_SETUP);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_LONG, 0) == QUOTA_ACTION_NONE);
    assert(navigation.screen == QUOTA_SCREEN_HOME);
}

static void test_navigation_after_external_settings_change(void)
{
    quota_navigation_t navigation;
    quota_navigation_init(&navigation, true, 300, true, 1);
    navigation.screen = QUOTA_SCREEN_INTERVAL;
    navigation.interval_focus = 0;

    quota_navigation_sync_settings(&navigation, 900, false);
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 1) ==
           QUOTA_ACTION_APPLY_SETTINGS);
    assert(navigation.auto_refresh && navigation.refresh_seconds == 900);

    /* A later remote change must survive editing just the interval. */
    quota_navigation_sync_settings(&navigation, 1800, false);
    navigation.screen = QUOTA_SCREEN_INTERVAL;
    navigation.interval_focus = 2;
    assert(quota_navigation_handle(&navigation, QUOTA_INPUT_OK_SHORT, 1) ==
           QUOTA_ACTION_APPLY_SETTINGS);
    assert(!navigation.auto_refresh && navigation.refresh_seconds == 300);

    quota_navigation_sync_settings(&navigation, 61, true);
    assert(!navigation.auto_refresh && navigation.refresh_seconds == 300);
}

static void test_idle_display_and_pairing_visibility(void)
{
    assert(!quota_display_should_dim(129999, 10000, false));
    assert(quota_display_should_dim(130000, 10000, false));
    assert(!quota_display_should_dim(130000, 10000, true));
    assert(!quota_display_should_dim(130000, 130000, false));
    assert(!quota_display_should_dim(10000, 130000, false));
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
    test_idle_display_and_pairing_visibility();
    puts("quota logic tests passed");
    return 0;
}
