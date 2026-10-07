#pragma once

#include "quota_portable.h"

typedef struct {
    quota_portable_submit_result_t (*submit)(const quota_portable_command_t *command,
                                            void *context);
    /* Return an already sanitized JSON document, never the device QR secrets. */
    bool (*state_json)(char *buffer, size_t capacity, size_t *length, void *context);
    bool (*session_active)(void *context);
    void *context;
} quota_portal_callbacks_t;

/* AP driver/lifecycle remains service-owned. Start only after AP is ready. */
bool quota_portal_start(const char *secret, const quota_portal_callbacks_t *callbacks);
void quota_portal_stop(void);
bool quota_portal_running(void);

/* Pure validation seams used by host tests and the HTTP handlers. */
bool quota_portal_host_is_valid(const char *host);
bool quota_portal_origin_is_valid(const char *origin);
bool quota_portal_secret_matches(const char *expected, const char *supplied);
bool quota_portal_parse_command(const char *json, size_t length,
                                quota_portable_command_t *command);
struct cJSON;
/* Wipe private parsed strings before releasing their allocations. */
void quota_portal_clear_json(struct cJSON *json);
