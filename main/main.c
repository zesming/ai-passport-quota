#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "bsp_power.h"
#include "quota_service.h"
#include "quota_testable.h"
#include "quota_ui.h"
#include "quota_usb.h"

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <time.h>

static const char *TAG = "ai_quota";

QUOTA_TESTABLE quota_navigation_t s_navigation;
QUOTA_TESTABLE quota_service_view_t s_view_work;
static TaskHandle_t s_application_task;
QUOTA_TESTABLE quota_display_state_t s_display;
QUOTA_TESTABLE uint8_t s_backlight_percent = 100;
QUOTA_TESTABLE int s_battery_percent = -1;
QUOTA_TESTABLE uint64_t s_battery_read_ms;
/* A USB host is connected. It is read on every drawn frame, so it needs no timer or wake-up. */
QUOTA_TESTABLE bool s_usb_powered;
QUOTA_TESTABLE esp_pm_lock_handle_t s_cpu_lock;
QUOTA_TESTABLE bool s_cpu_lock_held;
QUOTA_TESTABLE bool s_display_power_sleeping;
QUOTA_TESTABLE bool s_display_power_pending;

QUOTA_TESTABLE void configure_cpu_power_management(void)
{
    esp_err_t err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "quota_awake", &s_cpu_lock);
    if (err != ESP_OK)
        goto failed;
    const esp_pm_config_t config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = 40,
        /* Light sleep only happens with the screen off: the quota_awake lock blocks it while lit,
         * and a USB host (CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION) blocks it too. Wi-Fi is stopped
         * while the screen is off; a transfer still in flight holds its own PM locks. */
        .light_sleep_enable = true,
    };
    err = esp_pm_configure(&config);
    if (err == ESP_OK)
        err = esp_pm_lock_acquire(s_cpu_lock);
    if (err == ESP_OK) {
        s_cpu_lock_held = true;
        return;
    }
    esp_pm_lock_delete(s_cpu_lock);
    s_cpu_lock = NULL;
failed:
    ESP_LOGW(TAG, "CPU power management unavailable (%s)", esp_err_to_name(err));
}

/* Power order matters. Screen off: panel Sleep In, then bsp_power_enter_screen_off() (button
 * poll, LVGL timers and tick, pins), and only then release quota_awake so nothing sleeps with
 * the pins floating. Wake: take quota_awake first, then undo the screen-off state, then Sleep Out.
 * If the buttons never came up the BSP only blacks the screen (no light-sleep steps); the lock
 * then stays held. A lock that cannot be taken on wake is logged and retried on the next call
 * instead of blocking the wake: the screen works, it just may light-sleep between events. */
QUOTA_TESTABLE bool set_display_power(bool sleeping)
{
    if (sleeping == s_display_power_sleeping && !s_display_power_pending) {
        if (!sleeping && s_cpu_lock != NULL && !s_cpu_lock_held)
            s_cpu_lock_held = esp_pm_lock_acquire(s_cpu_lock) == ESP_OK;
        return true;
    }
    s_display_power_pending = true;
    bsp_display_backlight(0);
    s_backlight_percent = 0;
    bool ready;
    bool keep_lock = false;
    if (sleeping) {
        ready = bsp_lvgl_set_sleeping(true) && bsp_power_enter_screen_off() == ESP_OK;
        keep_lock = ready && !bsp_power_light_sleep_armed();
        if (ready && !keep_lock && s_cpu_lock_held && esp_pm_lock_release(s_cpu_lock) == ESP_OK) {
            s_cpu_lock_held = false;
        }
        ready = ready && (s_cpu_lock == NULL || s_cpu_lock_held == keep_lock);
    } else {
        if (s_cpu_lock != NULL && !s_cpu_lock_held) {
            s_cpu_lock_held = esp_pm_lock_acquire(s_cpu_lock) == ESP_OK;
            if (!s_cpu_lock_held)
                ESP_LOGW(TAG, "quota_awake lock unavailable on wake");
        }
        ready = bsp_power_exit_screen_off() == ESP_OK && bsp_lvgl_set_sleeping(false);
    }
    if (!ready)
        ESP_LOGW(TAG, "display power transition failed");
    if (ready) {
        s_display_power_sleeping = sleeping;
        s_display_power_pending = false;
    }
    return ready;
}

