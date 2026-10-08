"""Exercise the USB v2 parser and real serial-frame dispatcher on the host."""

import unittest

from runtime_helpers import compile_and_run


class UsbRuntime(unittest.TestCase):
    def test_parser_sessions_state_and_controller_handoff(self):
        harness = r'''
#define _POSIX_C_SOURCE 200809L
#include "quota_usb.h"
#include "quota_portal.h"
#include "cJSON.h"
#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern atomic_bool s_usb_requested;
extern atomic_uint_fast64_t s_usb_deadline;
extern quota_frame_decoder_t *s_usb_decoder;
extern char s_usb_session[QUOTA_USB_SESSION_BYTES + 1];
extern char s_usb_opener[9];
void handle_serial_frame(const char *frame, size_t length);

static uint64_t now_ms = 1000;
static unsigned state_calls, command_calls;
static char last_command_id[9];
static quota_portable_submit_result_t next_submit_result = QUOTA_PORTABLE_SUBMIT_ACCEPTED;
static bool state_succeeds = true, expire_while_serializing;
static size_t state_length = QUOTA_PORTABLE_STATE_BYTES;
static quota_service_view_t service_view;

int64_t esp_timer_get_time(void)
{
    return (int64_t)now_ms * 1000;
}
uint32_t esp_random(void)
{
    static unsigned next = 5;
    next = next * 1103515245u + 12345u;
    return next >> 8;
}
void quota_service_lock(void) {}
void quota_service_unlock(void) {}
quota_service_view_t *quota_service_view(void)
{
    return &service_view;
}
void quota_service_post(quota_app_event_kind_t kind)
{
    (void)kind;
}
void quota_service_wake_network(void) {}
bool quota_portable_service_prepare_usb(void)
{
    return true;
}
quota_portable_submit_result_t
quota_portable_service_submit(const quota_portable_command_t *command,
                              quota_setup_transport_t transport)
{
    assert(command && transport == QUOTA_SETUP_USB);
    command_calls++;
    memcpy(last_command_id, command->request_id, sizeof(last_command_id));
    return next_submit_result;
}
bool quota_portable_service_state_json(char *buffer, size_t capacity, size_t *length,
                                       quota_setup_transport_t transport)
{
    assert(transport == QUOTA_SETUP_USB);
    state_calls++;
    if (!state_succeeds)
        return false;
    if (state_length > QUOTA_PORTABLE_STATE_BYTES) {
        *length = state_length;
        return true;
    }
    if (capacity < state_length + 1)
        return false;
    assert(state_length >= 11);
    memcpy(buffer, "{\"data\":\"", 9);
    memset(buffer + 9, 'x', state_length - 11);
    memcpy(buffer + state_length - 2, "\"}", 2);
    buffer[state_length] = '\0';
    *length = state_length;
    if (expire_while_serializing)
        now_ms = atomic_load(&s_usb_deadline);
    return true;
}

typedef struct {
    FILE *stream;
    int saved_stdout;
} capture_t;
static capture_t capture_start(void)
{
    capture_t capture = {.stream = tmpfile(), .saved_stdout = dup(STDOUT_FILENO)};
    assert(capture.stream && capture.saved_stdout >= 0);
    assert(dup2(fileno(capture.stream), STDOUT_FILENO) == STDOUT_FILENO);
    return capture;
}
static char *capture_finish(capture_t capture)
{
    assert(fflush(stdout) == 0);
    assert(fseek(capture.stream, 0, SEEK_END) == 0);
    long size = ftell(capture.stream);
    assert(size >= 0 && fseek(capture.stream, 0, SEEK_SET) == 0);
    char *output = malloc((size_t)size + 1);
    assert(output);
    assert(fread(output, 1, (size_t)size, capture.stream) == (size_t)size);
    output[size] = '\0';
    assert(dup2(capture.saved_stdout, STDOUT_FILENO) == STDOUT_FILENO);
    close(capture.saved_stdout);
    fclose(capture.stream);
    return output;
}
static char *dispatch(const char *frame)
{
    size_t length = strlen(frame);
    assert(length <= QUOTA_MAX_PROVISION_FRAME_BYTES);
    s_usb_decoder = calloc(1, sizeof(*s_usb_decoder));
    assert(s_usb_decoder);
    memcpy(s_usb_decoder->bytes, frame, length + 1);
    s_usb_decoder->length = length;
    capture_t capture = capture_start();
    handle_serial_frame(s_usb_decoder->bytes, length);
    assert(s_usb_decoder == NULL); /* Parser runs before its borrowed decoder is freed. */
    return capture_finish(capture);
}
static void active_session(void)
{
    atomic_store(&s_usb_requested, true);
    now_ms = 1000;
    atomic_store(&s_usb_deadline, now_ms + QUOTA_USB_WINDOW_MS);
    memcpy(s_usb_session, "0123456789abcdef0123456789abcdef", 33);
    s_usb_opener[0] = '\0';
}
static void assert_contains(const char *haystack, const char *needle)
{
    if (!strstr(haystack, needle)) {
        fprintf(stderr, "missing response text: %s; response starts: %.180s\n", needle, haystack);
        assert(false);
    }
}
static bool parse(const char *json, quota_usb_request_t *request, const char **error)
{
    char frame[QUOTA_MAX_PROVISION_FRAME_BYTES + 1];
    int length = snprintf(frame, sizeof(frame), "@AIQ:%s", json);
    assert(length > 0 && (size_t)length < sizeof(frame));
    return quota_usb_parse(frame, (size_t)length, request, error);
}
static void make_open(char frame[160], const char *request_id)
{
    snprintf(frame, 160, "@AIQ:{\"v\":2,\"op\":\"session_open\",\"request_id\":\"%s\"}",
             request_id);
}
static void make_state(char frame[256], const char *request_id, const char *session)
{
    snprintf(frame, 256,
             "@AIQ:{\"v\":2,\"op\":\"state_get\",\"request_id\":\"%s\",\"session_id\":\"%s\"}",
             request_id, session);
}
static void make_command(char frame[512], const char *request_id, const char *session,
                         const char *body)
{
    snprintf(
        frame, 512,
        "@AIQ:{\"v\":2,\"op\":\"command\",\"request_id\":\"%s\",\"session_id\":\"%s\",\"body\":%s}",
        request_id, session, body);
}

static void test_parser_bounds_and_shapes(void)
{
    quota_usb_request_t request;
    const char *error = NULL;
    assert(
        parse("{\"v\":2,\"op\":\"session_open\",\"request_id\":\"a1b2c3d4\"}", &request, &error));
    assert(request.op == QUOTA_USB_OPEN && !strcmp(request.request_id, "a1b2c3d4") &&
           error == NULL);
    assert(parse("{\"v\":2,\"op\":\"state_get\",\"request_id\":\"a1b2c3d4\",\"session_id\":"
                 "\"0123456789abcdef0123456789abcdef\"}",
                 &request, &error));
    assert(request.op == QUOTA_USB_STATE);
    assert(parse("{\"v\":2,\"op\":\"command\",\"request_id\":\"a1b2c3d4\",\"session_id\":"
                 "\"0123456789abcdef0123456789abcdef\",\"body\":{\"v\":1,\"op\":\"refresh\","
                 "\"request_id\":\"a1b2c3d4\"}}",
                 &request, &error));
    assert(request.op == QUOTA_USB_COMMAND && request.command.op == QUOTA_PORTABLE_OP_REFRESH);
    /* The version 1 frame and the operations of removed features are refused. */
    char old_frame[1024];
    int old_length =
        snprintf(old_frame, sizeof(old_frame),
                 "@AIQ:{\"v\":1,\"op\":\"configure\",\"request_id\":\"a1b2c3d4\","
                 "\"ssid\":\"Office\",\"password\":\"password\",\"server_time\":1790899200}");
    assert(old_length > 0 && (size_t)old_length < sizeof(old_frame));
    assert(!quota_usb_parse(old_frame, (size_t)old_length, &request, &error));
    assert(!strcmp(error, "unsupported_version") && !strcmp(request.request_id, "a1b2c3d4"));
    assert(!parse("{\"v\":2,\"op\":\"removed_operation\",\"request_id\":\"a1b2c3d4\",\"session_"
                  "id\":\"0123456789abcdef0123456789abcdef\",\"endpoint\":{}}",
                  &request, &error));
    assert(!strcmp(error, "unsupported_operation"));
    quota_portable_clear_secret(&request, sizeof(request));

    const char *invalid[] = {
        "{\"v\":2,\"op\":\"session_open\",\"request_id\":\"a1b2c3d4\",\"extra\":0}",
        "{\"v\":2,\"op\":\"session_open\",\"request_id\":\"a1b2c3d4\",\"request_id\":\"a1b2c3d4\"}",
        "{\"v\":\"2\",\"op\":\"session_open\",\"request_id\":\"a1b2c3d4\"}",
        "{\"v\":2,\"op\":\"session_open\",\"request_id\":\"A1b2c3d4\"}",
        "{\"v\":2,\"op\":\"mystery\",\"request_id\":\"a1b2c3d4\"}",
        "{\"v\":2,\"op\":\"command\",\"request_id\":\"a1b2c3d4\",\"session_id\":"
        "\"0123456789abcdef0123456789abcdef\",\"body\":{\"v\":1,\"op\":\"refresh\",\"request_id\":"
        "\"deadbeef\"}}",
        "{\"v\":2,\"op\":\"command\",\"request_id\":\"a1b2c3d4\",\"session_id\":"
        "\"0123456789abcdef0123456789abcdef\",\"body\":{\"v\":1,\"op\":\"refresh\",\"request_id\":"
        "\"a1b2c3d4\",\"unknown\":true}}",
        "{\"v\":2,\"op\":\"command\",\"request_id\":\"a1b2c3d4\",\"session_id\":"
        "\"0123456789abcdef0123456789abcdef\",\"body\":[]}",
        "{\"v\":2,\"op\":\"command\",\"request_id\":\"a1b2c3d4\",\"session_id\":"
        "\"0123456789abcdef0123456789abcdef\",\"body\":{\"v\":1,\"op\":\"refresh\",\"request_id\":"
        "\"a1b2c3d4\",\"extra\":{\"nested\":{}}}}",
        "{\"v\":2,\"op\":\"session_open\",\"request_id\":\"a1b2\\u0000c3d4\"}",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(!parse(invalid[i], &request, &error));
    }

    char with_nul[160];
    make_open(with_nul, "a1b2c3d4");
    size_t nul_length = strlen(with_nul);
    char *close_brace = strrchr(with_nul, '}');
    assert(close_brace);
    memmove(close_brace + 1, close_brace, strlen(close_brace) + 1);
    *close_brace = '\0';
    assert(!quota_usb_parse(with_nul, nul_length + 1, &request, &error));

    char too_long[QUOTA_MAX_PROVISION_FRAME_BYTES + 2];
    memset(too_long, 'x', sizeof(too_long));
    memcpy(too_long, "@AIQ:", 5);
    assert(!quota_usb_parse(too_long, sizeof(too_long), &request, &error));

    puts("USB v2 parser bounds and operation checks passed");
}

static void test_session_state_and_expiry(void)
{
    active_session();
    char frame[512];
    char *response;
    make_open(frame, "01020304");
    response = dispatch(frame);
    assert_contains(response, "\"ok\":true");
    assert_contains(response, "\"session_id\":\"0123456789abcdef0123456789abcdef\"");
    assert(!strcmp(s_usb_opener, "01020304"));
    free(response);

    make_open(frame, "01020304");
    response = dispatch(frame);
    assert_contains(response, "\"ok\":true");
    free(response);
    make_open(frame, "05060708");
    response = dispatch(frame);
    assert_contains(response, "\"error_code\":\"session_busy\"");
    free(response);

    make_state(frame, "11111111", "ffffffffffffffffffffffffffffffff");
    response = dispatch(frame);
    assert_contains(response, "\"error_code\":\"invalid_session\"");
    assert(state_calls == 0);
    free(response);

    state_succeeds = true;
    state_length = QUOTA_PORTABLE_STATE_BYTES;
    make_state(frame, "22222222", s_usb_session);
    response = dispatch(frame);
    size_t response_length = strlen(response);
    assert(response_length < QUOTA_USB_RESPONSE_BYTES);
    assert(response[response_length - 1] == '\n');
    assert(strchr(response, '\n') == response + response_length - 1); /* Exactly one state frame. */
    assert_contains(response, "\"op\":\"state\"");
    assert(response[response_length - 2] == '}' && response[response_length - 3] == '}');
    const char *json_end = NULL;
    cJSON *whole_state_frame =
        cJSON_ParseWithLengthOpts(response + 5, response_length - 6, &json_end, false);
    assert(whole_state_frame && json_end == response + response_length - 1);
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(whole_state_frame, "state");
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(state, "data");
    assert(cJSON_IsString(payload) &&
           strlen(payload->valuestring) == QUOTA_PORTABLE_STATE_BYTES - 11);
    cJSON_Delete(whole_state_frame);
    free(response);

    state_length = QUOTA_PORTABLE_STATE_BYTES + 1;
    make_state(frame, "33333333", s_usb_session);
    response = dispatch(frame);
    assert_contains(response, "\"error_code\":\"state_unavailable\"");
    assert(!strstr(response, "\"op\":\"state\""));
    free(response);

    state_length = QUOTA_PORTABLE_STATE_BYTES;
    expire_while_serializing = true;
    make_state(frame, "44444444", s_usb_session);
    response = dispatch(frame);
    assert_contains(response, "\"error_code\":\"session_expired\"");
    assert(!strstr(response, "\"op\":\"state\""));
    free(response);
    expire_while_serializing = false;

    now_ms = atomic_load(&s_usb_deadline);
    make_state(frame, "55555555", s_usb_session);
    response = dispatch(frame);
    assert_contains(response, "\"error_code\":\"session_expired\"");
    assert(state_calls == 3);
    free(response);
    puts("USB session pinning, state bounds and expiry checks passed");
}

static void test_opener_idle_takeover(void)
{
    active_session();
    char frame[512];
    char *response;
    make_open(frame, "01020304");
    response = dispatch(frame);
    assert_contains(response, "\"ok\":true");
    free(response);
    /* Authorized traffic keeps the opener alive; silence past the idle limit frees it for a
     * reloaded page. */
    now_ms += QUOTA_USB_OPENER_IDLE_MS - 1;
    make_state(frame, "10101010", s_usb_session);
    response = dispatch(frame);
    free(response);
    now_ms += QUOTA_USB_OPENER_IDLE_MS - 1;
    make_open(frame, "05060708");
    response = dispatch(frame);
    assert_contains(response, "\"error_code\":\"session_busy\"");
    free(response);
    char old_session[QUOTA_USB_SESSION_BYTES + 1];
    memcpy(old_session, s_usb_session, sizeof(old_session));
    now_ms += 1;
    make_open(frame, "05060708");
    response = dispatch(frame);
    assert_contains(response, "\"ok\":true");
    assert(!strcmp(s_usb_opener, "05060708"));
    /* The takeover rotates the session: the new page gets the new id, the old page is rejected. */
    assert(strlen(s_usb_session) == QUOTA_USB_SESSION_BYTES &&
           strcmp(s_usb_session, old_session) != 0);
    assert_contains(response, s_usb_session);
    free(response);
    make_state(frame, "20202020", old_session);
    response = dispatch(frame);
    assert_contains(response, "\"error_code\":\"invalid_session\"");
    free(response);
    make_state(frame, "30303030", s_usb_session);
    response = dispatch(frame);
    assert_contains(response, "\"op\":\"state\"");
    free(response);
    char new_session[QUOTA_USB_SESSION_BYTES + 1];
    memcpy(new_session, s_usb_session, sizeof(new_session));
    make_open(frame, "05060708");
    response = dispatch(frame); /* The same opener re-opening keeps its session. */
    assert_contains(response, new_session);
    free(response);
    make_open(frame, "01020304");
    response = dispatch(frame);
    assert_contains(response, "\"error_code\":\"session_busy\"");
    free(response);
    puts("USB opener idle takeover checks passed");
}

static void test_command_dedup_delegation_and_version_gate(void)
{
    active_session();
    strcpy(s_usb_opener, "01020304");
    char frame[512];
    char *response;
    const char *body = "{\"v\":1,\"op\":\"refresh\",\"request_id\":\"aabbccdd\"}";
    make_command(frame, "aabbccdd", s_usb_session, body);
    response = dispatch(frame);
    assert_contains(response, "\"accepted\":true");
    free(response);
    /* Retries reach the controller with the same ID; its persisted receipt owns deduplication. */
    response = dispatch(frame);
    assert_contains(response, "\"accepted\":true");
    free(response);
    assert(command_calls == 2 && !strcmp(last_command_id, "aabbccdd"));

    next_submit_result = QUOTA_PORTABLE_SUBMIT_CONFLICT;
    body = "{\"v\":1,\"op\":\"reconnect\",\"request_id\":\"aabbccdd\"}";
    make_command(frame, "aabbccdd", s_usb_session, body);
    response = dispatch(frame);
    assert_contains(response, "\"error_code\":\"request_conflict\"");
    free(response);
    assert(command_calls == 3 && !strcmp(last_command_id, "aabbccdd"));

    next_submit_result = QUOTA_PORTABLE_SUBMIT_ACCEPTED;
    /* A version 1 frame from an old settings page gets an explicit version error. */
    char old_frame[512];
    snprintf(old_frame, sizeof(old_frame),
             "@AIQ:{\"v\":1,\"op\":\"configure\",\"request_id\":\"ccddeeff\",\"ssid\":\"Office\","
             "\"password\":\"secret-password\"}");
    response = dispatch(old_frame);
    assert_contains(response, "\"request_id\":\"ccddeeff\"");
    assert_contains(response, "\"error_code\":\"unsupported_version\"");
    assert(!strstr(response, "secret-password") && command_calls == 3);
    free(response);
    puts("USB command handoff, request receipt delegation and version checks passed");
}

int main(void)
{
    test_parser_bounds_and_shapes();
    active_session();
    test_session_state_and_expiry();
    test_opener_idle_takeover();
    test_command_dedup_delegation_and_version_gate();
    puts("USB runtime tests passed");
}
'''
        compile_and_run(
            harness,
            "ai-quota-usb-runtime-",
            (
                "main/quota_logic.c",
                "main/quota_usb.c",
                "main/quota_portal.c",
                "main/quota_json.c",
                "tests/host_sdk/host_sdk_embedded.c",
                "tests/cjson/cJSON.c",
            ),
            (
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                "-Wno-deprecated-declarations",
            ),
            host_sdk=True,
        )

    def test_window_lifecycle_partial_frames_and_sleep(self):
        harness = r"""
#define _POSIX_C_SOURCE 200809L
#include "quota_usb.h"
#include <assert.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern atomic_bool s_usb_requested, s_usb_io_busy;
extern bool s_usb_window_open;
extern int64_t s_usb_opened_at_ms;
extern quota_frame_decoder_t *s_usb_decoder;
extern char s_usb_session[QUOTA_USB_SESSION_BYTES + 1];

static uint64_t now_ms = 1000;
static quota_service_view_t service_view;
static unsigned notifications, events, prepare_calls, random_value;
static FILE *output;

int64_t esp_timer_get_time(void)
{
    return (int64_t)now_ms * 1000;
}
uint32_t esp_random(void)
{
    unsigned value = random_value++;
    return value + value / 16;
}
void quota_service_lock(void) {}
void quota_service_unlock(void) {}
quota_service_view_t *quota_service_view(void)
{
    return &service_view;
}
void quota_service_post(quota_app_event_kind_t kind)
{
    assert(kind == QUOTA_APP_EVENT_USB_WINDOW);
    events++;
}
void quota_service_wake_network(void)
{
    notifications++;
}
bool quota_portable_service_prepare_usb(void)
{
    prepare_calls++;
    return true;
}
quota_portable_submit_result_t
quota_portable_service_submit(const quota_portable_command_t *command,
                              quota_setup_transport_t transport)
{
    (void)command;
    (void)transport;
    assert(false);
    return QUOTA_PORTABLE_SUBMIT_INVALID;
}
bool quota_portable_service_state_json(char *buffer, size_t capacity, size_t *length,
                                       quota_setup_transport_t transport)
{
    (void)buffer;
    (void)capacity;
    (void)length;
    (void)transport;
    assert(false);
    return false;
}

/* Every serial line the owner handles answers once on stdout; count answers of one kind. */
static unsigned answers(const char *needle)
{
    fflush(stdout);
    fseek(output, 0, SEEK_END);
    long size = ftell(output);
    char *text = calloc((size_t)size + 1, 1);
    assert(text && fseek(output, 0, SEEK_SET) == 0);
    assert(fread(text, 1, (size_t)size, output) == (size_t)size);
    unsigned count = 0;
    for (const char *at = text; (at = strstr(at, needle)); at += strlen(needle))
        count++;
    free(text);
    return count;
}
static int input_fd = -1;
static void set_input(const char *bytes)
{
    if (input_fd < 0) {
        int descriptors[2];
        assert(pipe(descriptors) == 0);
        int flags = fcntl(descriptors[0], F_GETFL, 0);
        assert(flags >= 0 && fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK) == 0);
        assert(dup2(descriptors[0], STDIN_FILENO) == STDIN_FILENO);
        close(descriptors[0]);
        input_fd = descriptors[1];
    }
    if (bytes && bytes[0])
        assert(write(input_fd, bytes, strlen(bytes)) == (ssize_t)strlen(bytes));
}
#define FRAMES() answers("invalid_frame")

int main(void)
{
    output = tmpfile();
    assert(output && dup2(fileno(output), STDOUT_FILENO) == STDOUT_FILENO);
    set_input(NULL);
    set_input("stale input\n"); /* Bytes received before the physical window are discarded. */
    quota_usb_open_window();
    assert(atomic_load(&s_usb_requested) && service_view.usb_window_preparing &&
           !service_view.usb_window_active);
    assert(!s_usb_decoder && quota_usb_blocked());
    quota_usb_poll(false);
    assert(service_view.usb_window_active && !service_view.usb_window_preparing);
    assert(service_view.usb_window_seconds_left == QUOTA_USB_WINDOW_MS / 1000);
    assert(!s_usb_decoder && !quota_usb_blocked() && prepare_calls == 1);
    assert(s_usb_session[0] && FRAMES() == 0); /* stale bytes were drained on entry */
    char first_session[QUOTA_USB_SESSION_BYTES + 1];
    memcpy(first_session, s_usb_session, sizeof(first_session));

    set_input("whole-");
    quota_usb_poll(false);
    assert(s_usb_decoder->length == strlen("whole-") && FRAMES() == 0);
    assert(atomic_load(&s_usb_io_busy) && quota_usb_blocked());
    now_ms += 2999;
    quota_usb_poll(false);
    assert(s_usb_decoder && quota_usb_blocked());
    now_ms += 1;
    quota_usb_poll(false);
    assert(!s_usb_decoder && !atomic_load(&s_usb_io_busy) && !quota_usb_blocked());

    set_input("whole-frame\r\n");
    quota_usb_poll(false);
    assert(FRAMES() == 1 && !s_usb_decoder && !quota_usb_blocked());

    char *overlong = malloc(QUOTA_MAX_PROVISION_FRAME_BYTES + 2);
    assert(overlong);
    memset(overlong, 'x', QUOTA_MAX_PROVISION_FRAME_BYTES + 1);
    overlong[QUOTA_MAX_PROVISION_FRAME_BYTES + 1] = '\n';
    assert(write(input_fd, overlong, QUOTA_MAX_PROVISION_FRAME_BYTES + 2) ==
           QUOTA_MAX_PROVISION_FRAME_BYTES + 2);
    free(overlong);
    for (unsigned i = 0; i < 10 && answers("frame_too_long") == 0; ++i)
        quota_usb_poll(false);
    assert(answers("frame_too_long") == 1);
    assert(!s_usb_decoder && !atomic_load(&s_usb_io_busy) && !quota_usb_blocked());
    set_input("whole-frame\n");
    quota_usb_poll(false);
    assert(FRAMES() == 2 &&
           !quota_usb_blocked()); /* A rejected line does not poison the next frame. */

    set_input("stale-");
    quota_usb_poll(false);
    assert(s_usb_decoder->length == strlen("stale-") && FRAMES() == 2 && quota_usb_blocked());
    quota_usb_poll(true); /* Sleep immediately closes and clears the partial USB session. */
    assert(!atomic_load(&s_usb_requested) && !service_view.usb_window_active &&
           !service_view.usb_window_preparing);
    assert(!s_usb_decoder && !s_usb_session[0] && !atomic_load(&s_usb_io_busy));

    now_ms += 1000;
    quota_usb_open_window();
    quota_usb_poll(false);
    assert(!s_usb_decoder && service_view.usb_window_active);
    assert(strcmp(first_session, s_usb_session) !=
           0); /* Each physical reentry receives a new nonce. */
    quota_usb_poll(false);
    assert(!s_usb_decoder && !quota_usb_blocked()); /* No stale prefix crossed sleep. */
    set_input("whole-frame\n");
    quota_usb_poll(false);
    assert(FRAMES() == 3);

    set_input("expiry-");
    quota_usb_poll(false);
    assert(s_usb_decoder && quota_usb_blocked());
    now_ms = (uint64_t)s_usb_opened_at_ms + QUOTA_USB_WINDOW_MS;
    quota_usb_poll(false);
    assert(!atomic_load(&s_usb_requested) && !service_view.usb_window_active);
    assert(!s_usb_decoder && !atomic_load(&s_usb_io_busy) && !quota_usb_blocked());
    quota_usb_poll(false);
    assert(!s_usb_decoder && !s_usb_session[0] && !quota_usb_blocked());

    quota_usb_open_window();
    quota_usb_poll(false);
    assert(service_view.usb_window_active && !s_usb_decoder);
    set_input("manual-");
    quota_usb_poll(false);
    assert(s_usb_decoder && quota_usb_blocked());
    quota_usb_close_window();
    assert(quota_usb_blocked()); /* External close keeps HTTP gated until the owner releases partial
                                    bytes. */
    quota_usb_poll(false);       /* Manual exit revokes the session and clears the decoder. */
    assert(!s_usb_session[0] && !service_view.usb_window_active && !quota_usb_blocked());

    quota_usb_open_window();
    quota_usb_poll(false);
    assert(service_view.usb_window_active);
    quota_usb_poll(true); /* Sleeping also closes an idle physical window immediately. */
    assert(!s_usb_session[0] && !service_view.usb_window_active && !quota_usb_blocked());
    assert(prepare_calls == 4 && notifications >= 6 && events >= 4);
    close(input_fd);
    puts("USB session lifecycle, partial and overlong frames passed");
}
"""
        compile_and_run(
            harness,
            "ai-quota-usb-window-",
            (
                "main/quota_logic.c",
                "main/quota_usb.c",
                "main/quota_portal.c",
                "main/quota_json.c",
                "tests/host_sdk/host_sdk_embedded.c",
                "tests/cjson/cJSON.c",
            ),
            ("-Wno-deprecated-declarations",),
            host_sdk=True,
        )


if __name__ == "__main__":
    unittest.main()
