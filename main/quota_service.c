#include "quota_service.h"
#include "quota_portable_service.h"
#include "quota_store.h"
#include "quota_testable.h"
#include "quota_usb.h"
#include "quota_wifi.h"

#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "freertos/semphr.h"
#include "freertos/task.h"

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "quota_service";

#define EVENT_QUEUE_DEPTH 16
#define NETWORK_TASK_STACK 10240
#define NETWORK_TASK_PRIORITY 5

typedef struct {
    bool sleeping;
    uint32_t generation;
} display_state_t;

static QueueHandle_t s_events;
static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_network_task;
static portMUX_TYPE s_display_state_mux = portMUX_INITIALIZER_UNLOCKED;
static display_state_t s_display_scheduler = {.generation = 1};
static quota_service_view_t s_view;
static bool s_nvs_ready;
static uint32_t s_config_generation;

void quota_service_lock(void)
{
    if (s_mutex != NULL)
        (void)xSemaphoreTake(s_mutex, portMAX_DELAY);
}

void quota_service_unlock(void)
{
    if (s_mutex != NULL)
        (void)xSemaphoreGive(s_mutex);
}

bool quota_service_try_lock(void)
{
    return xSemaphoreTake(s_mutex, 0) == pdTRUE;
}

quota_service_view_t *quota_service_view(void)
{
    return &s_view;
}

static uint64_t current_epoch(void)
{
    time_t now = time(NULL);
    return now > 0 ? (uint64_t)now : 0;
}

TaskHandle_t quota_service_network_task(void)
{
    return s_network_task;
}

static display_state_t display_state_snapshot(void)
{
    display_state_t state;
    portENTER_CRITICAL(&s_display_state_mux);
    state = s_display_scheduler;
    portEXIT_CRITICAL(&s_display_state_mux);
    return state;
}

bool quota_service_display_sleeping(void)
{
    return display_state_snapshot().sleeping;
}

static bool display_generation_is_current(uint32_t generation)
{
    bool current;
    portENTER_CRITICAL(&s_display_state_mux);
    current = !s_display_scheduler.sleeping && s_display_scheduler.generation == generation;
    portEXIT_CRITICAL(&s_display_state_mux);
    return current;
}

static void post_event(const quota_app_event_t *event, TickType_t wait)
{
    if (s_events != NULL && event != NULL)
        (void)xQueueSend(s_events, event, wait);
}

void quota_service_post(quota_app_event_kind_t kind)
{
    quota_app_event_t event = {.kind = kind};
    post_event(&event, 0);
}

void quota_service_wake_network(void)
{
    if (s_network_task != NULL)
        xTaskNotifyGive(s_network_task);
}

static void portable_notify(void)
{
    quota_service_post(QUOTA_APP_EVENT_SNAPSHOT);
}
static uint32_t portable_config_generation_locked(void)
{
    return s_config_generation;
}
static void portable_config_changed_locked(void)
{
    if (s_config_generation < UINT32_MAX)
        s_config_generation++;
}

QUOTA_TESTABLE TickType_t network_wait(bool sleeping)
{
    uint64_t now = quota_monotonic_ms();
    uint64_t deadline = sleeping && !quota_wifi_started() ? UINT64_MAX : now + 500;
    uint64_t portable = quota_portable_service_next_deadline_ms(sleeping);
    if (portable < deadline)
        deadline = portable;
    if (quota_usb_requested())
        deadline = now + 20;
    if (deadline == UINT64_MAX)
        return portMAX_DELAY;
    if (deadline <= now)
        return 0;
    TickType_t ticks = pdMS_TO_TICKS(deadline - now);
    return ticks ? ticks : 1;
}

QUOTA_TESTABLE void network_task(void *arg)
{
    (void)arg;
    for (;;) {
        display_state_t waiting = display_state_snapshot();
        (void)ulTaskNotifyTake(pdTRUE, network_wait(waiting.sleeping));
        display_state_t display = display_state_snapshot();
        quota_usb_poll(display.sleeping);
        quota_portable_service_tick(display.sleeping, display.generation);
        if (display.sleeping)
            (void)quota_wifi_stop();
        quota_usb_poll(display.sleeping);
        quota_service_lock();
        quota_usb_fill_view_locked(&s_view);
        quota_service_unlock();
    }
}

