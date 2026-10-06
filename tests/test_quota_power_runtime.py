"""Execute power transitions and sleeping workers without hardware or transport."""
import unittest
from runtime_helpers import ROOT, extract_function, compile_and_run


class PowerRuntime(unittest.TestCase):
    def test_awake_press_skips_render_but_wake_and_power_retry_do_not(self):
        function = extract_function((ROOT / "main/main.c").read_text(), "process_event")
        harness = r'''
#include "quota_logic.h"
#include <assert.h>
#include <stdio.h>
enum { BSP_BTN_DOWN, BSP_BTN_OK };
enum { BSP_BTN_PRESS, BSP_BTN_CLICK };
enum { QUOTA_APP_EVENT_BUTTON, QUOTA_APP_EVENT_SNAPSHOT, QUOTA_APP_EVENT_SETTINGS_RESULT,
       QUOTA_APP_EVENT_CONFIGURATION_RESULT, QUOTA_APP_EVENT_CONNECTION, QUOTA_APP_EVENT_PAIRING_TICK };
typedef struct { unsigned kind, button, button_event; bool success,auto_refresh;
    uint16_t refresh_seconds,screen_timeout_seconds; } quota_app_event_t;
static quota_display_state_t s_display;
static quota_navigation_t s_navigation;
static bool s_display_power_pending;
static int s_view_work;
static unsigned reads,renders,actions;
static int64_t esp_timer_get_time(void) { return 1000000; }
static void quota_service_get_view(void *view) { (void)view; reads++; }
static void reconcile_account_selection(void *view) { (void)view; }
static void render_application(void) { renders++; }
static void process_button(const quota_app_event_t *e,void *view) {
    (void)view;
    if(quota_display_handle_key(&s_display,1000,e->button_event==BSP_BTN_PRESS?
        QUOTA_KEY_PRESS:QUOTA_KEY_CLICK,e->button==BSP_BTN_DOWN)) actions++;
}
'''+function+r'''
int main(void) {
    quota_app_event_t event={.kind=QUOTA_APP_EVENT_BUTTON,.button=BSP_BTN_OK,.button_event=BSP_BTN_PRESS};
    process_event(&event); assert(reads==0 && renders==0 && s_display.last_input_ms==1000);
    s_display.sleeping=true; process_event(&event);
    assert(reads==1 && renders==1 && !s_display.sleeping && s_display.consume_wake_gesture);
    event.button_event=BSP_BTN_CLICK; process_event(&event);
    assert(actions==0 && !s_display.consume_wake_gesture);
    event.button_event=BSP_BTN_PRESS; process_event(&event);
    assert(reads==2 && renders==2);
    event.button_event=BSP_BTN_CLICK; process_event(&event); assert(actions==1);
    s_display_power_pending=true; event.button_event=BSP_BTN_PRESS; process_event(&event);
    assert(reads==4 && renders==4 && actions==1);
    puts("awake PRESS fast path preserves complete wake and pending-power retries");
}
'''
        compile_and_run(harness, "quota-press-render-", ("main/quota_logic.c", "tests/cjson/cJSON.c"))

    def test_display_retry_cpu_lock_and_application_wait(self):
        source = (ROOT / "main/main.c").read_text()
        functions = "\n".join(extract_function(source, name) for name in (
            "configure_cpu_power_management", "set_display_power",
            "observe_login_navigation", "render_application", "application_task"))
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#include "quota_portable.h"
typedef int esp_err_t;
typedef void *esp_pm_lock_handle_t;
typedef struct { int max_freq_mhz, min_freq_mhz; bool light_sleep_enable; } esp_pm_config_t;
typedef unsigned TickType_t;
typedef void *QueueHandle_t;
typedef struct { int unused; } quota_app_event_t;
#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 160
#define ESP_PM_CPU_FREQ_MAX 1
#define ESP_OK 0
#define ESP_LOGW(...) ((void)0)
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY UINT32_MAX
static esp_pm_lock_handle_t s_cpu_lock;
static bool s_cpu_lock_held, s_display_power_sleeping, s_display_power_pending;
static uint8_t s_backlight_percent = 100;
static quota_display_state_t s_display;
static struct { uint16_t screen_timeout_seconds, refresh_seconds; bool pairing_active, pairing_preparing, auto_refresh; quota_portable_view_t portable; } s_view_work;
static quota_navigation_t s_navigation;
static int s_battery_percent;
static uint64_t s_battery_read_ms;
static int cpu_depth, acquires, releases, power_calls, renders, brightness = 100;
static bool fail_power, fail_refresh, fail_release, sleep_gate, pairing_hold_awake;
static unsigned wait_expected, queue_calls;
static jmp_buf done;
esp_err_t esp_pm_lock_create(int type, int arg, const char *name, void **lock) {
    (void)arg; (void)name; assert(type == ESP_PM_CPU_FREQ_MAX); *lock = (void *)1; return ESP_OK;
}
esp_err_t esp_pm_configure(const esp_pm_config_t *cfg) {
    assert(cfg->max_freq_mhz == 160 && cfg->min_freq_mhz == 40 && !cfg->light_sleep_enable);
    return ESP_OK;
}
esp_err_t esp_pm_lock_acquire(void *lock) {
    assert(lock); ++acquires; assert(++cpu_depth == 1); return ESP_OK;
}
esp_err_t esp_pm_lock_release(void *lock) {
    assert(lock); ++releases; if (fail_release) return 1; assert(--cpu_depth == 0); return ESP_OK;
}
esp_err_t esp_pm_lock_delete(void *lock) { (void)lock; return ESP_OK; }
void bsp_display_backlight(uint8_t level) { brightness = level; }
bool bsp_lvgl_set_sleeping(bool sleeping) {
    (void)sleeping; assert(brightness == 0); ++power_calls; return !fail_power;
}
bool bsp_lvgl_refresh(void) { assert(renders && brightness == 0); return !fail_refresh; }
void quota_service_get_view(void *view) { (void)view; }
int64_t esp_timer_get_time(void) { return 1000000; }
void quota_display_tick(quota_display_state_t *display, uint64_t now, uint16_t timeout, bool pairing) {
    (void)display; (void)now; (void)timeout; pairing_hold_awake = pairing;
}
void quota_navigation_sync_settings(quota_navigation_t *nav, uint16_t refresh, bool automatic, uint16_t timeout) { nav->refresh_seconds=refresh; nav->auto_refresh=automatic; nav->screen_timeout_seconds=timeout; }
void quota_service_set_display_sleeping(bool sleeping) { sleep_gate = sleeping; }
int bsp_battery_soc(void) { return 50; }
bool bsp_lvgl_lock(int timeout) { (void)timeout; return true; }
void bsp_lvgl_unlock(void) {}
void quota_ui_render(void *nav, void *view, int battery) {
    (void)nav; (void)view; (void)battery; ++renders;
}
QueueHandle_t quota_service_event_queue(void) { return (void *)1; }
int xQueueReceive(QueueHandle_t queue, quota_app_event_t *event, TickType_t wait) {
    (void)queue; (void)event; assert(wait == wait_expected);
    if (++queue_calls == 2) longjmp(done, 1);
    return pdTRUE;
}
void process_event(const quota_app_event_t *event) { (void)event; }
'''
        harness += functions
        harness += r'''
