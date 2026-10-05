#pragma once

#include "quota_service.h"
#include "quota_catalog.h"
#include "quota_store.h"

/* The existing network task serializes setup, USB, provider, collector and NVS work. */
typedef struct {
    quota_service_view_t *view;
    void (*lock)(void);
    void (*unlock)(void);
    bool (*try_lock)(void);
    uint32_t (*config_generation_locked)(void);
    void (*config_changed_locked)(void);
    bool (*pairing_requested)(void);
    bool (*wifi_ready)(void);
    bool (*wifi_stop)(void);
    void (*notify)(void);
    void (*wake)(void);
    bool (*display_current)(uint32_t generation);
    /* Optional v1 inventory; age limits observations, never identity inventory. */
    quota_store_read_result_t (*legacy_config)(quota_device_config_t *out,
                                                uint16_t *screen_timeout);
    quota_store_read_result_t (*legacy_inventory)(quota_snapshot_t *out);
    /* One temporary incoming snapshot, owned and freed by the common coordinator. */
    bool (*legacy_snapshot)(const quota_legacy_endpoint_t *endpoint, bool refresh,
                            uint32_t display_generation, quota_snapshot_t *out,
                            bool *deferred);
} quota_portable_service_hooks_t;

bool quota_portable_service_init(const quota_device_config_t *legacy,
                                const quota_portable_service_hooks_t *hooks);
bool quota_portable_service_owns_network(void);
/* Rechecked before every HTTP phase, including synchronously pending physical entry. */
bool quota_portable_service_http_allowed(void);
void quota_portable_service_tick(bool sleeping, uint32_t generation);
void quota_portable_service_countdown_overlay_locked(quota_service_view_t *view);
void quota_portable_service_overlay(quota_service_view_t *view);
uint64_t quota_portable_service_next_deadline_ms(bool sleeping);
bool quota_portable_service_prepare_pairing(void);
/* The modern v1 USB frame updates this model, never the historical v1 config key. */
bool quota_portable_service_configure_legacy(const quota_device_config_t *configuration,
                                            const char **error);
void quota_portable_service_disconnected(uint8_t reason);
void quota_portable_service_open(void);
void quota_portable_service_close(void);
void quota_portable_service_renew(void);
void quota_portable_service_cancel_auth(void);
void quota_portable_service_refresh(void);
void quota_portable_service_reconnect(void);
void quota_portable_service_settings(uint16_t interval, bool automatic,
                                     uint16_t screen_timeout);
void quota_portable_service_select(const char *account_id);
bool quota_portable_service_selected(char account_id[QUOTA_ACCOUNT_ID_BYTES + 1]);
