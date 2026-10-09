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
    assert(quota_usb_window_active(true, 1000, 121000));
    assert(quota_usb_window_active(true, 120999, 121000));
    assert(!quota_usb_window_active(true, 121000, 121000));
    assert(!quota_usb_window_active(true, 1000, 0));
    assert(!quota_usb_window_active(false, 2000, 121000));

    quota_window_t window = {
        .present = true, .remaining_percent = 0, .has_resets_at = true, .resets_at = 1500};
    assert(quota_metric_state(&window, 1499) == QUOTA_METRIC_VALUE);
    assert(quota_metric_state(&window, 1500) == QUOTA_METRIC_WAITING_FOR_SOURCE);
    window.present = false;
    assert(quota_metric_state(&window, 1500) == QUOTA_METRIC_UNAVAILABLE);
}

/* Drive the navigation from the home screen as the keys would. */
static quota_navigation_context_t ctx(uint8_t accounts, bool auth_active)
{
    return (quota_navigation_context_t){.account_count = accounts, .auth_active = auth_active};
}

static quota_action_t press(quota_navigation_t *n, quota_input_t input, uint8_t accounts)
{
    return quota_navigation_handle(n, input, ctx(accounts, false));
}

static void test_home_keys(void)
{
    quota_navigation_t n;
    quota_navigation_init(&n, true, 300, true, 120);
    assert(n.screen == QUOTA_SCREEN_HOME);
    /* UP/DOWN switch accounts and wrap; OK refreshes; long OK opens the menu. */
    assert(press(&n, QUOTA_INPUT_UP, 3) == QUOTA_ACTION_PERSIST_SELECTION);
    assert(n.selected_account == 2);
    assert(press(&n, QUOTA_INPUT_DOWN, 3) == QUOTA_ACTION_PERSIST_SELECTION);
    assert(n.selected_account == 0);
    assert(press(&n, QUOTA_INPUT_OK_SHORT, 3) == QUOTA_ACTION_REFRESH);
    assert(press(&n, QUOTA_INPUT_UP, 1) == QUOTA_ACTION_NONE); /* one account: nothing to switch */
    assert(press(&n, QUOTA_INPUT_OK_LONG, 3) == QUOTA_ACTION_NONE && n.screen == QUOTA_SCREEN_MENU);
    /* The home screen has no level above it: from the menu a long OK returns to it. */
    assert(press(&n, QUOTA_INPUT_OK_LONG, 3) == QUOTA_ACTION_NONE && n.screen == QUOTA_SCREEN_HOME);
    assert(press(&n, QUOTA_INPUT_OTHER, 3) == QUOTA_ACTION_NONE);
    /* While authorization runs, OK shows it instead of refreshing. */
    assert(quota_navigation_handle(&n, QUOTA_INPUT_OK_SHORT, ctx(3, true)) == QUOTA_ACTION_NONE);
    assert(n.screen == QUOTA_SCREEN_AUTH);
}

static void test_welcome_screen(void)
{
    quota_navigation_t n;
    quota_navigation_init(&n, true, 300, true, 120);
    /* No account: OK opens the hotspot setup; long OK goes back to the welcome screen. */
    assert(press(&n, QUOTA_INPUT_OK_SHORT, 0) == QUOTA_ACTION_OPEN_HOTSPOT);
    assert(n.screen == QUOTA_SCREEN_HOTSPOT && n.hotspot_page == 0);
    assert(press(&n, QUOTA_INPUT_OK_LONG, 0) == QUOTA_ACTION_CLOSE_HOTSPOT);
    assert(n.screen == QUOTA_SCREEN_HOME);
    /* "Long OK, DOWN, OK" reaches USB setup from the welcome screen, every time. */
    for (int round = 0; round < 3; round++) {
        assert(press(&n, QUOTA_INPUT_OK_LONG, 0) == QUOTA_ACTION_NONE);
        assert(n.screen == QUOTA_SCREEN_MENU && n.menu_focus == 0);
        assert(press(&n, QUOTA_INPUT_DOWN, 0) == QUOTA_ACTION_NONE);
        assert(press(&n, QUOTA_INPUT_OK_SHORT, 0) == QUOTA_ACTION_OPEN_USB);
        assert(n.screen == QUOTA_SCREEN_USB);
        assert(press(&n, QUOTA_INPUT_OK_LONG, 0) == QUOTA_ACTION_CLOSE_USB);
        assert(n.screen == QUOTA_SCREEN_MENU && n.menu_focus == 0); /* reset: no account */
        assert(press(&n, QUOTA_INPUT_OK_LONG, 0) == QUOTA_ACTION_NONE);
        assert(n.screen == QUOTA_SCREEN_HOME);
    }
}

