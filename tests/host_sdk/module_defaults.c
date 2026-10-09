/* Weak stand-ins for the firmware modules that sit next to the unit under test. Link the real
 * module and its strong definitions win; a test overrides single functions to observe calls. */
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "quota_portable_service.h"
#include "quota_store.h"
#include "quota_ui.h"
#include "quota_usb.h"
#include "quota_wifi.h"

#define WEAK __attribute__((weak))

WEAK bool quota_store_init(void)
{
    return true;
}
WEAK bool quota_store_erase_retired(bool ready)
{
    (void)ready;
    return true;
}

WEAK bool quota_portable_service_init(const quota_portable_service_hooks_t *hooks)
{
    (void)hooks;
    return true;
}
WEAK void quota_portable_service_tick(bool sleeping, uint32_t generation)
{
    (void)sleeping;
    (void)generation;
}
WEAK void quota_portable_service_countdown_overlay_locked(quota_service_view_t *view)
{
    (void)view;
}
WEAK uint64_t quota_portable_service_next_deadline_ms(bool sleeping)
{
    (void)sleeping;
    return UINT64_MAX;
}
WEAK bool quota_portable_service_prepare_usb(void)
{
    return true;
}
WEAK quota_portable_submit_result_t quota_portable_service_submit(
    const quota_portable_command_t *command, quota_setup_transport_t transport)
{
    (void)command;
    (void)transport;
    return QUOTA_PORTABLE_SUBMIT_INVALID;
}
WEAK bool quota_portable_service_state_json(char *buffer, size_t capacity, size_t *length,
                                            quota_setup_transport_t transport)
{
    (void)buffer;
    (void)capacity;
    (void)length;
    (void)transport;
    return false;
}
WEAK void quota_portable_service_disconnected(uint8_t reason)
{
    (void)reason;
}
WEAK void quota_portable_service_open(void) {}
WEAK void quota_portable_service_close(void) {}
WEAK void quota_portable_service_renew(void) {}
WEAK void quota_portable_service_cancel_auth(void) {}
WEAK void quota_portable_service_refresh(void) {}
WEAK void quota_portable_service_reconnect(void) {}
WEAK void quota_portable_service_factory_reset(void) {}
WEAK void quota_portable_service_settings(uint16_t interval, bool automatic,
                                          uint16_t screen_timeout)
{
    (void)interval;
    (void)automatic;
    (void)screen_timeout;
}
WEAK void quota_portable_service_select(const char *account_id)
{
    (void)account_id;
}
WEAK bool quota_portable_service_selected(char account_id[QUOTA_ACCOUNT_ID_BYTES + 1])
{
    account_id[0] = 0;
    return false;
}

WEAK void quota_usb_open_window(void) {}
WEAK void quota_usb_close_window(void) {}
WEAK void quota_usb_poll(bool sleeping)
{
    (void)sleeping;
}
WEAK bool quota_usb_requested(void)
{
    return false;
}
WEAK bool quota_usb_active(void)
{
    return false;
}
WEAK bool quota_usb_blocked(void)
{
    return false;
}
WEAK uint64_t quota_usb_deadline_ms(void)
{
    return 0;
}
WEAK void quota_usb_fill_view_locked(quota_service_view_t *view)
{
    (void)view;
}

WEAK bool quota_wifi_start(void)
{
    return true;
}
WEAK bool quota_wifi_stop(void)
{
    return true;
}
WEAK bool quota_wifi_started(void)
{
    return false;
}

WEAK esp_err_t bsp_i2c_init(void)
{
    return ESP_OK;
}
WEAK esp_err_t bsp_display_init(void)
{
    return ESP_OK;
}
WEAK void bsp_display_backlight(uint8_t percent)
{
    (void)percent;
}
WEAK struct _lv_display_t *bsp_lvgl_init(void)
{
    return (struct _lv_display_t *)1;
}
WEAK bool bsp_lvgl_lock(int timeout_ms)
{
    (void)timeout_ms;
    return true;
}
WEAK void bsp_lvgl_unlock(void) {}
WEAK bool bsp_lvgl_set_sleeping(bool sleeping)
{
    (void)sleeping;
    return true;
}
WEAK bool bsp_lvgl_refresh(void)
{
    return true;
}
WEAK esp_err_t bsp_battery_init(void)
{
    return ESP_OK;
}
WEAK int bsp_battery_soc(void)
{
    return 50;
}
WEAK esp_err_t bsp_button_init(bsp_btn_cb_t cb, void *user)
{
    (void)cb;
    (void)user;
    return ESP_OK;
}

WEAK void quota_ui_init(void) {}
WEAK void quota_ui_render(const quota_navigation_t *navigation, const quota_service_view_t *service,
                          int battery_percent)
{
    (void)navigation;
    (void)service;
    (void)battery_percent;
}

WEAK void quota_service_factory_reset(void) {}
WEAK bool quota_service_init(void)
{
    return true;
}
WEAK bool quota_service_start(void)
{
    return true;
}
WEAK QueueHandle_t quota_service_event_queue(void)
{
    return (void *)1;
}
WEAK void quota_service_send_button(bsp_btn_t button, bsp_btn_ev_t event)
{
    (void)button;
    (void)event;
}
WEAK void quota_service_set_display_sleeping(bool sleeping)
{
    (void)sleeping;
}
WEAK void quota_service_get_view(quota_service_view_t *view)
{
    (void)view;
}
WEAK void quota_service_get_selected_account_id(char account_id[QUOTA_ACCOUNT_ID_BYTES + 1])
{
    account_id[0] = 0;
}
WEAK void quota_service_request_refresh(void) {}
WEAK void quota_service_request_settings(uint16_t refresh_seconds, bool auto_refresh,
                                         uint16_t screen_timeout_seconds)
{
    (void)refresh_seconds;
    (void)auto_refresh;
    (void)screen_timeout_seconds;
}
WEAK void quota_service_select_account(const char *account_id)
{
    (void)account_id;
}
WEAK void quota_service_open_phone(void) {}
WEAK void quota_service_close_phone(void) {}
WEAK void quota_service_renew_phone(void) {}
WEAK void quota_service_cancel_auth(void) {}
WEAK void quota_service_reconnect(void) {}
WEAK TaskHandle_t quota_service_network_task(void)
{
    return (void *)1;
}
