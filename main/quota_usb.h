#pragma once

#include "quota_catalog.h"

#define QUOTA_USB_SESSION_BYTES 32
#define QUOTA_USB_RESPONSE_BYTES (QUOTA_PORTABLE_STATE_BYTES + 256)

typedef enum {
    QUOTA_USB_LEGACY = 0,
    QUOTA_USB_OPEN,
    QUOTA_USB_STATE,
    QUOTA_USB_COMMAND,
    QUOTA_USB_COLLECTOR,
} quota_usb_op_t;

/* Ephemeral owner scratch. Wipe and release before any HTTP work. */
typedef struct {
    quota_usb_op_t op;
    char request_id[9];
    char session_id[QUOTA_USB_SESSION_BYTES + 1];
    union {
        quota_portable_command_t command;
        quota_legacy_endpoint_t endpoint;
        quota_device_config_t legacy;
    } body;
} quota_usb_request_t;

bool quota_usb_parse(const char *frame, size_t length, quota_usb_request_t *request,
                     const char **error);
