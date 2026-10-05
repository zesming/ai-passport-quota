#include "quota_direct.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#ifdef ESP_PLATFORM
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#define AUTH_CODE_URL "https://auth.openai.com/api/accounts/deviceauth/usercode"
#define AUTH_POLL_URL "https://auth.openai.com/api/accounts/deviceauth/token"
#define AUTH_TOKEN_URL "https://auth.openai.com/oauth/token"
#define AUTH_CALLBACK_URL "https://auth.openai.com/deviceauth/callback"
#define CODEX_USAGE_URL "https://chatgpt.com/backend-api/wham/usage"
#define CODEX_RESET_URL "https://chatgpt.com/backend-api/wham/rate-limit-reset-credits"
#define DEEPSEEK_URL "https://api.deepseek.com/user/balance"
#define DIRECT_PHASE_TIMEOUT_MS 1000
#define DIRECT_BUDGET_US 15000000LL
#define DIRECT_INTERNAL_RESERVE_BYTES 12288
#define DIRECT_DMA_RESERVE_BYTES 4096
#define DIRECT_DMA_LARGEST_BYTES 2048
/* Reservation is one allocation, never grow an admitted grant response. */
#ifndef QUOTA_DIRECT_RESPONSE_CALLOC
#define QUOTA_DIRECT_RESPONSE_CALLOC calloc
#endif

struct quota_direct {
    quota_direct_hooks_t hooks;
    quota_direct_transport_t transport;
    void *transport_context;
    quota_direct_credential_t *pending;
    char login_id[QUOTA_ACCOUNT_ID_BYTES + 1];
    uint32_t login_generation;
    bool login_active;
    bool exchange_ready;
    bool login_persist_pending;
    uint64_t login_deadline_ms;
    uint64_t next_poll_ms;
    quota_direct_device_code_t device_code;
    quota_direct_authorization_t *authorization;
};

static quota_direct_result_t result(quota_direct_result_code_t code)
{
    quota_direct_result_t value = {0}; value.code = code; return value;
}

static bool current(quota_direct_t *direct, const quota_direct_credential_t *credential)
{
    return direct && credential && !credential->tombstone &&
        direct->hooks.account_current(direct->hooks.context, credential->id, credential->generation);
}

static bool admitted(quota_direct_t *direct, const quota_direct_credential_t *credential)
{
    return current(direct, credential) &&
        direct->hooks.admit(direct->hooks.context, credential->id, credential->generation);
}

static bool fixed_url(const char *url)
{
    return url && (!strcmp(url, AUTH_CODE_URL) || !strcmp(url, AUTH_POLL_URL) ||
        !strcmp(url, AUTH_TOKEN_URL) || !strcmp(url, CODEX_USAGE_URL) ||
        !strcmp(url, CODEX_RESET_URL) || !strcmp(url, DEEPSEEK_URL));
}

static void free_body(char *body);

#ifdef ESP_PLATFORM
static atomic_uint s_alloc_failures, s_alloc_size, s_alloc_caps;
static bool s_alloc_hook_registered;
static void allocation_failed(size_t size, uint32_t caps, const char *name)
{
    (void)name;
    atomic_store_explicit(&s_alloc_size, (unsigned)size, memory_order_relaxed);
    atomic_store_explicit(&s_alloc_caps, caps, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_alloc_failures, 1, memory_order_relaxed);
}

typedef struct {
    quota_direct_http_response_t *response;
    quota_direct_t *direct;
    quota_direct_http_request_t *request;
    int64_t deadline_us;
    size_t header_bytes, content_length;
    bool content_length_known, aborted, response_started;
    size_t minimum_internal, minimum_dma, minimum_largest_dma;
} esp_request_context_t;

static void sample_resources(esp_request_context_t *context)
{
    size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    if (internal < context->minimum_internal) context->minimum_internal = internal;
    if (dma < context->minimum_dma) context->minimum_dma = dma;
    if (largest < context->minimum_largest_dma) context->minimum_largest_dma = largest;
}

static void abort_request(esp_http_client_event_t *event, esp_request_context_t *context)
{
    if (context->aborted) return;
    context->aborted = true;
    /* IDF ignores HEADER/DATA callback results: close the actual transport. */
    (void)esp_http_client_close(event->client);
}

static void clear_authorization(esp_http_client_handle_t client)
{
    char *value = NULL;
    if (esp_http_client_get_header(client, "Authorization", &value) == ESP_OK && value)
        quota_direct_secure_clear(value, strlen(value));
    (void)esp_http_client_delete_header(client, "Authorization");
}

static void response_started(esp_http_client_event_t *event, esp_request_context_t *context)
{
    if (context->response_started) return;
    context->response_started = true;
    clear_authorization(event->client);
    /* IDF borrows post_data. Detach it before wiping the sole-owned body. */
    (void)esp_http_client_set_post_field(event->client, NULL, 0);
    free_body(context->request->body); context->request->body = NULL;
}