static void test_menu_order_wrap_and_memory(void)
{
    quota_navigation_t n;
    quota_navigation_init(&n, true, 300, true, 120);
    assert(QUOTA_MENU_ITEMS == 5);
    press(&n, QUOTA_INPUT_OK_LONG, 2);
    assert(n.screen == QUOTA_SCREEN_MENU && n.menu_focus == 0);
    /* Order: hotspot, USB, refresh rate, auto sleep, device information. */
    const struct {
        quota_screen_t screen;
        quota_action_t action;
    } items[QUOTA_MENU_ITEMS] = {
        {QUOTA_SCREEN_HOTSPOT, QUOTA_ACTION_OPEN_HOTSPOT},
        {QUOTA_SCREEN_USB, QUOTA_ACTION_OPEN_USB},
        {QUOTA_SCREEN_REFRESH, QUOTA_ACTION_NONE},
        {QUOTA_SCREEN_SLEEP, QUOTA_ACTION_NONE},
        {QUOTA_SCREEN_INFO, QUOTA_ACTION_NONE},
    };
    for (uint8_t i = 0; i < QUOTA_MENU_ITEMS; i++) {
        n.screen = QUOTA_SCREEN_MENU;
        n.menu_focus = i;
        assert(press(&n, QUOTA_INPUT_OK_SHORT, 2) == items[i].action);
        assert(n.screen == items[i].screen);
    }
    /* UP from the first item wraps to device information; DOWN from the last wraps back. */
    n.screen = QUOTA_SCREEN_MENU;
    n.menu_focus = 0;
    press(&n, QUOTA_INPUT_UP, 2);
    assert(n.menu_focus == 4);
    press(&n, QUOTA_INPUT_DOWN, 2);
    assert(n.menu_focus == 0);
    /* With an account the menu remembers its focus across leaving and re-entering it. */
    n.menu_focus = 3;
    press(&n, QUOTA_INPUT_OK_SHORT, 2);
    assert(n.screen == QUOTA_SCREEN_SLEEP);
    press(&n, QUOTA_INPUT_OK_LONG, 2);
    assert(n.screen == QUOTA_SCREEN_MENU && n.menu_focus == 3);
    press(&n, QUOTA_INPUT_OK_LONG, 2);
    assert(n.screen == QUOTA_SCREEN_HOME);
    press(&n, QUOTA_INPUT_OK_LONG, 2);
    assert(n.menu_focus == 3);
    /* Without an account it opens on the first item whatever was remembered. */
    n.screen = QUOTA_SCREEN_HOME;
    press(&n, QUOTA_INPUT_OK_LONG, 0);
    assert(n.menu_focus == 0);
}

