#include "quota_portal.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static httpd_handle_t s_server;
static quota_portal_callbacks_t s_callbacks;
static char s_secret[QUOTA_PORTABLE_SESSION_BYTES + 1];

extern const unsigned char setup_html_start[] __asm__("_binary_portable_setup_html_start");
extern const unsigned char setup_html_end[] __asm__("_binary_portable_setup_html_end");

bool quota_portal_host_is_valid(const char *host)
{
    return host != NULL && (strcmp(host, "192.168.4.1") == 0 || strcmp(host, "192.168.4.1:80") == 0);
}

bool quota_portal_origin_is_valid(const char *origin)
{
    return origin != NULL && (strcmp(origin, "http://192.168.4.1") == 0 ||
                              strcmp(origin, "http://192.168.4.1:80") == 0);
}

bool quota_portal_secret_matches(const char *expected, const char *supplied)
{
    if (expected == NULL || supplied == NULL ||
        strlen(expected) != QUOTA_PORTABLE_SESSION_BYTES ||
        strlen(supplied) != QUOTA_PORTABLE_SESSION_BYTES) return false;
    unsigned difference = 0;
    for (size_t i = 0; i < QUOTA_PORTABLE_SESSION_BYTES; i++)
        difference |= (unsigned char)expected[i] ^ (unsigned char)supplied[i];
    return difference == 0;
}

static bool unique_keys(const cJSON *object)
{
    if (!cJSON_IsObject(object)) return false;
    for (const cJSON *a = object->child; a != NULL; a = a->next) {
        if (a->string == NULL) return false;
        for (const cJSON *b = a->next; b != NULL; b = b->next)
            if (b->string != NULL && strcmp(a->string, b->string) == 0) return false;
    }
    return true;
}

static bool embedded_nul(const char *json, size_t length)
{
    if (memchr(json, 0, length) != NULL) return true;
    for (size_t i = 0; i + 1 < length; i++) {
        if (json[i] != '\\') continue;
        if (i + 5 < length && json[i + 1] == 'u' && memcmp(json + i + 2, "0000", 4) == 0)
            return true;
        i++; /* Skip an escaped backslash; its following text is not an escape. */
    }
    return false;
}

static bool structure_bounded(const char *json, size_t length)
{
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < length; i++) {
        char ch = json[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') quoted = true;
        else if (ch == '{' || ch == '[') {
            if (++depth > 2) return false;
        } else if (ch == '}' || ch == ']') {
            if (depth == 0) return false;
            depth--;
        }
    }
    return !quoted && depth == 0;
}

static void clear_json_strings(cJSON *object)
{
    for (cJSON *item = object; item != NULL; item = item->next) {
        if (item->valuestring != NULL)
            quota_portable_clear_secret(item->valuestring, strlen(item->valuestring));
        if (item->child != NULL) clear_json_strings(item->child);
    }
}

static bool copy_text(const cJSON *object, const char *name, char *output,
                       size_t capacity, bool required, bool empty)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (item == NULL) return !required;
    if (!cJSON_IsString(item) || item->valuestring == NULL) return false;
    size_t length = strlen(item->valuestring);
    if (length >= capacity || (!empty && length == 0) ||
        !quota_utf8_is_valid(item->valuestring, length)) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)item->valuestring[i];
        if (ch < 0x20 || ch == 0x7f) return false;
    }
    memcpy(output, item->valuestring, length + 1);
    return true;
}

static bool unsigned_field(const cJSON *object, const char *name, uint64_t maximum,
                            uint64_t *output, bool required)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (item == NULL) return !required;
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) || item->valuedouble < 0 ||
        item->valuedouble > (double)maximum || floor(item->valuedouble) != item->valuedouble)
        return false;
    *output = (uint64_t)item->valuedouble;
    return true;
}

static bool bool_field(const cJSON *object, const char *name, bool *output, bool required)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (item == NULL) return !required;
    if (!cJSON_IsBool(item)) return false;
    *output = cJSON_IsTrue(item);
    return true;
}