static bool reserve_response(esp_http_client_event_t *event, esp_request_context_t *context, size_t capacity, bool admission)
{
    quota_direct_http_response_t *response = context->response;
    if (response->body) return true;
    response->body = QUOTA_DIRECT_RESPONSE_CALLOC(1, capacity + 1);
    if (!response->body) {
        ESP_LOGW("quota_direct", "response reserve failed capacity=%u length=%u", (unsigned)capacity, (unsigned)response->length);
        response->allocation_failed = true; abort_request(event, context); return false;
    }
    response->capacity = capacity;
    sample_resources(context);
    if (admission && (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < DIRECT_INTERNAL_RESERVE_BYTES ||
        heap_caps_get_free_size(MALLOC_CAP_DMA) < DIRECT_DMA_RESERVE_BYTES ||
        heap_caps_get_largest_free_block(MALLOC_CAP_DMA) < DIRECT_DMA_LARGEST_BYTES)) {
        response->resource_limited = true; abort_request(event, context); return false;
    }
    return true;
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    esp_request_context_t *context = event->user_data;
    if (!context || !context->response) return ESP_OK;
    quota_direct_http_response_t *response = context->response;
    if (context->aborted) return ESP_FAIL;
    sample_resources(context);
    if (event->event_id == HTTP_EVENT_HEADERS_SENT) response->admitted = true;
    if (event->event_id == HTTP_EVENT_ON_CONNECTED) {
        if (!context->direct->hooks.account_current(context->direct->hooks.context,
            context->request->logical_id, context->request->generation) ||
            !context->direct->hooks.admit(context->direct->hooks.context,
            context->request->logical_id, context->request->generation)) {
            abort_request(event, context); return ESP_FAIL;
        }
        /* Poll can return a one-time grant too, so reserve every POST before
         * any request header is sent, after the TLS handshake is complete. */
        if (context->request->post && !reserve_response(event, context, QUOTA_DIRECT_BODY_BYTES, true)) return ESP_FAIL;
    }
    if (event->event_id != HTTP_EVENT_DISCONNECTED && event->event_id != HTTP_EVENT_ERROR &&
        esp_timer_get_time() >= context->deadline_us) { abort_request(event, context); return ESP_FAIL; }
    if (event->event_id == HTTP_EVENT_REDIRECT) response->redirected = true;
    if (event->event_id == HTTP_EVENT_ON_HEADER || event->event_id == HTTP_EVENT_ON_DATA)
        response_started(event, context);
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value) {
        size_t key_bytes = strlen(event->header_key), value_bytes = strlen(event->header_value);
        if (key_bytes > 256 || value_bytes > 4096 || key_bytes + value_bytes > 16384 - context->header_bytes) {
            response->overflow = true; abort_request(event, context); return ESP_FAIL;
        }
        context->header_bytes += key_bytes + value_bytes;
        if (!strcasecmp(event->header_key, "Content-Length")) {
            size_t length = 0; bool valid = value_bytes > 0 && !context->content_length_known;
            for (size_t i = 0; valid && i < value_bytes; ++i) {
                unsigned char byte = (unsigned char)event->header_value[i];
                if (byte < '0' || byte > '9' || length > QUOTA_DIRECT_BODY_BYTES / 10) valid = false;
                else { length = length * 10 + (byte - '0'); if (length > QUOTA_DIRECT_BODY_BYTES) valid = false; }
            }
            if (!valid) { response->overflow = true; abort_request(event, context); return ESP_FAIL; }
            context->content_length = length; context->content_length_known = true;
        }
        char *output = !strcasecmp(event->header_key, "Date") ? response->date :
            !strcasecmp(event->header_key, "Retry-After") ? response->retry_after : NULL;
        if (output && value_bytes <= 64) memcpy(output, event->header_value, value_bytes + 1);
    }
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data && event->data_len > 0) {
        size_t length = (size_t)event->data_len;
        size_t capacity = context->content_length_known ? context->content_length : QUOTA_DIRECT_BODY_BYTES;
        if (!response->body && !reserve_response(event, context, capacity, false)) return ESP_FAIL;
        if (response->length > response->capacity || length > response->capacity - response->length ||
            (context->content_length_known && (response->length > capacity || length > capacity - response->length))) {
            response->overflow = true; abort_request(event, context); return ESP_FAIL;
        }
        memcpy(response->body + response->length, event->data, length);
        response->length += length; response->body[response->length] = 0;
    }
    return ESP_OK;
}

