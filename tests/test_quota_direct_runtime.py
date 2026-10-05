#!/usr/bin/env python3
"""Compile the actual provider sources with a deterministic HTTP/storage fake."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ESP_HEADER = r'''
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_TIMEOUT 4
#define ESP_ERR_HTTP_EAGAIN 5
#define HTTP_METHOD_POST 1
#define HTTP_METHOD_GET 0
#define HTTP_TRANSPORT_OVER_SSL 1
#define HTTP_EVENT_REDIRECT 1
#define HTTP_EVENT_ON_HEADER 2
#define HTTP_EVENT_ON_DATA 3
#define HTTP_EVENT_DISCONNECTED 4
#define HTTP_EVENT_ERROR 5
#define HTTP_EVENT_ON_CONNECTED 6
#define HTTP_EVENT_HEADERS_SENT 7
typedef struct client *esp_http_client_handle_t;
typedef struct { int event_id; void *user_data; void *data; int data_len;
    char *header_key; char *header_value; esp_http_client_handle_t client;
} esp_http_client_event_t;
typedef struct { const char *url; int method; void *crt_bundle_attach; int timeout_ms;
    bool is_async; bool disable_auto_redirect; int max_authorization_retries;
    esp_err_t (*event_handler)(esp_http_client_event_t *); int transport_type;
    int buffer_size; int buffer_size_tx; void *user_data; bool skip_cert_common_name_check;
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t,const char *,const char *);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t,const char *,int);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t,int);
esp_err_t esp_http_client_perform(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
esp_err_t esp_http_client_close(esp_http_client_handle_t);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
int64_t esp_timer_get_time(void);
#define pdMS_TO_TICKS(n) (n)
void vTaskDelay(unsigned);
void *quota_test_response_realloc(void *,size_t);
'''

ESP_TEST = r'''
#include "quota_direct.h"
#include "esp_http_client.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
struct client { esp_http_client_config_t config; int status; bool closed; unsigned steps; };
static int mode;
static int64_t now;
static unsigned initialized, closed, performed, stores;
static unsigned growth_calls;
static size_t growth_sizes[8];
static bool awake;
static unsigned warnings;
static char warning_text[128];
static quota_direct_credential_t saved;
void quota_test_log(const char *tag,const char *format,...) {
    assert(!strcmp(tag,"quota_direct"));
    va_list args;va_start(args,format);
    int length=vsnprintf(warning_text,sizeof(warning_text),format,args);va_end(args);
    assert(length>0&&(size_t)length<sizeof(warning_text));warnings++;
}
void *quota_test_response_realloc(void *old,size_t bytes) {
    assert(growth_calls<8);growth_sizes[growth_calls++]=bytes;
    if(mode==10||mode==13)return NULL;
    return realloc(old,bytes);
}
int64_t esp_timer_get_time(void) { return now; }
void vTaskDelay(unsigned ticks) { now += (int64_t)ticks * 1000; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    assert(config->is_async && config->disable_auto_redirect && !config->skip_cert_common_name_check);
    assert(config->timeout_ms == 1000 && config->crt_bundle_attach);
    struct client *client = calloc(1,sizeof(*client)); assert(client);
    client->config=*config; client->status=200; initialized++; return client;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c,const char *k,const char *v) { (void)c;(void)k;(void)v;return ESP_OK; }
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c,const char *b,int n) { (void)c;assert(b&&n>0);return ESP_OK; }
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t c,int n) { (void)c;assert(n>0&&n<=1000);return ESP_OK; }
esp_err_t esp_http_client_close(esp_http_client_handle_t c) { c->closed=true;closed++;return ESP_OK; }
int esp_http_client_get_status_code(esp_http_client_handle_t c) { return c->status; }
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) { return !c->closed&&c->status!=401; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { free(c);return ESP_OK; }
static void data(esp_http_client_handle_t c,void *body,int length) {
    esp_http_client_event_t e={.event_id=HTTP_EVENT_ON_DATA,.user_data=c->config.user_data,
        .data=body,.data_len=length,.client=c};
    /* Deliberately ignore callback return, just like actual IDF HEADER/DATA. */
    (void)c->config.event_handler(&e);
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t c) {
    performed++; c->steps++;
    if(mode==6)return ESP_FAIL; /* TLS/connect failure before headers. */
    if(mode==7){awake=false;return ESP_ERR_HTTP_EAGAIN;} /* Sleep during handshake. */
    if(mode==8){
        awake=false;
        esp_http_client_event_t e={.event_id=HTTP_EVENT_ON_CONNECTED,.user_data=c->config.user_data,.client=c};
        (void)c->config.event_handler(&e);assert(c->closed);return ESP_FAIL;
    }
    if(c->steps==1){
        esp_http_client_event_t e={.event_id=HTTP_EVENT_HEADERS_SENT,.user_data=c->config.user_data,.client=c};
        (void)c->config.event_handler(&e);
    }
    if(mode==5){c->status=401;return ESP_FAIL;} /* Actual disabled-auth-retry behavior. */
    if (mode==0) {
        if(c->steps==1){now+=100000;return ESP_ERR_HTTP_EAGAIN;}
        char body[]="{\"is_available\":true,\"balance_infos\":[]}"; data(c,body,(int)strlen(body));return ESP_OK;
    }
    if(mode==1){ /* Continuous data within one perform must close at budget. */
        for(unsigned i=0;i<3&&!c->closed;i++){now+=6000000;data(c,"x",1);}return ESP_OK;
    }
    if(mode==2){now+=1000000;return ESP_ERR_HTTP_EAGAIN;}
    if(mode==3){data(c,"x",32769);return ESP_OK;}
    if(mode>=9){
        size_t bytes=(mode==11||mode==12)?32768:12000;
        char *body=malloc(bytes);assert(body);memset(body,' ',bytes);
        const char *json="{\"is_available\":true,\"balance_infos\":[]}";
        memcpy(body,json,strlen(json));
        data(c,body,3000);
        data(c,body+3000,3000); /* crosses initial 4 KiB without losing first chunk */
        if(!c->closed)data(c,body+6000,(int)(bytes-6000));
        if(mode==12&&!c->closed)data(c,"x",1); /* exact hard cap followed by one byte */
        free(body);return ESP_OK;
    }
    char large[4098]; memset(large,'x',sizeof(large)-1);large[sizeof(large)-1]=0;
    esp_http_client_event_t e={.event_id=HTTP_EVENT_ON_HEADER,.user_data=c->config.user_data,
        .header_key="X-Test",.header_value=large,.client=c};
    (void)c->config.event_handler(&e);return ESP_OK;
}
static bool current(void *ctx,const char *id,uint32_t gen){(void)ctx;return !strcmp(id,"0123456789abcdef0123456789abcdef")&&gen==1;}
static bool gate(void *ctx,const char *id,uint32_t gen){return awake&&current(ctx,id,gen);}
static bool save(void *ctx,const quota_direct_credential_t *c){(void)ctx;saved=*c;stores++;return true;}
int main(void) {
    for(mode=0;mode<14;mode++) {
        now=0;initialized=closed=performed=stores=0;
        growth_calls=0;memset(growth_sizes,0,sizeof(growth_sizes));
        warnings=0;warning_text[0]=0;
        awake=true;
        quota_direct_hooks_t hooks={.admit=gate,.account_current=current,.persist=save};
        quota_direct_t *d=quota_direct_create(&hooks,NULL,NULL);assert(d);
        quota_direct_credential_t c={.generation=1,.provider=QUOTA_PROVIDER_DEEPSEEK};
        strcpy(c.id,"0123456789abcdef0123456789abcdef");strcpy(c.api_key,"synthetic-key");
        if(mode==0||mode==5||mode==9||mode==11||mode==13) {
            quota_direct_result_code_t expected=mode==5?QUOTA_DIRECT_AUTH_REQUIRED:mode==13?QUOTA_DIRECT_NO_MEMORY:QUOTA_DIRECT_OK;
            assert(quota_direct_query(d,&c,1700000000).code==expected);
            assert(initialized==1&&performed==(mode==0?2u:1u));
            assert(closed==(mode==13?1u:0u));
            if(mode==9||mode==11){assert(growth_calls==2&&growth_sizes[0]==8193&&growth_sizes[1]==(mode==9?16385:32769));}
            if(mode==13)assert(growth_calls==1&&growth_sizes[0]==8193);
            if(mode==0||mode==5)assert(growth_calls==0); /* ordinary small response never allocates 32 KiB */
        } else if(mode>=6&&mode<=8) {
            c.provider=QUOTA_PROVIDER_CODEX;strcpy(c.refresh_token,"synthetic-refresh");
            assert(quota_direct_refresh(d,&c,1700000000).code==(mode==6?QUOTA_DIRECT_NETWORK_ERROR:QUOTA_DIRECT_DEFERRED));
            assert(initialized==1&&performed==1&&!saved.refresh_inflight&&!c.refresh_inflight&&stores==2);
        } else {
            c.provider=QUOTA_PROVIDER_CODEX;strcpy(c.refresh_token,"synthetic-refresh");
            assert(quota_direct_refresh(d,&c,1700000000).code==QUOTA_DIRECT_AUTH_REQUIRED);
            assert(initialized==1&&closed>=1&&saved.refresh_inflight&&stores==1);
            unsigned count=performed;
            assert(quota_direct_refresh(d,&c,1700000000).code==QUOTA_DIRECT_AUTH_REQUIRED&&performed==count);
            if(mode==2)assert(now>=15000000&&performed<=15);
            if(mode==10)assert(growth_calls==1&&growth_sizes[0]==8193);
            if(mode==12)assert(growth_calls==2&&growth_sizes[1]==32769);
        }
        assert(warnings==((mode==10||mode==13)?1u:0u));
        if(warnings)assert(!strcmp(warning_text,"response grow failed capacity=8192 length=3000"));
        quota_direct_destroy(d);
    }
    puts("ESP transport budget and close: PASS");return 0;
}
'''


class DirectProviderRuntime(unittest.TestCase):
    def test_exchange_body_allocation(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix="quota-direct-form-") as directory:
            temporary = Path(directory)
            source = temporary / "form-runtime.c"
            source.write_text(r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "quota_direct.h"
static size_t allocation_bytes;
static bool reject_allocation;
static void *tracked_calloc(size_t count, size_t bytes) {
    allocation_bytes = count * bytes;
    return reject_allocation ? NULL : calloc(count, bytes);
}
#define calloc tracked_calloc
#include "quota_direct.c"
#undef calloc
int main(void) {
    quota_direct_authorization_t code = {0};
    strcpy(code.authorization_code, "once+&=");
    strcpy(code.code_verifier, "proof-verifier");
    char *body = exchange_body(&code); assert(body);
    assert(allocation_bytes == strlen(body) + 1 && allocation_bytes < 512);
    assert(strstr(body, "code=once%2B%26%3D&"));
    assert(strstr(body, "redirect_uri=https%3A%2F%2Fauth.openai.com%2Fdeviceauth%2Fcallback&"));
    free_body(body);
    /* Every permitted printable character agrees with the encoder. */
    for (unsigned i = 0; i < 94; ++i) code.authorization_code[i] = (char)(0x21 + i);
    code.authorization_code[94] = 0;
    body = exchange_body(&code); assert(body);
    assert(allocation_bytes == strlen(body) + 1); free_body(body);
    /* Both bounded inputs at their maximum, all requiring three-byte escapes. */
    memset(code.authorization_code, '+', QUOTA_DIRECT_CODE_BYTES);
    code.authorization_code[QUOTA_DIRECT_CODE_BYTES] = 0;
    memset(code.code_verifier, '%', QUOTA_DIRECT_VERIFIER_BYTES);
    code.code_verifier[QUOTA_DIRECT_VERIFIER_BYTES] = 0;
    body = exchange_body(&code); assert(body);
    assert(allocation_bytes == strlen(body) + 1 && allocation_bytes < 13696);
    assert(strlen(body) > 3 * QUOTA_DIRECT_CODE_BYTES && strlen(body) <= QUOTA_DIRECT_BODY_BYTES);
    const char *encoded = strstr(body, "&code=") + strlen("&code=");
    for (unsigned i = 0; i < QUOTA_DIRECT_CODE_BYTES; ++i) assert(!memcmp(encoded + 3 * i, "%2B", 3));
    assert(encoded[3 * QUOTA_DIRECT_CODE_BYTES] == '&'); free_body(body);
    reject_allocation = true; assert(exchange_body(&code) == NULL); reject_allocation = false;
    /* Missing terminator at the input bound is rejected before allocation. */
    memset(code.authorization_code, '+', sizeof(code.authorization_code));
    allocation_bytes = 0; assert(exchange_body(&code) == NULL && allocation_bytes == 0);
    puts("exchange allocation: PASS"); return 0;
}
''')
            executable = temporary / "form-runtime"
            command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-Wno-deprecated-declarations", "-fsanitize=address,undefined",
                       "-fno-omit-frame-pointer", "-I", str(root / "main"),
                       "-I", str(root / "tests/cjson"), str(source),
                       str(root / "main/quota_direct_logic.c"), str(root / "tests/cjson/cJSON.c"),
                       "-lm", "-o", str(executable)]
            compilation = subprocess.run(command, cwd=root, capture_output=True, text=True)
            self.assertEqual(compilation.returncode, 0, compilation.stderr)
            completed = subprocess.run([str(executable)], cwd=root, capture_output=True, text=True)
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            self.assertIn("exchange allocation: PASS", completed.stdout)

    def test_actual_esp_transport(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix="quota-direct-esp-") as directory:
            temporary = Path(directory)
            (temporary / "freertos").mkdir()
            (temporary / "esp_http_client.h").write_text(ESP_HEADER)
            (temporary / "esp_crt_bundle.h").write_text("#define esp_crt_bundle_attach ((void *)1)\n")
            (temporary / "esp_log.h").write_text(
                'void quota_test_log(const char *,const char *,...);\n#define ESP_LOGW quota_test_log\n')
            for name in ("esp_timer.h", "freertos/FreeRTOS.h", "freertos/task.h"):
                (temporary / name).write_text('#include "esp_http_client.h"\n')
            source = temporary / "esp-runtime.c"
            source.write_text(ESP_TEST)
            executable = temporary / "esp-runtime"
            command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-Wno-deprecated-declarations", "-DESP_PLATFORM",
                       "-DQUOTA_DIRECT_RESPONSE_REALLOC=quota_test_response_realloc", "-fsanitize=address,undefined",
                       "-fno-omit-frame-pointer", "-I", str(temporary), "-I", str(root / "main"),
                       "-I", str(root / "tests/cjson"), str(source), str(root / "main/quota_direct.c"),
                       str(root / "main/quota_direct_logic.c"), str(root / "tests/cjson/cJSON.c"),
                       "-lm", "-o", str(executable)]
            compilation = subprocess.run(command, cwd=root, capture_output=True, text=True)
            self.assertEqual(compilation.returncode, 0, compilation.stderr)
            completed = subprocess.run([str(executable)], cwd=root, capture_output=True, text=True)
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            self.assertIn("ESP transport budget and close: PASS", completed.stdout)

    def test_actual_provider_sources(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix="quota-direct-") as directory:
            executable = Path(directory) / "test-quota-direct"
            command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-Wno-deprecated-declarations",  # bundled cJSON on current macOS SDK
                       "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                       "-I", str(root / "main"), "-I", str(root / "tests/cjson"),
                       str(root / "tests/test_quota_direct.c"),
                       str(root / "main/quota_direct.c"), str(root / "main/quota_direct_logic.c"),
                       str(root / "tests/cjson/cJSON.c"), "-lm", "-o", str(executable)]
            compilation = subprocess.run(command, cwd=root, capture_output=True, text=True)
            self.assertEqual(compilation.returncode, 0, compilation.stderr)
            completed = subprocess.run([str(executable)], cwd=root, capture_output=True, text=True)
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            self.assertIn("rotation runtime: PASS", completed.stdout)


if __name__ == "__main__":
    unittest.main()