static void test_option_lists(void)
{
    quota_navigation_t n;
    quota_navigation_init(&n, true, 300, true, 120);
    n.screen = QUOTA_SCREEN_MENU;
    n.menu_focus = 2;
    press(&n, QUOTA_INPUT_OK_SHORT, 1);
    assert(n.screen == QUOTA_SCREEN_REFRESH && n.option_focus == 2); /* starts on 5 minutes */
    press(&n, QUOTA_INPUT_UP, 1);
    assert(n.option_focus == 1);
    assert(press(&n, QUOTA_INPUT_OK_SHORT, 1) == QUOTA_ACTION_APPLY_SETTINGS);
    assert(n.screen == QUOTA_SCREEN_MENU && n.menu_focus == 2 && n.refresh_seconds == 60 &&
           n.auto_refresh);
    /* The list is manual, 1, 5, 15, 30 minutes and wraps in both directions. */
    press(&n, QUOTA_INPUT_OK_SHORT, 1);
    assert(n.option_focus == 1);
    press(&n, QUOTA_INPUT_UP, 1);
    press(&n, QUOTA_INPUT_UP, 1);
    assert(n.option_focus == QUOTA_REFRESH_OPTIONS - 1);
    press(&n, QUOTA_INPUT_DOWN, 1);
    assert(n.option_focus == 0);
    assert(press(&n, QUOTA_INPUT_OK_SHORT, 1) == QUOTA_ACTION_APPLY_SETTINGS);
    assert(!n.auto_refresh && n.refresh_seconds == 60);
    /* Manual is the current value now, so the list opens on it; choosing an interval clears it. */
    press(&n, QUOTA_INPUT_OK_SHORT, 1);
    assert(n.option_focus == 0);
    for (int i = 0; i < 4; i++)
        press(&n, QUOTA_INPUT_DOWN, 1);
    assert(n.option_focus == 4);
    assert(press(&n, QUOTA_INPUT_OK_SHORT, 1) == QUOTA_ACTION_APPLY_SETTINGS);
    assert(n.auto_refresh && n.refresh_seconds == 1800);
    /* A long OK leaves the list without saving. */
    press(&n, QUOTA_INPUT_OK_SHORT, 1);
    press(&n, QUOTA_INPUT_UP, 1);
    assert(press(&n, QUOTA_INPUT_OK_LONG, 1) == QUOTA_ACTION_NONE);
    assert(n.screen == QUOTA_SCREEN_MENU && n.refresh_seconds == 1800);

    n.menu_focus = 3;
    press(&n, QUOTA_INPUT_OK_SHORT, 1);
    assert(n.screen == QUOTA_SCREEN_SLEEP && n.option_focus == 3); /* starts on 2 minutes */
    press(&n, QUOTA_INPUT_UP, 1);
    assert(press(&n, QUOTA_INPUT_OK_SHORT, 1) == QUOTA_ACTION_APPLY_SETTINGS);
    assert(n.screen_timeout_seconds == 60 && n.screen == QUOTA_SCREEN_MENU);
    n.screen = QUOTA_SCREEN_SLEEP;
    n.option_focus = 0;
    press(&n, QUOTA_INPUT_UP, 1);
    assert(n.option_focus == QUOTA_SCREEN_TIMEOUT_COUNT - 1); /* never ... 10 minutes, wrapped */
}

static quota_action_t press_hotspot(quota_navigation_t *n, quota_input_t input,
                                    quota_hotspot_state_t state)
{
    quota_navigation_context_t context = ctx(2, false);
    context.hotspot = state;
    return quota_navigation_handle(n, input, context);
}

