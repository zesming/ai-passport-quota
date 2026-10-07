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
#define MALLOC_CAP_8BIT 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_DMA 4
typedef void (*esp_alloc_failed_hook_t)(size_t,uint32_t,const char*);
esp_err_t heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t);
size_t heap_caps_get_free_size(uint32_t);
size_t heap_caps_get_largest_free_block(uint32_t);
size_t heap_caps_get_minimum_free_size(uint32_t);
esp_err_t heap_caps_monitor_local_minimum_free_size_start(void);
esp_err_t heap_caps_monitor_local_minimum_free_size_stop(void);
unsigned uxTaskGetStackHighWaterMark(void *);
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
esp_err_t esp_http_client_get_header(esp_http_client_handle_t,const char *,char **);
esp_err_t esp_http_client_delete_header(esp_http_client_handle_t,const char *);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t,const char *,int);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t,int);
esp_err_t esp_http_client_perform(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
esp_err_t esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t,int*,int*);
esp_err_t esp_http_client_close(esp_http_client_handle_t);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
int64_t esp_timer_get_time(void);
#define pdMS_TO_TICKS(n) (n)
void vTaskDelay(unsigned);
void *quota_test_response_calloc(size_t,size_t);
'''

ESP_TEST = r'''
#include "quota_direct.h"
#include "esp_http_client.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
struct client { esp_http_client_config_t config; int status; bool closed; unsigned steps;
    char *authorization; const char *post; unsigned body_detach, auth_delete; };
