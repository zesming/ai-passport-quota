"""Execute power transitions and the application task without hardware or transport."""
import unittest
from runtime_helpers import compile_and_run

MAIN_SOURCES = ("main/main.c", "main/quota_logic.c", "tests/cjson/cJSON.c")

HARNESS_HEAD = r'''
#include "bsp_button.h"
#include "quota_service.h"
#include "quota_ui.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

extern quota_display_state_t s_display;
extern quota_navigation_t s_navigation;
extern quota_service_view_t s_view_work;
extern bool s_display_power_pending, s_display_power_sleeping, s_cpu_lock_held;
extern uint8_t s_backlight_percent;
extern bool s_usb_powered;
void configure_cpu_power_management(void);
void render_application(void);
void process_event(const quota_app_event_t *event);
void process_button(const quota_app_event_t *event, quota_service_view_t *view);
void observe_login_navigation(quota_portable_login_state_t login, bool navigate);
void application_task(void *arg);

/* The service view the application reads on every event. */
static quota_service_view_t fake_view;
static unsigned views, ui_renders, refreshes, opened, closed;
static bool last_render_usb, usb_host;
bool usb_serial_jtag_is_connected(void) { return usb_host; }

void quota_service_get_view(quota_service_view_t *view) { views++; *view = fake_view; }
void quota_ui_render(const quota_navigation_t *navigation, const quota_service_view_t *service,
                     int battery_percent, bool usb_powered) {
    (void)navigation; (void)service; (void)battery_percent; ui_renders++;
    last_render_usb = usb_powered;
}
void quota_service_request_refresh(void) { refreshes++; }
void quota_service_open_phone(void) { opened++; }
void quota_service_close_phone(void) { closed++; }
'''