static bool esp_transport(void *context, quota_direct_http_request_t *request,
                          quota_direct_http_response_t *response)
{
    quota_direct_t *direct = context;
    esp_request_context_t event_context = {.response = response, .direct = direct, .request = request,
        .deadline_us = esp_timer_get_time() + DIRECT_BUDGET_US,
        .minimum_internal = SIZE_MAX, .minimum_dma = SIZE_MAX, .minimum_largest_dma = SIZE_MAX};
    unsigned failure_start = atomic_load_explicit(&s_alloc_failures, memory_order_relaxed);
    size_t bearer_length = request->bearer ? strlen(request->bearer) : 0;
    if (bearer_length > QUOTA_DIRECT_ACCESS_BYTES) { response->overflow = true; return false; }
    /* SDK allocation-time low watermarks include transient AES/DMA use that
     * cannot be observed by sampling only HTTP callbacks. This global window
     * also includes other tasks' allocations during the same request. */
    bool local_heap_monitor = heap_caps_monitor_local_minimum_free_size_start() == ESP_OK;
    esp_http_client_config_t config = {
        .url = request->url, .method = request->post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = DIRECT_PHASE_TIMEOUT_MS,
        .is_async = true, .disable_auto_redirect = true, .max_authorization_retries = -1,
        .event_handler = http_event, .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .buffer_size = 1024, .buffer_size_tx = (int)(1024 + bearer_length),
        .user_data = &event_context, .skip_cert_common_name_check = false,
    };
    sample_resources(&event_context);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        response->allocation_failed = true; response->esp_error = ESP_ERR_NO_MEM;
        if (local_heap_monitor) (void)heap_caps_monitor_local_minimum_free_size_stop();
        return false;
    }
    esp_err_t error = esp_http_client_set_header(client, "Accept", "application/json");
    if (error == ESP_OK) error = esp_http_client_set_header(client, "User-Agent", "AIQuota-Portable-Experimental/1");
    if (error == ESP_OK && request->bearer) {
        char *authorization = malloc(bearer_length + sizeof("Bearer "));
        if (!authorization) error = ESP_ERR_NO_MEM;
        else {
            snprintf(authorization, bearer_length + sizeof("Bearer "), "Bearer %s", request->bearer);
            error = esp_http_client_set_header(client, "Authorization", authorization);
            quota_direct_secure_clear(authorization, bearer_length + sizeof("Bearer ")); free(authorization);
        }
    }
    if (error == ESP_OK && request->account_id)
        error = esp_http_client_set_header(client, "ChatGPT-Account-ID", request->account_id);
    if (error == ESP_OK && request->content_type)
        error = esp_http_client_set_header(client, "Content-Type", request->content_type);
    if (error == ESP_OK && request->body)
        error = esp_http_client_set_post_field(client, request->body, (int)strlen(request->body));
    /* A set-header failure is an allocation failure for these fixed valid keys. */
    if (error != ESP_OK) response->allocation_failed = true;
    if (error == ESP_OK && (!direct->hooks.account_current(direct->hooks.context, request->logical_id, request->generation) ||
        !direct->hooks.admit(direct->hooks.context, request->logical_id, request->generation))) error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK) {
        do {
            sample_resources(&event_context);
            int64_t remaining = event_context.deadline_us - esp_timer_get_time();
            if (remaining <= 0 || event_context.aborted) {
                event_context.aborted = true; (void)esp_http_client_close(client); error = ESP_ERR_TIMEOUT; break;
            }
            if (!response->admitted && (!direct->hooks.account_current(direct->hooks.context, request->logical_id, request->generation) ||
                !direct->hooks.admit(direct->hooks.context, request->logical_id, request->generation))) {
                event_context.aborted = true; (void)esp_http_client_close(client); error = ESP_ERR_INVALID_STATE; break;
            }
            int timeout = (int)((remaining + 999) / 1000); if (timeout > DIRECT_PHASE_TIMEOUT_MS) timeout = DIRECT_PHASE_TIMEOUT_MS;
            error = esp_http_client_set_timeout_ms(client, timeout); if (error != ESP_OK) break;
            error = esp_http_client_perform(client);
            if (error == ESP_ERR_HTTP_EAGAIN && !event_context.aborted) vTaskDelay(pdMS_TO_TICKS(10) ? pdMS_TO_TICKS(10) : 1);
        } while (error == ESP_ERR_HTTP_EAGAIN && !event_context.aborted);
    }
    response->status = esp_http_client_get_status_code(client);
    bool complete = esp_http_client_is_complete_data_received(client);
    bool within_budget = esp_timer_get_time() < event_context.deadline_us;
    bool auth_refusal = response->admitted && response->status == 401 && error == ESP_FAIL;
    int tls_flags = 0;
    esp_err_t tls_esp_error = esp_http_client_get_and_clear_last_tls_error(client, &response->tls_error, &tls_flags);
    response->esp_error = error != ESP_OK ? error : tls_esp_error;
    sample_resources(&event_context);
    unsigned failures = atomic_load_explicit(&s_alloc_failures, memory_order_relaxed) - failure_start;
    if (error != ESP_OK && failures && response->tls_error) response->resource_limited = true;
    if (error == ESP_ERR_NO_MEM) response->allocation_failed = true;
    clear_authorization(client);
    (void)esp_http_client_set_post_field(client, NULL, 0);
    free_body(request->body); request->body = NULL;
    esp_http_client_cleanup(client);
    size_t minimum8 = 0;
    if (local_heap_monitor) {
        minimum8 = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
        event_context.minimum_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        event_context.minimum_dma = heap_caps_get_minimum_free_size(MALLOC_CAP_DMA);
        (void)heap_caps_monitor_local_minimum_free_size_stop();
    }
    ESP_LOGI("quota_direct", "http phase done error=%d tls=%d alloc=%u bytes=%u caps=%u local_min=%u min8=%u internal_min=%u dma_min=%u dma_largest_sample_min=%u free8=%u largest8=%u stack=%u",
        response->esp_error, response->tls_error, failures,
        failures ? atomic_load_explicit(&s_alloc_size, memory_order_relaxed) : 0,
        failures ? atomic_load_explicit(&s_alloc_caps, memory_order_relaxed) : 0,
        (unsigned)local_heap_monitor, (unsigned)minimum8,
        (unsigned)event_context.minimum_internal, (unsigned)event_context.minimum_dma, (unsigned)event_context.minimum_largest_dma,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        (unsigned)uxTaskGetStackHighWaterMark(NULL));
    return ((error == ESP_OK && complete) || auth_refusal) && within_budget && !event_context.aborted &&
        !response->overflow && !response->allocation_failed && !response->resource_limited && !response->redirected;
}