/* A key sampled by the BSP while the screen is off lights it. The waking press itself is dropped
 * by the BSP (bsp_power_wake_gesture_drop), so a click-style wake leaves no swallow flag here. */
QUOTA_TESTABLE void wake_from_screen_off(void)
{
    if (!s_display.sleeping)
        return;
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    (void)quota_display_handle_key(&s_display, now_ms, QUOTA_KEY_CLICK, false);
    s_display.consume_wake_gesture = false; /* Only the BSP suppresses the waking press. */
    /* The fuel gauge is not read while the screen is off; refresh it as soon as it lights up. */
    s_battery_percent = bsp_battery_soc();
    s_battery_read_ms = now_ms;
}

QUOTA_TESTABLE quota_input_t map_input(bsp_btn_t button, bsp_btn_ev_t event)
{
    if (button == BSP_BTN_OK && event == BSP_BTN_LONG)
        return QUOTA_INPUT_OK_LONG;
    if (event != BSP_BTN_CLICK)
        return QUOTA_INPUT_OTHER;
    if (button == BSP_BTN_UP)
        return QUOTA_INPUT_UP;
    if (button == BSP_BTN_DOWN)
        return QUOTA_INPUT_DOWN;
    if (button == BSP_BTN_OK)
        return QUOTA_INPUT_OK_SHORT;
    return QUOTA_INPUT_OTHER;
}

static void persist_current_selection(const quota_service_view_t *view)
{
    if (view == NULL || view->snapshot.account_count == 0)
        return;
    size_t selected = s_navigation.selected_account;
    if (selected >= view->snapshot.account_count)
        selected = 0;
    quota_service_select_account(view->snapshot.accounts[selected].id);
}

static void reconcile_account_selection(const quota_service_view_t *view)
{
    if (view == NULL || !view->snapshot_valid)
        return;
    uint8_t count = view->snapshot.account_count;
    if (count == 0) {
        s_navigation.selected_account = 0;
        return;
    }

    char selected_id[QUOTA_ACCOUNT_ID_BYTES + 1];
    quota_service_get_selected_account_id(selected_id);
    if (selected_id[0] != '\0') {
        int index = quota_find_account_by_id(&view->snapshot, selected_id);
        if (index >= 0) {
            s_navigation.selected_account = (uint8_t)index;
            return;
        }
    }
    if (s_navigation.selected_account >= count)
        s_navigation.selected_account = 0;
    persist_current_selection(view);
}

static bool login_is_active(quota_portable_login_state_t login)
{
    return login >= QUOTA_PORTABLE_LOGIN_CONNECTING && login <= QUOTA_PORTABLE_LOGIN_EXCHANGING;
}

/* A new authorization takes the screen over, once, when it starts (except from the confirmation
 * box); later phases do not pull the user back from wherever they went. The hotspot screen
 * follows it to its end. */
QUOTA_TESTABLE void observe_login_navigation(quota_portable_login_state_t login, bool navigate)
{
    static quota_portable_login_state_t previous = QUOTA_PORTABLE_LOGIN_IDLE;
    bool changed = login != previous;
    bool started = login_is_active(login) && !login_is_active(previous);
    previous = login;
    if (!navigate || !changed)
        return;
    if ((started && s_navigation.screen != QUOTA_SCREEN_CONFIRM) ||
        (s_navigation.screen == QUOTA_SCREEN_HOTSPOT &&
         (login == QUOTA_PORTABLE_LOGIN_ERROR || login == QUOTA_PORTABLE_LOGIN_EXPIRED))) {
        s_navigation.screen = QUOTA_SCREEN_AUTH;
    } else if ((s_navigation.screen == QUOTA_SCREEN_AUTH ||
                s_navigation.screen == QUOTA_SCREEN_HOTSPOT) &&
               login == QUOTA_PORTABLE_LOGIN_SUCCESS) {
        s_navigation.screen = QUOTA_SCREEN_HOME;
    }
}