void check_wait(unsigned expected) {
    wait_expected = expected; queue_calls = 0;
    if (!setjmp(done)) application_task(NULL);
}
int main(void) {
    configure_cpu_power_management();
    assert(s_cpu_lock_held && cpu_depth == 1);
    s_view_work.pairing_preparing = true;
    render_application();
    assert(pairing_hold_awake); /* Preparation keeps the display awake before USB is ready. */
    s_view_work.pairing_preparing = false;
    s_display = (quota_display_state_t){0};
    s_display_power_sleeping = false; s_display_power_pending = false;
    s_backlight_percent = 100; brightness = 100; sleep_gate = false; renders = 0;
    s_display.sleeping = true; fail_power = true;
    render_application();
    assert(sleep_gate && brightness == 0 && renders == 0);
    assert(s_display_power_pending && !s_cpu_lock_held && cpu_depth == 0);
    check_wait(1000); /* Failed transitions retry instead of becoming permanent. */
    render_application(); assert(releases == 1);
    fail_power = false; render_application();
    assert(!s_display_power_pending && s_display_power_sleeping);
    check_wait(portMAX_DELAY);
    int calls = power_calls; render_application(); assert(power_calls == calls);
    s_display.sleeping = false; fail_power = true; render_application();
    assert(!sleep_gate && s_cpu_lock_held && brightness == 0 && renders == 0);
    render_application(); assert(acquires == 2 && cpu_depth == 1);
    fail_power = false; fail_refresh = true; render_application();
    assert(!s_display_power_pending && !s_display_power_sleeping && brightness == 0);
    fail_refresh = false; render_application(); assert(brightness == 100 && renders == 2);
    check_wait(1000);
    s_display.sleeping = true; fail_release = true; render_application();
    assert(s_display_power_pending && s_cpu_lock_held);
    fail_release = false; render_application();
    assert(!s_display_power_pending && !s_cpu_lock_held && cpu_depth == 0);
    check_wait(portMAX_DELAY);
    puts("display power and application wait tests passed");
}
'''
        compile_and_run(harness, "ai-quota-display-power-")

        source = (ROOT / "main/main.c").read_text()
        functions = "\n".join(extract_function(source, name, declaration) for name, declaration in (
            ("map_input", "static quota_input_t"), ("observe_login_navigation", "static void"),
            ("render_application", "static void"), ("process_button", "static void")))
        harness = r'''
