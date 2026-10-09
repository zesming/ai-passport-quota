#pragma once

#include <stdbool.h>
#include <stddef.h>

struct cJSON;

/* True for exactly `length` lowercase hexadecimal digits (a NUL-terminated string). */
bool quota_json_is_lower_hex(const char *value, size_t length);
/* The object has only named members and no member name appears twice. */
bool quota_json_unique_keys(const struct cJSON *object);
/* A uniquely keyed object whose member names are all in the NULL-terminated `allowed` array. */
bool quota_json_keys_allowed(const struct cJSON *object, const char *const *allowed);
/* Reject text before parsing: embedded NUL bytes or \u0000 escapes, nesting deeper than two
 * containers, unbalanced brackets and unterminated strings. */
bool quota_json_text_bounded(const char *json, size_t length);
