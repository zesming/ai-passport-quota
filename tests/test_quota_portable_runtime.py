"""Execute portable storage and local-command validation C against fake data."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from runtime_helpers import ROOT, compile_and_run, extract_function


class PortableRuntime(unittest.TestCase):
    def test_command_limits_and_session_boundaries(self):
        source = (ROOT / "main/quota_portal.c").read_text()
        pure = source[source.index("bool quota_portal_host_is_valid"):
                      source.index("static esp_err_t reply_error")]
        harness = r'''
#include "quota_portal.h"
#include "cJSON.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
'''+pure+r'''
static quota_portable_command_t command;
static bool parse(const char *json) {
    return quota_portal_parse_command(json, strlen(json), &command);
}
int main(void) {
    assert(quota_portal_host_is_valid("192.168.4.1"));
    assert(quota_portal_host_is_valid("192.168.4.1:80"));
    assert(!quota_portal_host_is_valid("evil.test"));
    assert(!quota_portal_host_is_valid("192.168.4.1.evil.test"));
    assert(!quota_portal_host_is_valid("192.168.4.1:81"));
    assert(quota_portal_origin_is_valid("http://192.168.4.1"));
    assert(!quota_portal_origin_is_valid("null"));
    assert(!quota_portal_origin_is_valid("https://192.168.4.1"));
    char secret[44], wrong[44]; memset(secret, 'a', 43); secret[43] = 0;
    memcpy(wrong, secret, 44); wrong[42] = 'b';
    assert(quota_portal_secret_matches(secret, secret));
    assert(!quota_portal_secret_matches(secret, wrong));
    assert(!quota_portal_secret_matches(secret, "a"));
    assert(parse("{\"v\":1,\"request_id\":\"abcdef01\",\"op\":\"network_save\",\"ssid\":\"Phone\",\"password\":\"12345678\",\"phone_utc\":1800000000}"));
    assert(command.op == QUOTA_PORTABLE_OP_NETWORK_SAVE && command.network_index == UINT8_MAX);
    assert(!parse("{\"v\":1,\"request_id\":\"abcdef01\",\"op\":\"network_save\",\"ssid\":\"Phone\",\"password\":\"short\"}"));
    assert(!parse("{\"v\":1,\"request_id\":\"abcdef01\",\"op\":\"network_save\",\"ssid\":\"Phone\"}"));
    assert(parse("{\"v\":1,\"request_id\":\"abcdef01\",\"op\":\"network_save\",\"ssid\":\"Cafe\",\"password\":\"\",\"open_network\":true}"));
    assert(command.open_network);
    assert(parse("{\"v\":1,\"request_id\":\"abcdef01\",\"op\":\"network_save\",\"network_index\":2}"));
    assert(command.network_index == 2 && command.ssid[0] == 0);
    assert(!parse("{\"v\":1,\"request_id\":\"abcdef01\",\"op\":\"network_save\",\"network_index\":3}"));
    assert(!parse("{\"v\":1,\"request_id\":\"abcdef01\",\"op\":\"deepseek_save\",\"label\":\"Only label\"}"));
    assert(parse("{\"v\":1,\"request_id\":\"abcdef01\",\"op\":\"deepseek_save\",\"label\":\"Only label\",\"account_id\":\"0123456789abcdef0123456789abcdef\"}"));
    assert(command.api_key[0] == 0);
    assert(parse("{\"v\":1,\"request_id\":\"abcdef01\",\"op\":\"deepseek_save\",\"label\":\"My API\",\"api_key\":\"sk-fake\"}"));
    assert(strcmp(command.api_key, "sk-fake") == 0);
    assert(parse("{\"v\":1,\"request_id\":\"abcdef01\",\"op\":\"codex_queue\",\"label\":\"Work\"}"));
    assert(parse("{\"v\":1,\"request_id\":\"abcdef01\",\"op\":\"codex_queue\",\"phone_utc\":1800000000}"));
    assert(command.phone_utc == 1800000000);
    assert(!parse("{\"v\":1,\"request_id\":\"abcdef01\",\"op\":\"deepseek_save\",\"label\":\"Fake\",\"api_key\":\"sk-fake\",\"phone_utc\":-1}"));
    assert(parse("{\"v\":1,\"request_id\":\"abcdef01\",\"op\":\"settings_save\",\"refresh_seconds\":300,\"auto_refresh\":false,\"screen_timeout_seconds\":0}"));
    assert(!parse("{\"v\":1,\"request_id\":\"abcdef01\",\"op\":\"settings_save\",\"refresh_seconds\":301,\"auto_refresh\":false,\"screen_timeout_seconds\":0}"));
    assert(parse("{\"v\":1,\"request_id\":\"abcdef01\",\"op\":\"refresh\"}"));
    assert(parse("{\"v\":1,\"request_id\":\"abcdef01\",\"op\":\"reconnect\"}"));
    assert(!parse("{\"v\":1,\"v\":1,\"request_id\":\"abcdef01\",\"op\":\"refresh\"}"));
    assert(!parse("{\"v\":1,\"request_id\":\"abcdef01\",\"op\":\"refresh\",\"unknown\":1}"));
    assert(!parse("{\"v\":1,\"request_id\":\"ABCDEF01\",\"op\":\"refresh\"}"));
    assert(!parse("{\"v\":1,\"request_id\":\"abcdef01\",\"op\":\"deepseek_save\",\"label\":\"X\\u0000Y\",\"api_key\":\"sk-fake\"}"));
    assert(!parse("{\"v\":1,\"request_id\":\"abcdef01\",\"op\":\"deepseek_save\",\"label\":\"X\\nY\",\"api_key\":\"sk-fake\"}"));
    assert(!parse("{\"v\":1,\"request_id\":\"abcdef01\",\"op\":\"refresh\"}junk"));
    assert(!parse("{\"v\":1,\"request_id\":\"abcdef01\",\"op\":\"refresh\",\"x\":[[[[1]]]]}"));
    unsigned char zero[sizeof(command)] = {0};
    assert(memcmp(&command, zero, sizeof(command)) == 0); /* Rejected secrets cleared. */
    char huge[QUOTA_PORTABLE_COMMAND_BYTES + 2]; memset(huge, ' ', sizeof(huge));
    assert(!quota_portal_parse_command(huge, sizeof(huge), &command));
    puts("portable command runtime checks passed");
}
'''
        compile_and_run(harness, "quota-portable-command-",
                        ("main/quota_logic.c", "tests/cjson/cJSON.c"))

    def test_ap_socket_session_and_origin_gate(self):
        source = (ROOT / "main/quota_portal.c").read_text()
        public = source[source.index("bool quota_portal_host_is_valid"):
                        source.index("static bool unique_keys")]
        functions = "\n".join(extract_function(source, name, "static bool")
                              for name in ("header", "socket_ipv4_address", "peer_is_ap", "authorize"))
        harness = r'''
#include "quota_portal.h"
#include <assert.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
enum { ESP_OK = 0 };
typedef struct { const char *host, *origin, *secret; } httpd_req_t;
static quota_portal_callbacks_t s_callbacks;
static char s_secret[44];
static bool active;
static uint32_t peer_ip, local_ip;
static unsigned peer_form, local_form;
static bool peer_failure, local_failure;
static const char *get(httpd_req_t *r, const char *name) {
    if (!strcmp(name,"Host")) return r->host;
    if (!strcmp(name,"Origin")) return r->origin;
    if (!strcmp(name,"X-AIQ-Setup")) return r->secret;
    return NULL;
}
static size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *name) {
    const char *value=get(r,name); return value?strlen(value):0;
}
static int httpd_req_get_hdr_value_str(httpd_req_t *r, const char *name, char *out, size_t cap) {
    const char *value=get(r,name); if (!value || strlen(value)>=cap) return 1;
    strcpy(out,value); return ESP_OK;
}
static int httpd_req_to_sockfd(httpd_req_t *r) {(void)r;return 1;}
static int fake_address(struct sockaddr *addr, socklen_t *len, uint32_t ip, unsigned form, bool failure) {
    if (failure) return -1;
    struct sockaddr_storage storage={0}; socklen_t size;
    if (form==0 || form==6) {
        struct sockaddr_in *p=(void *)&storage;
        p->sin_family=AF_INET;p->sin_addr.s_addr=htonl(ip);
        size=sizeof(*p)-(form==6?1:0);
    } else if (form==5) {
        storage.ss_family=AF_UNSPEC;size=sizeof(struct sockaddr);
    } else {
        struct sockaddr_in6 *p=(void *)&storage;p->sin6_family=AF_INET6;
        p->sin6_addr.s6_addr[10]=p->sin6_addr.s6_addr[11]=0xff;
        uint32_t network_ip=htonl(ip);memcpy(p->sin6_addr.s6_addr+12,&network_ip,4);
        if(form==2){p->sin6_addr.s6_addr[0]=0xfe;p->sin6_addr.s6_addr[1]=0x80;}
        if(form==3)p->sin6_addr.s6_addr[10]=p->sin6_addr.s6_addr[11]=0;
        size=sizeof(*p)-(form==4?1:0);
    }
    assert(*len>=size);memcpy(addr,&storage,size);*len=size;return 0;
}
static int fake_peer(int fd, struct sockaddr *addr, socklen_t *len) {
    (void)fd;return fake_address(addr,len,peer_ip,peer_form,peer_failure);
}
static int fake_local(int fd, struct sockaddr *addr, socklen_t *len) {
    (void)fd;return fake_address(addr,len,local_ip,local_form,local_failure);
}
#define getpeername fake_peer
#define getsockname fake_local
'''+public+functions+r'''
static bool session(void *ctx) {(void)ctx;return active;}
static bool allowed(httpd_req_t *r,bool secret,bool mutation) {
    const char *denial=NULL;bool ok=authorize(r,secret,mutation,&denial);
    if(!ok)assert(denial&&strstr(denial,active?"unauthorized":"session_expired"));
    return ok;
}
int main(void) {
    memset(s_secret,'a',43);s_secret[43]=0;s_callbacks.session_active=session;
    active=true;peer_ip=0xc0a80402;local_ip=0xc0a80401;
    httpd_req_t r={"192.168.4.1","http://192.168.4.1",s_secret};
    assert(allowed(&r,true,true));
    r.origin="http://evil.test";assert(!allowed(&r,true,true));
    assert(!allowed(&r,true,false)); /* Reject a supplied foreign GET Origin. */
    r.origin=NULL;assert(allowed(&r,true,false)); /* GET may omit Origin. */
    r.origin="http://192.168.4.1";r.secret="wrong";assert(!allowed(&r,true,false));
    assert(allowed(&r,false,false)); /* Static page contains no secret. */
    r.secret=s_secret;r.host="evil.test";assert(!allowed(&r,false,false));
    r.host="192.168.4.1";local_ip=0xc0a80464;assert(!allowed(&r,true,false)); /* STA socket. */
    local_ip=0xc0a80401;peer_ip=0x0a000001;assert(!allowed(&r,true,false));
    peer_ip=0xc0a80402;
    for(unsigned form=2;form<=6;form++) {
        peer_form=form;assert(!allowed(&r,true,false));
        peer_form=0;local_form=form;assert(!allowed(&r,true,false));local_form=0;
    }
    peer_failure=true;assert(!allowed(&r,true,false));peer_failure=false;
    local_failure=true;assert(!allowed(&r,true,false));local_failure=false;
    peer_form=local_form=1;
#if LWIP_IPV6
    assert(allowed(&r,false,false)); /* Actual IDF dual-stack IPv4 navigation. */
    assert(allowed(&r,true,true)); /* Mapped endpoints preserve secret/Origin gates. */
    local_ip=0xc0a80464;assert(!allowed(&r,true,false));local_ip=0xc0a80401;
    peer_ip=0x0a000001;assert(!allowed(&r,true,false));peer_ip=0xc0a80402;
    peer_ip=0xc0a80401;assert(!allowed(&r,true,false));
    peer_ip=0xc0a804ff;assert(!allowed(&r,true,false));peer_ip=0xc0a80402;
    peer_form=0;assert(allowed(&r,true,false)); /* Mixed IPv4/mapped forms normalize equally. */
    peer_form=1;local_form=0;assert(allowed(&r,true,false));
#else
    assert(!allowed(&r,true,false)); /* IPv6-disabled build keeps IPv4 only. */
#endif
    peer_form=local_form=0;active=false;assert(!allowed(&r,true,false));
    puts("portable AP authorization runtime checks passed");
}
'''
        for ipv6 in (1, 0):
            with self.subTest(ipv6=ipv6):
                compile_and_run(f"#define LWIP_IPV6 {ipv6}\n" + harness,
                                "quota-portable-session-")

    def test_atomic_credential_record_and_cache_identity(self):
        harness = r'''
#include "quota_store.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quota_store.c"
typedef struct {char key[16];size_t length;unsigned char data[16384];} item_t;
static item_t items[10],pending;
static size_t item_count;
static bool fail_commit;
static int commits;
esp_err_t nvs_flash_init_partition(const char *part) {assert(!strcmp(part,"portable"));return ESP_OK;}
esp_err_t nvs_open_from_partition(const char *part,const char *space,int mode,nvs_handle_t *out) {
    assert(!strcmp(part,"portable")&&!strcmp(space,"quota_port"));
    assert(mode==NVS_READONLY||mode==NVS_READWRITE);*out=1;return ESP_OK;
}
void nvs_close(nvs_handle_t h) {assert(h==1);memset(&pending,0,sizeof(pending));}
static item_t *find(const char *key) {
    for (size_t i=0;i<item_count;i++) if (!strcmp(items[i].key,key)) return &items[i];
    return NULL;
}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *bytes) {
    assert(h==1);item_t *item=find(key);if (!item) return 1;
    if (!out) {*bytes=item->length;return ESP_OK;}
    if (*bytes<item->length) return 1;
    memcpy(out,item->data,item->length);*bytes=item->length;return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *value,size_t bytes) {
    assert(h==1&&bytes<=sizeof(pending.data));strcpy(pending.key,key);
    memcpy(pending.data,value,bytes);pending.length=bytes;return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    assert(h==1);commits++;if (fail_commit) return 1;
    item_t *item=find(pending.key);if (!item) {assert(item_count<10);item=&items[item_count++];}
    *item=pending;return ESP_OK;
}
int main(void) {
    assert(quota_store_init());
    quota_portable_config_t config={0},restored_config;
    config.mode=QUOTA_MODE_DIRECT;config.refresh_seconds=300;config.screen_timeout_seconds=120;
    config.network_count=1;strcpy(config.networks[0].ssid,"Fake hotspot");strcpy(config.networks[0].password,"password");
    assert(quota_store_save_config(&config));assert(quota_store_load_config(&restored_config));
    assert(!strcmp(restored_config.networks[0].ssid,"Fake hotspot"));
    quota_portable_credential_t *c=calloc(1,sizeof(*c)),*read=calloc(1,sizeof(*read));assert(c&&read);
    strcpy(c->id,"0123456789abcdef0123456789abcdef");c->generation=1;c->slot=0;
    c->provider=QUOTA_PROVIDER_CODEX;c->auth_state=QUOTA_PORTABLE_AUTH_READY;
    strcpy(c->server_account_id,"fake-workspace");strcpy(c->access_token,"old-access");strcpy(c->refresh_token,"old-refresh");
    assert(quota_store_save_credential(0,c));assert(quota_store_load_credential(0,read));
    assert(!read->refresh_inflight);
    c->refresh_inflight=true;assert(quota_store_save_credential(0,c));
    assert(quota_store_load_credential(0,read)&&read->refresh_inflight);
    strcpy(c->access_token,"new-access");strcpy(c->refresh_token,"new-refresh");c->refresh_inflight=false;
    fail_commit=true;assert(!quota_store_save_credential(0,c));fail_commit=false;
    assert(quota_store_load_credential(0,read)&&read->refresh_inflight);
    assert(!strcmp(read->refresh_token,"old-refresh")); /* Marker survives lost final write. */
    assert(quota_store_save_credential(0,c));assert(quota_store_load_credential(0,read));
    assert(!read->refresh_inflight&&!strcmp(read->refresh_token,"new-refresh"));
    int before=commits;memset(c->access_token,'x',sizeof(c->access_token));
    assert(!quota_store_save_credential(0,c)&&commits==before);
    strcpy(c->access_token,"new-access");
    quota_snapshot_t snapshot={0},restored;
    snapshot.refresh_seconds=300;snapshot.account_count=1;snapshot.revision=7;snapshot.server_time=1800000000;
    strcpy(snapshot.accounts[0].id,c->id);snapshot.accounts[0].provider=QUOTA_PROVIDER_CODEX;
    snapshot.accounts[0].has_observed_at=true;snapshot.accounts[0].observed_at=1800000000;
    snapshot.accounts[0].five_hour.present=true;snapshot.accounts[0].five_hour.remaining_percent=0;
    snapshot.codex_extras[0].has_credits=true;strcpy(snapshot.codex_extras[0].credits_balance,"12.34");
    quota_portable_account_ref_t ref={.generation=1,.provider=QUOTA_PROVIDER_CODEX};strcpy(ref.id,c->id);
    assert(quota_store_save_snapshot(&snapshot,&ref,1,1800000000));
    assert(quota_store_load_snapshot(&ref,1,1800000010,&restored));
    assert(restored.account_count==1&&restored.accounts[0].five_hour.present&&restored.accounts[0].five_hour.remaining_percent==0);
    assert(!restored.codex_extras[0].has_credits); /* RAM-only extras preserved. */
    ref.generation=2;assert(quota_store_load_snapshot(&ref,1,1800000010,&restored));
    assert(restored.account_count==0); /* Replaced credential never inherits old quota. */
    ref.generation=1;assert(!quota_store_load_snapshot(&ref,1,1800000000+31ULL*86400,&restored));
    assert(quota_store_remove_credential(0,c->id,2));assert(quota_store_load_credential(0,read));
    assert(read->tombstone&&read->generation==2&&read->access_token[0]==0&&read->refresh_token[0]==0);
    item_t *record=find("account0");assert(record);record->data[20]^=1;
    assert(!quota_store_load_credential(0,read));assert(read->id[0]==0&&read->refresh_token[0]==0);
    free(c);free(read);puts("portable storage runtime checks passed");
}
'''
        nvs_header = '''#pragma once
#include <stddef.h>
typedef int nvs_handle_t;
typedef int esp_err_t;
enum { ESP_OK=0, NVS_READONLY=1, NVS_READWRITE=2 };
esp_err_t nvs_open_from_partition(const char*,const char*,int,nvs_handle_t*);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);
esp_err_t nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);
esp_err_t nvs_commit(nvs_handle_t);
'''
        with tempfile.TemporaryDirectory(prefix="quota-portable-store-") as directory:
            path = Path(directory)
            (path / "nvs.h").write_text(nvs_header)
            (path / "nvs_flash.h").write_text('#pragma once\n#include "nvs.h"\nesp_err_t nvs_flash_init_partition(const char*);\n')
            (path / "test.c").write_text(harness)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I"+str(path), "-I"+str(ROOT / "main"), "-I"+str(ROOT / "tests/cjson"),
                            str(path / "test.c"), str(ROOT / "main/quota_logic.c"),
                            str(ROOT / "tests/cjson/cJSON.c"), "-lm", "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
