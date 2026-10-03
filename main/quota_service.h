#pragma once

#include "bsp_button.h"
#include "quota_logic.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    QUOTA_APP_EVENT_BUTTON = 0,
    QUOTA_APP_EVENT_SNAPSHOT,
    QUOTA_APP_EVENT_CONNECTION,
    QUOTA_APP_EVENT_SETTINGS_RESULT,
    QUOTA_APP_EVENT_CONFIGURATION_RESULT,
    QUOTA_APP_EVENT_PAIRING_TICK,
} quota_app_event_kind_t;

typedef struct {
    quota_app_event_kind_t kind;
    bsp_btn_t button;
    bsp_btn_ev_t button_event;
    bool success;
    uint16_t refresh_seconds;
    bool auto_refresh;
    uint16_t screen_timeout_seconds;
} quota_app_event_t;

typedef struct {
    quota_snapshot_t snapshot;
    bool snapshot_valid;
    bool configured;
    bool connected;
    bool refreshing;
    bool request_failed;
    bool pairing_active;
    uint32_t pairing_seconds_left;
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
void quota_service_open_pairing_window(void);
void quota_service_close_pairing_window(void);
