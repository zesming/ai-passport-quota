#include "quota_direct_logic.h"
#include "cJSON.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

void quota_direct_secure_clear(void *memory, size_t size)
{
    volatile unsigned char *bytes = memory;
    while (size--) *bytes++ = 0;
}

bool quota_direct_token_is_safe(const char *value, size_t maximum)
{
    if (!value || !value[0]) return false;
    size_t length = 0;
    for (; length <= maximum && value[length]; ++length) {
        if ((unsigned char)value[length] < 0x21 || (unsigned char)value[length] > 0x7e)
            return false;
    }
    return length > 0 && length <= maximum;
}

static bool valid_utf8(const unsigned char *bytes, size_t length)
{
    for (size_t i = 0; i < length;) {
        unsigned char lead = bytes[i++];
        if (lead < 0x80) { if (!lead) return false; continue; }
        unsigned count; uint32_t value, minimum;
        if (lead >= 0xc2 && lead <= 0xdf) { count = 1; value = lead & 31; minimum = 0x80; }
        else if (lead >= 0xe0 && lead <= 0xef) { count = 2; value = lead & 15; minimum = 0x800; }
        else if (lead >= 0xf0 && lead <= 0xf4) { count = 3; value = lead & 7; minimum = 0x10000; }
        else return false;
        if (length - i < count) return false;
        while (count--) {
            unsigned char next = bytes[i++];
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            return false;
    }
    return true;
}

static bool unique_tree(const cJSON *node, unsigned depth, unsigned *nodes)
{
    if (!node || depth > 24 || ++*nodes > 4096) return false;
    if (cJSON_IsObject(node)) {
        for (const cJSON *item = node->child; item; item = item->next) {
            if (!item->string) return false;
            for (const cJSON *next = item->next; next; next = next->next)
                if (next->string && strcmp(item->string, next->string) == 0) return false;
        }
    }
    for (const cJSON *item = node->child; item; item = item->next)
        if (!unique_tree(item, depth + 1, nodes)) return false;
    return true;
}

static cJSON *parse_json(const char *body, size_t length)
{
    if (!body || !length || length > QUOTA_DIRECT_BODY_BYTES ||
        !valid_utf8((const unsigned char *)body, length)) return NULL;
    bool string = false, escaped = false; unsigned depth = 0;
    for (size_t i = 0; i < length; ++i) {
        char byte = body[i];
        if (escaped) {
            if (byte == 'u' && i + 4 < length && !memcmp(body + i + 1, "0000", 4))
                return NULL;
            escaped = false; continue;
        }
        if (string && byte == '\\') { escaped = true; continue; }
        if (byte == '"') { string = !string; continue; }
        if (!string && (byte == '{' || byte == '[')) { if (++depth > 24) return NULL; }
        if (!string && (byte == '}' || byte == ']')) { if (!depth) return NULL; --depth; }
    }
    const char *end = NULL;
    cJSON *json = cJSON_ParseWithLengthOpts(body, length, &end, false);
    bool complete = json && end && end >= body && end <= body + length;
    for (const char *tail = complete ? end : body + length; tail < body + length; ++tail)
        if (*tail != ' ' && *tail != '\t' && *tail != '\r' && *tail != '\n') complete = false;
    unsigned nodes = 0;
    if (!complete || !cJSON_IsObject(json) || !unique_tree(json, 0, &nodes)) {
        cJSON_Delete(json); return NULL;
    }
    return json;
}

static const cJSON *field(const cJSON *root, const char *name)
{
    return cJSON_GetObjectItemCaseSensitive(root, name);
}

static bool uint_field(const cJSON *value, uint64_t max, uint64_t *result)
{
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
        value->valuedouble < 0 || value->valuedouble > (double)max ||
        floor(value->valuedouble) != value->valuedouble) return false;
    *result = (uint64_t)value->valuedouble;
    return true;
}

