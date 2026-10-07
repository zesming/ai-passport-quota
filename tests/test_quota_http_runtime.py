"""Exercise actual HTTP admission and snapshot commits with fake dependencies."""
import re
import unittest
from runtime_helpers import ROOT, extract_function, compile_and_run


class HttpRuntime(unittest.TestCase):
    def test_actual_transport_admission_and_cleanup(self):
        source = (ROOT / "main/quota_service.c").read_text()
        definitions = []
        for kind, name in (("struct", "http_body_t"), ("struct", "display_scheduler_t"),
                           ("enum", "http_request_outcome_t")):
            match = re.search(r"typedef " + kind + r" \{[^{}]*\} " + name + r";", source)
            self.assertIsNotNone(match, name)
            definitions.append(match[0])
        functions = extract_function(source, "pairing_requested") + "\n"
        functions += "\n".join(line for line in source.splitlines() if line.startswith((
            "static uint64_t usb_deadline_ms(", "static bool usb_blocked("))) + "\n"
        functions += "\n".join(extract_function(source, name) for name in (
            "display_generation_is_current", "network_operation_is_current", "http_request"))
        harness = r'''
#include "quota_portable.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#define HTTP_BODY_BYTES 8192
#define HTTP_TIMEOUT_MS 8000
'''
        harness += "\n".join(definitions)
        harness += r'''
typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL, ESP_ERR_INVALID_SIZE, ESP_ERR_INVALID_STATE };
typedef enum { HTTP_METHOD_GET, HTTP_METHOD_POST, HTTP_METHOD_PATCH } esp_http_client_method_t;
enum { HTTP_TRANSPORT_OVER_SSL };
typedef struct { int unused; } esp_http_client_event_t;
typedef struct {
    const char *url, *cert_pem;
    esp_http_client_method_t method;
    int timeout_ms, max_authorization_retries, transport_type, buffer_size, buffer_size_tx;
    bool disable_auto_redirect, skip_cert_common_name_check;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
} esp_http_client_config_t;
typedef struct { bool alive; } fake_client_t;
typedef fake_client_t *esp_http_client_handle_t;
static fake_client_t client;
static http_body_t *response_body;
static display_scheduler_t s_display_scheduler;
static unsigned s_config_generation;
static atomic_bool s_pairing_requested;
static atomic_bool s_usb_io_busy;
static atomic_uint_fast64_t s_usb_deadline;
static struct { bool connected, configured; } s_view;
static int s_display_state_mux;
static unsigned critical_depth, mutex_depth, inits, performs, cleanups, content_types, fields;
static bool init_fails, header_fails, body_overflow, body_redirect;
static esp_err_t perform_error;
static int response_status;
static const char *TAG = "test";
static void (*on_mutex)(void), (*on_header)(void), (*on_cleanup)(void), (*on_perform)(void);
static void run_once(void (**hook)(void)) {
    void (*callback)(void) = *hook;
    *hook = NULL;
    if (callback != NULL) callback();
}
static void enter_critical(int *mux) { (void)mux; assert(critical_depth == 0); critical_depth++; }
static void exit_critical(int *mux) { (void)mux; assert(critical_depth == 1); critical_depth--; }
#define portENTER_CRITICAL(mux) enter_critical(mux)
#define portEXIT_CRITICAL(mux) exit_critical(mux)
static void mutex_lock(void) {
    assert(critical_depth == 0 && mutex_depth == 0);
    run_once(&on_mutex);
    mutex_depth++;
}
static void mutex_unlock(void) { assert(mutex_depth == 1); mutex_depth--; }
static esp_err_t http_event_handler(esp_http_client_event_t *event) { (void)event; return ESP_OK; }
static const char *esp_err_to_name(esp_err_t err) { (void)err; return "fake"; }
#define ESP_LOGW(tag, format, err, status) do { (void)(tag); (void)(format); (void)(err); (void)(status); } while (0)
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    assert(mutex_depth == 0 && critical_depth == 0);
    assert(config->timeout_ms == HTTP_TIMEOUT_MS);
    assert(config->transport_type == HTTP_TRANSPORT_OVER_SSL);
    assert(config->disable_auto_redirect && !config->skip_cert_common_name_check);
    assert(config->max_authorization_retries == -1);
    assert(strcmp(config->cert_pem, "synthetic certificate") == 0);
    assert(strncmp(config->url, "https://127.0.0.1:4318/", 23) == 0);
    inits++;
    if (init_fails) return NULL;
    client.alive = true;
    response_body = config->user_data;
    return &client;
}
static esp_err_t esp_http_client_set_header(esp_http_client_handle_t handle,
                                          const char *name, const char *value) {
    assert(handle == &client && client.alive);
    assert(value != NULL);
    if (strcmp(name, "Content-Type") == 0) content_types++;
    run_once(&on_header);
    return header_fails ? ESP_FAIL : ESP_OK;
}
static esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t handle,
                                               const char *data, int length) {
    assert(handle == &client && client.alive);
    assert(length == (int)strlen(data));
    fields++;
    return ESP_OK;
}
static esp_err_t esp_http_client_perform(esp_http_client_handle_t handle) {
    assert(handle == &client && client.alive);
    assert(mutex_depth == 0 && critical_depth == 0);
    performs++;
    run_once(&on_perform);
    response_body->overflow = body_overflow;
    response_body->redirect = body_redirect;
    return perform_error;
}
static int esp_http_client_get_status_code(esp_http_client_handle_t handle) {
    assert(handle == &client && client.alive);
    return response_status;
}
static void esp_http_client_cleanup(esp_http_client_handle_t handle) {
    assert(handle == &client && client.alive);
    assert(mutex_depth == 0 && critical_depth == 0);
    client.alive = false;
    cleanups++;
    run_once(&on_cleanup);
}
'''
        harness += functions
        harness += r'''
static quota_device_config_t config;
static http_body_t body;
static http_request_outcome_t outcome;
static void sleep_now(void) { s_display_scheduler.sleeping = true; s_display_scheduler.generation++; }
static void sleep_then_wake(void) { s_display_scheduler.generation += 2; s_display_scheduler.sleeping = false; }
static void disconnect_now(void) { s_view.connected = false; }
static void reconnect_now(void) { s_view.connected = true; }
static void change_config(void) { s_config_generation++; }
static void request_pairing(void) { atomic_store(&s_pairing_requested, true); }
static void reset(void) {
    s_display_scheduler = (display_scheduler_t){.generation = 1};
    s_config_generation = 1; s_view.configured = true; s_view.connected = true;
    atomic_store(&s_pairing_requested, false);
    atomic_store(&s_usb_io_busy, false); atomic_store(&s_usb_deadline, 0);
    config = (quota_device_config_t){0};
    strcpy(config.base_url, "https://127.0.0.1:4318");
    strcpy(config.server_cert_pem, "synthetic certificate");
    strcpy(config.pair_token, "example");
    body = (http_body_t){0};
    outcome = HTTP_REQUEST_ADMITTED;
    critical_depth = mutex_depth = inits = performs = cleanups = content_types = fields = 0;
    init_fails = header_fails = body_overflow = body_redirect = false;
    perform_error = ESP_OK; response_status = 200;
    on_mutex = on_header = on_cleanup = on_perform = NULL;
    client.alive = false;
}
static bool request(esp_http_client_method_t method, const char *payload) {
    bool result = http_request(&config, "/v1/test", method, payload, 200, 1, 1, &outcome, &body);
    assert(!client.alive && critical_depth == 0 && mutex_depth == 0);
    return result;
}
static void expect_deferred(bool initialized) {
    assert(outcome == HTTP_REQUEST_DEFERRED && performs == 0);
    assert(inits == (initialized ? 1u : 0u));
    assert(cleanups == (initialized ? 1u : 0u));
}
int main(void) {
    reset(); assert(request(HTTP_METHOD_GET, NULL));
    assert(outcome == HTTP_REQUEST_ADMITTED && performs == 1 && cleanups == 1);
    assert(content_types == 0 && fields == 0);
    reset(); assert(request(HTTP_METHOD_POST, "{}")); assert(fields == 1 && content_types == 1);
    reset(); assert(request(HTTP_METHOD_PATCH, "{}")); assert(fields == 1 && content_types == 1);

    reset(); sleep_now(); assert(!request(HTTP_METHOD_GET, NULL)); expect_deferred(false);
    reset(); disconnect_now(); assert(!request(HTTP_METHOD_GET, NULL)); expect_deferred(false);
    reset(); change_config(); assert(!request(HTTP_METHOD_GET, NULL)); expect_deferred(false);
    reset(); s_view.configured = false; assert(!request(HTTP_METHOD_GET, NULL)); expect_deferred(false);
    reset(); atomic_store(&s_pairing_requested, true);
    assert(!request(HTTP_METHOD_GET, NULL)); expect_deferred(false);
    reset(); atomic_store(&s_pairing_requested, true); atomic_store(&s_usb_deadline, 120000);
    assert(request(HTTP_METHOD_GET, NULL));
    reset(); atomic_store(&s_usb_io_busy, true);
    assert(!request(HTTP_METHOD_GET, NULL)); expect_deferred(false);
    /* The display changes while the preflight waits for the configuration mutex. */
    reset(); on_mutex = sleep_now; assert(!request(HTTP_METHOD_GET, NULL)); expect_deferred(false);
    /* Recheck after setup and header calls, immediately before perform. */
    reset(); on_header = sleep_now; assert(!request(HTTP_METHOD_POST, "{}")); expect_deferred(true);
    reset(); on_header = sleep_then_wake; assert(!request(HTTP_METHOD_GET, NULL)); expect_deferred(true);
    reset(); on_header = change_config; assert(!request(HTTP_METHOD_PATCH, "{}")); expect_deferred(true);
    reset(); on_header = request_pairing; assert(!request(HTTP_METHOD_POST, "{}")); expect_deferred(true);
    /* Preserve deferral even if the link has recovered by the time cleanup returns. */
    for (int method = HTTP_METHOD_GET; method <= HTTP_METHOD_PATCH; method++) {
        reset(); on_header = disconnect_now; on_cleanup = reconnect_now;
        assert(!request((esp_http_client_method_t)method, method == HTTP_METHOD_GET ? NULL : "{}"));
        assert(s_view.connected); expect_deferred(true);
    }
    /* Local failures are distinct; callers must not turn them into an immediate retry loop. */
    reset(); init_fails = true; assert(!request(HTTP_METHOD_POST, "{}"));
    assert(outcome == HTTP_REQUEST_NOT_ADMITTED && inits == 1 && performs == 0 && cleanups == 0);
    reset(); header_fails = true; assert(!request(HTTP_METHOD_PATCH, "{}"));
    assert(outcome == HTTP_REQUEST_NOT_ADMITTED && performs == 0 && cleanups == 1);
    reset(); perform_error = ESP_FAIL; assert(!request(HTTP_METHOD_GET, NULL));
    assert(outcome == HTTP_REQUEST_ADMITTED && performs == 1 && cleanups == 1);
    reset(); response_status = 302; assert(!request(HTTP_METHOD_GET, NULL));
    assert(outcome == HTTP_REQUEST_ADMITTED && cleanups == 1);
    reset(); body_overflow = true; assert(!request(HTTP_METHOD_GET, NULL));
    assert(outcome == HTTP_REQUEST_ADMITTED && cleanups == 1);
    reset(); body_redirect = true; assert(!request(HTTP_METHOD_GET, NULL));
    assert(outcome == HTTP_REQUEST_ADMITTED && cleanups == 1);
    /* An admitted request can finish safely; the real publish path separately rejects its token. */
    reset(); on_perform = sleep_now; assert(request(HTTP_METHOD_GET, NULL));
    assert(outcome == HTTP_REQUEST_ADMITTED && s_display_scheduler.sleeping && cleanups == 1);
    puts("quota HTTP runtime tests passed");
    return 0;
}
'''
        compile_and_run(harness, "ai-quota-http-test-")


