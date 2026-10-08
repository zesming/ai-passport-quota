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
void configure_cpu_power_management(void);
void render_application(void);
void process_event(const quota_app_event_t *event);
void process_button(const quota_app_event_t *event, quota_service_view_t *view);
void observe_login_navigation(quota_portable_login_state_t login, bool navigate);
void application_task(void *arg);

/* The service view the application reads on every event. */
static quota_service_view_t fake_view;
static unsigned views, ui_renders, refreshes, opened, closed;

void quota_service_get_view(quota_service_view_t *view) { views++; *view = fake_view; }
void quota_ui_render(const quota_navigation_t *navigation, const quota_service_view_t *service,
                     int battery_percent) {
    (void)navigation; (void)service; (void)battery_percent; ui_renders++;
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
        harness = HARNESS_HEAD + r'''
static jmp_buf done;
static int cpu_depth, acquires, releases, power_calls, brightness = 100;
static bool fail_power, fail_refresh, fail_release, sleep_gate;
static unsigned wait_expected, queue_calls;

esp_err_t esp_pm_lock_create(int type, int argument, const char *name,
                             esp_pm_lock_handle_t *lock) {
    (void)argument; (void)name; assert(type == ESP_PM_CPU_FREQ_MAX);
    *lock = (void *)1; return ESP_OK;
}
esp_err_t esp_pm_configure(const void *config) {
    const esp_pm_config_t *cfg = config;
    assert(cfg->max_freq_mhz == 160 && cfg->min_freq_mhz == 40 && !cfg->light_sleep_enable);
    return ESP_OK;
}
esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t lock) {
    assert(lock); ++acquires; assert(++cpu_depth == 1); return ESP_OK;
}
esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t lock) {
    assert(lock); ++releases; if (fail_release) return 1; assert(--cpu_depth == 0); return ESP_OK;
}
void bsp_display_backlight(uint8_t level) { brightness = level; }
bool bsp_lvgl_set_sleeping(bool sleeping) {
    (void)sleeping; assert(brightness == 0); ++power_calls; return !fail_power;
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
}
int main(void) {
    host_time_us = 100000000;
    configure_cpu_power_management();
    assert(s_cpu_lock_held && cpu_depth == 1);
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
    power_calls = 0; releases = 0; acquires = 0;
    s_display.sleeping = true; fail_power = true;
    render_application();
    assert(sleep_gate && brightness == 0 && ui_renders == 0);
    assert(s_display_power_pending && !s_cpu_lock_held && cpu_depth == 0);
    check_wait(1000); /* Failed transitions retry instead of becoming permanent. */
    render_application(); assert(releases == 1);
    fail_power = false; render_application();
    assert(!s_display_power_pending && s_display_power_sleeping);
    check_wait(portMAX_DELAY);
    int calls = power_calls; render_application(); assert(power_calls == calls);
    s_display.sleeping = false; fail_power = true; render_application();
    assert(!sleep_gate && s_cpu_lock_held && brightness == 0 && ui_renders == 0);
    render_application(); assert(acquires == 1 && cpu_depth == 1);
    fail_power = false; fail_refresh = true; render_application();
    assert(!s_display_power_pending && !s_display_power_sleeping && brightness == 0);
    fail_refresh = false; render_application(); assert(brightness == 100 && ui_renders == 2);
    check_wait(1000);
    s_display.sleeping = true; fail_release = true; render_application();
    assert(s_display_power_pending && s_cpu_lock_held);
    fail_release = false; render_application();
    assert(!s_display_power_pending && !s_cpu_lock_held && cpu_depth == 0);
    check_wait(portMAX_DELAY);
    puts("display power and application wait tests passed");
}
'''
        compile_and_run(harness, "ai-quota-display-power-", MAIN_SOURCES, host_sdk=True)

    def test_login_navigation_edge_reopen_and_back(self):
        harness = HARNESS_HEAD + r'''
void quota_service_set_display_sleeping(bool sleeping) { assert(!sleeping); }
void bsp_display_backlight(uint8_t percent) { assert(percent == 100); }
static void reset(quota_portable_login_state_t login) {
    memset(&fake_view, 0, sizeof(fake_view));
    fake_view.refresh_seconds = 300; fake_view.screen_timeout_seconds = 120;
    fake_view.configured = true; fake_view.portable.login_state = login;
    fake_view.portable.setup_active = true;
    s_view_work = fake_view;
    s_display = (quota_display_state_t){0};
    quota_navigation_init(&s_navigation, true, 300, true, 120, 0);
    observe_login_navigation(login, false); /* Baseline preexisting state on startup. */
}
static void key(bsp_btn_ev_t event) {
    quota_app_event_t button = {.kind = QUOTA_APP_EVENT_BUTTON, .button = BSP_BTN_OK,
                                .button_event = event};
    s_view_work = fake_view;
    process_button(&button, &s_view_work); render_application();
}
static void open_setup(void) {
    s_navigation.screen = QUOTA_SCREEN_DEVICE_SETTINGS; s_navigation.device_settings_focus = 0;
    key(BSP_BTN_CLICK); assert(s_navigation.screen == QUOTA_SCREEN_PHONE);
    render_application(); render_application();
    assert(s_navigation.screen == QUOTA_SCREEN_PHONE);
    key(BSP_BTN_LONG); assert(s_navigation.screen == QUOTA_SCREEN_DEVICE_SETTINGS);
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
        reset(QUOTA_PORTABLE_LOGIN_IDLE); s_navigation.screen = QUOTA_SCREEN_PHONE;
        fake_view.portable.login_state = terminal[i]; render_application();
        assert(s_navigation.screen == (terminal[i] == QUOTA_PORTABLE_LOGIN_SUCCESS ?
            QUOTA_SCREEN_HOME : QUOTA_SCREEN_AUTH));
        if (s_navigation.screen == QUOTA_SCREEN_AUTH) {
            key(BSP_BTN_LONG); assert(s_navigation.screen == QUOTA_SCREEN_ACCOUNTS);
        }
        open_setup(); /* Back then reopen cannot replay the same result. */
    }
    quota_portable_login_state_t active[] = {QUOTA_PORTABLE_LOGIN_CONNECTING,
        QUOTA_PORTABLE_LOGIN_WAITING, QUOTA_PORTABLE_LOGIN_EXCHANGING};
    for (unsigned i = 0; i < sizeof(active) / sizeof(active[0]); i++) {
        reset(QUOTA_PORTABLE_LOGIN_QUEUED); s_navigation.screen = QUOTA_SCREEN_PHONE;
        fake_view.portable.login_state = active[i]; render_application();
        assert(s_navigation.screen == QUOTA_SCREEN_AUTH);
        fake_view.portable.login_state = QUOTA_PORTABLE_LOGIN_SUCCESS;
        render_application(); assert(s_navigation.screen == QUOTA_SCREEN_HOME); open_setup();
    }
    reset(QUOTA_PORTABLE_LOGIN_IDLE); s_navigation.screen = QUOTA_SCREEN_ACCOUNTS;
    fake_view.portable.login_state = QUOTA_PORTABLE_LOGIN_ERROR;
    render_application(); assert(s_navigation.screen == QUOTA_SCREEN_ACCOUNTS);
    open_setup(); /* An edge consumed elsewhere cannot capture a later setup page. */
    assert(opened == closed && opened >= 12);
    puts("login navigation edge, reopen and Back tests passed");
}
'''
        compile_and_run(harness, "ai-quota-login-navigation-", MAIN_SOURCES, host_sdk=True)

    def test_resource_log_reports_both_task_stacks_and_heap(self):
        # The debug-only path (CONFIG_QUOTA_RESOURCE_LOG) is built here so it cannot rot.
        harness = r"""
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

int main(void) {
    app_main();
    assert(created.callback && !strcmp(created.name, "quota_resources"));
    assert(period == 30LL * 1000 * 1000);
    created.callback(created.arg);
    assert(stack_reads == 2 && heap_reads == 1);
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
