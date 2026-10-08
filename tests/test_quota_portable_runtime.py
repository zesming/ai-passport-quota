"""Execute portable storage and local-command validation C against fake data."""

import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

from runtime_helpers import ROOT, compile_and_run


class PortableRuntime(unittest.TestCase):
    def test_command_limits_and_session_boundaries(self):
        harness = r'''
#include "quota_portal.h"
#include "cJSON.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static quota_portable_command_t command;
static bool parse(const char *json)
{
    return quota_portal_parse_command(json, strlen(json), &command);
}
int main(void)
{
    assert(quota_portal_host_is_valid("192.168.4.1"));
    assert(quota_portal_host_is_valid("192.168.4.1:80"));
    assert(!quota_portal_host_is_valid("evil.test"));
    assert(!quota_portal_host_is_valid("192.168.4.1.evil.test"));
    assert(!quota_portal_host_is_valid("192.168.4.1:81"));
    assert(quota_portal_origin_is_valid("http://192.168.4.1"));
    assert(!quota_portal_origin_is_valid("null"));
    assert(!quota_portal_origin_is_valid("https://192.168.4.1"));
    const char *secret = "K7QM-2X9D-PA4T-Z8RW", *wrong = "K7QM-2X9D-PA4T-Z8RX";
    assert(quota_portal_secret_matches(secret, secret));
    assert(!quota_portal_secret_matches(secret, wrong));
    assert(!quota_portal_secret_matches(secret, "K7QM"));
    assert(!quota_portal_secret_matches(secret, "K7QM2X9DPA4TZ8RW"));
    assert(!quota_portal_secret_matches(NULL, secret));
    /* The access code is 16 Crockford Base32 characters in groups of four. */
    assert(quota_portal_access_code_is_valid(secret));
    assert(quota_portal_access_code_is_valid("0123-4567-89AB-CDEF"));
    assert(quota_portal_access_code_is_valid("GHJK-MNPQ-RSTV-WXYZ"));
    assert(!quota_portal_access_code_is_valid(NULL));
    assert(!quota_portal_access_code_is_valid("K7QM-2X9D-PA4T-Z8R"));
    assert(!quota_portal_access_code_is_valid("K7QM-2X9D-PA4T-Z8RWW"));
    assert(!quota_portal_access_code_is_valid("K7QM2X9D-PA4T-Z8RW-"));
    assert(!quota_portal_access_code_is_valid("k7qm-2x9d-pa4t-z8rw"));
    for (const char *bad = "ILOU"; *bad; bad++) {
        char code[20] = "K7QM-2X9D-PA4T-Z8RW";
        code[7] = *bad;
        assert(!quota_portal_access_code_is_valid(code));
    }
    assert(parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"network_save\",\"ssid\":\"Phone\","
                 "\"password\":\"12345678\",\"phone_utc\":1800000000}"));
    assert(command.op == QUOTA_PORTABLE_OP_NETWORK_SAVE && command.network_index == UINT8_MAX);
    assert(!parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"network_save\",\"ssid\":\"Phone\","
                  "\"password\":\"short\"}"));
    assert(
        !parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"network_save\",\"ssid\":\"Phone\"}"));
    assert(parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"network_save\",\"ssid\":\"Cafe\","
                 "\"password\":\"\",\"open_network\":true}"));
    assert(command.open_network);
    /* network_save always names the network; an index picks the one to change. */
    assert(!parse(
        "{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"network_save\",\"network_index\":2}"));
    assert(parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"network_save\",\"ssid\":\"Home\","
                 "\"password\":\"12345678\",\"network_index\":2}"));
    assert(command.network_index == 2 && !strcmp(command.ssid, "Home"));
    assert(!parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"network_save\",\"ssid\":\"Home\","
                  "\"password\":\"12345678\",\"network_index\":3}"));
    assert(parse(
        "{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"network_remove\",\"network_index\":2}"));
    assert(command.op == QUOTA_PORTABLE_OP_NETWORK_REMOVE && command.network_index == 2);
    assert(!parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"network_remove\"}"));
    assert(!parse(
        "{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"network_remove\",\"network_index\":3}"));
    assert(!parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"network_remove\",\"network_"
                  "index\":0,\"ssid\":\"Home\"}"));
    assert(parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"validate\"}"));
    assert(command.op == QUOTA_PORTABLE_OP_VALIDATE);
    assert(!parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"validate\",\"all\":true}"));
    /* Only protocol 3 bodies are commands. */
    for (const char *version = "0124"; *version; version++) {
        char frame[96];
        snprintf(frame, sizeof(frame), "{\"v\":%c,\"request_id\":\"abcdef01\",\"op\":\"refresh\"}",
                 *version);
        assert(!parse(frame));
    }
    assert(!parse(
        "{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"deepseek_save\",\"label\":\"Only label\"}"));
    assert(parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"deepseek_save\",\"label\":\"Only "
                 "label\",\"account_id\":\"0123456789abcdef0123456789abcdef\"}"));
    assert(command.api_key[0] == 0);
    assert(parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"deepseek_save\",\"label\":\"My "
                 "API\",\"api_key\":\"sk-fake\"}"));
    assert(strcmp(command.api_key, "sk-fake") == 0);
    assert(
        parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"codex_queue\",\"label\":\"Work\"}"));
    assert(parse(
        "{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"codex_queue\",\"phone_utc\":1800000000}"));
    assert(command.phone_utc == 1800000000);
    assert(!parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"deepseek_save\",\"label\":"
                  "\"Fake\",\"api_key\":\"sk-fake\",\"phone_utc\":-1}"));
    assert(parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"settings_save\",\"refresh_"
                 "seconds\":300,\"auto_refresh\":false,\"screen_timeout_seconds\":0}"));
    assert(!parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"settings_save\",\"refresh_"
                  "seconds\":301,\"auto_refresh\":false,\"screen_timeout_seconds\":0}"));
    assert(parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"refresh\"}"));
    assert(parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"reconnect\"}"));
    assert(parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"operation_cancel\",\"target_"
                 "request_id\":\"abcdef02\"}"));
    assert(command.op == QUOTA_PORTABLE_OP_OPERATION_CANCEL &&
           !strcmp(command.target_request_id, "abcdef02"));
    assert(!parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"operation_cancel\"}"));
    assert(!parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"operation_cancel\",\"target_"
                  "request_id\":\"ABCDEF02\"}"));
    assert(!parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"operation_cancel\",\"target_"
                  "request_id\":\"abcdef023\"}"));
    assert(!parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"refresh\",\"accepted_mode\":0}"));
    assert(!parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"refresh\",\"accepted_config_"
                  "generation\":1}"));
    assert(!parse("{\"v\":3,\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"refresh\"}"));
    assert(!parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"refresh\",\"unknown\":1}"));
    assert(!parse("{\"v\":3,\"request_id\":\"ABCDEF01\",\"op\":\"refresh\"}"));
    assert(!parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"deepseek_save\",\"label\":"
                  "\"X\\u0000Y\",\"api_key\":\"sk-fake\"}"));
    assert(!parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"deepseek_save\",\"label\":"
                  "\"X\\nY\",\"api_key\":\"sk-fake\"}"));
    assert(!parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"refresh\"}junk"));
    assert(!parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"refresh\",\"x\":[[[[1]]]]}"));
    /* Operations of removed features are unknown commands. */
    assert(!parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"account_activate\",\"account_id\":"
                  "\"0123456789abcdef0123456789abcdef\"}"));
    assert(!parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"account_deactivate\",\"account_"
                  "id\":\"0123456789abcdef0123456789abcdef\"}"));
    assert(!parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"external_import\",\"remote_"
                  "account_id\":\"0123456789abcdef0123456789abcdef\"}"));
    assert(!parse(
        "{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"network_activate\",\"replace_index\":2}"));
    assert(parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"account_remove\",\"account_id\":"
                 "\"0123456789abcdef0123456789abcdef\"}"));
    assert(command.op == QUOTA_PORTABLE_OP_ACCOUNT_REMOVE);
    /* Removed in protocol 3. codex_launch is the internal step of validation, not a command. */
    const char *removed[] = {"mode_select", "network_scan", "codex_launch", "network_activate"};
    for (size_t i = 0; i < sizeof(removed) / sizeof(removed[0]); i++) {
        char frame[128];
        snprintf(frame, sizeof(frame), "{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"%s\"}",
                 removed[i]);
        assert(!parse(frame));
    }
    assert(!parse("{\"v\":3,\"request_id\":\"abcdef01\",\"op\":\"refresh\",\"endpoint_epoch\":1}"));
    unsigned char zero[sizeof(command)] = {0};
    assert(memcmp(&command, zero, sizeof(command)) == 0); /* Rejected secrets cleared. */
    char huge[QUOTA_PORTABLE_COMMAND_BYTES + 2];
    memset(huge, ' ', sizeof(huge));
    assert(!quota_portal_parse_command(huge, sizeof(huge), &command));
    puts("portable command runtime checks passed");
}
'''
        compile_and_run(
            harness,
            "quota-portable-command-",
            (
                "main/quota_portal.c",
                "main/quota_json.c",
                "main/quota_logic.c",
                "tests/host_sdk/host_sdk_embedded.c",
                "tests/cjson/cJSON.c",
            ),
            host_sdk=True,
        )

    def test_ap_socket_session_and_origin_gate(self):
        harness = r'''
#include "quota_portal.h"
#include <assert.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include "esp_http_server.h"
typedef struct {
    const char *host, *origin, *secret;
} fixture_t;
static bool active;
static uint32_t peer_ip, local_ip;
static unsigned peer_form, local_form;
static bool peer_failure, local_failure;
static const char *get(httpd_req_t *r, const char *name)
{
    const fixture_t *f = r->user_ctx;
    if (!strcmp(name, "Host"))
        return f->host;
    if (!strcmp(name, "Origin"))
        return f->origin;
    if (!strcmp(name, "X-AIQ-Access"))
        return f->secret;
    return NULL;
}
size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *name)
{
    const char *value = get(r, name);
    return value ? strlen(value) : 0;
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *name, char *out, size_t cap)
{
    const char *value = get(r, name);
    if (!value || strlen(value) >= cap)
        return 1;
    strcpy(out, value);
    return ESP_OK;
}
static int fake_address(struct sockaddr *addr, socklen_t *len, uint32_t ip, unsigned form,
                        bool failure)
{
    if (failure)
        return -1;
    struct sockaddr_storage storage = {0};
    socklen_t size;
    if (form == 0 || form == 6) {
        struct sockaddr_in *p = (void *)&storage;
        p->sin_family = AF_INET;
        p->sin_addr.s_addr = htonl(ip);
        size = sizeof(*p) - (form == 6 ? 1 : 0);
    } else if (form == 5) {
        storage.ss_family = AF_UNSPEC;
        size = sizeof(struct sockaddr);
    } else {
        struct sockaddr_in6 *p = (void *)&storage;
        p->sin6_family = AF_INET6;
        p->sin6_addr.s6_addr[10] = p->sin6_addr.s6_addr[11] = 0xff;
        uint32_t network_ip = htonl(ip);
        memcpy(p->sin6_addr.s6_addr + 12, &network_ip, 4);
        if (form == 2) {
            p->sin6_addr.s6_addr[0] = 0xfe;
            p->sin6_addr.s6_addr[1] = 0x80;
        }
        if (form == 3)
            p->sin6_addr.s6_addr[10] = p->sin6_addr.s6_addr[11] = 0;
        size = sizeof(*p) - (form == 4 ? 1 : 0);
    }
    assert(*len >= size);
    memcpy(addr, &storage, size);
    *len = size;
    return 0;
}
static int fake_peer(int fd, struct sockaddr *addr, socklen_t *len)
{
    (void)fd;
    return fake_address(addr, len, peer_ip, peer_form, peer_failure);
}
static int fake_local(int fd, struct sockaddr *addr, socklen_t *len)
{
    (void)fd;
    return fake_address(addr, len, local_ip, local_form, local_failure);
}
/* What the handlers send, so the tests can read headers and bodies. */
static char sent_headers[8][2][512], sent_body[512], sent_status[64];
static unsigned sent_header_count;
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value)
{
    (void)r;
    snprintf(sent_headers[sent_header_count][0], 512, "%s", field);
    snprintf(sent_headers[sent_header_count++][1], 512, "%s", value);
    return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status)
{
    (void)r;
    snprintf(sent_status, sizeof(sent_status), "%s", status);
    return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t *r, const char *body, ssize_t length)
{
    (void)r;
    snprintf(sent_body, sizeof(sent_body), "%.*s", length < 0 ? 400 : (int)length, body);
    return ESP_OK;
}
static const char *sent_header(const char *field)
{
    for (unsigned i = 0; i < sent_header_count; i++)
        if (!strcmp(sent_headers[i][0], field))
            return sent_headers[i][1];
    return NULL;
}
#define getpeername fake_peer
#define getsockname fake_local
#include "quota_portal.c"
static bool session(void *ctx)
{
    (void)ctx;
    return active;
}
static quota_portable_submit_result_t submit_stub(const quota_portable_command_t *c, void *x)
{
    (void)c;
    (void)x;
    return QUOTA_PORTABLE_SUBMIT_ACCEPTED;
}
static bool state_stub(char *b, size_t c, size_t *l, void *x)
{
    (void)b;
    (void)c;
    (void)l;
    (void)x;
    return false;
}
static bool allowed(httpd_req_t *r, bool secret, bool mutation)
{
    const char *denial = NULL;
    bool ok = authorize(r, secret, mutation, &denial);
    if (!ok)
        assert(denial && strstr(denial, active ? "unauthorized" : "session_expired"));
    return ok;
}
int main(void)
{
    const char *locked_probe = NULL;
    strcpy(s_secret, "K7QM-2X9D-PA4T-Z8RW");
    s_callbacks.session_active = session;
    active = true;
    peer_ip = 0xc0a80402;
    local_ip = 0xc0a80401;
    fixture_t f = {"192.168.4.1", "http://192.168.4.1", s_secret};
    httpd_req_t r = {.user_ctx = &f};
    assert(allowed(&r, true, true));
    f.origin = "http://evil.test";
    assert(!allowed(&r, true, true));
    assert(!allowed(&r, true, false)); /* Reject a supplied foreign GET Origin. */
    f.origin = NULL;
    assert(allowed(&r, true, false)); /* GET may omit Origin. */
    f.origin = "http://192.168.4.1";
    f.secret = "wrong";
    assert(!allowed(&r, true, false));
    assert(allowed(&r, false, false)); /* Static page contains no secret. */
    f.secret = s_secret;
    f.host = "evil.test";
    assert(!allowed(&r, false, false));
    f.host = "192.168.4.1";
    local_ip = 0xc0a80464;
    assert(!allowed(&r, true, false)); /* STA socket. */
    local_ip = 0xc0a80401;
    peer_ip = 0x0a000001;
    assert(!allowed(&r, true, false));
    peer_ip = 0xc0a80402;
    for (unsigned form = 2; form <= 6; form++) {
        peer_form = form;
        assert(!allowed(&r, true, false));
        peer_form = 0;
        local_form = form;
        assert(!allowed(&r, true, false));
        local_form = 0;
    }
    peer_failure = true;
    assert(!allowed(&r, true, false));
    peer_failure = false;
    local_failure = true;
    assert(!allowed(&r, true, false));
    local_failure = false;
    peer_form = local_form = 1;
#if LWIP_IPV6
    assert(allowed(&r, false, false)); /* Actual IDF dual-stack IPv4 navigation. */
    assert(allowed(&r, true, true));   /* Mapped endpoints preserve secret/Origin gates. */
    local_ip = 0xc0a80464;
    assert(!allowed(&r, true, false));
    local_ip = 0xc0a80401;
    peer_ip = 0x0a000001;
    assert(!allowed(&r, true, false));
    peer_ip = 0xc0a80402;
    peer_ip = 0xc0a80401;
    assert(!allowed(&r, true, false));
    peer_ip = 0xc0a804ff;
    assert(!allowed(&r, true, false));
    peer_ip = 0xc0a80402;
    peer_form = 0;
    assert(allowed(&r, true, false)); /* Mixed IPv4/mapped forms normalize equally. */
    peer_form = 1;
    local_form = 0;
    assert(allowed(&r, true, false));
#else
    assert(!allowed(&r, true, false)); /* IPv6-disabled build keeps IPv4 only. */
#endif
    peer_form = local_form = 0;
    /* Five wrong access codes in a row lock the API, even against the right code. */
    s_failures = 0;
    /* The count is of wrong codes in a row: a right one starts it again. */
    for (unsigned round = 0; round < 3; round++) {
        f.secret = "K7QM-2X9D-PA4T-Z8RX";
        for (unsigned attempt = 1; attempt < QUOTA_PORTABLE_ACCESS_FAILURES; attempt++) {
            const char *denial = NULL;
            assert(!authorize(&r, true, false, &denial) && !strcmp(denial, "unauthorized"));
        }
        assert(s_failures == QUOTA_PORTABLE_ACCESS_FAILURES - 1);
        f.secret = s_secret;
        assert(authorize(&r, true, false, &locked_probe) && s_failures == 0);
    }
    f.secret = "K7QM-2X9D-PA4T-Z8RX";
    for (unsigned attempt = 1; attempt <= QUOTA_PORTABLE_ACCESS_FAILURES; attempt++) {
        const char *denial = NULL;
        assert(!authorize(&r, true, false, &denial));
        assert(!strcmp(denial, attempt < QUOTA_PORTABLE_ACCESS_FAILURES ? "unauthorized"
                                                                         : "access_locked"));
    }
    f.secret = s_secret;
    const char *locked = NULL;
    assert(!authorize(&r, true, false, &locked) && !strcmp(locked, "access_locked"));
    assert(!authorize(&r, true, true, &locked) && !strcmp(locked, "access_locked"));
    assert(authorize(&r, false, false, &locked)); /* The page itself holds no secret. */
    /* A header that is missing does not count; opening a new hotspot session resets the count. */
    quota_portal_callbacks_t callbacks = {
        .submit = submit_stub, .state_json = state_stub, .session_active = session};
    quota_portal_stop();
    assert(quota_portal_start("K7QM-2X9D-PA4T-Z8RW", &callbacks) && s_failures == 0);
    assert(!quota_portal_start("K7QM-2X9D-PA4T-Z8RW", &callbacks)); /* already running */
    f.secret = NULL;
    assert(!authorize(&r, true, false, &locked) && s_failures == 0);
    f.secret = s_secret;
    assert(authorize(&r, true, false, &locked));
    quota_portal_stop();
    assert(!quota_portal_start("too-short", &callbacks));
    assert(!quota_portal_start("k7qm-2x9d-pa4t-z8rw", &callbacks));
    /* The page is served with its own CSP plus frame-ancestors; errors name protocol and
     * firmware. */
    assert(quota_portal_start("K7QM-2X9D-PA4T-Z8RW", &callbacks));
    sent_header_count = 0;
    assert(page_handler(&r) == ESP_OK);
    assert(!strcmp(sent_header("Content-Security-Policy"),
                   "default-src 'none'; script-src 'sha256-test'; frame-ancestors 'none'"));
    assert(!strcmp(sent_header("X-Frame-Options"), "DENY"));
    assert(strstr(sent_body, "http-equiv"));
    sent_header_count = 0;
    assert(reply_error(&r, "403 Forbidden", "access_locked") == ESP_OK);
    assert(!strcmp(sent_status, "403 Forbidden"));
    assert(!strcmp(sent_body, "{\"ok\":false,\"error_code\":\"access_locked\",\"protocol\":3,"
                              "\"firmware\":\"3.0.0-test\"}"));
    char csp[CSP_BYTES];
    static const char page[] = "<meta http-equiv=\"Content-Security-Policy\" content=\"a; b\">";
    assert(portal_csp(page, sizeof(page) - 1, csp, sizeof(csp)) == strlen(csp) &&
           !strcmp(csp, "a; b; frame-ancestors 'none'"));
    assert(portal_csp("<html></html>", 13, csp, sizeof(csp)) == strlen(csp) &&
           !strcmp(csp, "default-src 'none'; frame-ancestors 'none'"));
    csp[0] = 'x';
    assert(portal_csp(page, sizeof(page) - 1, csp, 24) == 0 && csp[0] == 0); /* too small */
    quota_portal_stop();
    active = false;
    assert(!allowed(&r, true, false));
    puts("portable AP authorization runtime checks passed");
}
'''
        for ipv6 in (1, 0):
            with self.subTest(ipv6=ipv6):
                compile_and_run(
                    f"#define LWIP_IPV6 {ipv6}\n" + harness,
                    "quota-portable-session-",
                    (
                        "main/quota_json.c",
                        "main/quota_logic.c",
                        "tests/host_sdk/host_sdk_embedded.c",
                        "tests/cjson/cJSON.c",
                    ),
                    host_sdk=True,
                )

    def test_csp_header_is_taken_from_the_real_page(self):
        """The device repeats the page's own policy as a header and adds frame-ancestors."""
        harness = r'''
#include "quota_portal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
size_t portal_csp(const char *html, size_t length, char *out, size_t capacity);
int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    FILE *file = fopen(argv[1], "rb");
    char *html = malloc(1 << 20);
    size_t length = file ? fread(html, 1, 1 << 20, file) : 0;
    char csp[512];
    size_t used = portal_csp(html, length, csp, sizeof(csp));
    if (!used || used != strlen(csp))
        return 1;
    puts(csp);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="quota-portal-csp-") as directory:
            path = Path(directory)
            (path / "test.c").write_text(harness)
            sources = (
                "main/quota_portal.c",
                "main/quota_json.c",
                "main/quota_logic.c",
                "tests/host_sdk/host_sdk_embedded.c",
                "tests/cjson/cJSON.c",
                "tests/host_sdk/host_sdk_defaults.c",
                "tests/host_sdk/module_defaults.c",
            )
            subprocess.run(
                [os.environ.get("CC", "cc"), "-std=c11", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra",
                 "-Werror", "-DQUOTA_HOST_TEST", "-I" + str(ROOT / "tests/host_sdk"),
                 "-I" + str(ROOT / "tests/bsp_stubs"), "-I" + str(ROOT / "components/bsp/include"),
                 "-I" + str(ROOT / "main"), "-I" + str(ROOT / "tests/cjson"),
                 str(path / "test.c"), *(str(ROOT / name) for name in sources), "-lm",
                 "-o", str(path / "test")],
                check=True,
            )
            page = ROOT / "main/setup_page.html"
            result = subprocess.run(
                [str(path / "test"), str(page)], check=True, capture_output=True, text=True
            )
        meta = re.search(
            r'http-equiv="Content-Security-Policy" content="([^"]+)"', page.read_text()
        )
        self.assertEqual(result.stdout.strip(), meta[1] + "; frame-ancestors 'none'")
        self.assertIn("script-src 'sha256-", result.stdout)
        self.assertIn("style-src 'sha256-", result.stdout)

    def test_atomic_credential_record_and_cache_identity(self):
        harness = r'''