static void test_hotspot_pages(void)
{
    quota_navigation_t n;
    quota_navigation_init(&n, true, 300, true, 120);
    n.screen = QUOTA_SCREEN_MENU;
    n.menu_focus = 0;
    quota_navigation_context_t closed = ctx(2, false);
    closed.hotspot = QUOTA_HOTSPOT_CLOSED;
    assert(quota_navigation_handle(&n, QUOTA_INPUT_OK_SHORT, closed) == QUOTA_ACTION_OPEN_HOTSPOT);
    assert(n.screen == QUOTA_SCREEN_HOTSPOT && n.hotspot_page == 0 &&
           n.return_screen == QUOTA_SCREEN_MENU);
    /* While the pages show, OK turns them and wraps; it never asks the service for anything. */
    for (int i = 1; i <= 3; i++) {
        assert(press_hotspot(&n, QUOTA_INPUT_OK_SHORT, QUOTA_HOTSPOT_SHOWING) == QUOTA_ACTION_NONE);
        assert(n.hotspot_page == i % 3);
    }
    /* UP and DOWN turn pages too, in a loop. */
    press(&n, QUOTA_INPUT_UP, 2);
    assert(n.hotspot_page == 2);
    press(&n, QUOTA_INPUT_DOWN, 2);
    assert(n.hotspot_page == 0);
    /* Opening, validating or in error, OK does nothing at all: a second open would abort the
     * validation that is still using the Wi-Fi credentials. */
    n.hotspot_page = 1;
    assert(press_hotspot(&n, QUOTA_INPUT_OK_SHORT, QUOTA_HOTSPOT_BUSY) == QUOTA_ACTION_NONE);
    assert(n.hotspot_page == 1 && n.screen == QUOTA_SCREEN_HOTSPOT);
    /* A closed hotspot (with or without a result) is reopened by OK, from page one. */
    assert(press_hotspot(&n, QUOTA_INPUT_OK_SHORT, QUOTA_HOTSPOT_CLOSED) ==
           QUOTA_ACTION_RENEW_HOTSPOT);
    assert(n.hotspot_page == 0);
    assert(press(&n, QUOTA_INPUT_OK_LONG, 2) == QUOTA_ACTION_CLOSE_HOTSPOT);
    assert(n.screen == QUOTA_SCREEN_MENU);
    /* Entering from the menu only opens a closed hotspot, never one that is showing or busy. */
    n.menu_focus = 0;
    assert(quota_navigation_handle(&n, QUOTA_INPUT_OK_SHORT, closed) == QUOTA_ACTION_OPEN_HOTSPOT);
    n.screen = QUOTA_SCREEN_MENU;
    for (quota_hotspot_state_t state = QUOTA_HOTSPOT_SHOWING; state <= QUOTA_HOTSPOT_BUSY;
         state++) {
        n.screen = QUOTA_SCREEN_MENU;
        assert(press_hotspot(&n, QUOTA_INPUT_OK_SHORT, state) == QUOTA_ACTION_NONE);
        assert(n.screen == QUOTA_SCREEN_HOTSPOT);
    }
    /* The welcome screen opens the hotspot the same way. */
    n.screen = QUOTA_SCREEN_HOME;
    assert(quota_navigation_handle(&n, QUOTA_INPUT_OK_SHORT,
                                   (quota_navigation_context_t){.hotspot = QUOTA_HOTSPOT_CLOSED}) ==
           QUOTA_ACTION_OPEN_HOTSPOT);
    n.screen = QUOTA_SCREEN_HOME;
    assert(quota_navigation_handle(&n, QUOTA_INPUT_OK_SHORT,
                                   (quota_navigation_context_t){.hotspot = QUOTA_HOTSPOT_BUSY}) ==
           QUOTA_ACTION_NONE);
    assert(n.screen == QUOTA_SCREEN_HOTSPOT);
}

static void test_hotspot_state(void)
{
    assert(quota_hotspot_state(false, false, true, true, false) == QUOTA_HOTSPOT_SHOWING);
    assert(quota_hotspot_state(false, false, true, false, false) == QUOTA_HOTSPOT_BUSY);
    assert(quota_hotspot_state(false, false, false, false, true) == QUOTA_HOTSPOT_BUSY);
    assert(quota_hotspot_state(false, true, false, false, false) == QUOTA_HOTSPOT_BUSY);
    assert(quota_hotspot_state(true, false, false, false, false) == QUOTA_HOTSPOT_BUSY);
    assert(quota_hotspot_state(true, false, true, true, false) == QUOTA_HOTSPOT_BUSY);
    assert(quota_hotspot_state(false, false, false, false, false) == QUOTA_HOTSPOT_CLOSED);
}

static bool font_has(uint32_t codepoint)
{
    return codepoint == 0x5de5 || codepoint == 0x4f5c; /* 工 and 作 */
}

