#include "quota_json.h"

#include "cJSON.h"

#include <string.h>

bool quota_json_is_lower_hex(const char *value, size_t length)
{
    if (value == NULL || strlen(value) != length)
        return false;
    for (size_t i = 0; i < length; i++)
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f')))
            return false;
    return true;
}

bool quota_json_unique_keys(const cJSON *object)
{
    if (!cJSON_IsObject(object))
        return false;
    for (const cJSON *a = object->child; a != NULL; a = a->next) {
        if (a->string == NULL)
            return false;
        for (const cJSON *b = a->next; b != NULL; b = b->next)
            if (b->string != NULL && strcmp(a->string, b->string) == 0)
                return false;
    }
    return true;
}

bool quota_json_keys_allowed(const cJSON *object, const char *const *allowed)
{
    if (!quota_json_unique_keys(object))
        return false;
    for (const cJSON *item = object->child; item != NULL; item = item->next) {
        bool known = false;
        for (size_t i = 0; allowed[i] != NULL && !known; i++)
            known = strcmp(item->string, allowed[i]) == 0;
        if (!known)
            return false;
    }
    return true;
}

bool quota_json_text_bounded(const char *json, size_t length)
{
    if (memchr(json, 0, length) != NULL)
        return false;
    bool quoted = false, escaped = false;
    unsigned depth = 0;
    for (size_t i = 0; i < length; i++) {
        char ch = json[i];
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (ch == '\\') {
                if (i + 5 < length && json[i + 1] == 'u' && memcmp(json + i + 2, "0000", 4) == 0)
                    return false;
                escaped = true;
            } else if (ch == '"')
                quoted = false;
        } else if (ch == '"')
            quoted = true;
        else if (ch == '{' || ch == '[') {
            if (++depth > 2)
                return false;
        } else if (ch == '}' || ch == ']') {
            if (depth == 0)
                return false;
            depth--;
        }
    }
    return depth == 0 && !quoted;
}