QUOTA_TESTABLE void render_application(void)
{
    quota_service_get_view(&s_view_work);
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    observe_login_navigation(s_view_work.portable.login_state, true);
    quota_navigation_sync_settings(&s_navigation, s_view_work.refresh_seconds,
                                   s_view_work.auto_refresh, s_view_work.screen_timeout_seconds);
    quota_navigation_notice(&s_navigation, now_ms, false);
    if (s_navigation.factory_resetting && s_view_work.portable.factory_reset_failed) {
        s_navigation.factory_resetting = false; /* the erase failed: the box says so */
        s_navigation.factory_failed = true;
    }
    quota_display_tick(&s_display, now_ms, s_view_work.screen_timeout_seconds,
                       s_view_work.usb_window_active || s_view_work.usb_window_preparing ||
                           s_view_work.portable.setup_active ||
                           s_view_work.portable.auth_hold_awake);
    quota_service_set_display_sleeping(s_display.sleeping);
    bool display_ready = set_display_power(s_display.sleeping);
    if (s_display.sleeping || !display_ready)
        return;
    if (now_ms - s_battery_read_ms >= 30000) {
        s_battery_percent = bsp_battery_soc();
        s_battery_read_ms = now_ms;
    }
    if (!bsp_lvgl_lock(500))
        return;
    s_usb_powered = usb_serial_jtag_is_connected();
    quota_ui_render(&s_navigation, &s_view_work, s_battery_percent, s_usb_powered);
    bsp_lvgl_unlock();
    if (s_backlight_percent != 100) {
        if (!bsp_lvgl_refresh())
            return;
        bsp_display_backlight(100);
        s_backlight_percent = 100;
    }
}

QUOTA_TESTABLE void process_button(const quota_app_event_t *event, quota_service_view_t *view)
{
    quota_key_event_t key_event;
    switch (event->button_event) {
    case BSP_BTN_PRESS:
        key_event = QUOTA_KEY_PRESS;
        break;
    case BSP_BTN_CLICK:
        key_event = QUOTA_KEY_CLICK;
        break;
    case BSP_BTN_DOUBLE:
        key_event = QUOTA_KEY_DOUBLE;
        break;
    case BSP_BTN_LONG:
        key_event = QUOTA_KEY_LONG;
        break;
    default:
        return;
    }
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    bool navigate =
        quota_display_handle_key(&s_display, now_ms, key_event, event->button == BSP_BTN_DOWN);
    if (s_display.sleep_blocked) { /* a setup session is open: say why the screen stays on */
        s_display.sleep_blocked = false;
        quota_navigation_notice(&s_navigation, now_ms, true);
    }
    if (!navigate)
        return;
    /* Consume the latest service edge before a manual screen change. */
    observe_login_navigation(view->portable.login_state, true);
    quota_navigation_sync_settings(&s_navigation, view->refresh_seconds, view->auto_refresh,
                                   view->screen_timeout_seconds);
    quota_navigation_context_t context = {
        .account_count = view->snapshot.account_count,
        .auth_active = login_is_active(view->portable.login_state),
        .hotspot = quota_hotspot_state(view->portable.storage_error[0] != '\0',
                                       view->portable.validating, view->portable.setup_active,
                                       view->portable.setup_ready, view->portable.setup_opening),
        .usb_window_open = view->usb_window_active || view->usb_window_preparing,
    };
    quota_action_t action = quota_navigation_handle(
        &s_navigation, map_input(event->button, event->button_event), context);

    switch (action) {
    case QUOTA_ACTION_OPEN_HOTSPOT:
        quota_service_open_phone();
        break;
    case QUOTA_ACTION_RENEW_HOTSPOT:
        quota_service_renew_phone();
        break;
    case QUOTA_ACTION_CLOSE_HOTSPOT:
        quota_service_close_phone();
        break;
    case QUOTA_ACTION_OPEN_USB:
        quota_usb_open_window();
        break;
    case QUOTA_ACTION_CLOSE_USB:
        quota_usb_close_window();
        break;
    case QUOTA_ACTION_CANCEL_AUTH:
        quota_service_cancel_auth();
        break;
    case QUOTA_ACTION_FACTORY_RESET:
        quota_service_factory_reset();
        break;
    case QUOTA_ACTION_REFRESH:
        quota_service_request_refresh();
        break;
    case QUOTA_ACTION_APPLY_SETTINGS:
        if (view->configured) {
            quota_service_request_settings(s_navigation.refresh_seconds, s_navigation.auto_refresh,
                                           s_navigation.screen_timeout_seconds);
        } else {
            s_navigation.refresh_seconds = view->refresh_seconds;
            s_navigation.auto_refresh = view->auto_refresh;
            s_navigation.screen_timeout_seconds = view->screen_timeout_seconds;
        }
        break;
    case QUOTA_ACTION_PERSIST_SELECTION:
        persist_current_selection(view);
        break;
    default:
        break;
    }
}

