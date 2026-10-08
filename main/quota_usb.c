#include "quota_usb.h"
#include "quota_json.h"
#include "quota_portable_service.h"
#include "quota_portal.h"
#include "quota_testable.h"
#include "cJSON.h"
#include "esp_random.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

QUOTA_TESTABLE atomic_bool s_usb_requested;
QUOTA_TESTABLE atomic_uint_fast64_t s_usb_deadline;
QUOTA_TESTABLE atomic_bool s_usb_io_busy;
QUOTA_TESTABLE bool s_usb_window_open;
QUOTA_TESTABLE int64_t s_usb_opened_at_ms;
QUOTA_TESTABLE quota_frame_decoder_t *s_usb_decoder;
QUOTA_TESTABLE uint64_t s_usb_partial_at, s_usb_opener_at;
QUOTA_TESTABLE char s_usb_session[QUOTA_USB_SESSION_BYTES + 1], s_usb_opener[9];

static bool text(const cJSON *root, const char *name, char *out, size_t capacity)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(value) || !value->valuestring || !value->valuestring[0] ||
        strlen(value->valuestring) >= capacity)
        return false;
    memcpy(out, value->valuestring, strlen(value->valuestring) + 1);
    return true;
}
bool quota_usb_parse(const char *frame, size_t length, quota_usb_request_t *request,
                     const char **error)
{
    if (error)
        *error = "invalid_frame";
    if (!request)
        return false;
    memset(request, 0, sizeof(*request));
    memcpy(request->request_id, "00000000", 9);
    if (!frame || length < 6 || length > QUOTA_MAX_PROVISION_FRAME_BYTES ||
        memcmp(frame, "@AIQ:", 5))
        return false;
    const char *json = frame + 5, *end = NULL;
    if (!quota_json_text_bounded(json, length - 5))
        return false;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length - 5, &end, false);
    if (!root || !end) {
        quota_portal_clear_json(root);
        cJSON_Delete(root);
        return false;
    }
    while (end < frame + length && (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t'))
        end++;
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "v");
    bool valid = end == frame + length && cJSON_IsNumber(version);
    char op[32] = {0};
    if (valid && text(root, "request_id", request->request_id, sizeof(request->request_id)) &&
        quota_json_is_lower_hex(request->request_id, 8)) {
        if (version->valuedouble != 2) {
            if (error)
                *error = "unsupported_version";
            valid = false;
        }
    } else {
        memcpy(request->request_id, "00000000", 9);
        valid = false;
    }
    if (valid)
        valid = text(root, "op", op, sizeof(op));
    static const char *const open_keys[] = {"v", "op", "request_id", NULL};
    static const char *const state_keys[] = {"v", "op", "request_id", "session_id", NULL};
    static const char *const command_keys[] = {"v", "op", "request_id", "session_id", "body", NULL};
    if (valid && !strcmp(op, "session_open")) {
        request->op = QUOTA_USB_OPEN;
        valid = quota_json_keys_allowed(root, open_keys);
    } else if (valid) {
        valid = text(root, "session_id", request->session_id, sizeof(request->session_id)) &&
                quota_json_is_lower_hex(request->session_id, QUOTA_USB_SESSION_BYTES);
        if (!strcmp(op, "state_get")) {
            request->op = QUOTA_USB_STATE;
            valid = valid && quota_json_keys_allowed(root, state_keys);
        } else if (!strcmp(op, "command")) {
            request->op = QUOTA_USB_COMMAND;
            valid = valid && quota_json_keys_allowed(root, command_keys);
            const cJSON *body = cJSON_GetObjectItemCaseSensitive(root, "body");
            char *serialized = valid && cJSON_IsObject(body) ? cJSON_PrintUnformatted(body) : NULL;
            valid = serialized &&
                    quota_portal_parse_command(serialized, strlen(serialized), &request->command) &&
                    !strcmp(request->request_id, request->command.request_id);
            if (serialized) {
                quota_portable_clear_secret(serialized, strlen(serialized));
                cJSON_free(serialized);
            }
        } else {
            valid = false;
            if (error)
                *error = "unsupported_operation";
        }
    }
    quota_portal_clear_json(root);
    cJSON_Delete(root);
    if (valid && error)
        *error = NULL;
    return valid;
}