static bool keys_allowed(const cJSON *object, const char *const *extra)
{
    for (const cJSON *item = object->child; item != NULL; item = item->next) {
        bool allowed = strcmp(item->string, "v") == 0 || strcmp(item->string, "op") == 0 ||
                       strcmp(item->string, "request_id") == 0 || strcmp(item->string, "phone_utc") == 0;
        for (size_t i = 0; !allowed && extra[i] != NULL; i++)
            allowed = strcmp(item->string, extra[i]) == 0;
        if (!allowed) return false;
    }
    return true;
}

static bool hex_request_id(const char *id)
{
    if (strlen(id) != 8) return false;
    for (size_t i = 0; i < 8; i++)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
    return true;
}

static bool network_password_valid(const quota_portable_command_t *command)
{
    size_t length = strlen(command->password);
    if (length == 0) return command->open_network;
    if (command->open_network) return false;
    if (length >= 8 && length <= 63) return true;
    if (length != 64) return false;
    for (size_t i = 0; i < length; i++) {
        char ch = command->password[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')))
            return false;
    }
    return true;
}

bool quota_portal_parse_command(const char *json, size_t length, quota_portable_command_t *command)
{
    if (command == NULL) return false;
    memset(command, 0, sizeof(*command));
    if (json == NULL || length == 0 || length > QUOTA_PORTABLE_COMMAND_BYTES || embedded_nul(json, length) ||
        !structure_bounded(json, length))
        return false;
    command->network_index = UINT8_MAX;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &end, false);
    bool ok = root != NULL && end != NULL;
    if (ok) {
        while (end < json + length && (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t')) end++;
        ok = end == json + length && unique_keys(root);
    }
    uint64_t version = 0;
    char op[32] = {0};
    if (ok) ok = unsigned_field(root, "v", 1, &version, true) && version == 1 &&
                 unsigned_field(root, "phone_utc", 4102444800ULL, &command->phone_utc, false) &&
                 copy_text(root, "request_id", command->request_id, sizeof(command->request_id), true, false) &&
                 hex_request_id(command->request_id) && copy_text(root, "op", op, sizeof(op), true, false);
    static const char *const network_keys[] = {"ssid", "password", "phone_utc", "network_index", "open_network", NULL};
    static const char *const deepseek_keys[] = {"account_id", "api_key", "label", NULL};
    static const char *const account_keys[] = {"account_id", NULL};
    static const char *const codex_keys[] = {"account_id", "label", NULL};
    static const char *const settings_keys[] = {"refresh_seconds", "auto_refresh", "screen_timeout_seconds", NULL};
    static const char *const mode_keys[] = {"mode", NULL};
    static const char *const no_keys[] = {NULL};
    const char *const *allowed = no_keys;
    if (ok && strcmp(op, "network_save") == 0) {
        command->op = QUOTA_PORTABLE_OP_NETWORK_SAVE;
        allowed = network_keys;
        uint64_t index = UINT8_MAX;
        ok = unsigned_field(root, "network_index", QUOTA_PORTABLE_NETWORKS - 1, &index, false) &&
             copy_text(root, "ssid", command->ssid, sizeof(command->ssid), false, false) &&
             copy_text(root, "password", command->password, sizeof(command->password), false, true) &&
             bool_field(root, "open_network", &command->open_network, false);
        command->network_index = (uint8_t)index;
        if (ok && command->ssid[0] == 0)
            ok = index != UINT8_MAX && cJSON_GetObjectItemCaseSensitive(root, "password") == NULL &&
                 cJSON_GetObjectItemCaseSensitive(root, "open_network") == NULL;
        else if (ok) ok = network_password_valid(command);
    } else if (ok && strcmp(op, "deepseek_save") == 0) {
        command->op = QUOTA_PORTABLE_OP_DEEPSEEK_SAVE;
        allowed = deepseek_keys;
        ok = copy_text(root, "account_id", command->account_id, sizeof(command->account_id), false, false) &&
             copy_text(root, "api_key", command->api_key, sizeof(command->api_key), false, false) &&
             copy_text(root, "label", command->label, sizeof(command->label), true, false) &&
             (command->account_id[0] == 0 || quota_id_is_valid(command->account_id)) &&
             (command->api_key[0] != 0 || command->account_id[0] != 0);
    } else if (ok && strcmp(op, "codex_queue") == 0) {
        command->op = QUOTA_PORTABLE_OP_CODEX_QUEUE;
        allowed = codex_keys;
        ok = copy_text(root, "account_id", command->account_id, sizeof(command->account_id), false, false) &&
             copy_text(root, "label", command->label, sizeof(command->label), false, true) &&
             (command->account_id[0] == 0 || quota_id_is_valid(command->account_id));
    } else if (ok && strcmp(op, "account_remove") == 0) {
        command->op = QUOTA_PORTABLE_OP_ACCOUNT_REMOVE;
        allowed = account_keys;
        ok = copy_text(root, "account_id", command->account_id, sizeof(command->account_id), true, false) &&
             quota_id_is_valid(command->account_id);
    } else if (ok && strcmp(op, "settings_save") == 0) {
        command->op = QUOTA_PORTABLE_OP_SETTINGS_SAVE;
        allowed = settings_keys;
        uint64_t refresh = 0, timeout = 0;
        ok = unsigned_field(root, "refresh_seconds", UINT16_MAX, &refresh, true) &&
             unsigned_field(root, "screen_timeout_seconds", UINT16_MAX, &timeout, true) &&
             bool_field(root, "auto_refresh", &command->auto_refresh, true) &&
             quota_refresh_seconds_is_valid(refresh) && quota_screen_timeout_is_valid(timeout);
        command->refresh_seconds = (uint16_t)refresh;
        command->screen_timeout_seconds = (uint16_t)timeout;
    } else if (ok && strcmp(op, "mode_select") == 0) {
        command->op = QUOTA_PORTABLE_OP_MODE_SELECT;
        allowed = mode_keys;
        char mode[16] = {0};
        ok = copy_text(root, "mode", mode, sizeof(mode), true, false);
        if (ok && strcmp(mode, "direct") == 0) command->mode = QUOTA_MODE_DIRECT;
        else if (ok && strcmp(mode, "companion") == 0) command->mode = QUOTA_MODE_COMPANION;
        else ok = false;
    } else if (ok && strcmp(op, "codex_launch") == 0) command->op = QUOTA_PORTABLE_OP_CODEX_LAUNCH;
    else if (ok && strcmp(op, "network_scan") == 0) command->op = QUOTA_PORTABLE_OP_NETWORK_SCAN;
    else if (ok && strcmp(op, "setup_close") == 0) command->op = QUOTA_PORTABLE_OP_SETUP_CLOSE;
    else if (ok && strcmp(op, "refresh") == 0) command->op = QUOTA_PORTABLE_OP_REFRESH;
    else if (ok && strcmp(op, "reconnect") == 0) command->op = QUOTA_PORTABLE_OP_RECONNECT;
    else ok = false;
    if (ok) ok = keys_allowed(root, allowed);
    /* cJSON also owns copies of API keys/passwords. Clear its string values. */
    clear_json_strings(root);
    cJSON_Delete(root);
    if (!ok) quota_portable_clear_secret(command, sizeof(*command));
    return ok;
}

static esp_err_t reply_error(httpd_req_t *request, const char *status, const char *error)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, error, HTTPD_RESP_USE_STRLEN);
}