static bool copy_string(const cJSON *value, char *output, size_t capacity, bool empty)
{
    if (!cJSON_IsString(value) || !value->valuestring || !capacity) return false;
    size_t length = strlen(value->valuestring);
    if (length >= capacity || (!empty && !length)) return false;
    for (size_t i = 0; i < length; ++i)
        if ((unsigned char)value->valuestring[i] < 32 || value->valuestring[i] == 127) return false;
    memcpy(output, value->valuestring, length + 1); return true;
}

static bool optional_string(const cJSON *value, char *output, size_t capacity)
{
    if (!value || cJSON_IsNull(value)) { output[0] = 0; return true; }
    return copy_string(value, output, capacity, true);
}

static int base64_value(unsigned char value)
{
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '-') return 62;
    if (value == '_') return 63;
    return -1;
}

static cJSON *jwt_object(const char *jwt)
{
    if (!quota_direct_token_is_safe(jwt, QUOTA_DIRECT_ID_TOKEN_BYTES)) return NULL;
    const char *start = strchr(jwt, '.');
    if (!start || start == jwt) return NULL;
    const char *end = strchr(++start, '.');
    if (!end || end == start || !end[1] || strchr(end + 1, '.')) return NULL;
    size_t length = (size_t)(end - start);
    if (length % 4 == 1) return NULL;
    char *decoded = malloc(length * 3 / 4 + 2);
    if (!decoded) return NULL;
    uint32_t bits = 0; unsigned bit_count = 0; size_t count = 0;
    bool valid = true;
    for (size_t i = 0; i < length; ++i) {
        int value = base64_value((unsigned char)start[i]);
        if (value < 0) { valid = false; break; }
        bits = (bits << 6) | (uint32_t)value; bit_count += 6;
        if (bit_count >= 8) { bit_count -= 8; decoded[count++] = (char)(bits >> bit_count); }
    }
    if (bit_count && (bits & ((1u << bit_count) - 1u))) valid = false;
    decoded[count] = 0;
    cJSON *root = valid ? parse_json(decoded, count) : NULL;
    quota_direct_secure_clear(decoded, count); free(decoded);
    return root;
}

bool quota_direct_parse_identity(const char *jwt, quota_direct_identity_t *identity)
{
    if (!identity) return false;
    cJSON *root = jwt_object(jwt);
    if (!root) return false;
    quota_direct_identity_t parsed = {0};
    const cJSON *auth = field(root, "https://api.openai.com/auth");
    const cJSON *profile = field(root, "https://api.openai.com/profile");
    const cJSON *user = field(auth, "chatgpt_user_id");
    if (!user) user = field(auth, "user_id");
    if (!user) user = field(root, "sub");
    const cJSON *email = field(root, "email");
    if (!email) email = field(profile, "email");
    const cJSON *expiry = field(root, "exp");
    bool valid = cJSON_IsObject(auth) &&
        copy_string(field(auth, "chatgpt_account_id"), parsed.account_id, sizeof(parsed.account_id), false) &&
        copy_string(user, parsed.user_id, sizeof(parsed.user_id), false) &&
        quota_direct_token_is_safe(parsed.account_id, QUOTA_DIRECT_PROVIDER_ID_BYTES) &&
        quota_direct_token_is_safe(parsed.user_id, QUOTA_DIRECT_PROVIDER_ID_BYTES) &&
        optional_string(email, parsed.email, sizeof(parsed.email)) &&
        optional_string(field(auth, "chatgpt_plan_type"), parsed.plan, sizeof(parsed.plan)) &&
        (!expiry || uint_field(expiry, 9007199254740991ULL, &parsed.expires_at));
    cJSON_Delete(root);
    if (valid) *identity = parsed;
    return valid;
}

