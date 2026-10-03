#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "quota_service.h"
#include "quota_ui.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <time.h>

static const char *TAG = "ai_quota";

static quota_navigation_t s_navigation;
static quota_service_view_t s_view_work;
static TaskHandle_t s_application_task;
static quota_display_state_t s_display;
static uint8_t s_backlight_percent = 100;
static int s_battery_percent = -1;
static uint64_t s_battery_read_ms;

static quota_input_t map_input(bsp_btn_t button, bsp_btn_ev_t event)
{
    if (button == BSP_BTN_OK && event == BSP_BTN_LONG) return QUOTA_INPUT_OK_LONG;
    if (event != BSP_BTN_CLICK) return QUOTA_INPUT_OTHER;
    if (button == BSP_BTN_UP) return QUOTA_INPUT_UP;
    if (button == BSP_BTN_DOWN) return QUOTA_INPUT_DOWN;
    if (button == BSP_BTN_OK) return QUOTA_INPUT_OK_SHORT;
    return QUOTA_INPUT_OTHER;
}

static void persist_current_selection(const quota_service_view_t *view)
{
    if (view == NULL || view->snapshot.account_count == 0) return;
    size_t selected = s_navigation.selected_account;
    if (selected >= view->snapshot.account_count) selected = 0;
    quota_service_select_account(view->snapshot.accounts[selected].id);
}

static void reconcile_account_selection(const quota_service_view_t *view)
{
    if (view == NULL || !view->snapshot_valid) return;
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
    if (s_navigation.selected_account >= count) s_navigation.selected_account = 0;
    persist_current_selection(view);
}

static void render_application(void)
{
    quota_service_get_view(&s_view_work);
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    quota_display_tick(&s_display, now_ms, s_view_work.screen_timeout_seconds,
                        s_view_work.pairing_active);
    quota_service_set_display_sleeping(s_display.sleeping);
    uint8_t brightness = s_display.sleeping ? 0 : 100;
    if (brightness != s_backlight_percent) {
        bsp_display_backlight(brightness);
        s_backlight_percent = brightness;
    }
    if (s_display.sleeping) return;
    if (now_ms - s_battery_read_ms >= 30000) {
        s_battery_percent = bsp_battery_soc();
        s_battery_read_ms = now_ms;
    }
    if (!bsp_lvgl_lock(500)) return;
    quota_ui_render(&s_navigation, &s_view_work, s_battery_percent);
    bsp_lvgl_unlock();
}

static void process_button(const quota_app_event_t *event, quota_service_view_t *view)
{
    quota_key_event_t key_event;
    switch (event->button_event) {
        case BSP_BTN_PRESS: key_event = QUOTA_KEY_PRESS; break;
        case BSP_BTN_CLICK: key_event = QUOTA_KEY_CLICK; break;
        case BSP_BTN_DOUBLE: key_event = QUOTA_KEY_DOUBLE; break;
        case BSP_BTN_LONG: key_event = QUOTA_KEY_LONG; break;
        default: return;
    }
    if (!quota_display_handle_key(&s_display, (uint64_t)(esp_timer_get_time() / 1000),
                                  key_event, event->button == BSP_BTN_DOWN)) return;
    quota_navigation_sync_settings(&s_navigation, view->refresh_seconds,
                                   view->auto_refresh, view->screen_timeout_seconds);
    quota_screen_t previous_screen = s_navigation.screen;
    quota_action_t action = quota_navigation_handle(&s_navigation,
        map_input(event->button, event->button_event), view->snapshot.account_count);

    if (previous_screen != QUOTA_SCREEN_SETUP &&
        s_navigation.screen == QUOTA_SCREEN_SETUP) {
        quota_service_open_pairing_window();
    } else if (previous_screen == QUOTA_SCREEN_SETUP &&
               s_navigation.screen != QUOTA_SCREEN_SETUP) {
        quota_service_close_pairing_window();
    }

    if (action == QUOTA_ACTION_REFRESH) {
        quota_service_request_refresh();
    } else if (action == QUOTA_ACTION_APPLY_SETTINGS) {
        if (view->configured) {
            quota_service_request_settings(s_navigation.refresh_seconds,
                                           s_navigation.auto_refresh,
                                           s_navigation.screen_timeout_seconds);
        } else {
            s_navigation.refresh_seconds = view->refresh_seconds;
            s_navigation.auto_refresh = view->auto_refresh;
            s_navigation.screen_timeout_seconds = view->screen_timeout_seconds;
        }
    } else if (action == QUOTA_ACTION_PERSIST_SELECTION) {
        persist_current_selection(view);
    }
}

static void process_event(const quota_app_event_t *event)
{
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
        case QUOTA_APP_EVENT_CONFIGURATION_RESULT:
            if (event->success) {
                s_navigation.configured = true;
                s_navigation.screen = QUOTA_SCREEN_HOME;
                s_navigation.setup_return_screen = QUOTA_SCREEN_HOME;
            }
            break;
        case QUOTA_APP_EVENT_CONNECTION:
        case QUOTA_APP_EVENT_PAIRING_TICK:
        default:
            break;
    }
    render_application();
}

static void application_task(void *arg)
{
    (void)arg;
    QueueHandle_t events = quota_service_event_queue();
    quota_app_event_t event;
    for (;;) {
        if (xQueueReceive(events, &event, pdMS_TO_TICKS(1000)) == pdTRUE) {
            process_event(&event);
        } else {
            render_application();
        }
    }
}

static void on_button(bsp_btn_t button, bsp_btn_ev_t event, void *user)
{
    (void)user;
    quota_service_send_button(button, event);
}

void app_main(void)
{
    ESP_LOGI(TAG, "AI quota monitor starting");
    (void)setenv("TZ", "CST-8", 1);
    tzset();

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
    quota_navigation_init(&s_navigation, s_view_work.configured,
                         s_view_work.refresh_seconds, s_view_work.auto_refresh,
                         s_view_work.screen_timeout_seconds,
                         s_view_work.snapshot.account_count);
    reconcile_account_selection(&s_view_work);
    if (!s_view_work.configured) quota_service_open_pairing_window();

    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "LVGL lock unavailable during startup");
        return;
    }
    quota_ui_init();
    quota_ui_render(&s_navigation, &s_view_work, s_battery_percent);
    bsp_lvgl_unlock();

    esp_err_t button_result = bsp_button_init(on_button, NULL);
    if (button_result != ESP_OK) {
        ESP_LOGE(TAG, "button initialization failed (%s)", esp_err_to_name(button_result));
    }
    if (xTaskCreate(application_task, "quota_app", 6144, NULL, 5,
                    &s_application_task) != pdPASS) {
        ESP_LOGE(TAG, "application event task creation failed");
        return;
    }
    if (!quota_service_start()) {
        ESP_LOGE(TAG, "quota service task creation failed");
        return;
    }
    ESP_LOGI(TAG, "ready");
}