static void test_displayable_text(void)
{
    assert(quota_text_is_displayable("Work 1", NULL) && quota_text_is_displayable("", NULL));
    assert(!quota_text_is_displayable("工作", NULL)); /* no font: nothing above ASCII is drawn */
    assert(quota_text_is_displayable("工作", font_has));
    assert(!quota_text_is_displayable("工作笔", font_has)); /* one missing glyph is enough */
    assert(!quota_text_is_displayable("a\tb", font_has) &&
           !quota_text_is_displayable("a\x7f", NULL));
    assert(!quota_text_is_displayable("caf\xc3", font_has));          /* truncated UTF-8 */
    assert(!quota_text_is_displayable("\xf0\x9f\x98\x80", font_has)); /* an emoji */
    assert(!quota_text_is_displayable(NULL, font_has));
}

static void test_usb_screen_and_auth_screen(void)
{
    quota_navigation_t n;
    quota_navigation_init(&n, true, 300, true, 120);
    n.screen = QUOTA_SCREEN_USB;
    n.return_screen = QUOTA_SCREEN_MENU;
    /* The window is closed or ended: OK opens it again. While it is open or being prepared, OK
     * and a menu visit leave its session and clock alone. */
    assert(press(&n, QUOTA_INPUT_OK_SHORT, 2) == QUOTA_ACTION_OPEN_USB &&
           n.screen == QUOTA_SCREEN_USB);
    quota_navigation_context_t open = ctx(2, false);
    open.usb_window_open = true;
    assert(quota_navigation_handle(&n, QUOTA_INPUT_OK_SHORT, open) == QUOTA_ACTION_NONE);
    assert(press(&n, QUOTA_INPUT_UP, 2) == QUOTA_ACTION_NONE && n.screen == QUOTA_SCREEN_USB);
    n.screen = QUOTA_SCREEN_MENU;
    n.menu_focus = 1;
    assert(quota_navigation_handle(&n, QUOTA_INPUT_OK_SHORT, open) == QUOTA_ACTION_NONE);
    assert(n.screen == QUOTA_SCREEN_USB);
    n.screen = QUOTA_SCREEN_MENU;
    assert(press(&n, QUOTA_INPUT_OK_SHORT, 2) == QUOTA_ACTION_OPEN_USB);
    n.screen = QUOTA_SCREEN_USB;
    assert(press(&n, QUOTA_INPUT_OK_LONG, 2) == QUOTA_ACTION_CLOSE_USB &&
           n.screen == QUOTA_SCREEN_MENU);

    /* Authorization: OK asks to cancel, long OK goes home and leaves it running. */
    n.screen = QUOTA_SCREEN_AUTH;
    assert(press(&n, QUOTA_INPUT_OK_LONG, 2) == QUOTA_ACTION_NONE && n.screen == QUOTA_SCREEN_HOME);
    n.screen = QUOTA_SCREEN_AUTH;
    assert(quota_navigation_handle(&n, QUOTA_INPUT_OK_SHORT, ctx(2, true)) == QUOTA_ACTION_NONE);
    assert(n.screen == QUOTA_SCREEN_CONFIRM && n.confirm_kind == QUOTA_CONFIRM_CANCEL_AUTH &&
           n.confirm_focus == 0);
    /* Long OK in the box goes back to the authorization screen: nothing was cancelled. */
    assert(quota_navigation_handle(&n, QUOTA_INPUT_OK_LONG, ctx(2, true)) == QUOTA_ACTION_NONE);
    assert(n.screen == QUOTA_SCREEN_AUTH);
    /* OK on the harmless default also leaves it alone. */
    quota_navigation_handle(&n, QUOTA_INPUT_OK_SHORT, ctx(2, true));
    assert(quota_navigation_handle(&n, QUOTA_INPUT_OK_SHORT, ctx(2, true)) == QUOTA_ACTION_NONE);
    assert(n.screen == QUOTA_SCREEN_AUTH);
    /* Down to "取消授权", OK: cancelled, back to home. */
    quota_navigation_handle(&n, QUOTA_INPUT_OK_SHORT, ctx(2, true));
    quota_navigation_handle(&n, QUOTA_INPUT_DOWN, ctx(2, true));
    assert(n.confirm_focus == 1);
    assert(quota_navigation_handle(&n, QUOTA_INPUT_OK_SHORT, ctx(2, true)) ==
           QUOTA_ACTION_CANCEL_AUTH);
    assert(n.screen == QUOTA_SCREEN_HOME);
    /* A finished authorization: OK just returns home. */
    n.screen = QUOTA_SCREEN_AUTH;
    assert(press(&n, QUOTA_INPUT_OK_SHORT, 2) == QUOTA_ACTION_NONE &&
           n.screen == QUOTA_SCREEN_HOME);
}