#endif

quota_direct_t *quota_direct_create(const quota_direct_hooks_t *hooks,
    quota_direct_transport_t transport, void *transport_context)
{
    if (!hooks || !hooks->admit || !hooks->account_current || !hooks->persist) return NULL;
#ifndef ESP_PLATFORM
    if (!transport) return NULL;
#endif
#ifdef ESP_PLATFORM
    if (!s_alloc_hook_registered) {
        s_alloc_hook_registered = heap_caps_register_failed_alloc_callback(allocation_failed) == ESP_OK;
    }
#endif
    quota_direct_t *direct = calloc(1, sizeof(*direct));
    if (!direct) return NULL;
    direct->hooks = *hooks; direct->transport = transport; direct->transport_context = transport_context;
#ifdef ESP_PLATFORM
    if (!direct->transport) { direct->transport = esp_transport; direct->transport_context = direct; }
#endif
    return direct;
}

void quota_direct_login_cancel(quota_direct_t *direct)
{
    if (!direct) return;
    quota_direct_secure_clear(&direct->device_code, sizeof(direct->device_code));
    if (direct->authorization) {
        quota_direct_secure_clear(direct->authorization, sizeof(*direct->authorization));
        free(direct->authorization); direct->authorization = NULL;
    }
    direct->login_active = false; direct->exchange_ready = false;
    direct->login_deadline_ms = 0; direct->next_poll_ms = 0;
    /* Successful rotations awaiting persistence are NOT canceled by UI state. */
}

void quota_direct_destroy(quota_direct_t *direct)
{
    if (!direct) return;
    /* Pending credentials belong to the network owner, including on destroy. */
    quota_direct_login_cancel(direct);
    quota_direct_secure_clear(direct, sizeof(*direct)); free(direct);
}

static void free_response(quota_direct_http_response_t *response)
{
    if (response->body) { quota_direct_secure_clear(response->body, response->capacity + 1); free(response->body); }
    response->body = NULL;
}

static quota_direct_result_t perform(quota_direct_t *direct,
    const quota_direct_credential_t *credential, const quota_direct_http_request_t *request,
    uint64_t utc, quota_direct_http_response_t *response)
{
    memset(response, 0, sizeof(*response));
    quota_direct_http_request_t scoped_request = *request;
    quota_direct_result_t out;
    if (!fixed_url(request->url)) { out = result(QUOTA_DIRECT_UNSUPPORTED); goto cleanup; }
    if (!utc) { out = result(QUOTA_DIRECT_TIME_REQUIRED); goto cleanup; }
    if (!admitted(direct, credential)) { out = result(QUOTA_DIRECT_DEFERRED); goto cleanup; }
    bool injected = true;
#ifdef ESP_PLATFORM
    injected = direct->transport != esp_transport;
#endif
    if (injected) {
        response->capacity = QUOTA_DIRECT_BODY_BYTES;
        response->body = QUOTA_DIRECT_RESPONSE_CALLOC(1, response->capacity + 1);
        if (!response->body) { out = result(QUOTA_DIRECT_NO_MEMORY); goto cleanup; }
    }
    if (!admitted(direct, credential)) { out = result(QUOTA_DIRECT_DEFERRED); goto cleanup; }
    scoped_request.logical_id = credential->id; scoped_request.generation = credential->generation;
    bool success = direct->transport(direct->transport_context, &scoped_request, response);
    success = success && !response->overflow && !response->allocation_failed && !response->resource_limited && !response->redirected;
    if (response->length > response->capacity) { response->overflow = true; success = false; }
    out = result(success ? QUOTA_DIRECT_OK : response->overflow ? QUOTA_DIRECT_RESPONSE_TOO_LARGE :
        response->allocation_failed ? QUOTA_DIRECT_NO_MEMORY : response->resource_limited ? QUOTA_DIRECT_RESOURCE_ERROR :
        response->tls_error ? QUOTA_DIRECT_TLS_ERROR : QUOTA_DIRECT_NETWORK_ERROR);
    out.http_status = response->status; out.esp_error = response->esp_error; out.tls_error = response->tls_error;
    if (!success && !response->admitted && !admitted(direct, credential)) out.code = QUOTA_DIRECT_DEFERRED;
    if (!success) goto cleanup;
    if (response->status == 429) { out.code = QUOTA_DIRECT_RATE_LIMITED; out.retry_after_seconds = quota_direct_retry_after(response->retry_after, utc); }
    else if (response->status == 401 || response->status == 403) out.code = QUOTA_DIRECT_AUTH_REQUIRED;
    else if (response->status != 200) out.code = QUOTA_DIRECT_NETWORK_ERROR;
    uint64_t date = quota_direct_parse_date(response->date); out.source_epoch = date ? date : utc;
cleanup:
    free_body(scoped_request.body); scoped_request.body = NULL;
    return out;
}