class ResponseRuntime(unittest.TestCase):
    def test_response_size_redirect_and_unique_ack(self):
        source = (ROOT / "main/quota_service.c").read_text()
        body = re.search(r"typedef struct \{[^{}]*\} http_body_t;", source)[0]
        functions = "\n".join(extract_function(source, name, declaration) for name, declaration in (
            ("http_event_handler", "static esp_err_t"),
            ("json_has_unique_keys", "static bool"), ("parse_refresh_ack", "static bool")))
        harness = r'''
#include "cJSON.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define HTTP_BODY_BYTES 8192
#define ESP_OK 0
#define ESP_FAIL 1
#define HTTP_EVENT_REDIRECT 1
#define HTTP_EVENT_ON_DATA 2
typedef int esp_err_t;
typedef struct { void *user_data; int event_id; const char *data; int data_len; } esp_http_client_event_t;
''' + body + "\n" + functions + r'''
int main(void) {
    http_body_t body={0}; char payload[8193];memset(payload,'x',sizeof(payload));
    esp_http_client_event_t event={.user_data=&body,.event_id=HTTP_EVENT_ON_DATA,.data=payload,.data_len=8192};
    assert(http_event_handler(&event)==ESP_OK&&body.length==8192&&!body.overflow);
    event.data_len=1;assert(http_event_handler(&event)==ESP_FAIL&&body.overflow&&body.length==8192);
    event.event_id=HTTP_EVENT_REDIRECT;assert(http_event_handler(&event)==ESP_OK&&body.redirect);
    memset(&body,0,sizeof(body));strcpy(body.bytes,"{\"v\":1,\"accepted\":true}");body.length=strlen(body.bytes);assert(parse_refresh_ack(&body));
    strcpy(body.bytes,"{\"v\":1,\"v\":1,\"accepted\":true}");body.length=strlen(body.bytes);assert(!parse_refresh_ack(&body));
    strcpy(body.bytes,"{\"v\":1,\"accepted\":false}");body.length=strlen(body.bytes);assert(!parse_refresh_ack(&body));
    puts("response bounds and refresh acknowledgement passed");
}
'''
        compile_and_run(harness, "ai-quota-http-response-", ("tests/cjson/cJSON.c",))

    def test_usb_error_mapping_new_tail_entries(self):
        source = (ROOT / "main/quota_service.c").read_text()
        function = extract_function(source, "send_pairing_result")
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static char output[512];
static int capture_printf(const char *format,...){va_list args;va_start(args,format);int length=vsnprintf(output,sizeof(output),format,args);va_end(args);return length;}
static int capture_flush(FILE *stream){(void)stream;return 0;}
#define printf capture_printf
#define fflush capture_flush
''' + function + r'''
int main(void) {
    send_pairing_result("1234abcd",false,"storage_write_unknown");
    assert(strstr(output,"\"error\":\"storage_write_unknown\"") &&
           strstr(output,"\"request_id\":\"1234abcd\""));
    send_pairing_result("1234abcd",false,"generation_exhausted");
    assert(strstr(output,"\"error\":\"generation_exhausted\""));
    const char invalid_id[9]="bad-id";
    send_pairing_result(invalid_id,false,"unexpected");
    assert(strstr(output,"\"request_id\":\"00000000\"") &&
           strstr(output,"\"error\":\"invalid_frame\""));
    return 0;
}
'''
        compile_and_run(harness, "ai-quota-usb-error-map-")


if __name__ == "__main__":
    unittest.main()