bool quota_direct_parse_tokens(const char *body, size_t length,
                               const quota_direct_identity_t *expected,
                               char *access, size_t access_capacity,
                               char *refresh, size_t refresh_capacity,
                               quota_direct_identity_t *identity)
{
    if (!access || !refresh || !identity) return false;
    cJSON *root = parse_json(body, length);
    if (!root) return false;
    const cJSON *a = field(root, "access_token"), *r = field(root, "refresh_token");
    if (cJSON_IsNull(r)) r = NULL; /* serde Option in the official client. */
    const cJSON *id = field(root, "id_token");
    quota_direct_identity_t parsed = {0};
    bool valid = cJSON_IsString(a) &&
        quota_direct_token_is_safe(a->valuestring, QUOTA_DIRECT_ACCESS_BYTES) &&
        strlen(a->valuestring) < access_capacity &&
        (r ? cJSON_IsString(r) && quota_direct_token_is_safe(r->valuestring, QUOTA_DIRECT_REFRESH_BYTES) &&
             strlen(r->valuestring) < refresh_capacity : expected && quota_direct_token_is_safe(refresh, QUOTA_DIRECT_REFRESH_BYTES));
    if (valid) {
        cJSON *access_root = jwt_object(a->valuestring);
        valid = access_root != NULL;
        if (id && !cJSON_IsNull(id)) {
            valid = valid && cJSON_IsString(id) && quota_direct_parse_identity(id->valuestring, &parsed);
        } else if (expected) parsed = *expected;
        else valid = false;
        const cJSON *auth = field(access_root, "https://api.openai.com/auth");
        valid = valid && (!auth || cJSON_IsNull(auth) || cJSON_IsObject(auth));
        const cJSON *aid = field(auth, "chatgpt_account_id");
        const cJSON *uid = field(auth, "chatgpt_user_id");
        if (!uid) uid = field(auth, "user_id");
        if (aid) valid = valid && cJSON_IsString(aid) && !strcmp(aid->valuestring, parsed.account_id);
        if (uid) valid = valid && cJSON_IsString(uid) && !strcmp(uid->valuestring, parsed.user_id);
        valid = valid && uint_field(field(access_root, "exp"), 9007199254740991ULL, &parsed.expires_at) && parsed.expires_at;
        cJSON_Delete(access_root);
        if (expected) {
            valid = valid && strcmp(expected->account_id, parsed.account_id) == 0 &&
                strcmp(expected->user_id, parsed.user_id) == 0;
            if (!parsed.email[0]) memcpy(parsed.email, expected->email, sizeof(parsed.email));
            if (!parsed.plan[0]) memcpy(parsed.plan, expected->plan, sizeof(parsed.plan));
        }
    }
    if (valid) {
        memcpy(access, a->valuestring, strlen(a->valuestring) + 1);
        if (r) memcpy(refresh, r->valuestring, strlen(r->valuestring) + 1);
        *identity = parsed;
    }
    /* cJSON owns copies of credentials. Wipe before freeing. */
    if (cJSON_IsString(a)) quota_direct_secure_clear(a->valuestring, strlen(a->valuestring));
    if (cJSON_IsString(r)) quota_direct_secure_clear(r->valuestring, strlen(r->valuestring));
    if (cJSON_IsString(id)) quota_direct_secure_clear(id->valuestring, strlen(id->valuestring));
    cJSON_Delete(root); return valid;
}

bool quota_direct_parse_device_code(const char *body, size_t length,
                                    quota_direct_device_code_t *code)
{
    if (!code) return false;
    cJSON *root = parse_json(body, length); if (!root) return false;
    quota_direct_device_code_t parsed = {0};
    const cJSON *user = field(root, "user_code"), *alias = field(root, "usercode");
    const cJSON *interval = field(root, "interval");
    uint64_t seconds = 0;
    bool valid = !(user && alias) &&
        copy_string(field(root, "device_auth_id"), parsed.device_auth_id, sizeof(parsed.device_auth_id), false) &&
        copy_string(user ? user : alias, parsed.user_code, sizeof(parsed.user_code), false) &&
        quota_direct_token_is_safe(parsed.device_auth_id, QUOTA_DIRECT_AUTH_ID_BYTES) &&
        quota_direct_token_is_safe(parsed.user_code, QUOTA_DIRECT_USER_CODE_BYTES);
    if (valid && cJSON_IsString(interval)) {
        const char *p = interval->valuestring;
        valid = p && *p;
        for (; valid && *p; ++p) {
            if (*p < '0' || *p > '9' || seconds > 300) { valid = false; break; }
            seconds = seconds * 10 + (unsigned)(*p - '0');
        }
    } else if (valid) valid = uint_field(interval, 300, &seconds);
    valid = valid && seconds >= 1 && seconds <= 300;
    parsed.interval_seconds = (uint32_t)seconds;
    if (valid) *code = parsed;
    cJSON_Delete(root); return valid;
}