class PowerRuntime(unittest.TestCase):
    def test_awake_press_skips_render_but_wake_and_power_retry_do_not(self):
        harness = HARNESS_HEAD + r'''
int main(void) {
    host_time_us = 1000000;
    s_navigation.configured = true; /* Home + short OK is a refresh action. */
    fake_view.configured = true;
    fake_view.snapshot.account_count = 1; /* no account would be the welcome screen */
    /* A full event reads the view twice: once in process_event, once in render_application. */
    quota_app_event_t event = {.kind = QUOTA_APP_EVENT_BUTTON, .button = BSP_BTN_OK,
                               .button_event = BSP_BTN_PRESS};
    process_event(&event);
    assert(views == 0 && ui_renders == 0 && s_display.last_input_ms == 1000);
    s_display.sleeping = true;
    process_event(&event);
    assert(views == 2 && ui_renders == 1 && !s_display.sleeping && s_display.consume_wake_gesture);
    event.button_event = BSP_BTN_CLICK;
    process_event(&event);
    assert(refreshes == 0 && !s_display.consume_wake_gesture); /* The waking tap is swallowed. */
    assert(views == 4 && ui_renders == 2);
    event.button_event = BSP_BTN_PRESS;
    process_event(&event);
    assert(views == 4 && ui_renders == 2); /* Awake PRESS is the fast path. */
    event.button_event = BSP_BTN_CLICK;
    process_event(&event);
    assert(refreshes == 1 && views == 6 && ui_renders == 3);
    s_display_power_pending = true; event.button_event = BSP_BTN_PRESS;
    process_event(&event);
    assert(views == 8 && ui_renders == 4 && refreshes == 1 && !s_display_power_pending);
    puts("awake PRESS fast path preserves complete wake and pending-power retries");
}
'''
        compile_and_run(harness, "quota-press-render-", MAIN_SOURCES, host_sdk=True)

    def test_display_retry_cpu_lock_and_application_wait(self):
        harness = HARNESS_HEAD + r"""
static jmp_buf done;
static int cpu_depth, acquires, releases, power_calls, brightness = 100;
static bool fail_power, fail_refresh, fail_release, fail_enter, fail_exit, sleep_gate;
static bool fail_acquire, armed = true;
static unsigned wait_expected, queue_calls;
static char order[64];
static void note(char code) {
    size_t n = strlen(order); assert(n + 1 < sizeof(order)); order[n] = code; order[n + 1] = 0;
}
static void expect_order(const char *expected) {
    if (strcmp(order, expected) != 0) {
        fprintf(stderr, "order %s != %s\n", order, expected); assert(0);
    }
    order[0] = 0;
}

esp_err_t esp_pm_lock_create(int type, int argument, const char *name,
                             esp_pm_lock_handle_t *lock) {
    (void)argument; (void)name; assert(type == ESP_PM_CPU_FREQ_MAX);
    *lock = (void *)1; return ESP_OK;
}
esp_err_t esp_pm_configure(const void *config) {
    const esp_pm_config_t *cfg = config;
    assert(cfg->max_freq_mhz == 160 && cfg->min_freq_mhz == 40 && cfg->light_sleep_enable);
    return ESP_OK;
}
esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t lock) {
    assert(lock); if (fail_acquire) return 1;
    ++acquires; note('A'); assert(++cpu_depth == 1); return ESP_OK;
}
esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t lock) {
    assert(lock); ++releases; note('R');
    if (fail_release) return 1;
    assert(--cpu_depth == 0); return ESP_OK;
}
void bsp_display_backlight(uint8_t level) { brightness = level; }
bool bsp_lvgl_set_sleeping(bool sleeping) {
    assert(brightness == 0); ++power_calls; note(sleeping ? 'S' : 'W'); return !fail_power;
}
esp_err_t bsp_power_enter_screen_off(void) {
    assert(cpu_depth == 1); /* The awake lock is still held while the screen-off state is built. */
    note('E'); return fail_enter ? ESP_FAIL : ESP_OK;
}
bool bsp_power_light_sleep_armed(void) { return armed; }
esp_err_t bsp_power_exit_screen_off(void) {
    assert(cpu_depth == 1 || fail_acquire); /* The awake lock is taken before restoring. */
    note('X'); return fail_exit ? ESP_FAIL : ESP_OK;
}
bool bsp_lvgl_refresh(void) { assert(ui_renders && brightness == 0); return !fail_refresh; }
void quota_service_set_display_sleeping(bool sleeping) { sleep_gate = sleeping; }
BaseType_t xQueueReceive(QueueHandle_t queue, void *event, TickType_t wait) {
    (void)queue; (void)event; assert(wait == wait_expected);
    if (++queue_calls == 2) longjmp(done, 1);
    memset(event, 0, sizeof(quota_app_event_t));
    ((quota_app_event_t *)event)->kind = QUOTA_APP_EVENT_CONNECTION;
    return pdTRUE;
}
static void check_wait(unsigned expected) {
    wait_expected = expected; queue_calls = 0;
    if (!setjmp(done)) application_task(NULL);
    order[0] = 0; /* The task renders once while draining; its retries are not under test. */
}
int main(void) {
    host_time_us = 100000000;
    configure_cpu_power_management();
    assert(s_cpu_lock_held && cpu_depth == 1);
    order[0] = 0;
    /* Preparation keeps the display awake before USB is ready. */
    fake_view.screen_timeout_seconds = 30; fake_view.usb_window_preparing = true;
    s_display.last_input_ms = 0;
    render_application();
    assert(!s_display.sleeping && s_display.last_input_ms == 100000); /* Idle for 100 s of 30 s. */
    fake_view.usb_window_preparing = false;
    fake_view.screen_timeout_seconds = 0;
    s_display = (quota_display_state_t){0};
    s_display_power_sleeping = false; s_display_power_pending = false;
    s_backlight_percent = 100; brightness = 100; sleep_gate = false; ui_renders = 0;
    power_calls = 0; releases = 0; acquires = 0; order[0] = 0;

    /* A panel that fails to sleep keeps the awake lock: no light sleep with the pins unprepared. */
    s_display.sleeping = true; fail_power = true;
    render_application();
    assert(sleep_gate && brightness == 0 && ui_renders == 0);
    assert(s_display_power_pending && s_cpu_lock_held && cpu_depth == 1 && releases == 0);
    expect_order("S");
    check_wait(1000); /* Failed transitions retry instead of becoming permanent. */
    fail_power = false; fail_enter = true; render_application();
    assert(s_display_power_pending && s_cpu_lock_held && releases == 0);
    expect_order("SE");
    /* Full screen-off order: panel Sleep In, screen-off state, and only then the lock release. */
    fail_enter = false; render_application();
    expect_order("SER");
    assert(!s_display_power_pending && s_display_power_sleeping);
    assert(!s_cpu_lock_held && cpu_depth == 0);
    check_wait(portMAX_DELAY);
    int calls = power_calls; render_application(); assert(power_calls == calls && order[0] == 0);

    /* Wake order: acquire the lock first, restore the screen-off state, then Sleep Out. */
    s_display.sleeping = false; fail_exit = true; render_application();
    expect_order("AX");
    assert(!sleep_gate && s_cpu_lock_held && brightness == 0 && ui_renders == 0);
    assert(s_display_power_pending && s_display_power_sleeping);
    fail_exit = false; fail_power = true; render_application();
    expect_order("XW");
    assert(acquires == 1 && cpu_depth == 1 && ui_renders == 0 && s_display_power_pending);
    fail_power = false; fail_refresh = true; render_application();
    expect_order("XW");
    assert(!s_display_power_pending && !s_display_power_sleeping && brightness == 0);
    fail_refresh = false; render_application(); assert(brightness == 100 && ui_renders == 2);
    assert(order[0] == 0);
    check_wait(1000);

    /* The USB host state is read on every drawn frame and handed to the interface. */
    usb_host = true; s_display.sleeping = false; render_application();
    assert(last_render_usb && s_usb_powered);
    usb_host = false; render_application();
    assert(!last_render_usb && !s_usb_powered);

    /* A release failure retries the whole screen-off step (the BSP call is idempotent). */
    s_display.sleeping = true; fail_release = true; render_application();
    expect_order("SER");
    assert(s_display_power_pending && s_cpu_lock_held);
    fail_release = false; render_application();
    expect_order("SER");
    assert(!s_display_power_pending && !s_cpu_lock_held && cpu_depth == 0);
    check_wait(portMAX_DELAY);

    /* No buttons (init failed): the BSP only blacks the screen. One attempt, no 1 Hz retry loop,
     * and the awake lock stays held because nothing could ever wake a sleeping chip. */
    s_display.sleeping = false; render_application(); order[0] = 0;
    assert(s_cpu_lock_held && !s_display_power_sleeping);
    armed = false; releases = 0;
    s_display.sleeping = true; render_application();
    expect_order("SE");
    assert(!s_display_power_pending && s_display_power_sleeping && s_cpu_lock_held);
    assert(releases == 0 && cpu_depth == 1);
    check_wait(portMAX_DELAY);
    s_display.sleeping = false; render_application();
    expect_order("XW");
    assert(!s_display_power_pending && !s_display_power_sleeping && s_cpu_lock_held);
    armed = true;

    /* A wake that cannot take the lock still lights the screen and retries the lock later. */
    s_display.sleeping = true; render_application(); order[0] = 0;
    assert(!s_cpu_lock_held && s_display_power_sleeping);
    fail_acquire = true; s_display.sleeping = false; render_application();
    expect_order("XW");
    assert(!s_display_power_pending && !s_display_power_sleeping && !s_cpu_lock_held);
    fail_acquire = false; render_application();
    expect_order("A");
    assert(s_cpu_lock_held && cpu_depth == 1);
    puts("display power and application wait tests passed");
}
"""
        compile_and_run(harness, "ai-quota-display-power-", MAIN_SOURCES, host_sdk=True)

    def test_screen_off_wake_event_and_waking_press_suppression(self):
        harness = HARNESS_HEAD + r"""
static bool drop, queue_full;
static unsigned battery_reads, sent, queued;
static quota_app_event_t queued_event;
static bool gesture_dropped_calls;

bool bsp_power_wake_gesture_drop(bsp_btn_t button, bsp_btn_ev_t event) {
    (void)button; (void)event; gesture_dropped_calls = true; return drop;
}
int bsp_battery_soc(void) { return 61 + (int)battery_reads++; }
void quota_service_send_button(bsp_btn_t button, bsp_btn_ev_t event) {
    (void)button; (void)event; sent++;
}
QueueHandle_t quota_service_event_queue(void) { return (QueueHandle_t)1; }
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait) {
    assert(queue && wait == 0);
    if (queue_full) return 0;
    memcpy(&queued_event, item, sizeof(queued_event)); queued++; return pdTRUE;
}
void quota_service_set_display_sleeping(bool sleeping) { (void)sleeping; }
void bsp_display_backlight(uint8_t level) { (void)level; }
void on_button(bsp_btn_t button, bsp_btn_ev_t event, void *user);
bool on_screen_wake(void *user);
void wake_from_screen_off(void);

int main(void) {
    host_time_us = 5000000;
    /* The waking press is dropped inside the button callback: it never reaches the app queue. */
    drop = true;
    on_button(BSP_BTN_OK, BSP_BTN_PRESS, NULL);
    on_button(BSP_BTN_OK, BSP_BTN_CLICK, NULL);
    assert(gesture_dropped_calls && sent == 0);
    drop = false;
    on_button(BSP_BTN_OK, BSP_BTN_CLICK, NULL);
    assert(sent == 1);

    /* The BSP sampler reports a wake; a full queue is reported so it can retry. */
    queue_full = true;
    assert(!on_screen_wake(NULL) && queued == 0);
    queue_full = false;
    assert(on_screen_wake(NULL) && queued == 1 && queued_event.kind == QUOTA_APP_EVENT_WAKE);

    /* Wake event: lights the display without arming the old swallow flag, reads the fuel gauge. */
    s_display.sleeping = true; s_display.last_input_ms = 0;
    s_display_power_sleeping = true; s_display_power_pending = false;
    fake_view.configured = true;
    fake_view.snapshot.account_count = 1; /* no account would be the welcome screen */
    s_navigation.configured = true;
    quota_app_event_t event = {.kind = QUOTA_APP_EVENT_WAKE};
    process_event(&event);
    assert(!s_display.sleeping && !s_display.consume_wake_gesture);
    assert(s_display.last_input_ms == 5000 && battery_reads >= 1 && !s_display_power_sleeping);
    assert(ui_renders == 1 && refreshes == 0); /* Rendered once; no action was triggered. */
    /* The first real key after the waking press acts normally. */
    event = (quota_app_event_t){.kind = QUOTA_APP_EVENT_BUTTON, .button = BSP_BTN_OK,
                                .button_event = BSP_BTN_CLICK};
    process_event(&event);
    assert(refreshes == 1);

    /* A key event that raced the button timer stop is dropped while the BSP owns waking. */
    s_display.sleeping = true; s_display_power_sleeping = true; s_display_power_pending = false;
    unsigned renders = ui_renders, view_reads = views;
    event = (quota_app_event_t){.kind = QUOTA_APP_EVENT_BUTTON, .button = BSP_BTN_OK,
                                .button_event = BSP_BTN_PRESS};
    process_event(&event);
    event.button_event = BSP_BTN_CLICK;
    process_event(&event);
    assert(s_display.sleeping && !s_display.consume_wake_gesture);
    assert(ui_renders == renders && views == view_reads && refreshes == 1);
    s_display.sleeping = false; s_display_power_sleeping = false;

    /* A stale wake while the screen is already lit changes nothing and reads no battery. */
    unsigned reads = battery_reads;
    event = (quota_app_event_t){.kind = QUOTA_APP_EVENT_WAKE};
    process_event(&event);
    assert(!s_display.sleeping && battery_reads == reads && refreshes == 1);
    puts("screen-off wake event and waking-press suppression passed");
}
"""
        compile_and_run(harness, "ai-quota-screen-wake-", MAIN_SOURCES, host_sdk=True)

    def test_login_navigation_edge_reopen_and_back(self):
        harness = HARNESS_HEAD + r'''
void quota_service_set_display_sleeping(bool sleeping) { assert(!sleeping); }
void bsp_display_backlight(uint8_t percent) { assert(percent == 100); }
static void reset(quota_portable_login_state_t login) {
    memset(&fake_view, 0, sizeof(fake_view));
    fake_view.refresh_seconds = 300; fake_view.screen_timeout_seconds = 120;
    fake_view.configured = true; fake_view.portable.login_state = login;
    s_view_work = fake_view;
    s_display = (quota_display_state_t){0};
    quota_navigation_init(&s_navigation, true, 300, true, 120);
    observe_login_navigation(login, false); /* Baseline preexisting state on startup. */
}
static void key(bsp_btn_ev_t event) {
    quota_app_event_t button = {.kind = QUOTA_APP_EVENT_BUTTON, .button = BSP_BTN_OK,
                                .button_event = event};
    s_view_work = fake_view;
    process_button(&button, &s_view_work); render_application();
}
static void open_setup(void) {
    s_navigation.screen = QUOTA_SCREEN_MENU; s_navigation.menu_focus = 0;
    key(BSP_BTN_CLICK); assert(s_navigation.screen == QUOTA_SCREEN_HOTSPOT);
    render_application(); render_application();
    assert(s_navigation.screen == QUOTA_SCREEN_HOTSPOT);
    key(BSP_BTN_LONG); assert(s_navigation.screen == QUOTA_SCREEN_MENU);
}
int main(void) {
    host_time_us = 1000000;
    quota_portable_login_state_t terminal[] = {QUOTA_PORTABLE_LOGIN_SUCCESS,
        QUOTA_PORTABLE_LOGIN_ERROR, QUOTA_PORTABLE_LOGIN_EXPIRED};
    for (unsigned i = 0; i < sizeof(terminal) / sizeof(terminal[0]); i++) {
        reset(terminal[i]); open_setup(); open_setup(); /* Retained terminal never replays. */
        reset(QUOTA_PORTABLE_LOGIN_WAITING);
        fake_view.portable.login_state = terminal[i];
        open_setup(); /* Consume terminal in button view before the first render. */
        reset(QUOTA_PORTABLE_LOGIN_IDLE); s_navigation.screen = QUOTA_SCREEN_HOTSPOT;
        fake_view.portable.login_state = terminal[i]; render_application();
        assert(s_navigation.screen == (terminal[i] == QUOTA_PORTABLE_LOGIN_SUCCESS ?
            QUOTA_SCREEN_HOME : QUOTA_SCREEN_AUTH));
        if (s_navigation.screen == QUOTA_SCREEN_AUTH) {
            key(BSP_BTN_LONG); assert(s_navigation.screen == QUOTA_SCREEN_HOME);
        }
        open_setup(); /* Back then reopen cannot replay the same result. */
    }
    quota_portable_login_state_t active[] = {QUOTA_PORTABLE_LOGIN_CONNECTING,
        QUOTA_PORTABLE_LOGIN_WAITING, QUOTA_PORTABLE_LOGIN_EXCHANGING};
    for (unsigned i = 0; i < sizeof(active) / sizeof(active[0]); i++) {
        reset(QUOTA_PORTABLE_LOGIN_QUEUED); s_navigation.screen = QUOTA_SCREEN_HOTSPOT;
        fake_view.portable.login_state = active[i]; render_application();
        assert(s_navigation.screen == QUOTA_SCREEN_AUTH);
        fake_view.portable.login_state = QUOTA_PORTABLE_LOGIN_SUCCESS;
        render_application(); assert(s_navigation.screen == QUOTA_SCREEN_HOME); open_setup();
    }
    reset(QUOTA_PORTABLE_LOGIN_IDLE); s_navigation.screen = QUOTA_SCREEN_MENU;
    fake_view.portable.login_state = QUOTA_PORTABLE_LOGIN_ERROR;
    render_application(); assert(s_navigation.screen == QUOTA_SCREEN_MENU);
    open_setup(); /* An edge consumed elsewhere cannot capture a later setup page. */
    assert(opened == closed && opened >= 12);
    puts("login navigation edge, reopen and Back tests passed");
}
'''
        compile_and_run(harness, "ai-quota-login-navigation-", MAIN_SOURCES, host_sdk=True)

    def test_keys_reach_the_service_and_setup_blocks_sleep(self):
        harness = HARNESS_HEAD + r'''
static unsigned factory, usb_opened, usb_closed, renewed, cancelled;
void quota_service_set_display_sleeping(bool sleeping) { assert(!sleeping); }
void bsp_display_backlight(uint8_t percent) { assert(percent == 100); }
void quota_service_factory_reset(void) { factory++; }
void quota_usb_open_window(void) { usb_opened++; }
void quota_usb_close_window(void) { usb_closed++; }
void quota_service_renew_phone(void) { renewed++; }
void quota_service_cancel_auth(void) { cancelled++; }
static void key(bsp_btn_t button, bsp_btn_ev_t event) {
    quota_app_event_t press = {.kind = QUOTA_APP_EVENT_BUTTON, .button = button,
                               .button_event = event};
    s_view_work = fake_view;
    process_button(&press, &s_view_work);
}
int main(void) {
    host_time_us = 1000000;
    fake_view.refresh_seconds = 300; fake_view.screen_timeout_seconds = 120;
    fake_view.configured = true; fake_view.snapshot.account_count = 2;
    s_view_work = fake_view;
    quota_navigation_init(&s_navigation, true, 300, true, 120);
    /* J9: long OK, UP, OK, OK, DOWN, OK. Only the last key resets anything. */
    key(BSP_BTN_OK, BSP_BTN_LONG); assert(s_navigation.screen == QUOTA_SCREEN_MENU);
    key(BSP_BTN_UP, BSP_BTN_CLICK); key(BSP_BTN_OK, BSP_BTN_CLICK);
    assert(s_navigation.screen == QUOTA_SCREEN_INFO);
    key(BSP_BTN_OK, BSP_BTN_CLICK); assert(s_navigation.screen == QUOTA_SCREEN_CONFIRM);
    key(BSP_BTN_OK, BSP_BTN_LONG); assert(s_navigation.screen == QUOTA_SCREEN_INFO && !factory);
    key(BSP_BTN_OK, BSP_BTN_CLICK); key(BSP_BTN_OK, BSP_BTN_CLICK); /* default: cancel */
    assert(!factory && s_navigation.screen == QUOTA_SCREEN_INFO);
    key(BSP_BTN_OK, BSP_BTN_CLICK); key(BSP_BTN_DOWN, BSP_BTN_CLICK);
    key(BSP_BTN_OK, BSP_BTN_CLICK);
    assert(factory == 1 && s_navigation.factory_resetting);
    key(BSP_BTN_OK, BSP_BTN_CLICK); assert(factory == 1); /* no second request while it runs */

    /* USB opens and closes with its screen. */
    quota_navigation_init(&s_navigation, true, 300, true, 120);
    key(BSP_BTN_OK, BSP_BTN_LONG); key(BSP_BTN_DOWN, BSP_BTN_CLICK); key(BSP_BTN_OK, BSP_BTN_CLICK);
    assert(s_navigation.screen == QUOTA_SCREEN_USB && usb_opened == 1);
    key(BSP_BTN_OK, BSP_BTN_LONG);
    assert(s_navigation.screen == QUOTA_SCREEN_MENU && usb_closed == 1);

    /* Hotspot: OK turns the page; with the hotspot closed it also asks for a new one. */
    key(BSP_BTN_UP, BSP_BTN_CLICK); key(BSP_BTN_OK, BSP_BTN_CLICK);
    assert(s_navigation.screen == QUOTA_SCREEN_HOTSPOT && opened == 1);
    fake_view.portable.setup_active = false;
    key(BSP_BTN_OK, BSP_BTN_CLICK); assert(renewed == 1 && s_navigation.hotspot_page == 0);
    fake_view.portable.setup_active = true; fake_view.portable.setup_ready = true;
    key(BSP_BTN_OK, BSP_BTN_CLICK); assert(renewed == 1 && s_navigation.hotspot_page == 1);
    key(BSP_BTN_OK, BSP_BTN_LONG); assert(closed == 1 && s_navigation.screen == QUOTA_SCREEN_MENU);

    /* Opening, validating and error: OK does nothing, so it cannot restart a validation; and
     * entering the screen from the menu does not open a second hotspot over a busy one. */
    s_navigation.screen = QUOTA_SCREEN_HOTSPOT; s_navigation.return_screen = QUOTA_SCREEN_MENU;
    unsigned before_open = opened, before_renew = renewed;
    fake_view.portable.setup_active = false; fake_view.portable.validating = true;
    key(BSP_BTN_OK, BSP_BTN_CLICK); assert(opened == before_open && renewed == before_renew);
    fake_view.portable.validating = false; fake_view.portable.setup_opening = true;
    key(BSP_BTN_OK, BSP_BTN_CLICK); assert(opened == before_open && renewed == before_renew);
    fake_view.portable.setup_opening = false; fake_view.portable.storage_error[0] = 'x';
    key(BSP_BTN_OK, BSP_BTN_CLICK); assert(opened == before_open && renewed == before_renew);
    fake_view.portable.storage_error[0] = 0;
    s_navigation.screen = QUOTA_SCREEN_MENU; s_navigation.menu_focus = 0;
    fake_view.portable.validating = true;
    key(BSP_BTN_OK, BSP_BTN_CLICK);
    assert(s_navigation.screen == QUOTA_SCREEN_HOTSPOT && opened == before_open);
    fake_view.portable.validating = false;
    s_navigation.screen = QUOTA_SCREEN_MENU;
    fake_view.portable.setup_active = true; fake_view.portable.setup_ready = false;
    key(BSP_BTN_OK, BSP_BTN_CLICK); /* a hotspot that is still opening is not opened again */
    assert(s_navigation.screen == QUOTA_SCREEN_HOTSPOT && opened == before_open);
    fake_view.portable.setup_active = false;
    s_navigation.screen = QUOTA_SCREEN_MENU;

    /* USB: an open window keeps its session; the screen is only shown. A closed one reopens. */
    unsigned usb_before = usb_opened;
    s_navigation.menu_focus = 1;
    fake_view.usb_window_active = true;
    key(BSP_BTN_OK, BSP_BTN_CLICK);
    assert(s_navigation.screen == QUOTA_SCREEN_USB && usb_opened == usb_before);
    key(BSP_BTN_OK, BSP_BTN_CLICK); assert(usb_opened == usb_before); /* OK: nothing while open */
    fake_view.usb_window_active = false; fake_view.usb_window_preparing = true;
    key(BSP_BTN_OK, BSP_BTN_CLICK); assert(usb_opened == usb_before);
    fake_view.usb_window_preparing = false;
    key(BSP_BTN_OK, BSP_BTN_CLICK); assert(usb_opened == usb_before + 1); /* ended: reopens */
    s_navigation.screen = QUOTA_SCREEN_MENU; s_navigation.menu_focus = 0;

    /* An open setup session keeps the screen on and long DOWN says why instead of sleeping. */
    fake_view.portable.setup_active = true;
    render_application();
    assert(s_display.session_open && !s_display.sleeping);
    key(BSP_BTN_DOWN, BSP_BTN_PRESS); key(BSP_BTN_DOWN, BSP_BTN_LONG);
    assert(!s_display.sleeping && s_navigation.sleep_notice && !s_display.sleep_blocked);
    host_time_us += (QUOTA_NOTICE_MS + 100) * 1000LL;
    render_application(); assert(!s_navigation.sleep_notice);
    fake_view.portable.setup_active = false;
    render_application(); assert(!s_display.session_open);

    unsigned usb_before_cancel = usb_closed;
    /* Authorization: OK on the home screen shows it, a second OK asks, only the third cancels. */
    fake_view.portable.login_state = QUOTA_PORTABLE_LOGIN_WAITING;
    s_navigation.screen = QUOTA_SCREEN_HOME;
    render_application(); assert(s_navigation.screen == QUOTA_SCREEN_AUTH); /* it takes over */
    key(BSP_BTN_OK, BSP_BTN_LONG); assert(s_navigation.screen == QUOTA_SCREEN_HOME);
    /* Later phases of the same authorization do not pull the user back. */
    fake_view.portable.login_state = QUOTA_PORTABLE_LOGIN_EXCHANGING;
    render_application(); assert(s_navigation.screen == QUOTA_SCREEN_HOME);
    fake_view.portable.login_state = QUOTA_PORTABLE_LOGIN_WAITING;
    render_application(); assert(s_navigation.screen == QUOTA_SCREEN_HOME);
    s_navigation.screen = QUOTA_SCREEN_MENU;
    fake_view.portable.login_state = QUOTA_PORTABLE_LOGIN_REQUESTING_CODE;
    render_application(); assert(s_navigation.screen == QUOTA_SCREEN_MENU);
    s_navigation.screen = QUOTA_SCREEN_HOME;
    key(BSP_BTN_OK, BSP_BTN_CLICK); assert(s_navigation.screen == QUOTA_SCREEN_AUTH);
    key(BSP_BTN_OK, BSP_BTN_CLICK);
    assert(s_navigation.screen == QUOTA_SCREEN_CONFIRM && !cancelled);
    key(BSP_BTN_DOWN, BSP_BTN_CLICK); key(BSP_BTN_OK, BSP_BTN_CLICK);
    assert(cancelled == 1 && s_navigation.screen == QUOTA_SCREEN_HOME);
    assert(usb_closed == usb_before_cancel); /* cancelling does not end the USB window */
    puts("keys reach the service, setup blocks sleep");
}
'''
        compile_and_run(harness, "ai-quota-key-actions-", MAIN_SOURCES, host_sdk=True)

    def test_resource_log_reports_both_task_stacks_and_heap(self):
        # The debug-only path (CONFIG_QUOTA_RESOURCE_LOG) is built here so it cannot rot.
        harness = r"""
#include "bsp_display.h"
#include "lvgl.h"
#include "quota_service.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

void app_main(void);

static esp_timer_create_args_t created;
static uint64_t period;
static unsigned stack_reads, heap_reads;
static TaskHandle_t read_tasks[2];

BaseType_t xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *argument,
                       unsigned priority, TaskHandle_t *handle) {
    (void)task; (void)name; (void)stack; (void)argument; (void)priority;
    *handle = (TaskHandle_t)0x10; return pdPASS;
}
TaskHandle_t quota_service_network_task(void) { return (TaskHandle_t)0x20; }
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *timer) {
    created = *args; *timer = (void *)1; return ESP_OK;
}
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us) {
    (void)timer; period = period_us; return ESP_OK;
}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) {
    assert(stack_reads < 2); read_tasks[stack_reads++] = task; return 2048;
}
uint32_t esp_get_minimum_free_heap_size(void) { heap_reads++; return 40000; }
static bool lvgl_locked;
static unsigned pool_reads;
bool bsp_lvgl_lock(int timeout_ms) {
    assert(timeout_ms > 0 && !lvgl_locked); lvgl_locked = true; return true;
}
void bsp_lvgl_unlock(void) { assert(lvgl_locked); lvgl_locked = false; }
void lv_mem_monitor(lv_mem_monitor_t *monitor) {
    assert(lvgl_locked); /* the pool is only read under the LVGL lock */
    pool_reads++; monitor->total_size = 24576; monitor->free_size = 10000;
    monitor->max_used = 17000;
}

int main(void) {
    app_main();
    assert(created.callback && !strcmp(created.name, "quota_resources"));
    assert(period == 30LL * 1000 * 1000);
    created.callback(created.arg);
    assert(stack_reads == 2 && heap_reads == 1 && pool_reads == 1 && !lvgl_locked);
    assert(read_tasks[0] == (TaskHandle_t)0x10 && read_tasks[1] == (TaskHandle_t)0x20);
    puts("resource log passed");
}
"""
        compile_and_run(
            harness,
            "ai-quota-resource-log-",
            MAIN_SOURCES,
            ("-DCONFIG_QUOTA_RESOURCE_LOG",),
            host_sdk=True,
        )


if __name__ == "__main__":
    unittest.main()