bool quota_usb_requested(void)
{
    return atomic_load(&s_usb_requested);
}
uint64_t quota_usb_deadline_ms(void)
{
    return atomic_load(&s_usb_deadline);
}
bool quota_usb_active(void)
{
    return quota_usb_requested() && quota_monotonic_ms() < quota_usb_deadline_ms();
}
bool quota_usb_blocked(void)
{
    return atomic_load(&s_usb_io_busy) || (quota_usb_requested() && !quota_usb_deadline_ms());
}

void quota_usb_fill_view_locked(quota_service_view_t *view)
{
    uint64_t now = quota_monotonic_ms();
    view->usb_window_active =
        quota_usb_window_active(s_usb_window_open, now, (uint64_t)s_usb_opened_at_ms);
    view->usb_window_seconds_left =
        view->usb_window_active
            ? (uint32_t)((QUOTA_USB_WINDOW_MS - (now - (uint64_t)s_usb_opened_at_ms) + 999) / 1000)
            : 0;
}

QUOTA_TESTABLE void release_usb_decoder(void)
{
    if (s_usb_decoder) {
        quota_portable_clear_secret(s_usb_decoder, sizeof(*s_usb_decoder));
        free(s_usb_decoder);
        s_usb_decoder = NULL;
    }
    s_usb_partial_at = 0;
}
QUOTA_TESTABLE bool usb_authorized(const char *session)
{
    if (!quota_usb_active() || !s_usb_opener[0] || !session ||
        strlen(session) != QUOTA_USB_SESSION_BYTES)
        return false;
    unsigned difference = 0;
    for (unsigned i = 0; i < QUOTA_USB_SESSION_BYTES; i++)
        difference |= (unsigned char)session[i] ^ (unsigned char)s_usb_session[i];
    return difference == 0;
}
QUOTA_TESTABLE void send_usb_result(const char *id, const char *session, const char *error,
                                    bool accepted)
{
    if (!error && (!quota_usb_active() || (session && !usb_authorized(session))))
        error = "session_expired";
    char response[320];
    int length =
        error ? snprintf(response, sizeof(response),
                         "@AIQ:{\"v\":2,\"op\":\"result\",\"request_id\":\"%.8s\",\"ok\":false,"
                         "\"error_code\":\"%s\"}\n",
                         id ? id : "00000000", error)
              : snprintf(response, sizeof(response),
                         "@AIQ:{\"v\":2,\"op\":\"result\",\"request_id\":\"%.8s\",\"ok\":true,"
                         "\"session_id\":\"%.32s\",\"accepted\":%s}\n",
                         id ? id : "00000000", session ? session : "", accepted ? "true" : "false");
    if (length > 0 && (size_t)length < sizeof(response))
        (void)fwrite(response, 1, (size_t)length, stdout);
    (void)fflush(stdout);
}
QUOTA_TESTABLE void send_usb_state(const char *id, const char *session)
{
    char *response = malloc(QUOTA_USB_RESPONSE_BYTES);
    if (!response) {
        send_usb_result(id, session, "no_memory", false);
        return;
    }
    int prefix = snprintf(response, QUOTA_USB_RESPONSE_BYTES,
                          "@AIQ:{\"v\":2,\"op\":\"state\",\"request_id\":\"%.8s\",\"session_id\":"
                          "\"%.32s\",\"ok\":true,\"state\":",
                          id, session);
    size_t length = 0;
    bool ok =
        prefix > 0 && (size_t)prefix + QUOTA_PORTABLE_STATE_BYTES + 3 <= QUOTA_USB_RESPONSE_BYTES;
    if (ok)
        ok = quota_portable_service_state_json(response + prefix, QUOTA_PORTABLE_STATE_BYTES + 1,
                                               &length, QUOTA_SETUP_USB) &&
             length && length <= QUOTA_PORTABLE_STATE_BYTES;
    if (ok && usb_authorized(session)) {
        response[prefix + length] = '}';
        response[prefix + length + 1] = '\n';
        (void)fwrite(response, 1, (size_t)prefix + length + 2, stdout);
        (void)fflush(stdout);
    } else
        send_usb_result(id, NULL, usb_authorized(session) ? "state_unavailable" : "session_expired",
                        false);
    quota_portable_clear_secret(response, QUOTA_USB_RESPONSE_BYTES);
    free(response);
}
QUOTA_TESTABLE void new_usb_session(void)
{
    for (unsigned i = 0; i < QUOTA_USB_SESSION_BYTES; i++)
        s_usb_session[i] = "0123456789abcdef"[esp_random() & 15];
    s_usb_session[QUOTA_USB_SESSION_BYTES] = 0;
}
QUOTA_TESTABLE void handle_serial_frame(const char *frame, size_t length)
{
    quota_usb_request_t *request = calloc(1, sizeof(*request));
    const char *error = NULL;
    bool valid = request && quota_usb_parse(frame, length, request, &error);
    /* No borrowed frame survives processing or a state/HTTP allocation. */
    release_usb_decoder();
    if (!request) {
        send_usb_result(NULL, NULL, "no_memory", false);
        return;
    }
    if (!valid) {
        send_usb_result(request->request_id, NULL, error ? error : "invalid_frame", false);
        goto done;
    }
    if (request->op == QUOTA_USB_OPEN) {
        if (!quota_usb_active())
            send_usb_result(request->request_id, NULL, "session_expired", false);
        /* One serial port has one host owner, so an opener silent past the idle limit has lost its
         * link (page reload) and may be replaced. */
        else if (s_usb_opener[0] && strcmp(s_usb_opener, request->request_id) &&
                 quota_monotonic_ms() - s_usb_opener_at < QUOTA_USB_OPENER_IDLE_MS)
            send_usb_result(request->request_id, NULL, "session_busy", false);
        else {
            /* A takeover revokes the previous page's session so only one page keeps control. */
            if (s_usb_opener[0] && strcmp(s_usb_opener, request->request_id))
                new_usb_session();
            memcpy(s_usb_opener, request->request_id, 9);
            s_usb_opener_at = quota_monotonic_ms();
            char response[320];
            uint64_t left = (quota_usb_deadline_ms() - quota_monotonic_ms() + 999) / 1000;
            int bytes =
                snprintf(response, sizeof(response),
                         "@AIQ:{\"v\":2,\"op\":\"result\",\"request_id\":\"%.8s\",\"ok\":true,"
                         "\"session_id\":\"%.32s\",\"remaining_seconds\":%u,\"max_command_bytes\":"
                         "2048,\"max_frame_bytes\":4096,\"max_state_bytes\":16384}\n",
                         request->request_id, s_usb_session, (unsigned)left);
            if (quota_usb_active() && bytes > 0 && (size_t)bytes < sizeof(response))
                (void)fwrite(response, 1, (size_t)bytes, stdout);
            (void)fflush(stdout);
        }
    } else if (!usb_authorized(request->session_id))
        send_usb_result(request->request_id, NULL,
                        quota_usb_active() ? "invalid_session" : "session_expired", false);
    else if (request->op == QUOTA_USB_STATE) {
        s_usb_opener_at = quota_monotonic_ms();
        char id[9], session[QUOTA_USB_SESSION_BYTES + 1];
        memcpy(id, request->request_id, sizeof(id));
        memcpy(session, request->session_id, sizeof(session));
        quota_portable_clear_secret(request, sizeof(*request));
        free(request);
        request = NULL;
        send_usb_state(id, session);
        quota_portable_clear_secret(session, sizeof(session));
    } else {
        s_usb_opener_at = quota_monotonic_ms();
        quota_portable_submit_result_t result = QUOTA_PORTABLE_SUBMIT_INVALID;
        if (request->op == QUOTA_USB_COMMAND)
            result = quota_portable_service_submit(&request->command, QUOTA_SETUP_USB);
        static const char *errors[] = {NULL, "busy", "session_expired", "invalid_command",
                                       "request_conflict"};
        send_usb_result(request->request_id, request->session_id,
                        (unsigned)result < sizeof(errors) / sizeof(errors[0]) ? errors[result]
                                                                              : "invalid_command",
                        result == QUOTA_PORTABLE_SUBMIT_ACCEPTED);
    }
done:
    if (request) {
        quota_portable_clear_secret(request, sizeof(*request));
        free(request);
    }
}

