#include "quota_direct.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The context is one heap block plus an optional login authorization, both released by cancel. */
static void release_direct(quota_direct_t *direct)
{
    if (!direct) return;
    quota_direct_login_cancel(direct);
    free(direct);
}

#define LOCAL_ID "0123456789abcdef0123456789abcdef"
#define NOW 1700000000ULL

/* Host convenience composes the actual staged APIs. Production frees the HTTP
 * response between these stages; no legacy combined parser is linked. */
static bool quota_direct_parse_tokens(const char *body, size_t length,
    const quota_direct_identity_t *expected, char *access, size_t access_capacity,
    char *refresh, size_t refresh_capacity, quota_direct_identity_t *identity)
{
    quota_direct_tokens_t *tokens = NULL;
    bool valid = quota_direct_tokens_prepare(body, length, &tokens);
    if (valid) valid = quota_direct_tokens_finish(tokens, expected, access, access_capacity, refresh, refresh_capacity, identity);
    quota_direct_tokens_destroy(tokens); return valid;
}

static char access_token[2048], id_token[2048], token_response[5000];
static void jwt(const char *claims, char *output, size_t capacity)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t used = 0; const unsigned char *input = (const unsigned char *)claims;
    output[used++] = 'e'; output[used++] = '3'; output[used++] = '0'; output[used++] = '.';
    uint32_t bits = 0; unsigned count = 0;
    for (; *input; ++input) {
        bits = (bits << 8) | *input; count += 8;
        while (count >= 6) { count -= 6; assert(used + 16 < capacity); output[used++] = alphabet[(bits >> count) & 63]; }
    }
    if (count) output[used++] = alphabet[(bits << (6 - count)) & 63];
    memcpy(output + used, ".signature", 11);
}

static void fixtures(void)
{
    const char *claims = "{\"exp\":2000000000,\"sub\":\"user-1\",\"email\":\"device@example.com\","
        "\"https://api.openai.com/auth\":{\"chatgpt_account_id\":\"account-1\","
        "\"chatgpt_user_id\":\"user-1\",\"chatgpt_plan_type\":\"plus\"}}";
    jwt(claims, access_token, sizeof(access_token)); jwt(claims, id_token, sizeof(id_token));
    int count = snprintf(token_response, sizeof(token_response),
        "{\"access_token\":\"%s\",\"refresh_token\":\"rotated-refresh\",\"id_token\":\"%s\"}", access_token, id_token);
    assert(count > 0 && (size_t)count < sizeof(token_response));
}

static quota_direct_credential_t credential(void)
{
    quota_direct_credential_t value = {0};
    strcpy(value.id, LOCAL_ID); value.provider = QUOTA_PROVIDER_CODEX; value.generation = 3;
    value.auth_state = QUOTA_PORTABLE_AUTH_READY;
    strcpy(value.server_account_id, "account-1"); strcpy(value.server_user_id, "user-1");
    strcpy(value.email, "device@example.com"); strcpy(value.plan, "plus");
    strcpy(value.access_token, access_token); strcpy(value.refresh_token, "old-refresh");
    value.expires_at = 2000000000; return value;
}