static bool header(httpd_req_t *request, const char *name, char *value, size_t capacity)
{
    size_t bytes = httpd_req_get_hdr_value_len(request, name);
    return bytes != 0 && bytes < capacity && httpd_req_get_hdr_value_str(request, name, value, capacity) == ESP_OK;
}

static bool peer_is_ap(httpd_req_t *request)
{
    struct sockaddr_in peer = {0};
    struct sockaddr_in local = {0};
    socklen_t length = sizeof(peer);
    if (getpeername(httpd_req_to_sockfd(request), (struct sockaddr *)&peer, &length) != 0 ||
        peer.sin_family != AF_INET) return false;
    length = sizeof(local);
    if (getsockname(httpd_req_to_sockfd(request), (struct sockaddr *)&local, &length) != 0 ||
        local.sin_family != AF_INET || ntohl(local.sin_addr.s_addr) != 0xc0a80401U) return false;
    uint32_t ip = ntohl(peer.sin_addr.s_addr);
    return (ip & 0xffffff00U) == 0xc0a80400U && (ip & 0xffU) > 1 && (ip & 0xffU) < 255;
}

static bool authorize(httpd_req_t *request, bool secret, bool mutation)
{
    char host[32] = {0}, supplied[QUOTA_PORTABLE_SESSION_BYTES + 1] = {0}, origin[40] = {0};
    bool ok = s_callbacks.session_active != NULL && s_callbacks.session_active(s_callbacks.context) &&
              peer_is_ap(request) && header(request, "Host", host, sizeof(host)) && quota_portal_host_is_valid(host);
    if (ok && secret) ok = header(request, "X-AIQ-Setup", supplied, sizeof(supplied)) &&
                           quota_portal_secret_matches(s_secret, supplied);
    if (ok && mutation) ok = header(request, "Origin", origin, sizeof(origin)) && quota_portal_origin_is_valid(origin);
    if (ok && !mutation && httpd_req_get_hdr_value_len(request, "Origin") != 0)
        ok = header(request, "Origin", origin, sizeof(origin)) && quota_portal_origin_is_valid(origin);
    quota_portable_clear_secret(supplied, sizeof(supplied));
    return ok;
}

