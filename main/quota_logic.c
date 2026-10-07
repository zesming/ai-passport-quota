#include "quota_logic.h"

#include "cJSON.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool is_hex_lower(char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
}

static bool ascii_has_control(const char *text, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 0x20 || ch == 0x7f) return true;
    }
    return false;
}

bool quota_utf8_is_valid(const char *text, size_t length)
{
    if (text == NULL) return false;
    const unsigned char *bytes = (const unsigned char *)text;
    size_t i = 0;
    while (i < length) {
        uint32_t codepoint;
        unsigned char first = bytes[i++];
        if (first <= 0x7f) continue;
        if (first >= 0xc2 && first <= 0xdf) {
            if (i >= length || (bytes[i] & 0xc0) != 0x80) return false;
            codepoint = ((uint32_t)(first & 0x1f) << 6) | (bytes[i++] & 0x3f);
        } else if (first >= 0xe0 && first <= 0xef) {
            if (length - i < 2 || (bytes[i] & 0xc0) != 0x80 ||
                (bytes[i + 1] & 0xc0) != 0x80) return false;
            if ((first == 0xe0 && bytes[i] < 0xa0) ||
                (first == 0xed && bytes[i] >= 0xa0)) return false;
            codepoint = ((uint32_t)(first & 0x0f) << 12) |
                        ((uint32_t)(bytes[i] & 0x3f) << 6) |
                        (bytes[i + 1] & 0x3f);
            i += 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            if (length - i < 3 || (bytes[i] & 0xc0) != 0x80 ||
                (bytes[i + 1] & 0xc0) != 0x80 ||
                (bytes[i + 2] & 0xc0) != 0x80) return false;
            if ((first == 0xf0 && bytes[i] < 0x90) ||
                (first == 0xf4 && bytes[i] >= 0x90)) return false;
            codepoint = ((uint32_t)(first & 0x07) << 18) |
                        ((uint32_t)(bytes[i] & 0x3f) << 12) |
                        ((uint32_t)(bytes[i + 1] & 0x3f) << 6) |
                        (bytes[i + 2] & 0x3f);
            i += 3;
        } else {
            return false;
        }
        if (codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

bool quota_id_is_valid(const char *id)
{
    if (id == NULL || strlen(id) != QUOTA_ACCOUNT_ID_BYTES) return false;
    for (size_t i = 0; i < QUOTA_ACCOUNT_ID_BYTES; i++) {
        if (!is_hex_lower(id[i])) return false;
    }
    return true;
}

bool quota_url_is_private_ipv4(const char *url, char host_out[16])
{
    static const char prefix[] = "https://";
    static const char suffix[] = ":4318";
    if (url == NULL || host_out == NULL || strncmp(url, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }
    const char *host = url + sizeof(prefix) - 1;
    const char *port = strstr(host, suffix);
    if (port == NULL || port[sizeof(suffix) - 1] != '\0') return false;
    size_t host_length = (size_t)(port - host);
    if (host_length == 0 || host_length >= 16) return false;

    uint8_t octets[4] = {0};
    size_t octet_count = 0;
    const char *cursor = host;
    const char *host_end = port;
    while (cursor < host_end) {
        if (octet_count >= 4 || *cursor < '0' || *cursor > '9') return false;
        unsigned value = 0;
        size_t digits = 0;
        while (cursor < host_end && *cursor >= '0' && *cursor <= '9') {
            if (digits > 0 && value == 0) return false;
            value = value * 10 + (unsigned)(*cursor - '0');
            if (value > 255) return false;
            digits++;
            cursor++;
        }
        if (digits == 0) return false;
        octets[octet_count++] = (uint8_t)value;
        if (cursor == host_end) break;
        if (*cursor++ != '.' || cursor == host_end) return false;
    }
    if (octet_count != 4) return false;

    bool private_address = octets[0] == 10 ||
                           (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) ||
                           (octets[0] == 192 && octets[1] == 168);
    if (!private_address) return false;
    memcpy(host_out, host, host_length);
    host_out[host_length] = '\0';
    return true;
}

bool quota_pair_token_is_valid(const char *token)
{
    if (token == NULL || strlen(token) != QUOTA_PAIR_TOKEN_BYTES) return false;
    for (size_t i = 0; i < QUOTA_PAIR_TOKEN_BYTES; i++) {
        char ch = token[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')) return false;
    }
    return true;
}

void quota_copy_display_ascii(const char *source, char *destination, size_t capacity)
{
    if (destination == NULL || capacity == 0) return;
    destination[0] = '\0';
    if (source == NULL) return;

    const unsigned char *input = (const unsigned char *)source;
    size_t out = 0;
    for (size_t i = 0; input[i] != '\0' && out + 1 < capacity;) {
        unsigned char first = input[i];
        if (first >= 0x20 && first <= 0x7e) {
            destination[out++] = (char)first;
            i++;
        } else if (first < 0x80) {
            destination[out++] = '?';
            i++;
        } else {
            size_t sequence = first >= 0xf0 && first <= 0xf4 ? 4 :
                              first >= 0xe0 && first <= 0xef ? 3 :
                              first >= 0xc2 && first <= 0xdf ? 2 : 1;
            bool valid_sequence = true;
            for (size_t j = 1; j < sequence; j++) {
                if (input[i + j] == '\0' || (input[i + j] & 0xc0) != 0x80) {
                    valid_sequence = false;
                    break;
                }
            }
            destination[out++] = '?';
            i += valid_sequence ? sequence : 1;
        }
    }
    destination[out] = '\0';
}

void quota_frame_decoder_init(quota_frame_decoder_t *decoder)
{
    if (decoder != NULL) memset(decoder, 0, sizeof(*decoder));
}

quota_frame_result_t quota_frame_decoder_feed(quota_frame_decoder_t *decoder, char byte,
                                              const char **frame_out,
                                              size_t *frame_length_out)
{
    if (frame_out != NULL) *frame_out = NULL;
    if (frame_length_out != NULL) *frame_length_out = 0;
    if (decoder == NULL) return QUOTA_FRAME_PENDING;

    if (byte == '\n') {
        if (decoder->discarding_overlong_line) {
            decoder->discarding_overlong_line = false;
            decoder->length = 0;
            decoder->bytes[0] = '\0';
            return QUOTA_FRAME_TOO_LONG;
        }
        if (decoder->length > 0 && decoder->bytes[decoder->length - 1] == '\r') {
            decoder->length--;
        }
        if (decoder->length == 0) return QUOTA_FRAME_PENDING;
        decoder->bytes[decoder->length] = '\0';
        if (frame_out != NULL) *frame_out = decoder->bytes;
        if (frame_length_out != NULL) *frame_length_out = decoder->length;
        decoder->length = 0;
        return QUOTA_FRAME_COMPLETE;
    }
    if (decoder->discarding_overlong_line) return QUOTA_FRAME_PENDING;
    if (decoder->length >= QUOTA_MAX_PROVISION_FRAME_BYTES) {
        decoder->discarding_overlong_line = true;
        decoder->length = 0;
        decoder->bytes[0] = '\0';
        return QUOTA_FRAME_PENDING;
    }
    decoder->bytes[decoder->length++] = byte;
    return QUOTA_FRAME_PENDING;
}

static bool json_text_has_embedded_nul(const char *json, size_t length)
{
    if (memchr(json, '\0', length) != NULL) return true;
    for (size_t i = 0; i < length; i++) {
        if (json[i] != '\\' || i + 1 >= length) continue;
        if (json[i + 1] == 'u' && i + 5 < length &&
            json[i + 2] == '0' && json[i + 3] == '0' &&
            json[i + 4] == '0' && json[i + 5] == '0') return true;
        i++;
        if (json[i] == 'u' && i + 4 < length) i += 4;
    }
    return false;
}

static bool object_has_unique_keys(const cJSON *object)
{
    if (!cJSON_IsObject(object)) return false;
    for (const cJSON *first = object->child; first != NULL; first = first->next) {
        if (first->string == NULL) return false;
        for (const cJSON *later = first->next; later != NULL; later = later->next) {
            if (later->string != NULL && strcmp(first->string, later->string) == 0) {
                return false;
            }
        }
    }
    return true;
}

static const cJSON *json_field(const cJSON *object, const char *name)
{
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

static bool json_uint(const cJSON *item, uint64_t maximum, uint64_t *value_out)
{
    if (!cJSON_IsNumber(item) || value_out == NULL || !isfinite(item->valuedouble) ||
        item->valuedouble < 0 || item->valuedouble > (double)maximum ||
        floor(item->valuedouble) != item->valuedouble) return false;
    *value_out = (uint64_t)item->valuedouble;
    return true;
}

static bool json_copy_string(const cJSON *item, size_t maximum, bool allow_empty,
                             char *destination, size_t capacity)
{
    if (!cJSON_IsString(item) || item->valuestring == NULL) return false;
    size_t length = strlen(item->valuestring);
    if ((!allow_empty && length == 0) || length > maximum || length + 1 > capacity ||
        !quota_utf8_is_valid(item->valuestring, length)) return false;
    memcpy(destination, item->valuestring, length + 1);
    return true;
}

static bool json_whole_document(const char *json, size_t length, cJSON **root_out)
{
    if (json == NULL || root_out == NULL || length == 0 ||
        length > QUOTA_MAX_SNAPSHOT_BYTES || json_text_has_embedded_nul(json, length)) {
        return false;
    }
    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts((char *)json, length, &parse_end, false);
    if (root == NULL || parse_end == NULL) {
        cJSON_Delete(root);
        return false;
    }
    const char *end = json + length;
    while (parse_end < end && (*parse_end == ' ' || *parse_end == '\t' ||
                               *parse_end == '\r' || *parse_end == '\n')) parse_end++;
    if (parse_end != end) {
        cJSON_Delete(root);
        return false;
    }
    *root_out = root;
    return true;
}

static bool parse_optional_epoch(const cJSON *item, bool *present_out, uint64_t *value_out)
{
    if (item == NULL || present_out == NULL || value_out == NULL) return false;
    if (cJSON_IsNull(item)) {
        *present_out = false;
        *value_out = 0;
        return true;
    }
    if (!json_uint(item, UINT32_MAX, value_out)) return false;
    *present_out = true;
    return true;
}

static bool parse_window(const cJSON *item, quota_window_t *window)
{
    if (item == NULL || window == NULL) return false;
    if (cJSON_IsNull(item)) {
        memset(window, 0, sizeof(*window));
        return true;
    }
    if (!object_has_unique_keys(item)) return false;
    const cJSON *remaining = json_field(item, "remaining_percent");
    const cJSON *reset = json_field(item, "resets_at");
    uint64_t percent = 0;
    if (!json_uint(remaining, 100, &percent) || !parse_optional_epoch(reset,
            &window->has_resets_at, &window->resets_at)) return false;
    window->present = true;
    window->remaining_percent = (uint8_t)percent;
    return true;
}

static bool parse_account(const cJSON *item, quota_account_t *account)
{
    if (!object_has_unique_keys(item)) return false;
    const cJSON *id = json_field(item, "id");
    const cJSON *provider = json_field(item, "provider");
    const cJSON *email = json_field(item, "email");
    const cJSON *plan = json_field(item, "plan");
    const cJSON *status = json_field(item, "status");
    const cJSON *observed = json_field(item, "observed_at");
    const cJSON *five_hour = json_field(item, "five_hour");
    const cJSON *seven_day = json_field(item, "seven_day");

    if (!json_copy_string(id, QUOTA_ACCOUNT_ID_BYTES, false, account->id,
                          sizeof(account->id)) || !quota_id_is_valid(account->id) ||
        !json_copy_string(email, QUOTA_EMAIL_MAX_BYTES, true, account->email,
                          sizeof(account->email)) ||
        !json_copy_string(plan, QUOTA_PLAN_MAX_BYTES, true, account->plan,
                          sizeof(account->plan)) ||
        ascii_has_control(account->email, strlen(account->email)) ||
        ascii_has_control(account->plan, strlen(account->plan)) ||
        !parse_optional_epoch(observed, &account->has_observed_at, &account->observed_at) ||
        !parse_window(five_hour, &account->five_hour) ||
        !parse_window(seven_day, &account->seven_day)) return false;

    if (cJSON_IsString(provider) && strcmp(provider->valuestring, "codex") == 0) {
        account->provider = QUOTA_PROVIDER_CODEX;
    } else if (cJSON_IsString(provider) && strcmp(provider->valuestring, "claude") == 0) {
        account->provider = QUOTA_PROVIDER_CLAUDE;
    } else if (cJSON_IsString(provider) && strcmp(provider->valuestring, "deepseek") == 0) {
        account->provider = QUOTA_PROVIDER_DEEPSEEK;
        if (account->five_hour.present || account->seven_day.present) return false;
    } else {
        return false;
    }

    if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0) {
        account->status = QUOTA_STATUS_OK;
    } else if (cJSON_IsString(status) && strcmp(status->valuestring, "waiting") == 0) {
        account->status = QUOTA_STATUS_WAITING;
    } else if (cJSON_IsString(status) && strcmp(status->valuestring, "expired") == 0) {
        account->status = QUOTA_STATUS_EXPIRED;
    } else if (cJSON_IsString(status) && strcmp(status->valuestring, "error") == 0) {
        account->status = QUOTA_STATUS_ERROR;
    } else if (cJSON_IsString(status) && strcmp(status->valuestring, "unsupported") == 0) {
        account->status = QUOTA_STATUS_UNSUPPORTED;
    } else {
        return false;
    }
    return true;
}

static bool valid_balance_amount(const char *text)
{
    if (text == NULL) return false;
    const char *end = memchr(text, '\0', QUOTA_BALANCE_AMOUNT_BYTES + 1);
    if (end == NULL || end == text) return false;
    const char *p = text;
    if (*p == '-') p++;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') p++;
    if (*p == '.') {
        p++;
        if (*p < '0' || *p > '9') return false;
        while (*p >= '0' && *p <= '9') p++;
    }
    return *p == '\0';
}

bool quota_balance_is_valid(const quota_balance_t *balance)
{
    if (balance == NULL || balance->currency_count > QUOTA_BALANCE_CURRENCIES ||
        memchr(balance->label, '\0', sizeof(balance->label)) == NULL ||
        !quota_utf8_is_valid(balance->label, strlen(balance->label)) ||
        ascii_has_control(balance->label, strlen(balance->label))) return false;
    if (!balance->present) return balance->currency_count == 0;
    for (size_t i = 0; i < balance->currency_count; i++) {
        const quota_currency_balance_t *entry = &balance->balance_infos[i];
        if ((memcmp(entry->currency, "CNY", 4) != 0 && memcmp(entry->currency, "USD", 4) != 0) ||
            !valid_balance_amount(entry->total_balance) ||
            !valid_balance_amount(entry->granted_balance) ||
            !valid_balance_amount(entry->topped_up_balance) ||
            (i > 0 && memcmp(entry->currency, balance->balance_infos[0].currency, 4) == 0)) return false;
    }
    return true;
}

const quota_currency_balance_t *quota_balance_cny(const quota_balance_t *balance)
{
    if (balance == NULL || !balance->present ||
        balance->currency_count > QUOTA_BALANCE_CURRENCIES) return NULL;
    for (size_t i = 0; i < balance->currency_count; i++) {
        if (memcmp(balance->balance_infos[i].currency, "CNY", 4) == 0) {
            return &balance->balance_infos[i];
        }
    }
    return NULL;
}

static bool parse_balance(const cJSON *account, quota_balance_t *balance)
{
    const cJSON *label = json_field(account, "label");
    if (label != NULL && (!json_copy_string(label, QUOTA_PLAN_MAX_BYTES, true,
                                            balance->label, sizeof(balance->label)) ||
                           ascii_has_control(balance->label, strlen(balance->label)))) return false;
    const cJSON *item = json_field(account, "balance");
    if (item == NULL || cJSON_IsNull(item)) return true;
    if (!object_has_unique_keys(item)) return false;
    const cJSON *available = json_field(item, "is_available");
    const cJSON *infos = json_field(item, "balance_infos");
    if (!cJSON_IsBool(available) || !cJSON_IsArray(infos)) return false;
    int count = cJSON_GetArraySize(infos);
    if (count < 0 || count > QUOTA_BALANCE_CURRENCIES) return false;
    balance->present = true;
    balance->is_available = cJSON_IsTrue(available);
    balance->currency_count = (uint8_t)count;
    for (int i = 0; i < count; i++) {
        const cJSON *entry = cJSON_GetArrayItem(infos, i);
        quota_currency_balance_t *parsed = &balance->balance_infos[i];
        if (!object_has_unique_keys(entry) ||
            !json_copy_string(json_field(entry, "currency"), 3, false, parsed->currency, sizeof(parsed->currency)) ||
            !json_copy_string(json_field(entry, "total_balance"), QUOTA_BALANCE_AMOUNT_BYTES,
                              false, parsed->total_balance, sizeof(parsed->total_balance)) ||
            !json_copy_string(json_field(entry, "granted_balance"), QUOTA_BALANCE_AMOUNT_BYTES,
                              false, parsed->granted_balance, sizeof(parsed->granted_balance)) ||
            !json_copy_string(json_field(entry, "topped_up_balance"), QUOTA_BALANCE_AMOUNT_BYTES,
                              false, parsed->topped_up_balance, sizeof(parsed->topped_up_balance))) return false;
    }
    return quota_balance_is_valid(balance);
}

static bool parse_codex_extras(const cJSON *account, quota_codex_extras_t *extras)
{
    const cJSON *reset = json_field(account, "banked_reset");
    if (reset != NULL && !cJSON_IsNull(reset)) {
        if (!object_has_unique_keys(reset) ||
            !json_uint(json_field(reset, "available_count"), 9007199254740991ULL,
                       &extras->available_resets)) return false;
        extras->has_banked_reset = true;
        const cJSON *expiry = json_field(reset, "next_expires_at");
        if (expiry != NULL &&
            !parse_optional_epoch(expiry, &extras->has_next_reset_expiry,
                                  &extras->next_reset_expires_at)) return false;
    }
    const cJSON *credits = json_field(account, "credits");
    if (credits == NULL || cJSON_IsNull(credits)) return true;
    if (!object_has_unique_keys(credits)) return false;
    const cJSON *available = json_field(credits, "has_credits");
    const cJSON *unlimited = json_field(credits, "unlimited");
    const cJSON *balance = json_field(credits, "balance");
    if (!cJSON_IsBool(available) || !cJSON_IsBool(unlimited)) return false;
    if (balance != NULL && !cJSON_IsNull(balance) &&
        (!json_copy_string(balance, QUOTA_CREDITS_BALANCE_BYTES, false,
                           extras->credits_balance, sizeof(extras->credits_balance)) ||
         ascii_has_control(extras->credits_balance, strlen(extras->credits_balance)))) return false;
    extras->has_credits = cJSON_IsTrue(available) || cJSON_IsTrue(unlimited);
    extras->unlimited_credits = cJSON_IsTrue(unlimited);
    return true;
}

void quota_format_duration(uint64_t seconds, char *output, size_t capacity)
{
    if (output == NULL || capacity == 0) return;
    uint64_t hours = seconds / 3600;
    if (hours == 0) {
        snprintf(output, capacity, "<1h");
    } else {
        snprintf(output, capacity, "%llud %lluh", (unsigned long long)(hours / 24),
                 (unsigned long long)(hours % 24));
    }
}

void quota_format_reset_time(const quota_window_t *window, uint64_t now,
                             bool clock_synchronized, char *output, size_t capacity)
{
    if (output == NULL || capacity == 0) return;
    if (window == NULL || !window->present || !window->has_resets_at) {
        snprintf(output, capacity, "重置时间未知");
    } else if (!clock_synchronized) {
        snprintf(output, capacity, "时间待同步");
    } else if (now >= window->resets_at) {
        snprintf(output, capacity, "等待新数据");
    } else {
        char remaining[32];
        quota_format_duration(window->resets_at - now, remaining, sizeof(remaining));
        /* U+F021 is the refresh glyph in the built-in font fallback. */
        snprintf(output, capacity, "\xEF\x80\xA1 %s", remaining);
    }
}

bool quota_refresh_seconds_is_valid(uint64_t seconds)
{
    return seconds == 60 || seconds == 300 || seconds == 900 || seconds == 1800;
}

const uint16_t quota_screen_timeouts[QUOTA_SCREEN_TIMEOUT_COUNT] = {0, 30, 60, 120, 300, 600};

bool quota_screen_timeout_is_valid(uint64_t seconds)
{
    for (size_t i = 0; i < QUOTA_SCREEN_TIMEOUT_COUNT; i++) {
        if (seconds == quota_screen_timeouts[i]) return true;
    }
    return false;
}

void quota_copy_display_plan(const char *source, char *destination, size_t capacity)
{
    quota_copy_display_ascii(source, destination, capacity);
    if (destination != NULL && capacity > 0 && destination[0] >= 'a' && destination[0] <= 'z') {
        destination[0] = (char)(destination[0] - 'a' + 'A');
    }
}

static bool parse_settings_object(const cJSON *settings, quota_settings_t *parsed)
{
    if (!object_has_unique_keys(settings)) return false;
    const cJSON *refresh = json_field(settings, "refresh_seconds");
    const cJSON *automatic = json_field(settings, "auto_refresh");
    const cJSON *screen_timeout = json_field(settings, "screen_timeout_seconds");
    uint64_t seconds = 0;
    uint64_t timeout = 0;
    if (!json_uint(refresh, 1800, &seconds) || !quota_refresh_seconds_is_valid(seconds) ||
        !cJSON_IsBool(automatic) ||
        (screen_timeout != NULL && (!json_uint(screen_timeout, 600, &timeout) ||
                                   !quota_screen_timeout_is_valid(timeout)))) return false;
    parsed->refresh_seconds = (uint16_t)seconds;
    parsed->auto_refresh = cJSON_IsTrue(automatic);
    parsed->has_screen_timeout_seconds = screen_timeout != NULL;
    parsed->screen_timeout_seconds = (uint16_t)timeout;
    return true;
}

bool quota_parse_settings_ack(const char *json, size_t json_length, quota_settings_t *settings)
{
    if (settings == NULL || json_length > QUOTA_MAX_SNAPSHOT_BYTES) return false;
    cJSON *root = NULL;
    if (!json_whole_document(json, json_length, &root)) return false;
    uint64_t version = 0;
    quota_settings_t parsed = {0};
    bool valid = object_has_unique_keys(root) &&
        json_uint(json_field(root, "v"), 1, &version) && version == 1 &&
        parse_settings_object(json_field(root, "settings"), &parsed);
    cJSON_Delete(root);
    if (valid) *settings = parsed;
    return valid;
}

bool quota_parse_snapshot(const char *json, size_t json_length, quota_snapshot_t *snapshot)
{
    if (snapshot == NULL || json_length > QUOTA_MAX_SNAPSHOT_BYTES) return false;
    cJSON *root = NULL;
    if (!json_whole_document(json, json_length, &root)) return false;

    bool valid = object_has_unique_keys(root);
    quota_snapshot_t parsed = {0};
    uint64_t version = 0;
    const cJSON *version_field = json_field(root, "v");
    const cJSON *server_time = json_field(root, "server_time");
    const cJSON *revision = json_field(root, "revision");
    const cJSON *settings = json_field(root, "settings");
    const cJSON *accounts = json_field(root, "accounts");
    if (!json_uint(version_field, 1, &version) || version != 1 ||
        !json_uint(server_time, UINT32_MAX, &parsed.server_time) ||
        !json_uint(revision, 9007199254740991ULL, &parsed.revision) ||
        !object_has_unique_keys(settings) || !cJSON_IsArray(accounts)) valid = false;

    if (valid) {
        quota_settings_t parsed_settings = {0};
        if (!parse_settings_object(settings, &parsed_settings)) {
            valid = false;
        } else {
            parsed.refresh_seconds = parsed_settings.refresh_seconds;
            parsed.auto_refresh = parsed_settings.auto_refresh;
            parsed.has_screen_timeout_seconds = parsed_settings.has_screen_timeout_seconds;
            parsed.screen_timeout_seconds = parsed_settings.screen_timeout_seconds;
        }
    }

    if (valid) {
        int count = cJSON_GetArraySize(accounts);
        if (count < 0 || count > QUOTA_MAX_ACCOUNTS) {
            valid = false;
        } else {
            parsed.account_count = (uint8_t)count;
            for (int i = 0; i < count && valid; i++) {
                const cJSON *entry = cJSON_GetArrayItem(accounts, i);
                if (!parse_account(entry, &parsed.accounts[i])) {
                    valid = false;
                    break;
                }
                if (parsed.accounts[i].provider == QUOTA_PROVIDER_DEEPSEEK &&
                    !parse_balance(entry, &parsed.balances[i])) {
                    valid = false;
                    break;
                }
                if (parsed.accounts[i].provider == QUOTA_PROVIDER_CODEX &&
                    !parse_codex_extras(entry, &parsed.codex_extras[i])) {
                    valid = false;
                    break;
                }
                for (int previous = 0; previous < i; previous++) {
                    if (strcmp(parsed.accounts[previous].id, parsed.accounts[i].id) == 0) {
                        valid = false;
                        break;
                    }
                }
            }
        }
    }
    cJSON_Delete(root);
    if (!valid) return false;
    *snapshot = parsed;
    return true;
}

static void set_error(const char **error_out, const char *error)
{
    if (error_out != NULL) *error_out = error;
}

static bool json_request_id(const cJSON *root, char request_id_out[9])
{
    const cJSON *request_id = json_field(root, "request_id");
    if (!cJSON_IsString(request_id) || request_id->valuestring == NULL ||
        strlen(request_id->valuestring) != 8) return false;
    for (size_t i = 0; i < 8; i++) {
        if (!is_hex_lower(request_id->valuestring[i])) return false;
    }
    memcpy(request_id_out, request_id->valuestring, 9);
    return true;
}

static void clear_provision_bytes(void *buffer, size_t length)
{
    volatile unsigned char *bytes = buffer;
    while (length--) *bytes++ = 0;
}

static void clear_provision_json(cJSON *json)
{
    for (cJSON *item = json; item; item = item->next) {
        if (item->child) clear_provision_json(item->child);
        if (item->valuestring) clear_provision_bytes(item->valuestring, strlen(item->valuestring));
        if (item->string) clear_provision_bytes(item->string, strlen(item->string));
    }
}

bool quota_parse_provision_frame(const char *frame, size_t frame_length,
                                 quota_device_config_t *config,
                                 char request_id_out[9], const char **error_code_out)
{
    static const char prefix[] = "@AIQ:";
    if (request_id_out != NULL) memcpy(request_id_out, "00000000", 9);
    set_error(error_code_out, "invalid_frame");
    if (frame == NULL || config == NULL || frame_length < sizeof(prefix) - 1 ||
        frame_length > QUOTA_MAX_PROVISION_FRAME_BYTES ||
        memcmp(frame, prefix, sizeof(prefix) - 1) != 0) return false;

    const char *json = frame + sizeof(prefix) - 1;
    size_t json_length = frame_length - (sizeof(prefix) - 1);
    if (json_length == 0 || json_text_has_embedded_nul(json, json_length)) {
        set_error(error_code_out, "invalid_json");
        return false;
    }
    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts((char *)json, json_length, &parse_end, false);
    if (root == NULL || parse_end == NULL) {
        clear_provision_json(root);
        cJSON_Delete(root);
        set_error(error_code_out, "invalid_json");
        return false;
    }
    const char *end = json + json_length;
    while (parse_end < end && (*parse_end == ' ' || *parse_end == '\t' ||
                               *parse_end == '\r' || *parse_end == '\n')) parse_end++;
    if (parse_end != end || !object_has_unique_keys(root)) {
        clear_provision_json(root);
        cJSON_Delete(root);
        set_error(error_code_out, "invalid_json");
        return false;
    }

    char request_id[9] = "00000000";
    bool has_request_id = json_request_id(root, request_id);
    if (request_id_out != NULL && has_request_id) memcpy(request_id_out, request_id, 9);
    uint64_t version = 0;
    const cJSON *version_field = json_field(root, "v");
    const cJSON *operation = json_field(root, "op");
    if (!has_request_id) {
        set_error(error_code_out, "invalid_request");
    } else if (!json_uint(version_field, 1, &version) || version != 1) {
        set_error(error_code_out, "unsupported_version");
    } else if (!cJSON_IsString(operation) || strcmp(operation->valuestring, "configure") != 0) {
        set_error(error_code_out, "unsupported_operation");
    } else {
        quota_device_config_t parsed = {0};
        uint64_t server_time = 0;
        const cJSON *ssid = json_field(root, "ssid");
        const cJSON *password = json_field(root, "password");
        const cJSON *base_url = json_field(root, "base_url");
        const cJSON *pair_token = json_field(root, "pair_token");
        const cJSON *cert = json_field(root, "server_cert_pem");
        const cJSON *time = json_field(root, "server_time");
        bool strings_valid = json_copy_string(ssid, QUOTA_SSID_MAX_BYTES, false,
                                               parsed.ssid, sizeof(parsed.ssid)) &&
                             json_copy_string(password, QUOTA_PASSWORD_MAX_BYTES, true,
                                               parsed.password, sizeof(parsed.password)) &&
                             json_copy_string(base_url, QUOTA_BASE_URL_MAX_BYTES, false,
                                               parsed.base_url, sizeof(parsed.base_url)) &&
                             json_copy_string(pair_token, QUOTA_PAIR_TOKEN_BYTES, false,
                                               parsed.pair_token, sizeof(parsed.pair_token)) &&
                             json_copy_string(cert, QUOTA_CERT_MAX_BYTES, false,
                                               parsed.server_cert_pem,
                                               sizeof(parsed.server_cert_pem));
        char host[16];
        if (!strings_valid || strlen(parsed.ssid) > QUOTA_SSID_MAX_BYTES ||
            strlen(parsed.password) > QUOTA_PASSWORD_MAX_BYTES ||
            strlen(parsed.base_url) > QUOTA_BASE_URL_MAX_BYTES ||
            strlen(parsed.server_cert_pem) > QUOTA_CERT_MAX_BYTES ||
            !quota_url_is_private_ipv4(parsed.base_url, host) ||
            !quota_pair_token_is_valid(parsed.pair_token) ||
            !json_uint(time, UINT32_MAX, &server_time) || server_time == 0 ||
            strncmp(parsed.server_cert_pem, "-----BEGIN CERTIFICATE-----", 27) != 0 ||
            strstr(parsed.server_cert_pem, "-----END CERTIFICATE-----") == NULL) {
            set_error(error_code_out, "invalid_config");
        } else {
            memcpy(parsed.request_id, request_id, sizeof(parsed.request_id));
            parsed.server_time = server_time;
            parsed.refresh_seconds = QUOTA_REFRESH_DEFAULT_SECONDS;
            parsed.auto_refresh = true;
            *config = parsed;
            clear_provision_bytes(&parsed, sizeof(parsed));
            set_error(error_code_out, NULL);
            clear_provision_json(root);
            cJSON_Delete(root);
            return true;
        }
        clear_provision_bytes(&parsed, sizeof(parsed));
    }

    clear_provision_json(root);
    cJSON_Delete(root);
    return false;
}

bool quota_data_is_stale(uint64_t now, bool has_observed_at, uint64_t observed_at,
                         uint16_t refresh_seconds)
{
    if (!has_observed_at || now < observed_at) return true;
    uint64_t threshold = (uint64_t)refresh_seconds * 2;
    if (threshold < 900) threshold = 900;
    return now - observed_at > threshold;
}

bool quota_pairing_window_active(bool setup_screen_open, uint64_t now_ms,
                                 uint64_t opened_at_ms)
{
    return setup_screen_open && now_ms >= opened_at_ms &&
           now_ms - opened_at_ms < QUOTA_PAIRING_WINDOW_MS;
}

quota_metric_state_t quota_metric_state(const quota_window_t *window, uint64_t now)
{
    if (window == NULL || !window->present) return QUOTA_METRIC_UNAVAILABLE;
    if (window->has_resets_at && now >= window->resets_at) {
        return QUOTA_METRIC_WAITING_FOR_SOURCE;
    }
    return QUOTA_METRIC_VALUE;
}

void quota_display_tick(quota_display_state_t *display, uint64_t now_ms,
                        uint16_t timeout_seconds, bool pairing_active)
{
    if (display == NULL) return;
    /* Start a fresh idle period when pairing closes, including on clock rollback. */
    if (pairing_active || now_ms < display->last_input_ms) display->last_input_ms = now_ms;
    if (!pairing_active && timeout_seconds != 0 &&
        quota_screen_timeout_is_valid(timeout_seconds) &&
        now_ms - display->last_input_ms >= (uint64_t)timeout_seconds * 1000) {
        display->sleeping = true;
    }
}

bool quota_display_handle_key(quota_display_state_t *display, uint64_t now_ms,
                              quota_key_event_t event, bool down_key)
{
    if (display == NULL) return false;
    display->last_input_ms = now_ms;
    if (display->sleeping) {
        display->sleeping = false;
        display->consume_wake_gesture = event == QUOTA_KEY_PRESS;
        return false;
    }
    if (display->consume_wake_gesture) {
        if (event != QUOTA_KEY_PRESS) display->consume_wake_gesture = false;
        return false;
    }
    if (event == QUOTA_KEY_LONG && down_key) {
        display->sleeping = true;
        return false;
    }
    return event != QUOTA_KEY_PRESS;
}

void quota_navigation_init(quota_navigation_t *navigation, bool configured,
                           uint16_t refresh_seconds, bool auto_refresh,
                           uint16_t screen_timeout_seconds, uint8_t account_count)
{
    if (navigation == NULL) return;
    memset(navigation, 0, sizeof(*navigation));
    navigation->configured = configured;
    navigation->refresh_seconds = quota_refresh_seconds_is_valid(refresh_seconds)
                                ? refresh_seconds : QUOTA_REFRESH_DEFAULT_SECONDS;
    navigation->auto_refresh = auto_refresh;
    navigation->screen_timeout_seconds = quota_screen_timeout_is_valid(screen_timeout_seconds)
        ? screen_timeout_seconds : QUOTA_SCREEN_TIMEOUT_DEFAULT_SECONDS;
    navigation->screen = configured ? QUOTA_SCREEN_HOME : QUOTA_SCREEN_PHONE;
    navigation->setup_return_screen = QUOTA_SCREEN_HOME;
    if (account_count == 0) navigation->selected_account = 0;
}

void quota_navigation_sync_settings(quota_navigation_t *navigation,
                                     uint16_t refresh_seconds, bool auto_refresh,
                                     uint16_t screen_timeout_seconds)
{
    if (navigation == NULL || !quota_refresh_seconds_is_valid(refresh_seconds)) return;
    navigation->refresh_seconds = refresh_seconds;
    navigation->auto_refresh = auto_refresh;
    if (quota_screen_timeout_is_valid(screen_timeout_seconds)) {
        navigation->screen_timeout_seconds = screen_timeout_seconds;
    }
}

static const uint16_t quota_refresh_intervals[] = {60, 300, 900, 1800};

static uint8_t wrap_index(uint8_t current, int direction, uint8_t count)
{
    if (count == 0) return 0;
    if (direction < 0) return current == 0 ? (uint8_t)(count - 1) : (uint8_t)(current - 1);
    return (uint8_t)((current + 1) % count);
}

quota_action_t quota_navigation_handle(quota_navigation_t *navigation,
                                       quota_input_t input, uint8_t account_count)
{
    if (navigation == NULL || account_count > QUOTA_MAX_ACCOUNTS) return QUOTA_ACTION_NONE;
    if (input == QUOTA_INPUT_UP || input == QUOTA_INPUT_DOWN) {
        int direction = input == QUOTA_INPUT_UP ? -1 : 1;
        if (navigation->screen == QUOTA_SCREEN_HOME && account_count > 1) {
            navigation->selected_account = wrap_index(navigation->selected_account,
                                                      direction, account_count);
            return QUOTA_ACTION_PERSIST_SELECTION;
        }
        if (navigation->screen == QUOTA_SCREEN_SETTINGS) {
            navigation->settings_focus = wrap_index(navigation->settings_focus,
                                                    direction, 6);
        } else if (navigation->screen == QUOTA_SCREEN_ACCOUNTS) {
            navigation->account_focus = wrap_index(navigation->account_focus,
                                                   direction, (uint8_t)(account_count + 1));
        } else if (navigation->screen == QUOTA_SCREEN_INTERVAL) {
            navigation->interval_focus = wrap_index(navigation->interval_focus,
                                                    direction, 5);
        } else if (navigation->screen == QUOTA_SCREEN_SLEEP) {
            navigation->sleep_focus = wrap_index(navigation->sleep_focus,
                                                 direction, QUOTA_SCREEN_TIMEOUT_COUNT);
        } else if (navigation->screen == QUOTA_SCREEN_PHONE) {
            navigation->phone_step = wrap_index(navigation->phone_step, direction, 3);
        } else if (navigation->screen == QUOTA_SCREEN_DEVICE_SETTINGS) {
            navigation->device_settings_focus = wrap_index(
                navigation->device_settings_focus, direction, 2);
        }
        return QUOTA_ACTION_NONE;
    }

    if (input == QUOTA_INPUT_OK_LONG) {
        if (navigation->screen == QUOTA_SCREEN_HOME) {
            navigation->screen = QUOTA_SCREEN_SETTINGS;
        } else if (navigation->screen == QUOTA_SCREEN_SETUP) {
            navigation->screen = navigation->setup_return_screen;
        } else if (navigation->screen == QUOTA_SCREEN_PHONE) {
            navigation->screen = navigation->setup_return_screen;
            return QUOTA_ACTION_CLOSE_PHONE;
        } else if (navigation->screen == QUOTA_SCREEN_AUTH) {
            navigation->screen = QUOTA_SCREEN_ACCOUNTS;
            return QUOTA_ACTION_CANCEL_AUTH;
        } else if (navigation->screen == QUOTA_SCREEN_INTERVAL ||
                   navigation->screen == QUOTA_SCREEN_SLEEP ||
                   navigation->screen == QUOTA_SCREEN_NETWORK ||
                   navigation->screen == QUOTA_SCREEN_ACCOUNTS) {
            navigation->screen = QUOTA_SCREEN_SETTINGS;
        } else if (navigation->screen == QUOTA_SCREEN_DEVICE_SETTINGS) {
            navigation->screen = QUOTA_SCREEN_SETTINGS;
        } else {
            navigation->screen = QUOTA_SCREEN_HOME;
        }
        return QUOTA_ACTION_NONE;
    }

    if (input != QUOTA_INPUT_OK_SHORT) return QUOTA_ACTION_NONE;
    switch (navigation->screen) {
        case QUOTA_SCREEN_HOME:
            return navigation->configured ? QUOTA_ACTION_REFRESH : QUOTA_ACTION_NONE;
        case QUOTA_SCREEN_SETTINGS:
            if (navigation->settings_focus == 0) {
                navigation->screen = QUOTA_SCREEN_ACCOUNTS;
                navigation->account_focus = 0;
            } else if (navigation->settings_focus == 1) {
                navigation->screen = QUOTA_SCREEN_INTERVAL;
                navigation->interval_focus = 0;
                for (size_t i = 0; navigation->auto_refresh && i < 4; i++) {
                    if (quota_refresh_intervals[i] == navigation->refresh_seconds) {
                        navigation->interval_focus = (uint8_t)(i + 1);
                        break;
                    }
                }
            } else if (navigation->settings_focus == 2) {
                return QUOTA_ACTION_REFRESH;
            } else if (navigation->settings_focus == 3) {
                navigation->screen = QUOTA_SCREEN_SLEEP;
                navigation->sleep_focus = 0;
                for (size_t i = 0; i < QUOTA_SCREEN_TIMEOUT_COUNT; i++) {
                    if (quota_screen_timeouts[i] == navigation->screen_timeout_seconds) {
                        navigation->sleep_focus = (uint8_t)i;
                        break;
                    }
                }
            } else if (navigation->settings_focus == 4) {
                navigation->screen = QUOTA_SCREEN_NETWORK;
            } else {
                navigation->device_settings_focus = 0;
                navigation->screen = QUOTA_SCREEN_DEVICE_SETTINGS;
            }
            return QUOTA_ACTION_NONE;
        case QUOTA_SCREEN_ACCOUNTS:
            if (navigation->account_focus < account_count) {
                navigation->selected_account = navigation->account_focus;
                navigation->screen = QUOTA_SCREEN_HOME;
                return QUOTA_ACTION_PERSIST_SELECTION;
            }
            navigation->setup_return_screen = QUOTA_SCREEN_ACCOUNTS;
            navigation->screen = QUOTA_SCREEN_PHONE;
            navigation->phone_step = 0;
            return QUOTA_ACTION_OPEN_PHONE;
        case QUOTA_SCREEN_INTERVAL: {
            if (navigation->interval_focus == 0) {
                navigation->auto_refresh = !navigation->auto_refresh;
            } else {
                navigation->refresh_seconds = quota_refresh_intervals[navigation->interval_focus - 1];
                navigation->auto_refresh = true;
            }
            navigation->screen = QUOTA_SCREEN_SETTINGS;
            return QUOTA_ACTION_APPLY_SETTINGS;
        }
        case QUOTA_SCREEN_SETUP:
            return QUOTA_ACTION_NONE;
        case QUOTA_SCREEN_NETWORK:
            return QUOTA_ACTION_NONE;
        case QUOTA_SCREEN_DEVICE_SETTINGS:
            navigation->phone_step = 0;
            navigation->setup_return_screen = QUOTA_SCREEN_DEVICE_SETTINGS;
            if (navigation->device_settings_focus == 0) {
                navigation->screen = QUOTA_SCREEN_PHONE;
                return QUOTA_ACTION_OPEN_PHONE;
            }
            navigation->screen = QUOTA_SCREEN_SETUP;
            return QUOTA_ACTION_NONE;
        case QUOTA_SCREEN_PHONE:
            navigation->phone_step = wrap_index(navigation->phone_step, 1, 3);
            return QUOTA_ACTION_RENEW_PHONE;
        case QUOTA_SCREEN_AUTH:
            navigation->setup_return_screen = QUOTA_SCREEN_AUTH;
            navigation->screen = QUOTA_SCREEN_SETUP;
            return QUOTA_ACTION_NONE;
        case QUOTA_SCREEN_SLEEP:
            if (navigation->sleep_focus >= QUOTA_SCREEN_TIMEOUT_COUNT) return QUOTA_ACTION_NONE;
            navigation->screen_timeout_seconds = quota_screen_timeouts[navigation->sleep_focus];
            navigation->screen = QUOTA_SCREEN_SETTINGS;
            return QUOTA_ACTION_APPLY_SETTINGS;
        default:
            return QUOTA_ACTION_NONE;
    }
}

int quota_find_account_by_id(const quota_snapshot_t *snapshot, const char *id)
{
    if (snapshot == NULL || id == NULL || !quota_id_is_valid(id)) return -1;
    for (size_t i = 0; i < snapshot->account_count && i < QUOTA_MAX_ACCOUNTS; i++) {
        if (strcmp(snapshot->accounts[i].id, id) == 0) return (int)i;
    }
    return -1;
}