static void identity_from_credential(const quota_direct_credential_t *credential,
                                    quota_direct_identity_t *identity)
{
    memset(identity, 0, sizeof(*identity));
    memcpy(identity->account_id, credential->server_account_id, sizeof(identity->account_id));
    memcpy(identity->user_id, credential->server_user_id, sizeof(identity->user_id));
    memcpy(identity->email, credential->email, sizeof(identity->email));
    memcpy(identity->plan, credential->plan, sizeof(identity->plan));
    identity->expires_at = credential->expires_at;
}

static char *json_body(const char *first_key, const char *first_value,
                       const char *second_key, const char *second_value,
                       const char *third_key, const char *third_value)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return NULL;
    bool valid = cJSON_AddStringToObject(json, first_key, first_value) != NULL;
    if (second_key) valid = valid && cJSON_AddStringToObject(json, second_key, second_value) != NULL;
    if (third_key) valid = valid && cJSON_AddStringToObject(json, third_key, third_value) != NULL;
    char *body = valid ? cJSON_PrintUnformatted(json) : NULL;
    for (cJSON *item = json->child; item; item = item->next)
        if (cJSON_IsString(item)) quota_direct_secure_clear(item->valuestring, strlen(item->valuestring));
    cJSON_Delete(json);
    if (body && strlen(body) > QUOTA_DIRECT_BODY_BYTES) { quota_direct_secure_clear(body, strlen(body)); free(body); return NULL; }
    return body;
}

static void free_body(char *body)
{
    if (body) { quota_direct_secure_clear(body, strlen(body)); free(body); }
}

static quota_direct_result_t commit_received(quota_direct_t *direct,
    quota_direct_credential_t *credential)
{
    if (!current(direct, credential)) return result(QUOTA_DIRECT_CANCELED);
    credential->refresh_inflight = false;
    credential->auth_state = QUOTA_PORTABLE_AUTH_READY;
    if (!direct->hooks.persist(direct->hooks.context, credential)) {
        direct->pending = credential;
        return result(QUOTA_DIRECT_PERSIST_PENDING);
    }
    return result(QUOTA_DIRECT_OK);
}

bool quota_direct_has_pending_persist(const quota_direct_t *direct)
{
    return direct && direct->pending;
}

quota_direct_result_t quota_direct_retry_persist(quota_direct_t *direct)
{
    if (!direct || !direct->pending) return result(QUOTA_DIRECT_OK);
    quota_direct_credential_t *pending = direct->pending;
    if (!current(direct, pending)) {
        if (direct->login_persist_pending) quota_direct_login_cancel(direct);
        direct->pending = NULL; direct->login_persist_pending = false;
        return result(QUOTA_DIRECT_CANCELED);
    }
    if (!direct->hooks.persist(direct->hooks.context, pending)) return result(QUOTA_DIRECT_PERSIST_PENDING);
    direct->pending = NULL;
    if (direct->login_persist_pending) { quota_direct_login_cancel(direct); direct->login_persist_pending = false; }
    return result(QUOTA_DIRECT_OK);
}

quota_direct_result_t quota_direct_login_begin(quota_direct_t *direct,
    const quota_direct_credential_t *credential, uint64_t monotonic_ms, uint64_t utc)
{
    if (!direct || !credential || credential->provider != QUOTA_PROVIDER_CODEX) return result(QUOTA_DIRECT_UNSUPPORTED);
    if (direct->pending || direct->login_active) return result(QUOTA_DIRECT_WAITING);
    char *body = json_body("client_id", QUOTA_DIRECT_CODEX_CLIENT_ID, NULL, NULL, NULL, NULL);
    if (!body) return result(QUOTA_DIRECT_NO_MEMORY);
    quota_direct_http_request_t request = { .url = AUTH_CODE_URL, .post = true,
        .content_type = "application/json", .body = body };
    quota_direct_http_response_t response;
    quota_direct_result_t out = perform(direct, credential, &request, utc, &response);
    if (response.status == 404) out.code = QUOTA_DIRECT_LOGIN_DISABLED;
    if (out.code == QUOTA_DIRECT_OK) {
        if (!current(direct, credential)) out.code = QUOTA_DIRECT_CANCELED;
        else if (!quota_direct_parse_device_code(response.body, response.length, &direct->device_code)) out.code = QUOTA_DIRECT_PROTOCOL_ERROR;
        else {
            memcpy(direct->login_id, credential->id, sizeof(direct->login_id));
            direct->login_generation = credential->generation;
            direct->login_active = true;
            direct->login_deadline_ms = monotonic_ms + QUOTA_PORTABLE_LOGIN_MS;
            direct->next_poll_ms = monotonic_ms + (uint64_t)direct->device_code.interval_seconds * 1000;
            out.code = QUOTA_DIRECT_WAITING;
        }
    }
    free_response(&response); return out;
}