#include "quota_store.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bool reject_store_allocation;
static unsigned store_allocations;
static void *store_calloc(size_t count, size_t bytes)
{
    store_allocations++;
    return reject_store_allocation ? NULL : calloc(count, bytes);
}
#define calloc store_calloc
#include "quota_store.c"
#undef calloc
#define LOAD_OK(slot, record)                                                                      \
    (quota_store_load_credential_result((slot), (record)) == QUOTA_STORE_READ_OK)
/* The already deployed v1 layout, independent of the implementation typedef. */
typedef struct {
    uint32_t magic;
    uint16_t version, bytes;
    quota_portable_credential_t value;
    uint32_t crc;
} legacy_credential_record_t;
_Static_assert(sizeof(legacy_credential_record_t) == sizeof(credential_record_t),
               "v1 size changed");
_Static_assert(offsetof(legacy_credential_record_t, value) == offsetof(credential_record_t, value),
               "v1 value moved");
_Static_assert(offsetof(legacy_credential_record_t, crc) == offsetof(credential_record_t, crc),
               "v1 CRC moved");
typedef struct {
    char key[16];
    size_t length;
    unsigned char data[16384];
} item_t;
static item_t items[10], pending;
static size_t item_count;
static bool fail_commit;
static int commits;
static esp_err_t open_error, query_error, data_error;
esp_err_t nvs_flash_init_partition(const char *part)
{
    assert(!strcmp(part, "portable"));
    return ESP_OK;
}
esp_err_t nvs_open_from_partition(const char *part, const char *space, int mode, nvs_handle_t *out)
{
    assert(!strcmp(part, "portable") && !strcmp(space, "quota_port"));
    assert(mode == NVS_READONLY || mode == NVS_READWRITE);
    if (open_error != ESP_OK)
        return open_error;
    *out = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t h)
{
    assert(h == 1);
    memset(&pending, 0, sizeof(pending));
}
static item_t *find(const char *key)
{
    for (size_t i = 0; i < item_count; i++)
        if (!strcmp(items[i].key, key))
            return &items[i];
    return NULL;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *bytes)
{
    assert(h == 1);
    item_t *item = find(key);
    if (!item)
        return ESP_ERR_NVS_NOT_FOUND;
    if (!out) {
        if (query_error != ESP_OK)
            return query_error;
        *bytes = item->length;
        return ESP_OK;
    }
    if (data_error != ESP_OK)
        return data_error;
    if (*bytes < item->length)
        return ESP_FAIL;
    memcpy(out, item->data, item->length);
    *bytes = item->length;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t bytes)
{
    assert(h == 1 && bytes <= sizeof(pending.data));
    strcpy(pending.key, key);
    memcpy(pending.data, value, bytes);
    pending.length = bytes;
    return ESP_OK;
}
esp_err_t nvs_open(const char *space, int mode, nvs_handle_t *out)
{
    (void)space;
    (void)mode;
    (void)out;
    return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_erase_all(nvs_handle_t h)
{
    (void)h;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    (void)h;
    (void)key;
    return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_commit(nvs_handle_t h)
{
    assert(h == 1);
    commits++;
    if (fail_commit)
        return 1;
    item_t *item = find(pending.key);
    if (!item) {
        assert(item_count < 10);
        item = &items[item_count++];
    }
    *item = pending;
    return ESP_OK;
}
static void assert_workspace_clear(void)
{
    if (!s_credential_record)
        return;
    const unsigned char *p = (const void *)s_credential_record;
    for (size_t i = 0; i < sizeof(*s_credential_record); i++)
        assert(p[i] == 0);
}
int main(void)
{
    assert(quota_store_init());
    quota_portable_credential_t *c = calloc(1, sizeof(*c)), *read = calloc(1, sizeof(*read));
    assert(c && read);
    strcpy(c->id, "0123456789abcdef0123456789abcdef");
    c->generation = 1;
    c->slot = 0;
    c->provider = QUOTA_PROVIDER_CODEX;
    c->auth_state = QUOTA_PORTABLE_AUTH_READY;
    strcpy(c->server_account_id, "fake-workspace");
    strcpy(c->access_token, "old-access");
    strcpy(c->refresh_token, "old-refresh");
    unsigned allocations = store_allocations;
    reject_store_allocation = true;
    assert(!quota_store_credential_acquire());
    assert(!quota_store_save_credential(0, c));
    assert(!s_credential_record);
    reject_store_allocation = false;
    open_error = ESP_OK;
    query_error = ESP_OK;
    data_error = ESP_OK;
    memset(read, 0xa5, sizeof(*read));
    assert(quota_store_load_credential_result(3, read) == QUOTA_STORE_READ_MISSING);
    assert(read->id[0] == 0 && read->refresh_token[0] == 0);
    assert(!LOAD_OK(3, read));
    reject_store_allocation = true;
    assert(quota_store_load_credential_result(0, read) == QUOTA_STORE_READ_NO_MEMORY);
    assert(read->id[0] == 0 && read->refresh_token[0] == 0);
    reject_store_allocation = false;
    assert(quota_store_save_credential(0, c));
    assert(quota_store_load_credential_result(0, read) == QUOTA_STORE_READ_OK);
    item_t *stored_credential = find("account0");
    assert(stored_credential);
    size_t saved_credential_length = stored_credential->length;
    stored_credential->length--;
    assert(quota_store_load_credential_result(0, read) == QUOTA_STORE_READ_INVALID);
    assert(!LOAD_OK(0, read));
    stored_credential->length = saved_credential_length;
    open_error = ESP_FAIL;
    assert(quota_store_load_credential_result(0, read) == QUOTA_STORE_READ_IO_ERROR);
    assert(read->id[0] == 0 && read->refresh_token[0] == 0);
    open_error = ESP_OK;
    query_error = ESP_FAIL;
    assert(quota_store_load_credential_result(0, read) == QUOTA_STORE_READ_IO_ERROR);
    query_error = ESP_OK;
    data_error = ESP_FAIL;
    assert(quota_store_load_credential_result(0, read) == QUOTA_STORE_READ_IO_ERROR);
    data_error = ESP_OK;
    assert(LOAD_OK(0, read));
    assert_workspace_clear();
    assert(!read->refresh_inflight);
    c->refresh_inflight = true;
    assert(quota_store_save_credential(0, c));
    assert(LOAD_OK(0, read) && read->refresh_inflight);
    strcpy(c->access_token, "new-access");
    strcpy(c->refresh_token, "new-refresh");
    c->refresh_inflight = false;
    fail_commit = true;
    assert(!quota_store_save_credential(0, c));
    fail_commit = false;
    assert(LOAD_OK(0, read) && read->refresh_inflight);
    assert(!strcmp(read->refresh_token, "old-refresh")); /* Marker survives lost final write. */
    assert(quota_store_save_credential(0, c));
    assert(LOAD_OK(0, read));
    assert(!read->refresh_inflight && !strcmp(read->refresh_token, "new-refresh"));
    int before = commits;
    memset(c->access_token, 'x', sizeof(c->access_token));
    assert(!quota_store_save_credential(0, c) && commits == before);
    strcpy(c->access_token, "new-access");
    assert(store_allocations > allocations &&
           !s_credential_record); /* External records are temporary. */
    reject_store_allocation = false;
    quota_portable_credential_t *workspace = quota_store_credential_acquire();
    assert(workspace);
    assert(!quota_store_credential_acquire()); /* Exclusive borrowing lifetime. */
    strcpy(workspace->refresh_token, "pending-rotation");
    workspace->refresh_inflight = true;
    quota_portable_credential_t pending_copy = *workspace;
    strcpy(read->email, "caller-buffer");
    assert(quota_store_load_credential_result(0, read) == QUOTA_STORE_READ_BUSY);
    assert(!strcmp(read->email, "caller-buffer"));
    assert(!memcmp(workspace, &pending_copy, sizeof(pending_copy)));
    assert(!LOAD_OK(0, read));
    assert(!strcmp(read->email, "caller-buffer"));
    assert(!quota_store_save_credential(0, c));
    assert(!quota_store_remove_credential(0, c->id, 2));
    quota_portable_clear_secret(&pending_copy, sizeof(pending_copy));
    legacy_credential_record_t legacy = {0};
    legacy.magic = CREDENTIAL_MAGIC;
    legacy.version = 1;
    legacy.bytes = sizeof(legacy);
    legacy.value = *c;
    legacy.value.slot = 1;
    legacy.value.generation = 7;
    strcpy(legacy.value.id, "11111111111111111111111111111111");
    legacy.crc = store_crc(&legacy, offsetof(legacy_credential_record_t, crc));
    assert(nvs_set_blob(1, "account1", &legacy, sizeof(legacy)) == ESP_OK &&
           nvs_commit(1) == ESP_OK);
    nvs_close(1);
    allocations = store_allocations;
    reject_store_allocation = true;
    assert(LOAD_OK(1, workspace));
    assert(!memcmp(workspace, &legacy.value,
                   sizeof(*workspace))); /* Existing v1 data loads in place. */
    workspace->refresh_inflight = true;
    assert(quota_store_save_credential(1, workspace));
    assert(workspace->refresh_inflight && !strcmp(workspace->refresh_token, "new-refresh"));
    strcpy(workspace->access_token, "received-access");
    strcpy(workspace->refresh_token, "received-refresh");
    workspace->refresh_inflight = false;
    legacy.value = *workspace;
    fail_commit = true;
    assert(!quota_store_save_credential(1, workspace));
    fail_commit = false;
    assert(!memcmp(workspace, &legacy.value,
                   sizeof(*workspace))); /* Failed commit retains received rotation for retry. */
    legacy_credential_record_t committed;
    memcpy(&committed, find("account1")->data, sizeof(committed));
    assert(committed.value.refresh_inflight &&
           !strcmp(committed.value.refresh_token, "new-refresh"));
    assert(quota_store_save_credential(1, workspace));
    assert(!memcmp(workspace, &legacy.value, sizeof(*workspace)));
    assert(store_allocations ==
           allocations); /* Alias load/save and retry perform no heap allocation. */
    reject_store_allocation = false;
    quota_store_credential_release(workspace);
    assert(!s_credential_record);
    assert(quota_store_remove_credential(1, legacy.value.id, 8));
    assert(!s_credential_record);
    workspace = quota_store_credential_acquire();
    assert(workspace);
    allocations = store_allocations;
    reject_store_allocation = true;
    assert(LOAD_OK(1, workspace));
    assert(workspace->tombstone && workspace->generation == 8 &&
           !strcmp(workspace->id, legacy.value.id));
    assert(!LOAD_OK(QUOTA_MAX_ACCOUNTS, workspace));
    assert_workspace_clear();
    item_t *bad = find("account1");
    assert(bad);
    credential_record_t valid_credential;
    memcpy(&valid_credential, bad->data, sizeof(valid_credential));
    credential_record_t malformed_credential = valid_credential;
    malformed_credential.value.provider = (quota_provider_t)99;
    malformed_credential.crc = store_crc(&malformed_credential, offsetof(credential_record_t, crc));
    memcpy(bad->data, &malformed_credential, sizeof(malformed_credential));
    assert(quota_store_load_credential_result(1, workspace) == QUOTA_STORE_READ_INVALID);
    assert_workspace_clear();
    memcpy(bad->data, &valid_credential, sizeof(valid_credential));
    bad->data[20] ^= 1;
    assert(quota_store_load_credential_result(1, workspace) == QUOTA_STORE_READ_INVALID);
    assert_workspace_clear();
    assert(store_allocations == allocations);
    quota_store_credential_release(workspace);
    assert(!s_credential_record);
    reject_store_allocation = false;
    assert(quota_store_remove_credential(0, c->id, 2));
    assert(quota_store_load_credential_result(0, read) == QUOTA_STORE_READ_OK);
    assert(read->tombstone && read->generation == 2 && read->access_token[0] == 0 &&
           read->refresh_token[0] == 0);
    item_t *record = find("account0");
    assert(record);
    record->data[20] ^= 1;
    assert(!LOAD_OK(0, read));
    assert(read->id[0] == 0 && read->refresh_token[0] == 0);
    assert_workspace_clear();
    free(c);
    free(read);
    puts("portable storage runtime checks passed");
}
'''
        nvs_header = '''#pragma once
#include <stddef.h>
typedef int nvs_handle_t;
typedef int esp_err_t;
enum { ESP_OK=0, NVS_READONLY=1, NVS_READWRITE=2, ESP_FAIL=3, ESP_ERR_NO_MEM=4,
       ESP_ERR_NVS_NOT_FOUND=5, ESP_ERR_NVS_PART_NOT_FOUND=6,
       ESP_ERR_NVS_TYPE_MISMATCH=7 };
esp_err_t nvs_open_from_partition(const char*,const char*,int,nvs_handle_t*);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);
esp_err_t nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);
esp_err_t nvs_commit(nvs_handle_t);
esp_err_t nvs_open(const char*,int,nvs_handle_t*);
esp_err_t nvs_erase_all(nvs_handle_t);
esp_err_t nvs_erase_key(nvs_handle_t,const char*);
'''
        with tempfile.TemporaryDirectory(prefix="quota-portable-store-") as directory:
            path = Path(directory)
            (path / "nvs.h").write_text(nvs_header)
            (path / "nvs_flash.h").write_text(
                '#pragma once\n#include "nvs.h"\nesp_err_t nvs_flash_init_partition(const char*);\n'
            )
            (path / "test.c").write_text(harness)
            subprocess.run(
                [
                    os.environ.get("CC", "cc"),
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wno-deprecated-declarations",
                    "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer",
                    "-I" + str(path),
                    "-I" + str(ROOT / "main"),
                    "-I" + str(ROOT / "tests/cjson"),
                    str(path / "test.c"),
                    str(ROOT / "main/quota_logic.c"),
                    str(ROOT / "main/quota_catalog.c"),
                    str(ROOT / "tests/cjson/cJSON.c"),
                    "-lm",
                    "-o",
                    str(path / "test"),
                ],
                check=True,
            )
            subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
