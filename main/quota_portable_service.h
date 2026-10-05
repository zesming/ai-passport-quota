#pragma once

#include "quota_service.h"

/* Uses the existing network task and Wi-Fi stack; no second network worker. */
typedef struct {
    bool (*wifi_ready)(void);
    bool (*wifi_stop)(void);
    void (*notify)(void);
    void (*wake)(void);
    bool (*display_current)(uint32_t generation);
} quota_portable_service_hooks_t;

bool quota_portable_service_init(const quota_device_config_t *legacy,
                                const quota_portable_service_hooks_t *hooks);
bool quota_portable_service_owns_network(void);
void quota_portable_service_tick(bool sleeping, uint32_t generation);
void quota_portable_service_overlay(quota_service_view_t *view);
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