static void test_factory_reset_confirmation(void)
{
    quota_navigation_t n;
    quota_navigation_init(&n, true, 300, true, 120);
    n.screen = QUOTA_SCREEN_MENU;
    /* Fresh device, J9: long OK (menu), UP (wraps to device info), OK, OK, DOWN, OK = 6 keys. */
    n.screen = QUOTA_SCREEN_HOME;
    int keys = 0;
    press(&n, QUOTA_INPUT_OK_LONG, 2), keys++;
    press(&n, QUOTA_INPUT_UP, 2), keys++;
    press(&n, QUOTA_INPUT_OK_SHORT, 2), keys++;
    assert(n.screen == QUOTA_SCREEN_INFO);
    press(&n, QUOTA_INPUT_OK_SHORT, 2), keys++;
    assert(n.screen == QUOTA_SCREEN_CONFIRM && n.confirm_kind == QUOTA_CONFIRM_FACTORY_RESET);
    assert(n.confirm_focus == 0); /* the default is cancel */
    press(&n, QUOTA_INPUT_DOWN, 2), keys++;
    assert(n.confirm_focus == 1);
    assert(press(&n, QUOTA_INPUT_OK_SHORT, 2) == QUOTA_ACTION_FACTORY_RESET);
    keys++;
    assert(keys == 6 && n.factory_resetting);
    /* While erasing every key is ignored, a long OK too, and nothing repeats the reset. */
    assert(press(&n, QUOTA_INPUT_OK_SHORT, 2) == QUOTA_ACTION_NONE);
    assert(press(&n, QUOTA_INPUT_DOWN, 2) == QUOTA_ACTION_NONE);
    assert(press(&n, QUOTA_INPUT_OK_LONG, 2) == QUOTA_ACTION_NONE);
    assert(n.screen == QUOTA_SCREEN_CONFIRM && n.factory_resetting);
    /* The erase failed (the caller says so): the box reports it; only a long OK works. */
    n.factory_resetting = false;
    n.factory_failed = true;
    assert(press(&n, QUOTA_INPUT_OK_SHORT, 2) == QUOTA_ACTION_NONE &&
           n.screen == QUOTA_SCREEN_CONFIRM);
    press(&n, QUOTA_INPUT_UP, 2);
    assert(n.confirm_focus == 1 && n.screen == QUOTA_SCREEN_CONFIRM);
    assert(press(&n, QUOTA_INPUT_OK_LONG, 2) == QUOTA_ACTION_NONE);
    assert(n.screen == QUOTA_SCREEN_INFO && !n.factory_failed);

    /* OK on cancel, UP/DOWN in a loop of two, and a long OK never reset anything. */
    press(&n, QUOTA_INPUT_OK_SHORT, 2);
    assert(press(&n, QUOTA_INPUT_OK_SHORT, 2) == QUOTA_ACTION_NONE &&
           n.screen == QUOTA_SCREEN_INFO);
    press(&n, QUOTA_INPUT_OK_SHORT, 2);
    press(&n, QUOTA_INPUT_UP, 2);
    assert(n.confirm_focus == 1);
    assert(press(&n, QUOTA_INPUT_OK_LONG, 2) == QUOTA_ACTION_NONE && n.screen == QUOTA_SCREEN_INFO);
    /* The box always opens on its harmless choice, also after the delete choice was left. */
    press(&n, QUOTA_INPUT_OK_SHORT, 2);
    assert(n.confirm_focus == 0);
}

