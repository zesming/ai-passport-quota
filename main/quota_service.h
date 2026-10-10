#pragma once

#include "bsp_button.h"
#include "quota_logic.h"
#include "quota_portable.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_timer.h"

typedef enum {
    QUOTA_APP_EVENT_BUTTON = 0,
    QUOTA_APP_EVENT_SNAPSHOT,
    QUOTA_APP_EVENT_CONNECTION,
    QUOTA_APP_EVENT_SETTINGS_RESULT,
    QUOTA_APP_EVENT_USB_WINDOW,
    QUOTA_APP_EVENT_WAKE, /* screen-off key sampled by the BSP */
} quota_app_event_kind_t;

typedef struct {
    quota_app_event_kind_t kind;
    bsp_btn_t button;
    bsp_btn_ev_t button_event;
    uint16_t refresh_seconds;
    bool auto_refresh;
    uint16_t screen_timeout_seconds;
} quota_app_event_t;

typedef struct {
    quota_snapshot_t snapshot;
    quota_portable_view_t portable;
    bool snapshot_valid;
    bool configured;
    bool connected;
    int8_t wifi_rssi; /* dBm of the current link, 0 when unknown (set by quota_service_get_view) */
    char wifi_mac[18]; /* STA MAC, available offline; empty when it could not be read. */
    bool refreshing;
    bool request_failed;
    bool usb_window_active;
    bool usb_window_preparing;
    bool usb_page_connected; /* a settings page has opened its session in the USB window */
    uint32_t usb_window_seconds_left;
    uint64_t now_epoch;
    bool clock_synchronized;
    uint16_t refresh_seconds;
    bool auto_refresh;
    uint16_t screen_timeout_seconds;
} quota_service_view_t;

bool quota_service_init(void);
bool quota_service_start(void);
QueueHandle_t quota_service_event_queue(void);
void quota_service_send_button(bsp_btn_t button, bsp_btn_ev_t event);
/* Idempotent, non-blocking gate for board-originated network activity. */
void quota_service_set_display_sleeping(bool sleeping);
void quota_service_get_view(quota_service_view_t *view);
void quota_service_get_selected_account_id(char account_id[QUOTA_ACCOUNT_ID_BYTES + 1]);
void quota_service_request_refresh(void);
void quota_service_request_settings(uint16_t refresh_seconds, bool auto_refresh,
                                    uint16_t screen_timeout_seconds);
void quota_service_select_account(const char *account_id);

void quota_service_open_phone(void);
void quota_service_close_phone(void);
void quota_service_renew_phone(void);
void quota_service_cancel_auth(void);
void quota_service_reconnect(void);
/* Posts the request; the network task erases storage and restarts the device. */
void quota_service_factory_reset(void);

/* Internals shared with quota_wifi.c and quota_usb.c. The lock guards the view. */
void quota_service_lock(void);
void quota_service_unlock(void);
bool quota_service_try_lock(void);
quota_service_view_t *quota_service_view(void); /* caller holds the lock */
bool quota_service_display_sleeping(void);
TaskHandle_t quota_service_network_task(void); /* NULL before quota_service_start() */
void quota_service_post(quota_app_event_kind_t kind);
void quota_service_wake_network(void);

static inline uint64_t quota_monotonic_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}