QUOTA_TESTABLE void process_event(const quota_app_event_t *event)
{
    /* With the screen-off state built the BSP sampler owns waking. A key event that reaches the
     * queue now was produced just before the button timer stopped (a press racing the screen
     * timeout); acting on it would arm a second wake suppression the BSP never clears. */
    if (event->kind == QUOTA_APP_EVENT_BUTTON && s_display_power_sleeping &&
        !s_display_power_pending)
        return;
    if (event->kind == QUOTA_APP_EVENT_BUTTON && event->button_event == BSP_BTN_PRESS &&
        !s_display.sleeping && !s_display_power_pending) {
        /* Awake PRESS resets idle time; the debounced release or LONG does the work. */
        s_display.last_input_ms = (uint64_t)(esp_timer_get_time() / 1000);
        return;
    }
    quota_service_get_view(&s_view_work);
    switch (event->kind) {
    case QUOTA_APP_EVENT_BUTTON:
        process_button(event, &s_view_work);
        break;
    case QUOTA_APP_EVENT_SNAPSHOT:
        reconcile_account_selection(&s_view_work);
        break;
    case QUOTA_APP_EVENT_SETTINGS_RESULT:
        s_navigation.refresh_seconds = event->refresh_seconds;
        s_navigation.auto_refresh = event->auto_refresh;
        s_navigation.screen_timeout_seconds = event->screen_timeout_seconds;
        break;
    case QUOTA_APP_EVENT_WAKE:
        wake_from_screen_off();
        break;
    case QUOTA_APP_EVENT_CONNECTION:
    case QUOTA_APP_EVENT_USB_WINDOW:
    default:
        break;
    }
    render_application();
}

QUOTA_TESTABLE void application_task(void *arg)
{
    (void)arg;
    QueueHandle_t events = quota_service_event_queue();
    quota_app_event_t event;
    for (;;) {
        TickType_t wait =
            s_display.sleeping && !s_display_power_pending ? portMAX_DELAY : pdMS_TO_TICKS(1000);
        if (xQueueReceive(events, &event, wait) == pdTRUE) {
            process_event(&event);
        } else {
            render_application();
        }
    }
}

#ifdef CONFIG_QUOTA_RESOURCE_LOG
#include "esp_system.h"
#include "lvgl.h"

/* Debug builds report task stack and heap headroom so hardware runs can be checked against the
 * limits in docs/development/README.md. Water marks are bytes still unused at the deepest point. */
#define RESOURCE_LOG_PERIOD_US (30LL * 1000 * 1000)