static void test_no_long_press_is_destructive(void)
{
    /* Over every screen and every focus, a long OK can only go back: never reset or cancel. */
    for (int screen = QUOTA_SCREEN_HOME; screen <= QUOTA_SCREEN_CONFIRM; screen++) {
        for (int focus = 0; focus < 6; focus++) {
            quota_navigation_t n;
            quota_navigation_init(&n, true, 300, true, 120);
            n.screen = (quota_screen_t)screen;
            n.return_screen = QUOTA_SCREEN_MENU;
            n.menu_focus = n.option_focus = n.confirm_focus = (uint8_t)(focus % 2);
            for (int auth = 0; auth < 2; auth++) {
                quota_action_t action =
                    quota_navigation_handle(&n, QUOTA_INPUT_OK_LONG, ctx(2, auth));
                assert(action != QUOTA_ACTION_FACTORY_RESET && action != QUOTA_ACTION_CANCEL_AUTH &&
                       action != QUOTA_ACTION_APPLY_SETTINGS && action != QUOTA_ACTION_REFRESH &&
                       !n.factory_resetting);
            }
        }
    }
}

static void test_navigation_after_external_settings_change(void)
{
    quota_navigation_t navigation;
    quota_navigation_init(&navigation, true, 300, true, 120);
    navigation.screen = QUOTA_SCREEN_REFRESH;
    navigation.option_focus = 0;

    quota_navigation_sync_settings(&navigation, 900, false, 120);
    assert(press(&navigation, QUOTA_INPUT_OK_SHORT, 1) == QUOTA_ACTION_APPLY_SETTINGS);
    assert(!navigation.auto_refresh && navigation.refresh_seconds == 900);

    /* A later remote change must survive editing just the interval. */
    quota_navigation_sync_settings(&navigation, 1800, false, 120);
    navigation.screen = QUOTA_SCREEN_REFRESH;
    navigation.option_focus = 2;
    assert(press(&navigation, QUOTA_INPUT_OK_SHORT, 1) == QUOTA_ACTION_APPLY_SETTINGS);
    assert(navigation.auto_refresh && navigation.refresh_seconds == 300);

    quota_navigation_sync_settings(&navigation, 61, false, 120);
    assert(navigation.auto_refresh && navigation.refresh_seconds == 300);
}

static void test_status_line_priority(void)
{
    quota_status_input_t input = {0};
    assert(quota_status_line_select(&input) == QUOTA_STATUS_LINE_NO_DATA);
    input.has_observed_at = true;
    assert(quota_status_line_select(&input) == QUOTA_STATUS_LINE_UPDATED);
    input.refreshing = true;
    assert(quota_status_line_select(&input) == QUOTA_STATUS_LINE_REFRESHING);
    input.update_failed = true;
    assert(quota_status_line_select(&input) == QUOTA_STATUS_LINE_UPDATE_FAILED);
    input.rate_limited = true;
    assert(quota_status_line_select(&input) == QUOTA_STATUS_LINE_RATE_LIMITED);
    input.wifi_failed = true;
    assert(quota_status_line_select(&input) == QUOTA_STATUS_LINE_WIFI_FAILED);
    input.unverified_items = 1;
    assert(quota_status_line_select(&input) == QUOTA_STATUS_LINE_UNVERIFIED);
    input.reauth_needed = true;
    assert(quota_status_line_select(&input) == QUOTA_STATUS_LINE_REAUTH);
    input.authorizing = true;
    assert(quota_status_line_select(&input) == QUOTA_STATUS_LINE_AUTHORIZING);
    input.storage_error = true;
    assert(quota_status_line_select(&input) == QUOTA_STATUS_LINE_STORAGE_ERROR);
    assert(quota_status_line_select(NULL) == QUOTA_STATUS_LINE_NO_DATA);
}