bool quota_service_init(void)
{
    if (s_events)
        return true;
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex)
        return false;
    s_events = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(quota_app_event_t));
    if (!s_events)
        return false;
    esp_err_t err = nvs_flash_init();
    s_nvs_ready = err == ESP_OK;
    if (!s_nvs_ready)
        ESP_LOGE(TAG, "NVS init failed (%s); stored credentials are unavailable",
                 esp_err_to_name(err));
    (void)quota_store_init();
    if (!quota_store_erase_retired(s_nvs_ready))
        ESP_LOGW(TAG, "retired storage cleanup incomplete; it repeats at next boot");
    s_config_generation = 1;
    s_view.screen_timeout_seconds = QUOTA_SCREEN_TIMEOUT_DEFAULT_SECONDS;
    s_view.refresh_seconds = QUOTA_REFRESH_DEFAULT_SECONDS;
    s_view.auto_refresh = true;
    quota_portable_service_hooks_t hooks = {.view = &s_view,
                                            .lock = quota_service_lock,
                                            .unlock = quota_service_unlock,
                                            .try_lock = quota_service_try_lock,
                                            .config_generation_locked =
                                                portable_config_generation_locked,
                                            .config_changed_locked = portable_config_changed_locked,
                                            .notify = portable_notify,
                                            .wake = quota_service_wake_network,
                                            .display_current = display_generation_is_current};
    if (!quota_portable_service_init(&hooks))
        return false;
    return true;
}

bool quota_service_start(void)
{
    if (s_events == NULL || s_mutex == NULL)
        return false;
    /* The basic USB console polls the FIFO without waiting. In IDF 5.5.3,
     * O_NONBLOCK instead checks a driver ring buffer that we do not install. */
    usb_serial_jtag_vfs_use_nonblocking();
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK) < 0)
        return false;
    if (s_network_task == NULL &&
        xTaskCreate(network_task, "quota_network", NETWORK_TASK_STACK, NULL, NETWORK_TASK_PRIORITY,
                    &s_network_task) != pdPASS) {
        s_network_task = NULL;
        return false;
    }
    return true;
}

QueueHandle_t quota_service_event_queue(void)
{
    return s_events;
}

void quota_service_send_button(bsp_btn_t button, bsp_btn_ev_t event)
{
    quota_app_event_t app_event = {
        .kind = QUOTA_APP_EVENT_BUTTON,
        .button = button,
        .button_event = event,
    };
    post_event(&app_event, 0);
}

void quota_service_set_display_sleeping(bool sleeping)
{
    bool changed = false;
    portENTER_CRITICAL(&s_display_state_mux);
    if (s_display_scheduler.sleeping != sleeping) {
        s_display_scheduler.sleeping = sleeping;
        s_display_scheduler.generation++;
        if (s_display_scheduler.generation == 0)
            s_display_scheduler.generation = 1;
        changed = true;
    }
    portEXIT_CRITICAL(&s_display_state_mux);
    if (!changed)
        return;
    if (sleeping)
        quota_usb_close_window();

    if (sleeping && s_mutex != NULL && xSemaphoreTake(s_mutex, 0) == pdTRUE) {
        s_view.refreshing = false;
        (void)xSemaphoreGive(s_mutex);
    }
    quota_service_wake_network();
}

void quota_service_get_view(quota_service_view_t *view)
{
    if (view == NULL)
        return;
    quota_service_lock();
    *view = s_view;
    if (display_state_snapshot().sleeping)
        view->refreshing = false;
    view->now_epoch = current_epoch();
    quota_usb_fill_view_locked(view);
    quota_portable_service_countdown_overlay_locked(view);
    quota_service_unlock();
}

void quota_service_get_selected_account_id(char account_id[QUOTA_ACCOUNT_ID_BYTES + 1])
{
    if (!account_id)
        return;
    account_id[0] = 0;
    (void)quota_portable_service_selected(account_id);
}

void quota_service_request_refresh(void)
{
    quota_portable_service_refresh();
}

void quota_service_request_settings(uint16_t refresh_seconds, bool auto_refresh,
                                    uint16_t screen_timeout_seconds)
{
    quota_portable_service_settings(refresh_seconds, auto_refresh, screen_timeout_seconds);
}

void quota_service_select_account(const char *account_id)
{
    quota_portable_service_select(account_id);
}

void quota_service_open_phone(void)
{
    quota_portable_service_open();
}
void quota_service_close_phone(void)
{
    quota_portable_service_close();
}
void quota_service_renew_phone(void)
{
    quota_portable_service_renew();
}
void quota_service_cancel_auth(void)
{
    quota_portable_service_cancel_auth();
}
void quota_service_reconnect(void)
{
    quota_portable_service_reconnect();
}