void quota_usb_open_window(void)
{
    atomic_store(&s_usb_deadline, 0);
    atomic_store(&s_usb_requested, true);
    quota_service_lock();
    quota_service_view_t *view = quota_service_view();
    s_usb_window_open = false;
    view->usb_window_preparing = true;
    view->usb_window_active = false;
    view->usb_window_seconds_left = 0;
    quota_service_unlock();
    quota_service_wake_network();
    quota_service_post(QUOTA_APP_EVENT_USB_WINDOW);
}

void quota_usb_close_window(void)
{
    atomic_store(&s_usb_requested, false);
    atomic_store(&s_usb_deadline, 0);
    quota_service_lock();
    quota_service_view_t *view = quota_service_view();
    s_usb_window_open = false;
    view->usb_window_preparing = view->usb_window_active = false;
    view->usb_window_seconds_left = 0;
    quota_service_unlock();
    quota_service_wake_network();
}

/* Only this owner reads USB; idle sessions retain no decoder or TLS scratch. */
void quota_usb_poll(bool sleeping)
{
    if (sleeping && quota_usb_requested())
        quota_usb_close_window();
    if (!quota_usb_requested()) {
        release_usb_decoder();
        atomic_store(&s_usb_io_busy, false);
        atomic_store(&s_usb_deadline, 0);
        quota_portable_clear_secret(s_usb_session, sizeof(s_usb_session));
        s_usb_opener[0] = 0;
        return;
    }
    if (!quota_usb_deadline_ms()) {
        release_usb_decoder();
        atomic_store(&s_usb_io_busy, false);
        if (!quota_portable_service_prepare_usb())
            return;
        unsigned char ignored[64];
        unsigned drained = 0;
        while (drained < 4096) {
            ssize_t count = read(STDIN_FILENO, ignored, sizeof(ignored));
            if (count <= 0)
                break;
            drained += (unsigned)count;
        }
        new_usb_session();
        s_usb_opener[0] = 0;
        quota_service_lock();
        quota_service_view_t *view = quota_service_view();
        s_usb_window_open = true;
        s_usb_opened_at_ms = (int64_t)quota_monotonic_ms();
        atomic_store(&s_usb_deadline, (uint64_t)s_usb_opened_at_ms + QUOTA_USB_WINDOW_MS);
        view->usb_window_preparing = false;
        view->usb_window_active = true;
        view->usb_window_seconds_left = QUOTA_USB_WINDOW_MS / 1000;
        quota_service_unlock();
        quota_service_post(QUOTA_APP_EVENT_USB_WINDOW);
    }
    if (!quota_usb_active()) {
        quota_usb_close_window();
        release_usb_decoder();
        atomic_store(&s_usb_io_busy, false);
        return;
    }
    if (s_usb_decoder && quota_monotonic_ms() - s_usb_partial_at >= 3000) {
        release_usb_decoder();
        atomic_store(&s_usb_io_busy, false);
    }
    for (unsigned i = 0; i < 512 && quota_usb_requested(); i++) {
        unsigned char input;
        if (read(STDIN_FILENO, &input, 1) != 1)
            break;
        if (!quota_usb_active()) {
            quota_usb_close_window();
            release_usb_decoder();
            atomic_store(&s_usb_io_busy, false);
            break;
        }
        if (!s_usb_decoder) {
            s_usb_decoder = calloc(1, sizeof(*s_usb_decoder));
            if (!s_usb_decoder)
                break;
            quota_frame_decoder_init(s_usb_decoder);
            s_usb_partial_at = quota_monotonic_ms();
        }
        atomic_store(&s_usb_io_busy, true);
        const char *frame = NULL;
        size_t length = 0;
        quota_frame_result_t result =
            quota_frame_decoder_feed(s_usb_decoder, (char)input, &frame, &length);
        if (result == QUOTA_FRAME_COMPLETE) {
            handle_serial_frame(frame, length);
            atomic_store(&s_usb_io_busy, false);
        } else if (result == QUOTA_FRAME_TOO_LONG) {
            release_usb_decoder();
            atomic_store(&s_usb_io_busy, false);
            send_usb_result(NULL, NULL, "frame_too_long", false);
        } else if (input == '\n') {
            release_usb_decoder();
            atomic_store(&s_usb_io_busy, false);
        }
    }
    if (!quota_usb_requested()) {
        release_usb_decoder();
        atomic_store(&s_usb_io_busy, false);
    }
}