static void test_auth_parsers(void)
{
    char unterminated[QUOTA_DIRECT_ACCESS_BYTES + 1];
    memset(unterminated, 'A', sizeof(unterminated));
    assert(!quota_direct_token_is_safe(unterminated, QUOTA_DIRECT_ACCESS_BYTES));
    quota_direct_identity_t identity;
    assert(quota_direct_parse_identity(id_token, &identity));
    assert(!strcmp(identity.account_id, "account-1") && !strcmp(identity.user_id, "user-1"));
    assert(identity.expires_at == 2000000000);
    assert(!quota_direct_parse_identity("a.%%%%.c", &identity));
    quota_direct_credential_t value = credential();
    memset(value.access_token + strlen(value.access_token) + 1, 'X', sizeof(value.access_token) - strlen(value.access_token) - 1);
    memset(value.refresh_token + strlen(value.refresh_token) + 1, 'Y', sizeof(value.refresh_token) - strlen(value.refresh_token) - 1);
    assert(quota_direct_parse_tokens(token_response, strlen(token_response), &identity,
        value.access_token, sizeof(value.access_token), value.refresh_token, sizeof(value.refresh_token), &identity));
    assert(!strcmp(value.refresh_token, "rotated-refresh"));
    for (size_t i = strlen(value.access_token) + 1; i < sizeof(value.access_token); ++i) assert(value.access_token[i] == 0);
    for (size_t i = strlen(value.refresh_token) + 1; i < sizeof(value.refresh_token); ++i) assert(value.refresh_token[i] == 0);
    char body[3000];
    char metadata_free_access[128]; jwt("{\"exp\":2000000000}", metadata_free_access, sizeof(metadata_free_access));
    snprintf(body, sizeof(body), "{\"access_token\":\"%s\"}", metadata_free_access);
    assert(quota_direct_parse_tokens(body, strlen(body), &identity,
        value.access_token, sizeof(value.access_token), value.refresh_token, sizeof(value.refresh_token), &identity));
    assert(!strcmp(identity.account_id, "account-1"));
    snprintf(body, sizeof(body), "{\"access_token\":\"%s\"}", access_token);
    assert(quota_direct_parse_tokens(body, strlen(body), &identity,
        value.access_token, sizeof(value.access_token), value.refresh_token, sizeof(value.refresh_token), &identity));
    assert(!strcmp(value.refresh_token, "rotated-refresh"));
    snprintf(body, sizeof(body), "{\"access_token\":\"%s\",\"refresh_token\":null}", access_token);
    assert(quota_direct_parse_tokens(body, strlen(body), &identity,
        value.access_token, sizeof(value.access_token), value.refresh_token, sizeof(value.refresh_token), &identity));
    assert(!quota_direct_parse_tokens(body, strlen(body), NULL,
        value.access_token, sizeof(value.access_token), value.refresh_token, sizeof(value.refresh_token), &identity));
    strcpy(identity.account_id, "different-account");
    quota_direct_credential_t unchanged = value;
    assert(!quota_direct_parse_tokens(token_response, strlen(token_response), &identity,
        value.access_token, sizeof(value.access_token), value.refresh_token, sizeof(value.refresh_token), &identity));
    assert(!strcmp(value.refresh_token, "rotated-refresh"));
    assert(!memcmp(&value, &unchanged, sizeof(value)));
    assert(!quota_direct_parse_tokens("{\"access_token\":null}", 21, &identity,
        value.access_token, sizeof(value.access_token), value.refresh_token, sizeof(value.refresh_token), &identity));

    const char *device = "{\"device_auth_id\":\"private-id\",\"user_code\":\"ABCD-1234\",\"interval\":\"5\"}";
    quota_direct_device_code_t code;
    assert(quota_direct_parse_device_code(device, strlen(device), &code) && code.interval_seconds == 5);
    const char *bad[] = {
        "{\"device_auth_id\":\"id\",\"user_code\":\"ABCD\",\"interval\":\"0\"}",
        "{\"device_auth_id\":\"id\",\"user_code\":\"ABCD\",\"interval\":\"5oops\"}",
        "{\"device_auth_id\":\"id\",\"user_code\":\"ABCD\",\"interval\":\"999999999999999\"}",
        "{\"device_auth_id\":\"id\",\"user_code\":\"ABCD\",\"interval\":5,\"interval\":6}",
        "{\"device_auth_id\":\"id\",\"user_code\":\"ABCD\",\"usercode\":\"EFGH\",\"interval\":5}",
        "{\"device_auth_id\":\"id\\u0000secret\",\"user_code\":\"ABCD\",\"interval\":5}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        assert(!quota_direct_parse_device_code(bad[i], strlen(bad[i]), &code));
    char encoded[80]; assert(quota_direct_form_encode("a+&= /%", encoded, sizeof(encoded)));
    assert(!strcmp(encoded, "a%2B%26%3D%20%2F%25"));
    assert(!quota_direct_form_encode("+", encoded, 3));
}

static char *extended_token(const char *token, size_t length)
{
    char *value = malloc(length + 1); assert(value && strlen(token) <= length);
    memset(value, 'A', length); memcpy(value, token, strlen(token)); value[length] = 0;
    return value;
}

static void test_bounded_json_and_tokens(void)
{
    const char device[] = "{\"device_auth_id\":\"private-id\",\"user_code\":\"ABCD\",\"interval\":5}";
    size_t length = strlen(device);
    char *bounded = malloc(length); assert(bounded);
    memcpy(bounded, device, length); /* Deliberately no NUL and no spare byte. */
    quota_direct_device_code_t code;
    assert(quota_direct_parse_device_code(bounded, length, &code));
    free(bounded);
    char tail[256];
    snprintf(tail, sizeof(tail), "%s \t\r\n", device);
    assert(quota_direct_parse_device_code(tail, strlen(tail), &code));
    const char *junk[] = {"{}", "false", "x", "\v"};
    for (size_t i = 0; i < sizeof(junk) / sizeof(junk[0]); i++) {
        snprintf(tail, sizeof(tail), "%s%s", device, junk[i]);
        assert(!quota_direct_parse_device_code(tail, strlen(tail), &code));
    }
    bounded = malloc(QUOTA_DIRECT_BODY_BYTES + 1); assert(bounded);
    memset(bounded, ' ', QUOTA_DIRECT_BODY_BYTES + 1); memcpy(bounded, device, length);
    assert(quota_direct_parse_device_code(bounded, QUOTA_DIRECT_BODY_BYTES, &code));
    assert(!quota_direct_parse_device_code(bounded, QUOTA_DIRECT_BODY_BYTES + 1, &code));
    bounded[length / 2] = 0;
    assert(!quota_direct_parse_device_code(bounded, QUOTA_DIRECT_BODY_BYTES, &code));
    free(bounded);

    quota_direct_credential_t *value = calloc(1, sizeof(*value)); assert(value);
    quota_direct_identity_t identity;
    char *a = extended_token(access_token, QUOTA_DIRECT_ACCESS_BYTES);
    char *r = extended_token("refresh-", QUOTA_DIRECT_REFRESH_BYTES);
    char *id = extended_token(id_token, QUOTA_DIRECT_ID_TOKEN_BYTES);
    bounded = malloc(QUOTA_DIRECT_BODY_BYTES); assert(bounded);
    int size = snprintf(bounded, QUOTA_DIRECT_BODY_BYTES,
        "{\"access_token\":\"%s\",\"refresh_token\":\"%s\",\"id_token\":\"%s\"}", a, r, id);
    assert(size > 0 && size < QUOTA_DIRECT_BODY_BYTES);
    memset(bounded + size, ' ', QUOTA_DIRECT_BODY_BYTES - (size_t)size);
    quota_direct_tokens_t *tokens = NULL;
    assert(quota_direct_tokens_prepare(bounded, QUOTA_DIRECT_BODY_BYTES, &tokens));
    /* The detached envelope must not borrow the response. ASan catches any
     * JWT stage access after this actual release of the maximum-sized body. */
    memset(bounded, 0, QUOTA_DIRECT_BODY_BYTES); free(bounded); bounded = NULL;
    assert(quota_direct_tokens_finish(tokens, NULL,
        value->access_token, sizeof(value->access_token), value->refresh_token, sizeof(value->refresh_token), &identity));
    quota_direct_tokens_destroy(tokens);
    assert(strlen(value->access_token) == QUOTA_DIRECT_ACCESS_BYTES && strlen(value->refresh_token) == QUOTA_DIRECT_REFRESH_BYTES);
    bounded = malloc(QUOTA_DIRECT_BODY_BYTES); assert(bounded);
    /* One byte above each token's bound is rejected without losing output. */
    for (unsigned field = 0; field < 3; field++) {
        char *too_big = extended_token(field == 0 ? a : field == 1 ? r : id,
            (field == 1 ? QUOTA_DIRECT_REFRESH_BYTES : QUOTA_DIRECT_ACCESS_BYTES) + 1);
        size = snprintf(bounded, QUOTA_DIRECT_BODY_BYTES,
            "{\"access_token\":\"%s\",\"refresh_token\":\"%s\",\"id_token\":\"%s\"}",
            field == 0 ? too_big : a, field == 1 ? too_big : r, field == 2 ? too_big : id);
        assert(size > 0 && size < QUOTA_DIRECT_BODY_BYTES);
        assert(!quota_direct_parse_tokens(bounded, (size_t)size, NULL,
            value->access_token, sizeof(value->access_token), value->refresh_token, sizeof(value->refresh_token), &identity));
        assert(strlen(value->access_token) == QUOTA_DIRECT_ACCESS_BYTES && strlen(value->refresh_token) == QUOTA_DIRECT_REFRESH_BYTES);
        free(too_big);
    }
    free(a); free(r); free(id); free(bounded); free(value);

    char complex[2048]; size_t used = (size_t)snprintf(complex, sizeof(complex),
        "{\"device_auth_id\":\"id\",\"user_code\":\"ABCD\",\"interval\":5,\"extra\":[");
    for (unsigned i = 0; i < 129; i++) used += (size_t)snprintf(complex + used, sizeof(complex) - used, "%s0", i ? "," : "");
    strcpy(complex + used, "]}");
    assert(!quota_direct_parse_device_code(complex, strlen(complex), &code));
    tokens = (quota_direct_tokens_t *)1;
    assert(!quota_direct_tokens_prepare(complex, strlen(complex), &tokens) && !tokens);
}

static unsigned json_allocations, fail_json_allocation;
static void *json_allocate(size_t bytes)
{
    return ++json_allocations == fail_json_allocation ? NULL : malloc(bytes);
}

static void test_staged_parser_allocation_failure(void)
{
    cJSON_Hooks hooks = {.malloc_fn = json_allocate, .free_fn = free};
    cJSON_InitHooks(&hooks);
    quota_direct_credential_t value = credential(), original = value;
    quota_direct_identity_t identity = {0}, before = identity;
    quota_direct_tokens_t *tokens = NULL;
    json_allocations = fail_json_allocation = 0;
    assert(quota_direct_tokens_prepare(token_response, strlen(token_response), &tokens));
    assert(quota_direct_tokens_finish(tokens, NULL, value.access_token, sizeof(value.access_token),
        value.refresh_token, sizeof(value.refresh_token), &identity));
    unsigned total = json_allocations; quota_direct_tokens_destroy(tokens);
    assert(total > 10); /* Includes envelope and both sequential JWT trees. */
    for (unsigned failure = 1; failure <= total; failure++) {
        json_allocations = 0; fail_json_allocation = failure;
        value = original; identity = before; tokens = NULL;
        bool valid = quota_direct_tokens_prepare(token_response, strlen(token_response), &tokens);
        if (valid) valid = quota_direct_tokens_finish(tokens, NULL, value.access_token, sizeof(value.access_token),
            value.refresh_token, sizeof(value.refresh_token), &identity);
        assert(!valid && !memcmp(&value, &original, sizeof(value)) && !memcmp(&identity, &before, sizeof(identity)));
        quota_direct_tokens_destroy(tokens);
    }
    cJSON_InitHooks(NULL);
}

static const char usage[] = "{\"account_id\":\"account-1\",\"plan_type\":\"plus\","
    "\"rate_limit\":{\"primary_window\":{\"used_percent\":100,\"limit_window_seconds\":18000,\"reset_at\":1700000100},"
    "\"secondary_window\":{\"used_percent\":25,\"limit_window_seconds\":604800,\"reset_at\":1700100100}},"
    "\"credits\":{\"has_credits\":true,\"unlimited\":false,\"balance\":\"000.1200\"},"
    "\"rate_limit_reset_credits\":{\"available_count\":2},"
    "\"additional_rate_limits\":[{\"rate_limit\":{\"primary_window\":{\"used_percent\":0,\"limit_window_seconds\":18000}}}]}";

static void test_quota_parsers(void)
{
    quota_account_t account = {0}; quota_codex_extras_t extras = {0};
    assert(quota_direct_parse_codex_usage(usage, strlen(usage), "account-1", &account, &extras));
    assert(account.five_hour.present && account.five_hour.remaining_percent == 0);
    assert(account.seven_day.remaining_percent == 75 && account.five_hour.resets_at == 1700000100);
    assert(extras.has_credits && !strcmp(extras.credits_balance, "000.1200"));
    assert(extras.has_banked_reset && extras.available_resets == 2 && !extras.has_next_reset_expiry);
    assert(!quota_direct_parse_codex_usage(usage, strlen(usage), "wrong", &account, &extras));
    const char *missing = "{\"plan_type\":\"pro\",\"rate_limit\":{\"primary_window\":{\"used_percent\":3,\"limit_window_seconds\":3600}}}";
    assert(quota_direct_parse_codex_usage(missing, strlen(missing), "account-1", &account, &extras));
    assert(!account.five_hour.present && !account.seven_day.present && !extras.has_credits);
    const char *details = "{\"available_count\":2,\"credits\":["
        "{\"status\":\"available\",\"reset_type\":\"codex_rate_limits\",\"expires_at\":\"2026-07-17T00:00:00Z\"},"
        "{\"status\":\"available\",\"reset_type\":\"codex_rate_limits\",\"expires_at\":null}]}";
    assert(quota_direct_parse_reset_details(details, strlen(details), &extras));
    assert(extras.available_resets == 2 && extras.has_next_reset_expiry);
    assert(extras.next_reset_expires_at == quota_direct_parse_date("2026-07-17T00:00:00Z"));
    const char *capped = "{\"available_count\":4,\"credits\":[]}";
    assert(quota_direct_parse_reset_details(capped, strlen(capped), &extras));
    assert(extras.available_resets == 4 && !extras.has_next_reset_expiry);
    assert(!quota_direct_parse_reset_details("{\"available_count\":-1,\"credits\":[]}", 36, &extras));
    assert(extras.available_resets == 4);

    const char *balance = "{\"is_available\":true,\"balance_infos\":[{\"currency\":\"CNY\","
        "\"total_balance\":\"001.234500\",\"granted_balance\":\"-0.0001\",\"topped_up_balance\":\"0\"}]}";
    quota_balance_t parsed = {0}; strcpy(parsed.label, "My key");
    assert(quota_direct_parse_deepseek(balance, strlen(balance), &parsed));
    assert(parsed.present && parsed.currency_count == 1 && !strcmp(parsed.balance_infos[0].total_balance, "001.234500"));
    assert(!strcmp(parsed.label, "My key"));
    const char *bad = "{\"is_available\":true,\"balance_infos\":[{\"currency\":\"CNY\",\"total_balance\":1,\"granted_balance\":\"0\",\"topped_up_balance\":\"0\"}]}";
    assert(!quota_direct_parse_deepseek(bad, strlen(bad), &parsed));
    assert(!strcmp(parsed.balance_infos[0].total_balance, "001.234500"));
    assert(quota_direct_parse_date("Tue, 14 Nov 2023 22:13:20 GMT") == NOW);
    assert(quota_direct_parse_date("2023-11-14T22:13:20Z") == NOW);
    assert(quota_direct_parse_date("2023-11-15T06:13:20+08:00") == NOW);
    assert(!quota_direct_parse_date("2023-02-30T12:00:00Z"));
    assert(quota_direct_retry_after("120", NOW) == 120);
    assert(quota_direct_retry_after("999999999999999999999", NOW) == 86400);
    assert(quota_direct_retry_after("Tue, 14 Nov 2023 22:15:20 GMT", NOW) == 120);
}

typedef struct {
    bool awake;
    bool exists;
    bool transport_failure;
    bool sleep_on_http;
    bool sleep_on_marker;
    bool details_failure;
    bool authorization_failure;
    bool overflow;
    bool refresh_429;
    bool refresh_503;
    bool refresh_redirect;
    bool refresh_partial_429;
    unsigned requests;
    unsigned stores;
    unsigned fail_store;
    unsigned login_phase;
    const quota_direct_credential_t *persisted_pointer;
    quota_direct_credential_t stored;
} fake_t;

static bool admit(void *context, const char *id, uint32_t generation)
{
    fake_t *fake = context;
    return fake->awake && fake->exists && !strcmp(id, LOCAL_ID) && generation == 3;
}
static bool account_current(void *context, const char *id, uint32_t generation)
{
    fake_t *fake = context;
    return fake->exists && !strcmp(id, LOCAL_ID) && generation == 3;
}
static bool persist(void *context, const quota_direct_credential_t *value)
{
    fake_t *fake = context;
    assert(account_current(context, value->id, value->generation));
    fake->persisted_pointer = value;
    ++fake->stores;
    if (fake->stores == fake->fail_store) return false;
    fake->stored = *value;
    if (fake->sleep_on_marker && value->refresh_inflight) fake->awake = false;
    return true;
}

static bool transport(void *context, quota_direct_http_request_t *request,
                       quota_direct_http_response_t *response)
{
    fake_t *fake = context; ++fake->requests; response->admitted = true;
    assert(!strcmp(request->logical_id, LOCAL_ID) && request->generation == 3);
    if (fake->sleep_on_http) fake->awake = false;
    if (fake->transport_failure) return false;
    if (fake->overflow) { response->overflow = true; return false; }
    response->status = 200;
    strcpy(response->date, "Tue, 14 Nov 2023 22:13:20 GMT");
    const char *body = NULL;
    if (strstr(request->url, "deviceauth/usercode")) {
        assert(request->post && !strcmp(request->content_type, "application/json"));
        body = "{\"device_auth_id\":\"private-auth\",\"user_code\":\"ABCD-1234\",\"interval\":\"5\"}";
    } else if (strstr(request->url, "deviceauth/token")) {
        assert(request->post && strstr(request->body, "private-auth"));
        if (fake->login_phase++ == 0) { response->status = 403; body = "{}"; }
        else body = "{\"authorization_code\":\"once+&=\",\"code_verifier\":\"proof-verifier\",\"code_challenge\":\"challenge\"}";
    } else if (strstr(request->url, "/oauth/token")) {
        assert(request->post);
        if (!strcmp(request->content_type, "application/json")) {
            assert(fake->stored.refresh_inflight); /* durable BEFORE actual POST */
            assert(strstr(request->body, "old-refresh"));
        } else {
            assert(!strcmp(request->content_type, "application/x-www-form-urlencoded"));
            assert(strstr(request->body, "code=once%2B%26%3D"));
            assert(strstr(request->body, "redirect_uri=https%3A%2F%2Fauth.openai.com%2Fdeviceauth%2Fcallback"));
        }
        body = fake->authorization_failure ? "{\"access_token\":\"bad-token\"}" : token_response;
        if (fake->refresh_429) { response->status = 429; strcpy(response->retry_after, "120"); body = "{\"error\":\"too_many_requests\"}"; }
        if (fake->refresh_503) { response->status = 503; body = "{}"; }
        if (fake->refresh_redirect) { response->status = 302; response->redirected = true; body = "{}"; }
        if (fake->refresh_partial_429) { response->status = 429; return false; }
    } else if (strstr(request->url, "/wham/usage")) {
        assert(!request->post && !strcmp(request->bearer, access_token) && !strcmp(request->account_id, "account-1"));
        body = usage;
    } else if (strstr(request->url, "/wham/rate-limit-reset-credits")) {
        assert(!request->post);
        if (fake->details_failure) { response->status = 503; body = "{}"; }
        else body = "{\"available_count\":2,\"credits\":[]}";
    } else {
        assert(!strcmp(request->url, "https://api.deepseek.com/user/balance") && !request->post);
        body = "{\"is_available\":true,\"balance_infos\":[]}";
    }
    assert(body && strlen(body) <= response->capacity);
    strcpy(response->body, body); response->length = strlen(body); return true;
}

static quota_direct_t *create(fake_t *fake)
{
    fake->awake = true; fake->exists = true;
    quota_direct_hooks_t hooks = { .context = fake, .admit = admit,
        .account_current = account_current, .persist = persist };
    quota_direct_t *direct = quota_direct_create(&hooks, transport, fake); assert(direct); return direct;
}

static void test_rotation(void)
{
    fake_t fake = {0}; quota_direct_t *direct = create(&fake);
    quota_direct_credential_t value = credential(); fake.stored = value;
    fake.sleep_on_http = true;
    assert(quota_direct_refresh(direct, &value, NOW).code == QUOTA_DIRECT_OK);
    assert(fake.requests == 1 && fake.stores == 2 && !fake.stored.refresh_inflight);
    assert(!fake.awake && !strcmp(fake.stored.refresh_token, "rotated-refresh"));
    release_direct(direct);

    for (unsigned scenario = 0; scenario < 3; scenario++) {
        memset(&fake, 0, sizeof(fake)); direct = create(&fake); value = credential(); fake.stored = value;
        fake.refresh_503 = scenario == 0;
        fake.refresh_redirect = scenario == 1;
        fake.refresh_partial_429 = scenario == 2;
        assert(quota_direct_refresh(direct, &value, NOW).code == QUOTA_DIRECT_AUTH_REQUIRED);
        assert(fake.stored.refresh_inflight && value.refresh_inflight && fake.requests == 1);
        assert(quota_direct_refresh(direct, &value, NOW).code == QUOTA_DIRECT_AUTH_REQUIRED && fake.requests == 1);
        release_direct(direct);
    }

    memset(&fake, 0, sizeof(fake)); direct = create(&fake); value = credential(); fake.stored = value;
    fake.refresh_429 = true;
    quota_direct_result_t limited = quota_direct_refresh(direct, &value, NOW);
    assert(limited.code == QUOTA_DIRECT_RATE_LIMITED && limited.http_status == 429 && limited.retry_after_seconds == 120);
    assert(!value.refresh_inflight && !fake.stored.refresh_inflight && !strcmp(fake.stored.refresh_token, "old-refresh"));
    assert(fake.requests == 1 && fake.stores == 2);
    release_direct(direct);

    memset(&fake, 0, sizeof(fake)); direct = create(&fake); value = credential(); fake.stored = value;
    fake.overflow = true;
    assert(quota_direct_refresh(direct, &value, NOW).code == QUOTA_DIRECT_AUTH_REQUIRED);
    assert(fake.stored.refresh_inflight && fake.requests == 1);
    assert(quota_direct_refresh(direct, &value, NOW).code == QUOTA_DIRECT_AUTH_REQUIRED && fake.requests == 1);
    release_direct(direct);

    memset(&fake, 0, sizeof(fake)); direct = create(&fake); value = credential(); fake.stored = value;
    fake.fail_store = 2;
    assert(quota_direct_refresh(direct, &value, NOW).code == QUOTA_DIRECT_PERSIST_PENDING);
    assert(fake.stored.refresh_inflight && quota_direct_has_pending_persist(direct));
    assert(fake.persisted_pointer == &value && !strcmp(value.refresh_token, "rotated-refresh"));
    assert(quota_direct_refresh(direct, &value, NOW).code == QUOTA_DIRECT_PERSIST_PENDING);
    assert(fake.requests == 1);
    fake.awake = false;
    assert(quota_direct_retry_persist(direct).code == QUOTA_DIRECT_OK);
    assert(!fake.stored.refresh_inflight && !strcmp(fake.stored.refresh_token, "rotated-refresh"));
    assert(fake.requests == 1 && !quota_direct_has_pending_persist(direct));
    assert(fake.persisted_pointer == &value && !strcmp(value.refresh_token, "rotated-refresh"));
    release_direct(direct);

    memset(&fake, 0, sizeof(fake)); direct = create(&fake); value = credential(); fake.stored = value;
    fake.transport_failure = true;
    assert(quota_direct_refresh(direct, &value, NOW).code == QUOTA_DIRECT_AUTH_REQUIRED);
    assert(fake.stored.refresh_inflight && value.refresh_inflight);
    assert(quota_direct_refresh(direct, &value, NOW).code == QUOTA_DIRECT_AUTH_REQUIRED && fake.requests == 1);
    release_direct(direct);

    memset(&fake, 0, sizeof(fake)); direct = create(&fake); value = credential(); fake.stored = value;
    fake.sleep_on_marker = true;
    assert(quota_direct_refresh(direct, &value, NOW).code == QUOTA_DIRECT_DEFERRED);
    assert(fake.requests == 0 && fake.stores == 2 && !fake.stored.refresh_inflight);
    release_direct(direct);

    memset(&fake, 0, sizeof(fake)); direct = create(&fake); value = credential(); fake.stored = value;
    fake.authorization_failure = true;
    quota_direct_credential_t unchanged = value; unchanged.refresh_inflight = true;
    assert(quota_direct_refresh(direct, &value, NOW).code == QUOTA_DIRECT_AUTH_REQUIRED);
    assert(fake.stored.refresh_inflight);
    assert(!memcmp(&value, &unchanged, sizeof(value)));
    release_direct(direct);
}

static void test_borrowed_pending(void)
{
    /* Destroy and account cancellation release a borrowed pointer without
     * freeing/wiping it, including a stack-backed caller in this host test. */
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        fake_t fake = {0}; quota_direct_t *direct = create(&fake);
        quota_direct_credential_t value = credential(); fake.stored = value;
        fake.fail_store = 2; fake.refresh_429 = scenario == 2;
        assert(quota_direct_refresh(direct, &value, NOW).code == QUOTA_DIRECT_PERSIST_PENDING);
        assert(fake.persisted_pointer == &value && quota_direct_has_pending_persist(direct));
        quota_direct_credential_t received = value;
        unsigned requests = fake.requests;
        if (scenario == 1) {
            fake.exists = false;
            assert(quota_direct_retry_persist(direct).code == QUOTA_DIRECT_CANCELED);
            assert(!quota_direct_has_pending_persist(direct));
        } else if (scenario == 2) {
            fake.awake = false;
            assert(quota_direct_retry_persist(direct).code == QUOTA_DIRECT_OK);
            assert(!value.refresh_inflight && !fake.stored.refresh_inflight);
            assert(!strcmp(value.refresh_token, "old-refresh"));
        }
        assert(fake.requests == requests);
        release_direct(direct);
        assert(!memcmp(&value, &received, sizeof(value)));
    }
    fake_t fake = {0}; quota_direct_t *direct = create(&fake);
    quota_direct_credential_t value = credential();
    assert(quota_direct_login_begin(direct, &value, 0, NOW).code == QUOTA_DIRECT_WAITING);
    assert(quota_direct_login_step(direct, &value, 5000, NOW).code == QUOTA_DIRECT_WAITING);
    assert(quota_direct_login_step(direct, &value, 10000, NOW).code == QUOTA_DIRECT_WAITING);
    fake.fail_store = 1;
    assert(quota_direct_login_step(direct, &value, 10000, NOW).code == QUOTA_DIRECT_PERSIST_PENDING);
    assert(fake.persisted_pointer == &value && !strcmp(value.refresh_token, "rotated-refresh"));
    unsigned requests = fake.requests;
    assert(quota_direct_login_step(direct, &value, 10001, NOW).code == QUOTA_DIRECT_PERSIST_PENDING);
    quota_direct_login_cancel(direct); fake.awake = false;
    assert(quota_direct_retry_persist(direct).code == QUOTA_DIRECT_OK);
    assert(fake.requests == requests && fake.persisted_pointer == &value);
    assert(!strcmp(value.refresh_token, "rotated-refresh"));
    release_direct(direct);
    assert(!strcmp(value.refresh_token, "rotated-refresh"));
}