static bool append_form(char *body, size_t capacity, const char *name, const char *value)
{
    size_t used = strlen(body);
    int size = snprintf(body + used, capacity - used, "%s%s=", used ? "&" : "", name);
    if (size < 0 || (size_t)size >= capacity - used) return false;
    used += (size_t)size;
    return quota_direct_form_encode(value, body + used, capacity - used);
}

static char *exchange_body(const quota_direct_authorization_t *code)
{
    if (!code || !quota_direct_token_is_safe(code->authorization_code, QUOTA_DIRECT_CODE_BYTES) ||
        !quota_direct_token_is_safe(code->code_verifier, QUOTA_DIRECT_VERIFIER_BYTES)) return NULL;
    /* Field names, separators and NUL, plus the actual escaped values. Avoid
     * reserving the maximum code size throughout the TLS handshake. */
    size_t capacity = sizeof("grant_type=&client_id=&code=&redirect_uri=&code_verifier=");
    const char *values[] = { "authorization_code", QUOTA_DIRECT_CODEX_CLIENT_ID,
        code->authorization_code, AUTH_CALLBACK_URL, code->code_verifier };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        for (const unsigned char *p = (const unsigned char *)values[i]; *p; ++p) {
            bool plain = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~';
            size_t bytes = plain ? 1 : 3;
            if (bytes > QUOTA_DIRECT_BODY_BYTES + 1 - capacity) return NULL;
            capacity += bytes;
        }
    }
    char *body = calloc(1, capacity);
    if (!body) return NULL;
    bool valid = append_form(body, capacity, "grant_type", "authorization_code") &&
        append_form(body, capacity, "client_id", QUOTA_DIRECT_CODEX_CLIENT_ID) &&
        append_form(body, capacity, "code", code->authorization_code) &&
        append_form(body, capacity, "redirect_uri", AUTH_CALLBACK_URL) &&
        append_form(body, capacity, "code_verifier", code->code_verifier);
    if (!valid) { quota_direct_secure_clear(body, capacity); free(body); return NULL; }
    return body;
}

static quota_direct_result_t accept_tokens(quota_direct_t *direct,
    quota_direct_credential_t *credential, quota_direct_http_response_t *response,
    bool require_existing_identity)
{
    if (!current(direct, credential)) { free_response(response); return result(QUOTA_DIRECT_CANCELED); }
    quota_direct_identity_t expected, parsed;
    identity_from_credential(credential, &expected);
    bool expected_set = require_existing_identity || credential->server_account_id[0];
    quota_direct_tokens_t *tokens = NULL;
#ifdef ESP_PLATFORM
    unsigned failures_before = atomic_load_explicit(&s_alloc_failures, memory_order_relaxed);
#endif
    bool valid = quota_direct_tokens_prepare(response->body, response->length, &tokens);
    /* The parser validates the complete response before writing any outputs.
     * Keep received tokens in the owner's stable buffer, and release the HTTP
     * body before persistence; no second full credential allocation is needed. */
    free_response(response);
    if (valid) valid = quota_direct_tokens_finish(tokens, expected_set ? &expected : NULL,
        credential->access_token, sizeof(credential->access_token), credential->refresh_token, sizeof(credential->refresh_token), &parsed);
    quota_direct_tokens_destroy(tokens);
    if (!valid) {
#ifdef ESP_PLATFORM
        ESP_LOGW("quota_direct", "token response invalid bytes=%u", (unsigned)response->length);
        if (atomic_load_explicit(&s_alloc_failures, memory_order_relaxed) != failures_before)
            return result(QUOTA_DIRECT_NO_MEMORY);
#endif
        return result(QUOTA_DIRECT_AUTH_REQUIRED);
    }
    memcpy(credential->server_account_id, parsed.account_id, sizeof(credential->server_account_id));
    memcpy(credential->server_user_id, parsed.user_id, sizeof(credential->server_user_id));
    memcpy(credential->email, parsed.email, sizeof(credential->email));
    memcpy(credential->plan, parsed.plan, sizeof(credential->plan));
    credential->expires_at = parsed.expires_at;
    return commit_received(direct, credential);
}

