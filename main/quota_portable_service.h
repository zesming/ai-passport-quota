#pragma once

#include "quota_service.h"
#include "quota_catalog.h"
#include "quota_store.h"

/* The existing network task serializes setup, USB, provider and NVS work. The hooks are the seams
 * the host tests replace; USB and Wi-Fi are called directly (quota_usb.h, quota_wifi.h). */
typedef struct {
    quota_service_view_t *view;
    void (*lock)(void);
    void (*unlock)(void);
    bool (*try_lock)(void);
    uint32_t (*config_generation_locked)(void);
    void (*config_changed_locked)(void);
    void (*notify)(void);
    void (*wake)(void);
    bool (*display_current)(uint32_t generation);
} quota_portable_service_hooks_t;

bool quota_portable_service_init(const quota_portable_service_hooks_t *hooks);
/* Rechecked before every HTTP phase, including synchronously pending physical entry. */
bool quota_portable_service_http_allowed(void);
void quota_portable_service_tick(bool sleeping, uint32_t generation);
void quota_portable_service_countdown_overlay_locked(quota_service_view_t *view);
uint64_t quota_portable_service_next_deadline_ms(bool sleeping);
bool quota_portable_service_prepare_usb(void);
/* The one validation function, for both entry points: the USB `validate` command and the
 * hotspot after setup_close has closed the access point. It checks the Wi-Fi networks, DeepSeek
 * keys and queued ChatGPT authorization whose validation is pending or failed, one after the
 * other, in the network task. Returns false when nothing needs validating. */
bool quota_portable_validate_pending(void);
/* Firmware version for protocol replies. */
const char *quota_portable_service_firmware(void);
quota_portable_submit_result_t
quota_portable_service_submit(const quota_portable_command_t *command,
                              quota_setup_transport_t transport);
bool quota_portable_service_state_json(char *buffer, size_t capacity, size_t *length,
                                       quota_setup_transport_t transport);
void quota_portable_service_disconnected(uint8_t reason);
void quota_portable_service_open(void);
void quota_portable_service_close(void);
void quota_portable_service_renew(void);
void quota_portable_service_cancel_auth(void);
void quota_portable_service_refresh(void);
void quota_portable_service_reconnect(void);
/* Only the physical interface asks for this. The next tick of the network task stops setup and
 * USB, erases all stored data and restarts; a failed erase shows as a storage error. */
void quota_portable_service_factory_reset(void);
void quota_portable_service_settings(uint16_t interval, bool automatic, uint16_t screen_timeout);
void quota_portable_service_select(const char *account_id);
bool quota_portable_service_selected(char account_id[QUOTA_ACCOUNT_ID_BYTES + 1]);