#include "quota_portable.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef enum { BSP_BTN_OK, BSP_BTN_UP, BSP_BTN_DOWN } bsp_btn_t;
typedef enum { BSP_BTN_PRESS, BSP_BTN_CLICK, BSP_BTN_DOUBLE, BSP_BTN_LONG } bsp_btn_ev_t;
typedef struct { bsp_btn_t button; bsp_btn_ev_t button_event; } quota_app_event_t;
typedef struct {
    quota_snapshot_t snapshot; quota_portable_view_t portable;
    bool configured, pairing_active, pairing_preparing, auto_refresh;
    uint16_t refresh_seconds, screen_timeout_seconds;
} quota_service_view_t;
static quota_navigation_t s_navigation;
static quota_service_view_t s_view_work;
static quota_display_state_t s_display;
static uint8_t s_backlight_percent = 100;
static int s_battery_percent;
static uint64_t s_battery_read_ms;
static unsigned opened, closed;
static bool set_display_power(bool sleeping) { return !sleeping; }
static void quota_service_get_view(void *view) { (void)view; }
static int64_t esp_timer_get_time(void) { return 1000000; }
static void quota_service_set_display_sleeping(bool sleeping) { assert(!sleeping); }
static int bsp_battery_soc(void) { return 50; }
static bool bsp_lvgl_lock(int timeout) { (void)timeout; return true; }
static void bsp_lvgl_unlock(void) {}
static void quota_ui_render(void *nav, void *view, int battery) { (void)nav; (void)view; (void)battery; }
static bool bsp_lvgl_refresh(void) { return true; }
static void bsp_display_backlight(uint8_t percent) { assert(percent == 100); }
static void persist_current_selection(void *view) { (void)view; }
static void quota_service_open_phone(void) { ++opened; }
static void quota_service_close_phone(void) { ++closed; }
static void quota_service_renew_phone(void) {}
static void quota_service_open_pairing_window(void) {}
static void quota_service_close_pairing_window(void) {}
static void quota_service_cancel_auth(void) {}
static void quota_service_reconnect(void) {}
static void quota_service_request_refresh(void) {}
static void quota_service_request_settings(uint16_t refresh, bool automatic, uint16_t timeout) {
    (void)refresh; (void)automatic; (void)timeout;
}
'''+functions+r'''
static void reset(quota_portable_login_state_t login) {
    memset(&s_view_work, 0, sizeof(s_view_work));
    s_view_work.refresh_seconds=300; s_view_work.screen_timeout_seconds=120;
    s_view_work.configured=true; s_view_work.portable.login_state=login;
    s_view_work.portable.setup_active=true;
    s_display=(quota_display_state_t){0};
    quota_navigation_init(&s_navigation, true, 300, true, 120, 0);
    observe_login_navigation(login, false); /* Baseline preexisting state on startup. */
}
static void key(bsp_btn_ev_t event) {
    quota_app_event_t button={.button=BSP_BTN_OK, .button_event=event};
    process_button(&button, &s_view_work); render_application();
}
static void open_setup(void) {
    s_navigation.screen=QUOTA_SCREEN_DEVICE_SETTINGS; s_navigation.device_settings_focus=0;
    key(BSP_BTN_CLICK); assert(s_navigation.screen==QUOTA_SCREEN_PHONE);
    render_application(); render_application();
    assert(s_navigation.screen==QUOTA_SCREEN_PHONE);
    key(BSP_BTN_LONG); assert(s_navigation.screen==QUOTA_SCREEN_DEVICE_SETTINGS);
}
int main(void) {
    quota_portable_login_state_t terminal[]={QUOTA_PORTABLE_LOGIN_SUCCESS,
        QUOTA_PORTABLE_LOGIN_ERROR, QUOTA_PORTABLE_LOGIN_EXPIRED};
    for (unsigned i=0; i<sizeof(terminal)/sizeof(terminal[0]); i++) {
        reset(terminal[i]); open_setup(); open_setup(); /* Retained terminal never replays. */
        reset(QUOTA_PORTABLE_LOGIN_WAITING);
        s_view_work.portable.login_state=terminal[i];
        open_setup(); /* Consume terminal in button view before the first render. */
        reset(QUOTA_PORTABLE_LOGIN_IDLE); s_navigation.screen=QUOTA_SCREEN_PHONE;
        s_view_work.portable.login_state=terminal[i]; render_application();
        assert(s_navigation.screen==(terminal[i]==QUOTA_PORTABLE_LOGIN_SUCCESS?
            QUOTA_SCREEN_HOME:QUOTA_SCREEN_AUTH));
        if (s_navigation.screen==QUOTA_SCREEN_AUTH) {
            key(BSP_BTN_LONG); assert(s_navigation.screen==QUOTA_SCREEN_ACCOUNTS);
        }
        open_setup(); /* Back then reopen cannot replay the same result. */
    }
    quota_portable_login_state_t active[]={QUOTA_PORTABLE_LOGIN_CONNECTING,
        QUOTA_PORTABLE_LOGIN_WAITING, QUOTA_PORTABLE_LOGIN_EXCHANGING};
    for (unsigned i=0; i<sizeof(active)/sizeof(active[0]); i++) {
        reset(QUOTA_PORTABLE_LOGIN_QUEUED); s_navigation.screen=QUOTA_SCREEN_PHONE;
        s_view_work.portable.login_state=active[i]; render_application();
        assert(s_navigation.screen==QUOTA_SCREEN_AUTH);
        s_view_work.portable.login_state=QUOTA_PORTABLE_LOGIN_SUCCESS;
        render_application(); assert(s_navigation.screen==QUOTA_SCREEN_HOME); open_setup();
    }
    reset(QUOTA_PORTABLE_LOGIN_IDLE); s_navigation.screen=QUOTA_SCREEN_ACCOUNTS;
    s_view_work.portable.login_state=QUOTA_PORTABLE_LOGIN_ERROR;
    render_application(); assert(s_navigation.screen==QUOTA_SCREEN_ACCOUNTS);
    open_setup(); /* An edge consumed elsewhere cannot capture a later setup page. */
    assert(opened==closed && opened>=12);
    puts("login navigation edge, reopen and Back tests passed");
}
'''
        compile_and_run(harness, "ai-quota-login-navigation-", (
            "main/quota_logic.c", "tests/cjson/cJSON.c"))

    def test_wifi_stop_restart_and_late_events(self):
        source = (ROOT / "main/quota_service.c").read_text()
        functions = "\n".join(extract_function(source, name) for name in (
            "wifi_event_handler", "ip_event_handler", "init_wifi", "stop_wifi_for_sleep"))
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef int esp_err_t;
typedef int esp_event_base_t;
#define ESP_OK 0
#define WIFI_EVENT 1
#define IP_EVENT 2
#define WIFI_EVENT_STA_DISCONNECTED 3
#define IP_EVENT_STA_GOT_IP 4
#define WIFI_STORAGE_RAM 1
#define WIFI_MODE_STA 1
#define WIFI_RETRY_MIN_MS 2000
#define WIFI_RETRY_MAX_MS 30000
#define QUOTA_APP_EVENT_CONNECTION 1
#define ESP_LOGW(...) ((void)0)
static bool s_wifi_started, s_handlers_registered, s_wifi_retry_pending, s_fetch_after_connect;
static int s_wifi_handler, s_ip_handler;
static int64_t s_wifi_retry_at_ms;
static uint32_t s_wifi_retry_delay_ms = WIFI_RETRY_MIN_MS;
static struct { bool connected; } s_view;
static void *s_network_task = (void *)1;
static bool sleeping, fail_start, fail_stop;
static bool associated;
typedef struct { int unused; } wifi_ap_record_t;
static esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap) {
    (void)ap; return associated ? ESP_OK : 1;
}
static unsigned registrations, starts, stops, settings, events, notifications;
static uint64_t now_ms;
static bool prepare_network_stack(void) { return true; }
static void mutex_lock(void) {}
static void mutex_unlock(void) {}
static uint64_t monotonic_ms(void) { return now_ms; }
typedef struct { bool sleeping; } display_state_t;
static display_state_t display_state_snapshot(void) {
    return (display_state_t){.sleeping = sleeping};
}
static void post_simple_event(int kind) { (void)kind; ++events; }
static void xTaskNotifyGive(void *task) { (void)task; ++notifications; }
static esp_err_t esp_event_handler_instance_register(int base, int event,
    void (*handler)(void *, int, int32_t, void *), void *arg, int *instance) {
    (void)base; (void)event; (void)handler; (void)arg; (void)instance; ++registrations; return ESP_OK;
}
static esp_err_t esp_event_handler_instance_unregister(int base, int event, int instance) {
    (void)base; (void)event; (void)instance; return ESP_OK;
}
static esp_err_t esp_wifi_set_storage(int value) { (void)value; ++settings; return ESP_OK; }
static esp_err_t esp_wifi_set_mode(int value) { (void)value; ++settings; return ESP_OK; }
static esp_err_t esp_wifi_start(void) { ++starts; return fail_start ? 1 : ESP_OK; }
static esp_err_t esp_wifi_stop(void) {
    ++stops; if (fail_stop) return 1; associated = false; return ESP_OK;
}
'''
        harness += functions
        harness += r'''
int main(void) {
    assert(init_wifi() && s_wifi_started && s_handlers_registered);
    assert(registrations == 2 && settings == 2 && starts == 1 && s_wifi_retry_pending);
    associated = true;
    ip_event_handler(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(s_view.connected && s_fetch_after_connect && !s_wifi_retry_pending);
    sleeping = true;
    fail_stop = true; stop_wifi_for_sleep(); assert(s_wifi_started && s_view.connected);
    associated = false;
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    assert(!s_view.connected && s_wifi_retry_pending);
    sleeping = false; assert(init_wifi() && s_wifi_retry_pending && starts == 1);
    /* Failed stop plus asleep disconnect must retain reconnect work on wake. */
    sleeping = true;
    fail_stop = false; stop_wifi_for_sleep();
    assert(!s_wifi_started && !s_view.connected && !s_fetch_after_connect && !s_wifi_retry_pending);
    unsigned before = events;
    ip_event_handler(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    assert(!s_view.connected && !s_wifi_retry_pending && events == before);
    stop_wifi_for_sleep(); assert(stops == 2); /* No repeated stop after success. */
    sleeping = false;
    ip_event_handler(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(!s_view.connected); /* A queued old IP event cannot revive a stopped radio. */
    now_ms = 70000; assert(init_wifi());
    assert(starts == 2 && registrations == 2 && settings == 2 && s_wifi_retry_at_ms == 70000);
    before = events;
    ip_event_handler(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(!s_view.connected && s_wifi_retry_pending && events == before);
    /* The late old IP event after restart cannot cancel the next connect. */
    associated = true;
    ip_event_handler(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(s_view.connected && s_fetch_after_connect);
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    assert(!s_view.connected && s_wifi_retry_pending && s_wifi_retry_at_ms == 72000);
    sleeping = true; stop_wifi_for_sleep();
    sleeping = false; fail_start = true; assert(!init_wifi());
    assert(!s_wifi_started && !s_handlers_registered);
    fail_start = false; assert(init_wifi() && s_wifi_started);
    assert(registrations == 4 && settings == 4);
    puts("Wi-Fi sleep lifecycle tests passed");
}
'''
        compile_and_run(harness, "ai-quota-wifi-power-")

    def test_network_owner_pairing_window_sleep_and_partial_frames(self):
        source = (ROOT / "main/quota_service.c").read_text()
        harness = r'''
#include "quota_portable.h"
#include <assert.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
enum { QUOTA_APP_EVENT_PAIRING_TICK };
typedef struct { quota_mode_t mode; } portable_view_t;
typedef struct {
    bool pairing_preparing, pairing_active;
    uint32_t pairing_seconds_left;
    portable_view_t portable;
} view_t;
static view_t s_view;
static atomic_bool s_pairing_requested;
static bool s_pairing_screen_open;
static int64_t s_pairing_opened_at_ms;
static quota_frame_decoder_t *s_usb_decoder;
static void *s_companion_work, *s_network_task = (void *)1;
static uint64_t now_ms = 1000;
static unsigned frames, notifications, events, wifi_stops, workspace_releases;
static bool s_workspace_available = true;
static uint64_t monotonic_ms(void) { return now_ms; }
static void mutex_lock(void) {}
static void mutex_unlock(void) {}
static void xTaskNotifyGive(void *task) { assert(task == s_network_task); notifications++; }
static void post_simple_event(int kind) { assert(kind == QUOTA_APP_EVENT_PAIRING_TICK); events++; }
static bool acquire_companion_work(void) {
    if (!s_workspace_available) return false;
    s_companion_work = (void *)1;
    return true;
}
static void release_companion_work(void) { s_companion_work = NULL; workspace_releases++; }
static void stop_wifi_for_sleep(void) { wifi_stops++; }
static void handle_serial_frame(const char *frame, size_t size) {
    assert(size == 11 && memcmp(frame, "whole-frame", size) == 0); frames++;
}
static void send_pairing_result(const char *id, bool success, const char *error) {
    (void)id; (void)success; (void)error; assert(false);
}
void quota_service_close_pairing_window(void);
static int input_fd = -1;
static void set_input(const char *bytes) {
    if (input_fd < 0) {
        int descriptors[2]; assert(pipe(descriptors) == 0);
        int flags = fcntl(descriptors[0], F_GETFL, 0);
        assert(flags >= 0 && fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK) == 0);
        assert(dup2(descriptors[0], STDIN_FILENO) == STDIN_FILENO);
        close(descriptors[0]); input_fd = descriptors[1];
    }
    if (bytes && bytes[0]) assert(write(input_fd, bytes, strlen(bytes)) == (ssize_t)strlen(bytes));
}
'''
        functions = "\n".join(extract_function(source, name) for name in (
            "pairing_requested", "pairing_active_locked", "pairing_active",
            "service_pairing_tick"))
        functions += "\n" + extract_function(source, "quota_service_open_pairing_window", "void")
        functions += "\n" + extract_function(source, "quota_service_close_pairing_window", "void")
        harness += functions
        harness += r'''
int main(void) {
    set_input(NULL);
    s_view.portable.mode = QUOTA_MODE_DIRECT;
    quota_service_open_pairing_window();
    assert(atomic_load(&s_pairing_requested) && s_view.pairing_preparing && !s_view.pairing_active);
    service_pairing_tick(false);
    assert(s_view.pairing_active && !s_view.pairing_preparing);
    assert(s_view.pairing_seconds_left == QUOTA_PAIRING_WINDOW_MS / 1000);
    assert(s_usb_decoder && s_companion_work && wifi_stops == 1);

    set_input("whole-"); service_pairing_tick(false);
    assert(s_usb_decoder->length == strlen("whole-") && frames == 0);
    set_input("frame\r\n"); service_pairing_tick(false);
    assert(frames == 1 && s_usb_decoder->length == 0);

    set_input("stale-"); service_pairing_tick(false);
    assert(s_usb_decoder->length == strlen("stale-") && frames == 1);
    service_pairing_tick(true); /* Sleep closes the window and discards its partial line. */
    assert(!atomic_load(&s_pairing_requested) && !s_view.pairing_active && !s_view.pairing_preparing);
    assert(!s_usb_decoder && !s_companion_work && workspace_releases == 1);

    now_ms += 1000;
    quota_service_open_pairing_window(); service_pairing_tick(false);
    assert(s_usb_decoder && s_view.pairing_active);
    set_input("\n"); service_pairing_tick(false);
    assert(frames == 1 && s_usb_decoder->length == 0); /* No stale prefix crossed sleep. */
    set_input("whole-frame\n"); service_pairing_tick(false);
    assert(frames == 2);

    now_ms = (uint64_t)s_pairing_opened_at_ms + QUOTA_PAIRING_WINDOW_MS;
    service_pairing_tick(false);
    assert(!atomic_load(&s_pairing_requested) && !s_view.pairing_active);
    service_pairing_tick(false); /* The next owner tick frees the now-closed reader workspace. */
    assert(!s_usb_decoder && !s_companion_work && workspace_releases == 2);
    assert(notifications == 4 && events >= 2);
    close(input_fd);
    puts("network-owned pairing window tests passed");
}
'''
        compile_and_run(harness, "ai-quota-pairing-power-", (
            "main/quota_logic.c", "tests/cjson/cJSON.c"))


if __name__ == "__main__":
    unittest.main()
