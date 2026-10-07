"""Exercise the USB v2 parser and real serial-frame dispatcher on the host."""
import unittest

from runtime_helpers import ROOT, compile_and_run, extract_function


class UsbRuntime(unittest.TestCase):
    def test_parser_sessions_state_and_controller_handoff(self):
        portal = (ROOT / "main/quota_portal.c").read_text()
        portal_parser = "\n".join(
            extract_function(portal, name, declaration)
            for name, declaration in (
                ("unique_keys", "static bool"),
                ("embedded_nul", "static bool"),
                ("structure_bounded", "static bool"),
                ("quota_portal_clear_json", "void"),
                ("copy_text", "static bool"),
                ("unsigned_field", "static bool"),
                ("bool_field", "static bool"),
                ("keys_allowed", "static bool"),
                ("hex_request_id", "static bool"),
                ("network_password_valid", "static bool"),
                ("quota_portal_parse_command", "bool"),
            )
        )
        service = (ROOT / "main/quota_service.c").read_text()
        service_functions = "\n".join(
            (
                extract_function(service, "pairing_requested"),
                next(line for line in service.splitlines() if line.startswith("static uint64_t usb_deadline_ms(")),
                next(line for line in service.splitlines() if line.startswith("static bool usb_active(")),
                extract_function(service, "release_usb_decoder"),
                extract_function(service, "usb_authorized"),
                extract_function(service, "send_usb_result"),
                extract_function(service, "send_usb_state"),
                extract_function(service, "new_usb_session"),
                extract_function(service, "handle_serial_frame"),
            )
        )
        harness = r'''
#define _POSIX_C_SOURCE 200809L
#define ESP_PLATFORM 1
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

enum { QUOTA_APP_EVENT_CONFIGURATION_RESULT };
typedef struct { unsigned kind; bool success; } quota_app_event_t;
static atomic_bool s_pairing_requested;
static atomic_uint_fast64_t s_usb_deadline;
static quota_frame_decoder_t *s_usb_decoder;
static uint64_t now_ms = 1000;
static char s_usb_session[QUOTA_USB_SESSION_BYTES + 1];
static char s_usb_opener[9];
static unsigned state_calls, command_calls, collector_calls, legacy_calls, posted_events;
static char last_command_id[9], last_collector_id[9];
static quota_portable_submit_result_t next_submit_result = QUOTA_PORTABLE_SUBMIT_ACCEPTED;
static bool state_succeeds = true, expire_while_serializing;
static size_t state_length = QUOTA_PORTABLE_STATE_BYTES;
static uint64_t s_usb_partial_at, s_usb_opener_at;
static void handle_serial_frame(const char *frame, size_t length);

static uint64_t monotonic_ms(void) { return now_ms; }
static unsigned esp_random(void) { static unsigned next = 5; next = next * 1103515245u + 12345u; return next >> 8; }
static void post_event(const quota_app_event_t *event, int wait)
{
    (void)wait; assert(event && event->kind == QUOTA_APP_EVENT_CONFIGURATION_RESULT); posted_events++;
}
static void send_pairing_result(const char *id, bool ok, const char *error)
{ (void)id; (void)ok; (void)error; assert(false); }
static void set_system_time_if_newer(uint64_t time) { (void)time; }
static void quota_service_close_pairing_window(void)
{
    atomic_store(&s_pairing_requested, false);
    atomic_store(&s_usb_deadline, 0);
}
static quota_portable_submit_result_t quota_portable_service_submit(
    const quota_portable_command_t *command, quota_setup_transport_t transport)
{
    assert(command && transport == QUOTA_SETUP_USB);
    command_calls++;
    memcpy(last_command_id, command->request_id, sizeof(last_command_id));
    return next_submit_result;
}
static quota_portable_submit_result_t quota_portable_service_submit_collector(
    const quota_legacy_endpoint_t *endpoint, const char request_id[9])
{
    assert(endpoint && endpoint->enabled);
    collector_calls++;
    memcpy(last_collector_id, request_id, sizeof(last_collector_id));
    return next_submit_result;
}
static bool quota_portable_service_configure_legacy(
    const quota_device_config_t *config, const char **error)
{ (void)config; (void)error; legacy_calls++; return true; }
static bool quota_portable_service_state_json(char *buffer, size_t capacity,
                                               size_t *length, quota_setup_transport_t transport)
{
    assert(transport == QUOTA_SETUP_USB);
    state_calls++;
    if (!state_succeeds) return false;
    if (state_length > QUOTA_PORTABLE_STATE_BYTES) { *length = state_length; return true; }
    if (capacity < state_length + 1) return false;
    assert(state_length >= 11);
    memcpy(buffer, "{\"data\":\"", 9);
    memset(buffer + 9, 'x', state_length - 11);
    memcpy(buffer + state_length - 2, "\"}", 2);
    buffer[state_length] = '\0';
    *length = state_length;
    if (expire_while_serializing) now_ms = atomic_load(&s_usb_deadline);
    return true;
}

typedef struct { FILE *stream; int saved_stdout; } capture_t;
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
    atomic_store(&s_pairing_requested, true);
    now_ms = 1000;
    atomic_store(&s_usb_deadline, now_ms + QUOTA_PAIRING_WINDOW_MS);
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
    snprintf(frame, 160, "@AIQ:{\"v\":2,\"op\":\"session_open\",\"request_id\":\"%s\"}", request_id);
}
static void make_state(char frame[256], const char *request_id, const char *session)
{
    snprintf(frame, 256, "@AIQ:{\"v\":2,\"op\":\"state_get\",\"request_id\":\"%s\",\"session_id\":\"%s\"}", request_id, session);
}
static void make_command(char frame[512], const char *request_id, const char *session,
                         const char *body)
{
    snprintf(frame, 512, "@AIQ:{\"v\":2,\"op\":\"command\",\"request_id\":\"%s\",\"session_id\":\"%s\",\"body\":%s}",
             request_id, session, body);
}

''' + portal_parser + "\n" + service_functions + r'''
static void test_parser_bounds_and_shapes(void)
{
    quota_usb_request_t request;
    const char *error = NULL;
    assert(parse("{\"v\":2,\"op\":\"session_open\",\"request_id\":\"a1b2c3d4\"}", &request, &error));
    assert(request.op == QUOTA_USB_OPEN && !strcmp(request.request_id, "a1b2c3d4") && error == NULL);
    assert(parse("{\"v\":2,\"op\":\"state_get\",\"request_id\":\"a1b2c3d4\",\"session_id\":\"0123456789abcdef0123456789abcdef\"}", &request, &error));
    assert(request.op == QUOTA_USB_STATE);
    assert(parse("{\"v\":2,\"op\":\"command\",\"request_id\":\"a1b2c3d4\",\"session_id\":\"0123456789abcdef0123456789abcdef\",\"body\":{\"v\":1,\"op\":\"refresh\",\"request_id\":\"a1b2c3d4\"}}", &request, &error));
    assert(request.op == QUOTA_USB_COMMAND && request.body.command.op == QUOTA_PORTABLE_OP_REFRESH);
    static const char legacy_token[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq";
    char legacy_frame[1024];
    int legacy_length = snprintf(legacy_frame, sizeof(legacy_frame),
        "@AIQ:{\"v\":1,\"op\":\"configure\",\"request_id\":\"a1b2c3d4\","
        "\"ssid\":\"Office\",\"password\":\"p\\\"ass\\\\word\","
        "\"base_url\":\"https://192.168.1.20:4318\",\"pair_token\":\"%s\","
        "\"server_cert_pem\":\"-----BEGIN CERTIFICATE-----\\nabc\\n-----END CERTIFICATE-----\\n\","
        "\"server_time\":1790899200}", legacy_token);
    assert(legacy_length > 0 && (size_t)legacy_length < sizeof(legacy_frame));
    assert(quota_usb_parse(legacy_frame, (size_t)legacy_length, &request, &error));
    assert(request.op == QUOTA_USB_LEGACY && !strcmp(request.request_id, "a1b2c3d4"));
    assert(!strcmp(request.body.legacy.password, "p\"ass\\word"));
    quota_portable_clear_secret(&request, sizeof(request));

    const char *invalid[] = {
        "{\"v\":2,\"op\":\"session_open\",\"request_id\":\"a1b2c3d4\",\"extra\":0}",
        "{\"v\":2,\"op\":\"session_open\",\"request_id\":\"a1b2c3d4\",\"request_id\":\"a1b2c3d4\"}",
        "{\"v\":\"2\",\"op\":\"session_open\",\"request_id\":\"a1b2c3d4\"}",
        "{\"v\":2,\"op\":\"session_open\",\"request_id\":\"A1b2c3d4\"}",
        "{\"v\":2,\"op\":\"mystery\",\"request_id\":\"a1b2c3d4\"}",
        "{\"v\":2,\"op\":\"command\",\"request_id\":\"a1b2c3d4\",\"session_id\":\"0123456789abcdef0123456789abcdef\",\"body\":{\"v\":1,\"op\":\"refresh\",\"request_id\":\"deadbeef\"}}",
        "{\"v\":2,\"op\":\"command\",\"request_id\":\"a1b2c3d4\",\"session_id\":\"0123456789abcdef0123456789abcdef\",\"body\":{\"v\":1,\"op\":\"refresh\",\"request_id\":\"a1b2c3d4\",\"unknown\":true}}",
        "{\"v\":2,\"op\":\"command\",\"request_id\":\"a1b2c3d4\",\"session_id\":\"0123456789abcdef0123456789abcdef\",\"body\":[]}",
        "{\"v\":2,\"op\":\"command\",\"request_id\":\"a1b2c3d4\",\"session_id\":\"0123456789abcdef0123456789abcdef\",\"body\":{\"v\":1,\"op\":\"refresh\",\"request_id\":\"a1b2c3d4\",\"extra\":{\"nested\":{}}}}",
        "{\"v\":2,\"op\":\"session_open\",\"request_id\":\"a1b2\\u0000c3d4\"}",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(!parse(invalid[i], &request, &error));
    }

    char with_nul[160];
    make_open(with_nul, "a1b2c3d4");
    size_t nul_length = strlen(with_nul);
    char *close_brace = strrchr(with_nul, '}'); assert(close_brace);
    memmove(close_brace + 1, close_brace, strlen(close_brace) + 1);
    *close_brace = '\0';
    assert(!quota_usb_parse(with_nul, nul_length + 1, &request, &error));

    char too_long[QUOTA_MAX_PROVISION_FRAME_BYTES + 2];
    memset(too_long, 'x', sizeof(too_long)); memcpy(too_long, "@AIQ:", 5);
    assert(!quota_usb_parse(too_long, sizeof(too_long), &request, &error));

    char private_endpoint[1024];
    char valid_token[QUOTA_PAIR_TOKEN_BYTES + 1];
    memset(valid_token, 'A', QUOTA_PAIR_TOKEN_BYTES); valid_token[QUOTA_PAIR_TOKEN_BYTES] = '\0';
    snprintf(private_endpoint, sizeof(private_endpoint),
        "{\"v\":2,\"op\":\"collector_configure\",\"request_id\":\"a1b2c3d4\",\"session_id\":\"0123456789abcdef0123456789abcdef\",\"endpoint\":{\"base_url\":\"https://192.168.4.2:4318\",\"pair_token\":\"%s\",\"server_cert_pem\":\"-----BEGIN CERTIFICATE-----\\nabc\\n-----END CERTIFICATE-----\",\"server_time\":1800000000}}",
        valid_token);
    assert(parse(private_endpoint, &request, &error) && request.op == QUOTA_USB_COLLECTOR);
    char *host = strstr(private_endpoint, "192.168.4.2"); assert(host); memcpy(host, "203.0.113.2", 11);
    assert(!parse(private_endpoint, &request, &error));
    puts("USB v2 parser bounds and operation checks passed");
}

static void test_session_state_and_expiry(void)
{
    active_session();
    char frame[512]; char *response;
    make_open(frame, "01020304");
    response = dispatch(frame);
    assert_contains(response, "\"ok\":true");
    assert_contains(response, "\"session_id\":\"0123456789abcdef0123456789abcdef\"");
    assert(!strcmp(s_usb_opener, "01020304")); free(response);

    make_open(frame, "01020304"); response = dispatch(frame);
    assert_contains(response, "\"ok\":true"); free(response);
    make_open(frame, "05060708"); response = dispatch(frame);
    assert_contains(response, "\"error_code\":\"session_busy\""); free(response);

    make_state(frame, "11111111", "ffffffffffffffffffffffffffffffff");
    response = dispatch(frame);
    assert_contains(response, "\"error_code\":\"invalid_session\"");
    assert(state_calls == 0); free(response);

    state_succeeds = true; state_length = QUOTA_PORTABLE_STATE_BYTES;
    make_state(frame, "22222222", s_usb_session); response = dispatch(frame);
    size_t response_length = strlen(response);
    assert(response_length < QUOTA_USB_RESPONSE_BYTES);
    assert(response[response_length - 1] == '\n');
    assert(strchr(response, '\n') == response + response_length - 1); /* Exactly one state frame. */
    assert_contains(response, "\"op\":\"state\"");
    assert(response[response_length - 2] == '}' && response[response_length - 3] == '}');
    assert(!strstr(response, "-----BEGIN CERTIFICATE-----"));
    assert(!strstr(response, "pair_token"));
    const char *json_end = NULL;
    cJSON *whole_state_frame = cJSON_ParseWithLengthOpts(response + 5, response_length - 6,
                                                          &json_end, false);
    assert(whole_state_frame && json_end == response + response_length - 1);
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(whole_state_frame, "state");
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(state, "data");
    assert(cJSON_IsString(payload) && strlen(payload->valuestring) == QUOTA_PORTABLE_STATE_BYTES - 11);
    cJSON_Delete(whole_state_frame);
    free(response);

    state_length = QUOTA_PORTABLE_STATE_BYTES + 1;
    make_state(frame, "33333333", s_usb_session); response = dispatch(frame);
    assert_contains(response, "\"error_code\":\"state_unavailable\"");
    assert(!strstr(response, "\"op\":\"state\"")); free(response);

    state_length = QUOTA_PORTABLE_STATE_BYTES; expire_while_serializing = true;
    make_state(frame, "44444444", s_usb_session); response = dispatch(frame);
    assert_contains(response, "\"error_code\":\"session_expired\"");
    assert(!strstr(response, "\"op\":\"state\"")); free(response);
    expire_while_serializing = false;

    now_ms = atomic_load(&s_usb_deadline);
    make_state(frame, "55555555", s_usb_session); response = dispatch(frame);
    assert_contains(response, "\"error_code\":\"session_expired\"");
    assert(state_calls == 3); free(response);
    puts("USB session pinning, state bounds and expiry checks passed");
}

static void test_opener_idle_takeover(void)
{
    active_session();
    char frame[512]; char *response;
    make_open(frame, "01020304"); response = dispatch(frame); assert_contains(response, "\"ok\":true"); free(response);
    /* Authorized traffic keeps the opener alive; silence past the idle limit frees it for a reloaded page. */
    now_ms += QUOTA_USB_OPENER_IDLE_MS - 1; make_state(frame, "10101010", s_usb_session); response = dispatch(frame); free(response);
    now_ms += QUOTA_USB_OPENER_IDLE_MS - 1; make_open(frame, "05060708"); response = dispatch(frame);
    assert_contains(response, "\"error_code\":\"session_busy\""); free(response);
    char old_session[QUOTA_USB_SESSION_BYTES + 1]; memcpy(old_session, s_usb_session, sizeof(old_session));
    now_ms += 1; make_open(frame, "05060708"); response = dispatch(frame);
    assert_contains(response, "\"ok\":true"); assert(!strcmp(s_usb_opener, "05060708"));
    /* The takeover rotates the session: the new page gets the new id, the old page is rejected. */
    assert(strlen(s_usb_session) == QUOTA_USB_SESSION_BYTES && strcmp(s_usb_session, old_session) != 0);
    assert_contains(response, s_usb_session); free(response);
    make_state(frame, "20202020", old_session); response = dispatch(frame);
    assert_contains(response, "\"error_code\":\"invalid_session\""); free(response);
    make_state(frame, "30303030", s_usb_session); response = dispatch(frame);
    assert_contains(response, "\"op\":\"state\""); free(response);
    char new_session[QUOTA_USB_SESSION_BYTES + 1]; memcpy(new_session, s_usb_session, sizeof(new_session));
    make_open(frame, "05060708"); response = dispatch(frame); /* The same opener re-opening keeps its session. */
    assert_contains(response, new_session); free(response);
    make_open(frame, "01020304"); response = dispatch(frame);
    assert_contains(response, "\"error_code\":\"session_busy\""); free(response);
    puts("USB opener idle takeover checks passed");
}

static void test_command_dedup_delegation_and_collector_validation(void)
{
    active_session(); strcpy(s_usb_opener, "01020304");
    char frame[512]; char *response;
    const char *body = "{\"v\":1,\"op\":\"refresh\",\"request_id\":\"aabbccdd\"}";
    make_command(frame, "aabbccdd", s_usb_session, body);
    response = dispatch(frame); assert_contains(response, "\"accepted\":true"); free(response);
    /* Retries reach the controller with the same ID; its persisted receipt owns deduplication. */
    response = dispatch(frame); assert_contains(response, "\"accepted\":true"); free(response);
    assert(command_calls == 2 && !strcmp(last_command_id, "aabbccdd"));

    next_submit_result = QUOTA_PORTABLE_SUBMIT_CONFLICT;
    body = "{\"v\":1,\"op\":\"reconnect\",\"request_id\":\"aabbccdd\"}";
    make_command(frame, "aabbccdd", s_usb_session, body);
    response = dispatch(frame); assert_contains(response, "\"error_code\":\"request_conflict\""); free(response);
    assert(command_calls == 3 && !strcmp(last_command_id, "aabbccdd"));

    next_submit_result = QUOTA_PORTABLE_SUBMIT_ACCEPTED;
    char endpoint[1024];
    snprintf(endpoint, sizeof(endpoint),
        "@AIQ:{\"v\":2,\"op\":\"collector_configure\",\"request_id\":\"ccddeeff\",\"session_id\":\"%s\",\"endpoint\":{\"base_url\":\"https://192.168.4.2:4318\",\"pair_token\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\",\"server_cert_pem\":\"-----BEGIN CERTIFICATE-----\\nsecret-cert\\n-----END CERTIFICATE-----\",\"server_time\":1800000000}}",
        s_usb_session);
    response = dispatch(endpoint);
    assert_contains(response, "\"accepted\":true");
    assert(collector_calls == 1 && !strcmp(last_collector_id, "ccddeeff")); free(response);

    char *public_url = strstr(endpoint, "192.168.4.2"); assert(public_url);
    memcpy(public_url, "203.0.113.2", 11);
    response = dispatch(endpoint);
    assert_contains(response, "\"error_code\":\"invalid_frame\"");
    assert(collector_calls == 1);
    assert(!strstr(response, "AAAAAAAAAAAAAAAA"));
    assert(!strstr(response, "secret-cert")); free(response);
    puts("USB command handoff, request receipt delegation and endpoint checks passed");
}

int main(void)
{
    test_parser_bounds_and_shapes();
    active_session();
    test_session_state_and_expiry();
    test_opener_idle_takeover();
    test_command_dedup_delegation_and_collector_validation();
    assert(legacy_calls == 0 && posted_events == 0);
    puts("USB runtime tests passed");
}
'''
        compile_and_run(
            harness,
            "ai-quota-usb-runtime-",
            ("main/quota_logic.c", "main/quota_usb.c", "tests/cjson/cJSON.c"),
            ("-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-Wno-deprecated-declarations"),
        )


if __name__ == "__main__":
    unittest.main()