static void log_resources(void *arg)
{
    (void)arg;
    TaskHandle_t network = quota_service_network_task();
    /* The LVGL pool is read under the LVGL lock: its peak is the number the 24 KB pool is judged
     * by (docs/development/README.md). */
    lv_mem_monitor_t pool = {0};
    if (bsp_lvgl_lock(100)) {
        lv_mem_monitor(&pool);
        bsp_lvgl_unlock();
    }
    ESP_LOGI(TAG, "lvgl_pool: total=%u used=%u max_used=%u free_biggest=%u",
             (unsigned)pool.total_size, (unsigned)(pool.total_size - pool.free_size),
             (unsigned)pool.max_used, (unsigned)pool.free_biggest_size);
    ESP_LOGI(TAG,
             "resources: quota_app_stack_free_min=%u quota_network_stack_free_min=%u "
             "heap_free=%u heap_free_min=%u",
             (unsigned)uxTaskGetStackHighWaterMark(s_application_task),
             network ? (unsigned)uxTaskGetStackHighWaterMark(network) : 0,
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
}

static void start_resource_log(void)
{
    static esp_timer_handle_t timer;
    const esp_timer_create_args_t args = {.callback = log_resources, .name = "quota_resources"};
    if (esp_timer_create(&args, &timer) != ESP_OK ||
        esp_timer_start_periodic(timer, RESOURCE_LOG_PERIOD_US) != ESP_OK) {
        ESP_LOGW(TAG, "resource log unavailable");
    }
}
#endif

QUOTA_TESTABLE void on_button(bsp_btn_t button, bsp_btn_ev_t event, void *user)
{
    (void)user;
    if (bsp_power_wake_gesture_drop(button, event))
        return;
    quota_service_send_button(button, event);
}

/* Runs in the esp_timer task: only enqueues. A full queue returns false and the BSP retries. */
QUOTA_TESTABLE bool on_screen_wake(void *user)
{
    (void)user;
    QueueHandle_t events = quota_service_event_queue();
    quota_app_event_t event = {.kind = QUOTA_APP_EVENT_WAKE};
    return events != NULL && xQueueSend(events, &event, 0) == pdTRUE;
}

void app_main(void)
{
#ifdef QUOTA_GIT_REV
    /* The startup log already prints the app version; the revision is only for logs. */
    ESP_LOGI(TAG, "AI quota monitor starting (git %s)", QUOTA_GIT_REV);
#else
    ESP_LOGI(TAG, "AI quota monitor starting");
#endif
    (void)setenv("TZ", "CST-8", 1);
    tzset();
    configure_cpu_power_management();

    bsp_i2c_init();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display initialization failed");
        return;
    }
    bsp_display_backlight(100);
    s_display.last_input_ms = (uint64_t)(esp_timer_get_time() / 1000);
    (void)bsp_battery_init();
    s_battery_percent = bsp_battery_soc();
    s_battery_read_ms = s_display.last_input_ms;

    if (!quota_service_init()) {
        ESP_LOGE(TAG, "quota service initialization failed");
        return;
    }
    quota_service_get_view(&s_view_work);
    quota_navigation_init(&s_navigation, s_view_work.configured, s_view_work.refresh_seconds,
                          s_view_work.auto_refresh, s_view_work.screen_timeout_seconds);
    reconcile_account_selection(&s_view_work);
    observe_login_navigation(s_view_work.portable.login_state, false);

    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "LVGL lock unavailable during startup");
        return;
    }
    quota_ui_init();
    s_usb_powered = usb_serial_jtag_is_connected();
    quota_ui_render(&s_navigation, &s_view_work, s_battery_percent, s_usb_powered);
    bsp_lvgl_unlock();

    bsp_power_set_wake_callback(on_screen_wake, NULL);
    esp_err_t button_result = bsp_button_init(on_button, NULL);
    if (button_result != ESP_OK) {
        ESP_LOGE(TAG, "button initialization failed (%s)", esp_err_to_name(button_result));
    }
    if (xTaskCreate(application_task, "quota_app", 6144, NULL, 5, &s_application_task) != pdPASS) {
        ESP_LOGE(TAG, "application event task creation failed");
        return;
    }
    if (!quota_service_start()) {
        ESP_LOGE(TAG, "quota service task creation failed");
        return;
    }
#ifdef CONFIG_QUOTA_RESOURCE_LOG
    start_resource_log();
#endif
    ESP_LOGI(TAG, "ready");
}