static int mode;
static int64_t now;
static unsigned initialized,closed,performed,stores,headers_sent,response_allocations;
static size_t response_bytes;
static bool awake;
static quota_direct_credential_t saved;
static esp_alloc_failed_hook_t failed_hook;
esp_err_t heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t hook){failed_hook=hook;return ESP_OK;}
size_t heap_caps_get_free_size(uint32_t caps){(void)caps;return mode==7?12000:98000;}
static bool heap_monitor;
esp_err_t heap_caps_monitor_local_minimum_free_size_start(void){assert(!heap_monitor);heap_monitor=true;return ESP_OK;}
esp_err_t heap_caps_monitor_local_minimum_free_size_stop(void){assert(heap_monitor);heap_monitor=false;return ESP_OK;}
size_t heap_caps_get_minimum_free_size(uint32_t caps){assert(heap_monitor);return heap_caps_get_free_size(caps);}
size_t heap_caps_get_largest_free_block(uint32_t caps){(void)caps;return 65000;}
unsigned uxTaskGetStackHighWaterMark(void *task){(void)task;return 2000;}
void quota_test_log(const char *tag,const char *format,...){(void)tag;(void)format;}
void *quota_test_response_calloc(size_t count,size_t bytes) {
    response_allocations++;response_bytes=count*bytes;
    if(mode==5||mode==6||mode==17)return NULL;
    return calloc(count,bytes);
}
int64_t esp_timer_get_time(void){return now;}
void vTaskDelay(unsigned ticks){now+=(int64_t)ticks*1000;}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config){
    assert(config->is_async&&config->disable_auto_redirect&&!config->skip_cert_common_name_check);
    assert(config->timeout_ms==1000&&config->crt_bundle_attach&&config->max_authorization_retries==-1);
    assert(config->buffer_size_tx>=1024&&config->buffer_size_tx<=9216);
    assert(response_allocations==0); /* No response allocation before TLS. */
    initialized++;if(mode==15)return NULL;
    struct client *c=calloc(1,sizeof(*c));assert(c);c->config=*config;c->status=200;return c;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c,const char *key,const char *value){
    if(mode==16)return ESP_ERR_NO_MEM;
    if(!strcmp(key,"Authorization")){
        assert(value&&!strncmp(value,"Bearer ",7));
        c->authorization=malloc(strlen(value)+1);assert(c->authorization);strcpy(c->authorization,value);
        if(mode==19)assert(strlen(value)==8199&&c->config.buffer_size_tx==9216);
    }return ESP_OK;
}
esp_err_t esp_http_client_get_header(esp_http_client_handle_t c,const char *key,char **value){
    assert(!strcmp(key,"Authorization"));*value=c->authorization;return ESP_OK;
}
esp_err_t esp_http_client_delete_header(esp_http_client_handle_t c,const char *key){
    assert(!strcmp(key,"Authorization"));if(c->authorization){assert(c->authorization[0]==0);free(c->authorization);c->authorization=NULL;}
    c->auth_delete++;return ESP_OK;
}
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c,const char *body,int length){
    assert((body&&length>0)||(!body&&!length));
    if(body)c->post=body;else {c->post=NULL;c->body_detach++;}return ESP_OK;
}
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t c,int n){(void)c;assert(n>0&&n<=1000);return ESP_OK;}
esp_err_t esp_http_client_close(esp_http_client_handle_t c){c->closed=true;closed++;return ESP_OK;}
int esp_http_client_get_status_code(esp_http_client_handle_t c){return c->status;}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c){return !c->closed&&c->status!=401;}
esp_err_t esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t c,int *code,int *flags){
    (void)c;*flags=0;*code=(mode==10||mode==11||mode==21)?1:0;return *code?123:ESP_OK;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c){
    assert(!c->post&&!c->authorization&&c->body_detach>=1);free(c);return ESP_OK;
}
static void event(esp_http_client_handle_t c,int id,char *key,char *value,void *body,int length){
    esp_http_client_event_t e={.event_id=id,.user_data=c->config.user_data,.header_key=key,
        .header_value=value,.data=body,.data_len=length,.client=c};
    (void)c->config.event_handler(&e); /* IDF does not use callback result. */
}
static void payload(esp_http_client_handle_t c,size_t bytes){
    const char *json="{\"is_available\":true,\"balance_infos\":[]}";
    char *body=malloc(bytes);assert(body);memset(body,' ',bytes);memcpy(body,json,strlen(json));
    event(c,HTTP_EVENT_ON_DATA,NULL,NULL,body,10);
    if(!c->closed)event(c,HTTP_EVENT_ON_DATA,NULL,NULL,body+10,(int)(bytes-10));
    free(body);
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t c){
    performed++;c->steps++;
    if(mode==10||mode==21)return ESP_FAIL;
    if(mode==9){awake=false;return ESP_ERR_HTTP_EAGAIN;}
    if(c->steps==1){
        if(mode==8)awake=false;
        event(c,HTTP_EVENT_ON_CONNECTED,NULL,NULL,NULL,0);
        if(c->closed)return ESP_FAIL;
        if(c->config.method==HTTP_METHOD_POST)assert(response_allocations==1&&response_bytes==32769&&c->post);
        event(c,HTTP_EVENT_HEADERS_SENT,NULL,NULL,NULL,0);headers_sent++;
    }
    if(mode==11){assert(failed_hook);failed_hook(24,MALLOC_CAP_DMA,"allocator");return ESP_FAIL;}
    if(mode==18){now+=1000000;return ESP_ERR_HTTP_EAGAIN;}
    if(mode==13){c->status=401;return ESP_FAIL;} /* No header callbacks: cleanup still owns body. */
    if(mode==4){event(c,HTTP_EVENT_ON_HEADER,"Content-Length","32769",NULL,0);assert(c->closed);return ESP_FAIL;}
    size_t length=(mode==1||mode==3)?12000:mode==2||mode==14?32768:47;
    if(c->config.method==HTTP_METHOD_POST){
        c->status=429;event(c,HTTP_EVENT_ON_HEADER,"Retry-After","120",NULL,0);
        assert(!c->post&&c->body_detach==1);
        event(c,HTTP_EVENT_ON_HEADER,"Date","Tue, 14 Nov 2023 22:13:20 GMT",NULL,0);
        assert(c->body_detach==1); /* Many headers do not double-release body. */
    }
    if(mode!=1&&mode!=3&&c->steps==1){char decimal[16];snprintf(decimal,sizeof(decimal),"%u",(unsigned)length);
        event(c,HTTP_EVENT_ON_HEADER,"Content-Length",decimal,NULL,0);}
    if(mode==0&&c->steps==1)return ESP_ERR_HTTP_EAGAIN;
    payload(c,length);
    if((mode==3||mode==14)&&!c->closed)event(c,HTTP_EVENT_ON_DATA,NULL,NULL,"x",32769);
    return c->closed?ESP_FAIL:ESP_OK;
}
static bool current(void *ctx,const char *id,uint32_t gen){(void)ctx;return !strcmp(id,"0123456789abcdef0123456789abcdef")&&gen==1;}
static bool gate(void *ctx,const char *id,uint32_t gen){return awake&&current(ctx,id,gen);}
static bool save(void *ctx,const quota_direct_credential_t *c){(void)ctx;saved=*c;stores++;return !((mode==12||mode==17)&&stores==2);}
int main(void){
    for(mode=0;mode<=21;mode++){
        now=0;initialized=closed=performed=stores=headers_sent=response_allocations=0;response_bytes=0;awake=true;
        quota_direct_hooks_t hooks={.admit=gate,.account_current=current,.persist=save};
        quota_direct_t *d=quota_direct_create(&hooks,NULL,NULL);assert(d);
        quota_direct_credential_t c={.generation=1,.provider=QUOTA_PROVIDER_DEEPSEEK};
        strcpy(c.id,"0123456789abcdef0123456789abcdef");strcpy(c.api_key,"synthetic-key");
        bool post=(mode>=6&&mode<=18);
        if(post){c.provider=QUOTA_PROVIDER_CODEX;strcpy(c.refresh_token,"synthetic-refresh");}
        if(mode==19){memset(c.api_key,'A',512);c.api_key[512]=0;c.provider=QUOTA_PROVIDER_CODEX;
            memset(c.access_token,'A',8192);c.access_token[8192]=0;strcpy(c.server_account_id,"account");strcpy(c.server_user_id,"user");}
        if(mode==20){c.provider=QUOTA_PROVIDER_CODEX;memset(c.access_token,'A',sizeof(c.access_token));}
        quota_direct_result_t out=post?quota_direct_refresh(d,&c,1700000000):quota_direct_query(d,&c,1700000000);
        if(mode==0||mode==1||mode==2){assert(out.code==QUOTA_DIRECT_OK&&response_allocations==1);
            assert(response_bytes==((mode==1||mode==2)?32769:48));}
        else if(mode==19)assert(out.code==QUOTA_DIRECT_PROTOCOL_ERROR&&response_allocations==1&&response_bytes==48);
        else if(mode==3||mode==4)assert(out.code==QUOTA_DIRECT_RESPONSE_TOO_LARGE);
        else if(mode==5)assert(out.code==QUOTA_DIRECT_NO_MEMORY);
        else if(mode==6||mode==15||mode==16)assert(out.code==QUOTA_DIRECT_NO_MEMORY&&!headers_sent&&!c.refresh_inflight&&stores==2);
        else if(mode==7)assert(out.code==QUOTA_DIRECT_RESOURCE_ERROR&&!headers_sent&&!c.refresh_inflight&&stores==2);
        else if(mode==8||mode==9)assert(out.code==QUOTA_DIRECT_DEFERRED&&!headers_sent&&!c.refresh_inflight&&stores==2);
        else if(mode==10)assert(out.code==QUOTA_DIRECT_TLS_ERROR&&!headers_sent&&!c.refresh_inflight&&stores==2);
        else if(mode==11||mode==13||mode==14||mode==18){assert(out.code==QUOTA_DIRECT_AUTH_REQUIRED&&headers_sent&&c.refresh_inflight&&stores==1);
            unsigned count=performed;assert(quota_direct_refresh(d,&c,1700000000).code==QUOTA_DIRECT_AUTH_REQUIRED&&performed==count);}
        else if(mode==12||mode==17){assert(out.code==QUOTA_DIRECT_PERSIST_PENDING&&quota_direct_has_pending_persist(d));
            assert(headers_sent==(mode==12?1u:0u));unsigned count=performed;awake=false;
            assert(quota_direct_retry_persist(d).code==QUOTA_DIRECT_OK&&performed==count&&!saved.refresh_inflight);}
        else if(mode==20)assert(out.code==QUOTA_DIRECT_AUTH_REQUIRED&&!initialized);
        else if(mode==21)assert(out.code==QUOTA_DIRECT_TLS_ERROR&&out.tls_error==1&&!headers_sent);
        if(mode==18)assert(now>=15000000&&performed<=15);
        if(post&&mode!=15&&mode!=16&&mode!=10&&mode!=8&&mode!=9)assert(response_allocations==1);
        quota_direct_login_cancel(d);free(d);
    }
    puts("ESP transport reservation and ownership: PASS");return 0;
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
                'void quota_test_log(const char *,const char *,...);\n#define ESP_LOGW quota_test_log\n#define ESP_LOGI quota_test_log\n')
            for name in ("esp_timer.h", "esp_heap_caps.h", "freertos/FreeRTOS.h", "freertos/task.h"):
                (temporary / name).write_text('#include "esp_http_client.h"\n')
            source = temporary / "esp-runtime.c"
            source.write_text(ESP_TEST)
            executable = temporary / "esp-runtime"
            command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-Wno-deprecated-declarations", "-DESP_PLATFORM",
                       "-DQUOTA_DIRECT_RESPONSE_CALLOC=quota_test_response_calloc", "-fsanitize=address,undefined",
                       "-fno-omit-frame-pointer", "-I", str(temporary), "-I", str(root / "main"),
                       "-I", str(root / "tests/cjson"), str(source), str(root / "main/quota_direct.c"),
                       str(root / "main/quota_direct_logic.c"), str(root / "tests/cjson/cJSON.c"),
                       "-lm", "-o", str(executable)]
            compilation = subprocess.run(command, cwd=root, capture_output=True, text=True)
            self.assertEqual(compilation.returncode, 0, compilation.stderr)
            completed = subprocess.run([str(executable)], cwd=root, capture_output=True, text=True)
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            self.assertIn("ESP transport reservation and ownership: PASS", completed.stdout)

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
