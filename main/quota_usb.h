#pragma once

#include "quota_catalog.h"
#include "quota_service.h"

#define QUOTA_USB_SESSION_BYTES 32
#define QUOTA_USB_RESPONSE_BYTES (QUOTA_PORTABLE_STATE_BYTES + 256)

typedef enum {
    QUOTA_USB_OPEN = 0,
    QUOTA_USB_STATE,
    QUOTA_USB_COMMAND,
} quota_usb_op_t;

/* Ephemeral owner scratch. Wipe and release before any HTTP work. */
typedef struct {
    quota_usb_op_t op;
    char request_id[9];
    char session_id[QUOTA_USB_SESSION_BYTES + 1];
    quota_portable_command_t command;
} quota_usb_request_t;

bool quota_usb_parse(const char *frame, size_t length, quota_usb_request_t *request,
                     const char **error);

/* USB setup window. Physical entry requests it; quota_usb_poll() on the network task opens the
 * session, reads frames and closes it. */
void quota_usb_open_window(void);
void quota_usb_close_window(void);
void quota_usb_poll(bool sleeping);
bool quota_usb_requested(void);
bool quota_usb_active(void);
/* Entry pending, or serial input in flight: HTTP work must wait. */
bool quota_usb_blocked(void);
uint64_t quota_usb_deadline_ms(void);
/* Refresh the window fields of a view copy; the caller holds the service lock. */
void quota_usb_fill_view_locked(quota_service_view_t *view);