static esp_err_t page_handler(httpd_req_t *request)
{
    if (!authorize(request, false, false)) return reply_error(request, "403 Forbidden", "{\"error\":\"setup_closed\"}");
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(request, "Content-Security-Policy",
                       "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; img-src data:; base-uri 'none'; frame-ancestors 'none'; form-action 'self'");
    size_t length = (size_t)(setup_html_end - setup_html_start);
    if (length != 0 && setup_html_start[length - 1] == 0) length--;
    return httpd_resp_send(request, (const char *)setup_html_start, length);
}

static esp_err_t state_handler(httpd_req_t *request)
{
    if (!authorize(request, true, false)) return reply_error(request, "403 Forbidden", "{\"error\":\"setup_closed\"}");
    char *body = malloc(QUOTA_PORTABLE_STATE_BYTES + 1);
    if (body == NULL) return reply_error(request, "503 Service Unavailable", "{\"error\":\"busy\"}");
    size_t length = 0;
    bool ok = s_callbacks.state_json(body, QUOTA_PORTABLE_STATE_BYTES + 1, &length, s_callbacks.context) &&
              length != 0 && length <= QUOTA_PORTABLE_STATE_BYTES;
    esp_err_t result;
    if (ok && authorize(request, true, false)) {
        httpd_resp_set_type(request, "application/json");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        result = httpd_resp_send(request, body, length);
    } else result = reply_error(request, "503 Service Unavailable", "{\"error\":\"state_unavailable\"}");
    free(body);
    return result;
}