quota_direct_result_t quota_direct_login_step(quota_direct_t *direct,
    quota_direct_credential_t *credential, uint64_t monotonic_ms, uint64_t utc)
{
    if (!direct || !credential) return result(QUOTA_DIRECT_UNSUPPORTED);
    if (direct->pending) return result(QUOTA_DIRECT_PERSIST_PENDING);
    if (!direct->login_active) return result(QUOTA_DIRECT_CANCELED);
    if (strcmp(direct->login_id, credential->id) || direct->login_generation != credential->generation || !current(direct, credential)) {
        quota_direct_login_cancel(direct); return result(QUOTA_DIRECT_CANCELED);
    }
    if (monotonic_ms >= direct->login_deadline_ms) { quota_direct_login_cancel(direct); return result(QUOTA_DIRECT_EXPIRED); }
    if (monotonic_ms < direct->next_poll_ms) return result(QUOTA_DIRECT_WAITING);
    char *body = direct->exchange_ready ? exchange_body(direct->authorization) :
        json_body("device_auth_id", direct->device_code.device_auth_id, "user_code", direct->device_code.user_code, NULL, NULL);
    if (!body) return result(QUOTA_DIRECT_NO_MEMORY);
    quota_direct_http_request_t request = { .url = direct->exchange_ready ? AUTH_TOKEN_URL : AUTH_POLL_URL,
        .post = true, .content_type = direct->exchange_ready ? "application/x-www-form-urlencoded" : "application/json", .body = body };
    quota_direct_http_response_t response;
    bool exchange = direct->exchange_ready;
    if (exchange && direct->authorization) {
        quota_direct_secure_clear(direct->authorization, sizeof(*direct->authorization));
        free(direct->authorization); direct->authorization = NULL;
    }
    quota_direct_result_t out = perform(direct, credential, &request, utc, &response);
    if (!exchange && response.admitted && (response.status == 403 || response.status == 404)) {
        out.code = QUOTA_DIRECT_WAITING;
        direct->next_poll_ms = monotonic_ms + (uint64_t)direct->device_code.interval_seconds * 1000;
    } else if (out.code == QUOTA_DIRECT_OK) {
        if (exchange) {
            out = accept_tokens(direct, credential, &response, false);
            out.http_status = response.status;
            direct->login_persist_pending = out.code == QUOTA_DIRECT_PERSIST_PENDING;
            if (!direct->login_persist_pending) quota_direct_login_cancel(direct);
        } else if (!current(direct, credential)) {
            out.code = QUOTA_DIRECT_CANCELED; quota_direct_login_cancel(direct);
        } else {
            direct->authorization = calloc(1, sizeof(*direct->authorization));
            if (!direct->authorization) { out.code = QUOTA_DIRECT_NO_MEMORY; quota_direct_login_cancel(direct); }
            else if (!quota_direct_parse_authorization(response.body, response.length, direct->authorization)) {
                out.code = QUOTA_DIRECT_PROTOCOL_ERROR; quota_direct_login_cancel(direct);
            } else { direct->exchange_ready = true; direct->next_poll_ms = monotonic_ms; out.code = QUOTA_DIRECT_WAITING; }
        }
    } else if (exchange) {
        /* A one-time code may have been consumed. Never replay on uncertainty. */
        quota_direct_login_cancel(direct);
    } else if (out.code == QUOTA_DIRECT_RATE_LIMITED) {
        direct->next_poll_ms = monotonic_ms + (uint64_t)out.retry_after_seconds * 1000;
    } else if (out.code != QUOTA_DIRECT_DEFERRED && out.code != QUOTA_DIRECT_TIME_REQUIRED && out.code != QUOTA_DIRECT_NO_MEMORY) {
        direct->next_poll_ms = monotonic_ms + (uint64_t)direct->device_code.interval_seconds * 1000;
    }
    free_response(&response); return out;
}

void quota_direct_login_view(const quota_direct_t *direct, uint64_t monotonic_ms,
                             quota_direct_login_view_t *view)
{
    if (!view) return;
    memset(view, 0, sizeof(*view));
    if (!direct || !direct->login_active) return;
    view->active = true; view->exchanging = direct->exchange_ready;
    memcpy(view->account_id, direct->login_id, sizeof(view->account_id));
    snprintf(view->verification_url, sizeof(view->verification_url), "%s", QUOTA_DIRECT_VERIFICATION_URL);
    memcpy(view->user_code, direct->device_code.user_code, sizeof(view->user_code));
    view->remaining_seconds = monotonic_ms < direct->login_deadline_ms ?
        (uint32_t)((direct->login_deadline_ms - monotonic_ms + 999) / 1000) : 0;
    view->poll_after_seconds = monotonic_ms < direct->next_poll_ms ?
        (uint32_t)((direct->next_poll_ms - monotonic_ms + 999) / 1000) : 0;
}

quota_direct_result_t quota_direct_refresh(quota_direct_t *direct,
    quota_direct_credential_t *credential, uint64_t utc)
{
    if (!direct || !credential || credential->provider != QUOTA_PROVIDER_CODEX) return result(QUOTA_DIRECT_UNSUPPORTED);
    if (direct->pending) return result(QUOTA_DIRECT_PERSIST_PENDING);
    if (credential->refresh_inflight || !quota_direct_token_is_safe(credential->refresh_token, QUOTA_DIRECT_REFRESH_BYTES))
        return result(QUOTA_DIRECT_AUTH_REQUIRED);
    if (!admitted(direct, credential)) return result(QUOTA_DIRECT_DEFERRED);
    if (!utc) return result(QUOTA_DIRECT_TIME_REQUIRED);
    char *body = json_body("grant_type", "refresh_token", "client_id", QUOTA_DIRECT_CODEX_CLIENT_ID,
        "refresh_token", credential->refresh_token);
    if (!body) return result(QUOTA_DIRECT_NO_MEMORY);
    credential->refresh_inflight = true;
    if (!direct->hooks.persist(direct->hooks.context, credential)) {
        credential->refresh_inflight = false; free_body(body); return result(QUOTA_DIRECT_STORAGE_ERROR);
    }
    quota_direct_http_request_t request = { .url = AUTH_TOKEN_URL, .post = true,
        .content_type = "application/json", .body = body };
    quota_direct_http_response_t response;
    quota_direct_result_t out = perform(direct, credential, &request, utc, &response);
    if (out.code == QUOTA_DIRECT_OK) {
        out = accept_tokens(direct, credential, &response, true);
        out.http_status = response.status;
    } else if (out.code == QUOTA_DIRECT_RATE_LIMITED) {
        /* A complete HTTP 429 is an explicit refusal, not an uncertain grant.
         * Restore the reusable chain and honor Retry-After before another POST. */
        credential->refresh_inflight = false;
        if (!direct->hooks.persist(direct->hooks.context, credential)) {
            direct->pending = credential; out.code = QUOTA_DIRECT_PERSIST_PENDING;
        }
    }
    else if (!response.admitted) {
        credential->refresh_inflight = false;
        if (!direct->hooks.persist(direct->hooks.context, credential)) {
            direct->pending = credential; out.code = QUOTA_DIRECT_PERSIST_PENDING;
        }
    } else {
        /* The durable marker remains set. It is unsafe to issue this token again. */
        credential->auth_state = QUOTA_PORTABLE_AUTH_REAUTH;
        out.code = QUOTA_DIRECT_AUTH_REQUIRED;
    }
    free_response(&response); return out;
}