static void test_login_and_reads(void)
{
    fake_t fake = {0}; quota_direct_t *direct = create(&fake);
    quota_direct_credential_t value = credential();
    value.server_account_id[0] = 0; value.server_user_id[0] = 0;
    value.access_token[0] = 0; value.refresh_token[0] = 0;
    assert(quota_direct_login_begin(direct, &value, 0, NOW).code == QUOTA_DIRECT_WAITING);
    quota_direct_login_view_t view; quota_direct_login_view(direct, 1000, &view);
    assert(view.active && !strcmp(view.user_code, "ABCD-1234") && view.remaining_seconds == 899);
    assert(!strcmp(view.verification_url, QUOTA_DIRECT_VERIFICATION_URL));
    assert(quota_direct_login_step(direct, &value, 4999, NOW).code == QUOTA_DIRECT_WAITING && fake.requests == 1);
    assert(quota_direct_login_step(direct, &value, 5000, NOW).code == QUOTA_DIRECT_WAITING && fake.requests == 2);
    assert(quota_direct_login_step(direct, &value, 10000, NOW).code == QUOTA_DIRECT_WAITING && fake.requests == 3);
    fake.sleep_on_http = true;
    assert(quota_direct_login_step(direct, &value, 10000, NOW).code == QUOTA_DIRECT_OK && fake.requests == 4);
    assert(!strcmp(fake.stored.refresh_token, "rotated-refresh") && !fake.awake);
    quota_direct_login_view(direct, 11000, &view); assert(!view.active);
    fake.awake = true; fake.sleep_on_http = false; fake.details_failure = true;
    quota_direct_result_t read = quota_direct_query(direct, &value, NOW + 3600);
    assert(read.code == QUOTA_DIRECT_OK && read.source_valid && read.account.observed_at == NOW);
    assert(read.extras.available_resets == 2 && !read.extras.has_next_reset_expiry && !read.details_available);
    unsigned requests = fake.requests;
    fake.awake = false;
    assert(quota_direct_query(direct, &value, NOW).code == QUOTA_DIRECT_DEFERRED && fake.requests == requests);
    fake.awake = true;
    assert(quota_direct_query(direct, &value, 0).code == QUOTA_DIRECT_TIME_REQUIRED && fake.requests == requests);
    value.provider = (quota_provider_t)1; /* Reserved provider value. */
    assert(quota_direct_query(direct, &value, NOW).code == QUOTA_DIRECT_UNSUPPORTED && fake.requests == requests);
    release_direct(direct);

    memset(&fake, 0, sizeof(fake)); direct = create(&fake); value = credential();
    assert(quota_direct_login_begin(direct, &value, 0, NOW).code == QUOTA_DIRECT_WAITING);
    assert(quota_direct_login_step(direct, &value, 5000, NOW).code == QUOTA_DIRECT_WAITING);
    assert(quota_direct_login_step(direct, &value, 10000, NOW).code == QUOTA_DIRECT_WAITING);
    fake.transport_failure = true;
    assert(quota_direct_login_step(direct, &value, 10000, NOW).code == QUOTA_DIRECT_NETWORK_ERROR);
    requests = fake.requests;
    assert(quota_direct_login_step(direct, &value, 10001, NOW).code == QUOTA_DIRECT_CANCELED && fake.requests == requests);
    release_direct(direct);
}

int main(void)
{
    fixtures(); test_auth_parsers(); test_bounded_json_and_tokens(); test_staged_parser_allocation_failure(); test_quota_parsers(); test_rotation(); test_login_and_reads(); test_borrowed_pending();
    puts("quota direct parsers, login phases and rotation runtime: PASS");
    return 0;
}