bool quota_direct_parse_authorization(const char *body, size_t length,
                                      quota_direct_authorization_t *code)
{
    if (!code) return false;
    cJSON *root = parse_json(body, length); if (!root) return false;
    bool valid = copy_string(field(root, "authorization_code"), code->authorization_code,
        sizeof(code->authorization_code), false) && copy_string(field(root, "code_verifier"),
        code->code_verifier, sizeof(code->code_verifier), false) &&
        quota_direct_token_is_safe(code->authorization_code, QUOTA_DIRECT_CODE_BYTES) &&
        quota_direct_token_is_safe(code->code_verifier, QUOTA_DIRECT_VERIFIER_BYTES);
    const cJSON *auth = field(root, "authorization_code"), *verifier = field(root, "code_verifier");
    if (cJSON_IsString(auth)) quota_direct_secure_clear(auth->valuestring, strlen(auth->valuestring));
    if (cJSON_IsString(verifier)) quota_direct_secure_clear(verifier->valuestring, strlen(verifier->valuestring));
    cJSON_Delete(root); return valid;
}

static bool parse_usage_window(const cJSON *value, quota_account_t *account)
{
    if (!value || cJSON_IsNull(value)) return true;
    if (!cJSON_IsObject(value)) return false;
    uint64_t duration;
    const cJSON *used = field(value, "used_percent");
    if (!uint_field(field(value, "limit_window_seconds"), UINT32_MAX, &duration) ||
        !cJSON_IsNumber(used) || !isfinite(used->valuedouble) || used->valuedouble < 0) return false;
    quota_window_t *window = duration == 18000 ? &account->five_hour :
                             duration == 604800 ? &account->seven_day : NULL;
    if (!window) return true;
    if (window->present) return false;
    double remaining = round(100 - used->valuedouble);
    window->present = true;
    window->remaining_percent = (uint8_t)fmin(100, fmax(0, remaining));
    const cJSON *reset = field(value, "reset_at");
    if (reset && !cJSON_IsNull(reset)) {
        if (!uint_field(reset, 9007199254740991ULL, &window->resets_at) || !window->resets_at) return false;
        window->has_resets_at = true;
    }
    return true;
}

bool quota_direct_parse_codex_usage(const char *body, size_t length,
                                    const char *expected_account_id,
                                    quota_account_t *account, quota_codex_extras_t *extras)
{
    if (!account || !extras) return false;
    cJSON *root = parse_json(body, length); if (!root) return false;
    quota_account_t parsed = *account;
    memset(&parsed.five_hour, 0, sizeof(parsed.five_hour));
    memset(&parsed.seven_day, 0, sizeof(parsed.seven_day));
    quota_codex_extras_t ex = {0};
    const cJSON *account_id = field(root, "account_id");
    const cJSON *limits = field(root, "rate_limit");
    bool valid = !account_id || cJSON_IsNull(account_id) ||
        (cJSON_IsString(account_id) && expected_account_id &&
         strcmp(account_id->valuestring, expected_account_id) == 0);
    valid = valid && copy_string(field(root, "plan_type"), parsed.plan, sizeof(parsed.plan), false);
    if (limits && !cJSON_IsNull(limits)) valid = valid && cJSON_IsObject(limits) &&
        parse_usage_window(field(limits, "primary_window"), &parsed) &&
        parse_usage_window(field(limits, "secondary_window"), &parsed);
    const cJSON *credits = field(root, "credits");
    if (credits && !cJSON_IsNull(credits)) {
        const cJSON *has = field(credits, "has_credits"), *unlimited = field(credits, "unlimited");
        valid = valid && cJSON_IsObject(credits) && cJSON_IsBool(has) && cJSON_IsBool(unlimited);
        if (valid && (cJSON_IsTrue(has) || cJSON_IsTrue(unlimited))) {
            ex.has_credits = true; ex.unlimited_credits = cJSON_IsTrue(unlimited);
            valid = optional_string(field(credits, "balance"), ex.credits_balance, sizeof(ex.credits_balance));
        }
    }
    const cJSON *reset = field(root, "rate_limit_reset_credits");
    if (reset && !cJSON_IsNull(reset)) {
        valid = valid && cJSON_IsObject(reset) &&
            uint_field(field(reset, "available_count"), 9007199254740991ULL, &ex.available_resets);
        ex.has_banked_reset = valid;
    }
    if (valid) { *account = parsed; *extras = ex; }
    cJSON_Delete(root); return valid;
}