static esp_err_t command_handler(httpd_req_t *request)
{
    if (!authorize(request, true, true)) return reply_error(request, "403 Forbidden", "{\"error\":\"setup_closed\"}");
    char content_type[64] = {0};
    if (!header(request, "Content-Type", content_type, sizeof(content_type)) ||
        (strcmp(content_type, "application/json") != 0 && strcmp(content_type, "application/json; charset=utf-8") != 0))
        return reply_error(request, "415 Unsupported Media Type", "{\"error\":\"invalid_content_type\"}");
    if (request->content_len == 0 || request->content_len > QUOTA_PORTABLE_COMMAND_BYTES)
        return reply_error(request, "413 Payload Too Large", "{\"error\":\"invalid_size\"}");
    char body[QUOTA_PORTABLE_COMMAND_BYTES + 1];
    size_t received = 0;
    int64_t deadline = esp_timer_get_time() + 3000000;
    while (received < request->content_len) {
        if (esp_timer_get_time() >= deadline) {
            quota_portable_clear_secret(body, sizeof(body));
            return reply_error(request, "408 Request Timeout", "{\"error\":\"incomplete_request\"}");
        }
        int count = httpd_req_recv(request, body + received, request->content_len - received);
        if (count <= 0) {
            quota_portable_clear_secret(body, sizeof(body));
            return reply_error(request, "400 Bad Request", "{\"error\":\"incomplete_request\"}");
        }
        received += (size_t)count;
    }
    body[received] = 0;
    quota_portable_command_t command;
    bool ok = quota_portal_parse_command(body, received, &command);
    quota_portable_clear_secret(body, sizeof(body));
    if (!ok) return reply_error(request, "400 Bad Request", "{\"error\":\"invalid_command\"}");
    quota_portable_submit_result_t submitted = QUOTA_PORTABLE_SUBMIT_CLOSED;
    if (authorize(request, true, true)) submitted = s_callbacks.submit(&command, s_callbacks.context);
    char ack[80];
    int length = snprintf(ack, sizeof(ack), "{\"request_id\":\"%.8s\",\"accepted\":true}", command.request_id);
    quota_portable_clear_secret(&command, sizeof(command));
    if (submitted != QUOTA_PORTABLE_SUBMIT_ACCEPTED) {
        if (submitted == QUOTA_PORTABLE_SUBMIT_BUSY)
            return reply_error(request, "503 Service Unavailable", "{\"error\":\"busy\"}");
        if (submitted == QUOTA_PORTABLE_SUBMIT_CONFLICT)
            return reply_error(request, "409 Conflict", "{\"error\":\"request_conflict\"}");
        if (submitted == QUOTA_PORTABLE_SUBMIT_INVALID)
            return reply_error(request, "400 Bad Request", "{\"error\":\"invalid_command\"}");
        return reply_error(request, "403 Forbidden", "{\"error\":\"setup_closed\"}");
    }
    httpd_resp_set_status(request, "202 Accepted");
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, ack, length);
}

bool quota_portal_start(const char *secret, const quota_portal_callbacks_t *callbacks)
{
    if (s_server != NULL) return false;
    if (secret == NULL || !quota_pair_token_is_valid(secret) || callbacks == NULL ||
        callbacks->submit == NULL || callbacks->state_json == NULL || callbacks->session_active == NULL)
        return false;
    s_callbacks = *callbacks;
    memcpy(s_secret, secret, sizeof(s_secret));
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_open_sockets = 2;
    config.max_uri_handlers = 3;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 3;
    config.send_wait_timeout = 3;
    if (httpd_start(&s_server, &config) != ESP_OK) { quota_portal_stop(); return false; }
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_handler},
        {.uri = "/api/state", .method = HTTP_GET, .handler = state_handler},
        {.uri = "/api/command", .method = HTTP_POST, .handler = command_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        if (httpd_register_uri_handler(s_server, &routes[i]) != ESP_OK) {
            quota_portal_stop();
            return false;
        }
    }
    return true;
}

void quota_portal_stop(void)
{
    if (s_server != NULL) {
        if (httpd_stop(s_server) != ESP_OK) return;
        s_server = NULL;
    }
    quota_portable_clear_secret(s_secret, sizeof(s_secret));
    memset(&s_callbacks, 0, sizeof(s_callbacks));
}

bool quota_portal_running(void)
{
    return s_server != NULL;
}