static void test_email_mask(void)
{
    char out[32];
    quota_mask_email("alice@mail.com", out, sizeof(out));
    assert(strcmp(out, "a***@mail.com") == 0);
    quota_mask_email("a@x.org", out, sizeof(out));
    assert(strcmp(out, "a***@x.org") == 0);
    quota_mask_email("no-at-sign", out, sizeof(out));
    assert(strcmp(out, "no-at-sign") == 0);
    quota_mask_email("你好@x.org", out, sizeof(out));
    assert(strcmp(out, "?***@x.org") == 0);
    quota_mask_email("", out, sizeof(out));
    assert(out[0] == '\0');
    quota_mask_email("alice@mail.com", out, 6);
    assert(strlen(out) == 5);
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

static void test_long_down_is_refused_during_a_session(void)
{
    quota_display_state_t display = {.last_input_ms = 10000};
    quota_display_tick(&display, 11000, 120, true); /* a setup session keeps the screen on */
    assert(display.session_open);
    assert(!quota_display_handle_key(&display, 11100, QUOTA_KEY_PRESS, true));
    assert(!quota_display_handle_key(&display, 11600, QUOTA_KEY_LONG, true));
    assert(!display.sleeping && display.sleep_blocked);
    display.sleep_blocked = false;
    /* Without a session it switches the screen off as before. */
    quota_display_tick(&display, 12000, 120, false);
    assert(!display.session_open);
    assert(!quota_display_handle_key(&display, 12100, QUOTA_KEY_LONG, true));
    assert(display.sleeping && !display.sleep_blocked);

    quota_navigation_t navigation;
    quota_navigation_init(&navigation, true, 300, true, 120);
    quota_navigation_notice(&navigation, 1000, true);
    assert(navigation.sleep_notice);
    quota_navigation_notice(&navigation, 1000 + QUOTA_NOTICE_MS - 1, false);
    assert(navigation.sleep_notice);
    quota_navigation_notice(&navigation, 1000 + QUOTA_NOTICE_MS, false);
    assert(!navigation.sleep_notice);
}

static void test_screen_timeout_settings(void)
{
    for (size_t i = 0; i < QUOTA_SCREEN_TIMEOUT_COUNT; i++)
        assert(quota_screen_timeout_is_valid(quota_screen_timeouts[i]));
    assert(!quota_screen_timeout_is_valid(31) && !quota_screen_timeout_is_valid(601));
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
        {1, "不到 1 分钟后重置"},       {59, "不到 1 分钟后重置"},
        {60, "1 分钟后重置"},           {3599, "59 分钟后重置"},
        {3600, "1 小时 0 分后重置"},    {8040, "2 小时 14 分后重置"},
        {86399, "23 小时 59 分后重置"}, {86400, "1 天 0 小时后重置"},
        {97200, "1 天 3 小时后重置"},   {3 * 86400 + 5 * 3600, "3 天 5 小时后重置"},
        {604800, "7 天 0 小时后重置"},
    };
    char output[48];
    quota_window_t window = {.present = true, .has_resets_at = true};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        window.resets_at = 1700000000 + cases[i].seconds;
        quota_format_reset_time(&window, 1700000000, true, output, sizeof(output));
        assert(strcmp(output, cases[i].text) == 0);
    }
    /* Cached boot time is not a current-clock observation. */
    quota_format_reset_time(&window, 1700000000, false, output, sizeof(output));
    assert(strcmp(output, "待校时") == 0);
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
    test_home_keys();
    test_welcome_screen();
    test_menu_order_wrap_and_memory();
    test_option_lists();
    test_hotspot_pages();
    test_hotspot_state();
    test_displayable_text();
    test_usb_screen_and_auth_screen();
    test_factory_reset_confirmation();
    test_no_long_press_is_destructive();
    test_navigation_after_external_settings_change();
    test_status_line_priority();
    test_email_mask();
    test_display_sleep_and_wake_gestures();
    test_long_down_is_refused_during_a_session();
    test_screen_timeout_settings();
    test_deepseek_balance_contract();
    test_remaining_duration();
    puts("quota logic tests passed");
    return 0;
}