static int64_t civil_days(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    int era = (year >= 0 ? year : year - 399) / 400;
    unsigned yoe = (unsigned)(year - era * 400);
    unsigned doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    return (int64_t)era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

static uint64_t epoch(int year, int month, int day, int hour, int minute, int second)
{
    static const unsigned days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (year < 1970 || year > 9999 || month < 1 || month > 12 || day < 1 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return 0;
    unsigned limit = days[month - 1] + (month == 2 && year % 4 == 0 && (year % 100 || year % 400 == 0));
    if ((unsigned)day > limit) return 0;
    return (uint64_t)(civil_days(year, (unsigned)month, (unsigned)day) * 86400 + hour * 3600 + minute * 60 + second);
}

uint64_t quota_direct_parse_date(const char *date)
{
    if (!date || strlen(date) > 64) return 0;
    int year, month, day, hour, minute, second, used = 0;
    if (sscanf(date, "%4d-%2d-%2dT%2d:%2d:%2d%n", &year, &month, &day,
               &hour, &minute, &second, &used) == 6 && used == 19) {
        const char *suffix = date + used;
        if (*suffix == '.') { ++suffix; const char *start = suffix;
            while (*suffix >= '0' && *suffix <= '9') ++suffix;
            if (start == suffix) return 0; }
        uint64_t value = epoch(year, month, day, hour, minute, second);
        if (!strcmp(suffix, "Z")) return value;
        int zh, zm, length = 0;
        if ((*suffix == '+' || *suffix == '-') &&
            sscanf(suffix + 1, "%2d:%2d%n", &zh, &zm, &length) == 2 &&
            length == 5 && suffix[6] == 0 && zh >= 0 && zh <= 23 && zm >= 0 && zm <= 59) {
            int64_t adjusted = (int64_t)value - (*suffix == '+' ? 1 : -1) * (zh * 3600 + zm * 60);
            return value && adjusted > 0 ? (uint64_t)adjusted : 0;
        }
        return 0;
    }
    char weekday[4] = {0}, mon[4] = {0}, zone[4] = {0}; used = 0;
    if (sscanf(date, "%3s, %2d %3s %4d %2d:%2d:%2d %3s%n", weekday, &day, mon,
        &year, &hour, &minute, &second, zone, &used) != 8 || date[used] || strcmp(zone, "GMT")) return 0;
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    month = 0;
    for (int i = 0; i < 12; ++i) if (!memcmp(mon, months + i * 3, 3)) { month = i + 1; break; }
    return epoch(year, month, day, hour, minute, second);
}

uint32_t quota_direct_retry_after(const char *header, uint64_t now)
{
    if (!header || !*header || strlen(header) > 64) return 60;
    uint64_t seconds = 0; bool numeric = true;
    for (const char *p = header; *p; ++p) {
        if (*p < '0' || *p > '9') { numeric = false; break; }
        if (seconds > 86400) return 86400;
        seconds = seconds * 10 + (unsigned)(*p - '0');
    }
    if (!numeric) {
        uint64_t future = quota_direct_parse_date(header);
        seconds = now && future > now ? future - now : 60;
    }
    return (uint32_t)(seconds > 86400 ? 86400 : seconds < 1 ? 1 : seconds);
}

bool quota_direct_parse_reset_details(const char *body, size_t length, quota_codex_extras_t *extras)
{
    if (!extras) return false;
    cJSON *root = parse_json(body, length); if (!root) return false;
    uint64_t count = 0;
    const cJSON *rows = field(root, "credits");
    bool valid = uint_field(field(root, "available_count"), 9007199254740991ULL, &count) && cJSON_IsArray(rows);
    bool complete = valid && (uint64_t)cJSON_GetArraySize(rows) == count;
    uint64_t next = 0;
    for (const cJSON *row = complete ? rows->child : NULL; row; row = row->next) {
        const cJSON *status = field(row, "status"), *type = field(row, "reset_type"), *expiry = field(row, "expires_at");
        if (!cJSON_IsObject(row) || !cJSON_IsString(status) || strcmp(status->valuestring, "available") ||
            !cJSON_IsString(type) || strcmp(type->valuestring, "codex_rate_limits") || !expiry) { complete = false; break; }
        if (cJSON_IsNull(expiry)) continue;
        uint64_t value = cJSON_IsString(expiry) ? quota_direct_parse_date(expiry->valuestring) : 0;
        if (!value) { complete = false; break; }
        if (!next || value < next) next = value;
    }
    if (valid) {
        extras->has_banked_reset = true; extras->available_resets = count;
        extras->has_next_reset_expiry = complete && next;
        extras->next_reset_expires_at = complete ? next : 0;
    }
    cJSON_Delete(root); return valid;
}

static bool decimal_string(const cJSON *value, char *output, size_t capacity)
{
    if (!copy_string(value, output, capacity, false)) return false;
    const char *p = output; if (*p == '-') ++p;
    const char *digits = p; while (*p >= '0' && *p <= '9') ++p;
    if (digits == p) return false;
    if (*p == '.') { digits = ++p; while (*p >= '0' && *p <= '9') ++p; if (digits == p) return false; }
    return !*p;
}

bool quota_direct_parse_deepseek(const char *body, size_t length, quota_balance_t *balance)
{
    if (!balance) return false;
    cJSON *root = parse_json(body, length); if (!root) return false;
    quota_balance_t parsed = {0}; memcpy(parsed.label, balance->label, sizeof(parsed.label));
    const cJSON *available = field(root, "is_available"), *infos = field(root, "balance_infos");
    int count = cJSON_GetArraySize(infos);
    bool valid = cJSON_IsBool(available) && cJSON_IsArray(infos) && count <= QUOTA_BALANCE_CURRENCIES;
    parsed.present = true; parsed.is_available = cJSON_IsTrue(available); parsed.currency_count = (uint8_t)count;
    for (int i = 0; valid && i < count; ++i) {
        const cJSON *entry = cJSON_GetArrayItem(infos, i); quota_currency_balance_t *out = &parsed.balance_infos[i];
        valid = cJSON_IsObject(entry) && copy_string(field(entry, "currency"), out->currency, sizeof(out->currency), false) &&
            (!strcmp(out->currency, "CNY") || !strcmp(out->currency, "USD")) &&
            (i == 0 || strcmp(out->currency, parsed.balance_infos[0].currency)) &&
            decimal_string(field(entry, "total_balance"), out->total_balance, sizeof(out->total_balance)) &&
            decimal_string(field(entry, "granted_balance"), out->granted_balance, sizeof(out->granted_balance)) &&
            decimal_string(field(entry, "topped_up_balance"), out->topped_up_balance, sizeof(out->topped_up_balance));
    }
    if (valid) *balance = parsed;
    cJSON_Delete(root); return valid;
}

bool quota_direct_form_encode(const char *value, char *output, size_t capacity)
{
    if (!value || !output || !capacity) return false;
    static const char hex[] = "0123456789ABCDEF"; size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        bool plain = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                     (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~';
        size_t need = plain ? 1 : 3;
        if (need >= capacity - used) { output[0] = 0; return false; }
        if (plain) output[used++] = (char)*p;
        else { output[used++] = '%'; output[used++] = hex[*p >> 4]; output[used++] = hex[*p & 15]; }
    }
    output[used] = 0; return true;
}