quota_direct_result_t quota_direct_query(quota_direct_t *direct,
    quota_direct_credential_t *credential, uint64_t utc)
{
    if (!direct || !credential) return result(QUOTA_DIRECT_UNSUPPORTED);
    if (direct->pending) return result(QUOTA_DIRECT_PERSIST_PENDING);
    if (credential->provider != QUOTA_PROVIDER_CODEX && credential->provider != QUOTA_PROVIDER_DEEPSEEK)
        return result(QUOTA_DIRECT_UNSUPPORTED);
    if (credential->refresh_inflight) return result(QUOTA_DIRECT_AUTH_REQUIRED);
    bool codex = credential->provider == QUOTA_PROVIDER_CODEX;
    const char *bearer = codex ? credential->access_token : credential->api_key;
    if (!quota_direct_token_is_safe(bearer, codex ? QUOTA_DIRECT_ACCESS_BYTES : QUOTA_PORTABLE_KEY_BYTES))
        return result(QUOTA_DIRECT_AUTH_REQUIRED);
    if (codex && (!credential->server_account_id[0] || !credential->server_user_id[0])) return result(QUOTA_DIRECT_AUTH_REQUIRED);
    quota_direct_http_request_t request = { .url = codex ? CODEX_USAGE_URL : DEEPSEEK_URL,
        .bearer = bearer, .account_id = codex ? credential->server_account_id : NULL };
    quota_direct_http_response_t response;
    quota_direct_result_t out = perform(direct, credential, &request, utc, &response);
    if (out.code == QUOTA_DIRECT_OK && !admitted(direct, credential)) out.code = QUOTA_DIRECT_DEFERRED;
    if (out.code == QUOTA_DIRECT_OK) {
        memcpy(out.account.id, credential->id, sizeof(out.account.id));
        out.account.provider = credential->provider;
        memcpy(out.account.email, credential->email, sizeof(out.account.email));
        memcpy(out.account.plan, credential->plan, sizeof(out.account.plan));
        memcpy(out.balance.label, credential->label, sizeof(out.balance.label));
        bool parsed = codex ? quota_direct_parse_codex_usage(response.body, response.length,
            credential->server_account_id, &out.account, &out.extras) :
            quota_direct_parse_deepseek(response.body, response.length, &out.balance);
        if (!parsed) out.code = QUOTA_DIRECT_PROTOCOL_ERROR;
        else {
            out.source_valid = true; out.account.status = QUOTA_STATUS_OK;
            out.account.has_observed_at = out.source_epoch != 0; out.account.observed_at = out.source_epoch;
        }
    }
    free_response(&response);
    if (out.code != QUOTA_DIRECT_OK || !codex) return out;
    if (!admitted(direct, credential)) { out.code = QUOTA_DIRECT_DEFERRED; out.source_valid = false; return out; }
    request.url = CODEX_RESET_URL;
    quota_direct_result_t details = perform(direct, credential, &request, utc, &response);
    if (details.code == QUOTA_DIRECT_OK && admitted(direct, credential))
        out.details_available = quota_direct_parse_reset_details(response.body, response.length, &out.extras);
    free_response(&response);
    /* Optional detail failure cannot invalidate an authoritative usage read. */
    if (!admitted(direct, credential)) { out.code = QUOTA_DIRECT_DEFERRED; out.source_valid = false; }
    return out;
}

const char *quota_direct_error_name(quota_direct_result_code_t code)
{
    static const char *const names[] = { "ok", "waiting", "deferred", "auth_required",
        "login_disabled", "login_expired", "canceled", "rate_limited", "network_error",
        "protocol_error", "time_required", "storage_error", "persist_pending", "no_memory", "unsupported",
        "tls_error", "resource_error", "response_too_large" };
    return (unsigned)code < sizeof(names) / sizeof(names[0]) ? names[code] : "unknown";
}
