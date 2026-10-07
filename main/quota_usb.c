#include "quota_usb.h"
#include "quota_portal.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

static bool hex(const char *value, size_t length)
{
    if (!value || strlen(value) != length) return false;
    for (size_t i = 0; i < length; i++)
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return false;
    return true;
}
static bool keys(const cJSON *object, const char *const *allowed)
{
    if (!cJSON_IsObject(object)) return false;
    for (const cJSON *a = object->child; a; a = a->next) {
        if (!a->string) return false;
        bool known = false;
        for (unsigned i = 0; allowed[i]; i++) if (!strcmp(a->string, allowed[i])) known = true;
        if (!known) return false;
        for (const cJSON *b = a->next; b; b = b->next) if (b->string && !strcmp(a->string, b->string)) return false;
    }
    return true;
}
static bool text(const cJSON *root, const char *name, char *out, size_t capacity)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(value) || !value->valuestring || !value->valuestring[0] || strlen(value->valuestring) >= capacity) return false;
    memcpy(out, value->valuestring, strlen(value->valuestring) + 1);
    return true;
}
static bool bounded(const char *json, size_t length)
{
    if (memchr(json, 0, length)) return false;
    bool quoted = false, escaped = false; unsigned depth = 0;
    for (size_t i = 0; i < length; i++) {
        char c = json[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') {
                if (i + 5 < length && json[i + 1] == 'u' && !memcmp(json + i + 2, "0000", 4)) return false;
                escaped = true;
            } else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > 2) return false; }
        else if (c == '}' || c == ']') { if (!depth) return false; depth--; }
    }
    return !depth && !quoted;
}
bool quota_usb_parse(const char *frame, size_t length, quota_usb_request_t *request, const char **error)
{
    if (error) *error = "invalid_frame";
    if (!request) return false;
    memset(request, 0, sizeof(*request)); memcpy(request->request_id, "00000000", 9);
    if (!frame || length < 6 || length > QUOTA_MAX_PROVISION_FRAME_BYTES || memcmp(frame, "@AIQ:", 5)) return false;
    const char *json = frame + 5, *end = NULL;
    if (!bounded(json, length - 5)) return false;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length - 5, &end, false);
    if (!root || !end) { quota_portal_clear_json(root); cJSON_Delete(root); return false; }
    while (end < frame + length && (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t')) end++;
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "v");
    bool valid = end == frame + length && cJSON_IsNumber(version);
    char op[32] = {0};
    if (valid && text(root, "request_id", request->request_id, sizeof(request->request_id)) && hex(request->request_id, 8)) {
        if (version->valuedouble != 2) { if (error) *error = "unsupported_version"; valid = false; }
    } else { memcpy(request->request_id, "00000000", 9); valid = false; }
    if (valid) valid = text(root, "op", op, sizeof(op));
    static const char *const open_keys[] = {"v", "op", "request_id", NULL};
    static const char *const state_keys[] = {"v", "op", "request_id", "session_id", NULL};
    static const char *const command_keys[] = {"v", "op", "request_id", "session_id", "body", NULL};
    if (valid && !strcmp(op, "session_open")) { request->op = QUOTA_USB_OPEN; valid = keys(root, open_keys); }
    else if (valid) {
        valid = text(root, "session_id", request->session_id, sizeof(request->session_id)) && hex(request->session_id, QUOTA_USB_SESSION_BYTES);
        if (!strcmp(op, "state_get")) { request->op = QUOTA_USB_STATE; valid = valid && keys(root, state_keys); }
        else if (!strcmp(op, "command")) {
            request->op = QUOTA_USB_COMMAND; valid = valid && keys(root, command_keys);
            const cJSON *body = cJSON_GetObjectItemCaseSensitive(root, "body");
            char *serialized = valid && cJSON_IsObject(body) ? cJSON_PrintUnformatted(body) : NULL;
            valid = serialized && quota_portal_parse_command(serialized, strlen(serialized), &request->command) && !strcmp(request->request_id, request->command.request_id);
            if (serialized) { quota_portable_clear_secret(serialized, strlen(serialized)); cJSON_free(serialized); }
        } else { valid = false; if (error) *error = "unsupported_operation"; }
    }
    quota_portal_clear_json(root); cJSON_Delete(root);
    if (valid && error) *error = NULL;
    return valid;
}
