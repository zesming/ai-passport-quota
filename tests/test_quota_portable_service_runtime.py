"""Exercise the controller's real queue and source scheduling functions."""
import re
import unittest

from runtime_helpers import ROOT, compile_and_run, extract_function


class PortableServiceRuntime(unittest.TestCase):
    def test_boot_merges_old_cache_without_hiding_new_accounts(self):
        source = (ROOT / "main/quota_portable_service.c").read_text()
        meta_type = re.search(r"typedef struct \{[^{}]*\} account_meta_t;", source)[0]
        init = extract_function(source, "quota_portable_service_init", "bool")
        harness = r'''
#include "quota_direct.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
typedef struct {
    bool (*wifi_ready)(void);bool (*wifi_stop)(void);void (*notify)(void);void (*wake)(void);
    bool (*display_current)(uint32_t generation);
} quota_portable_service_hooks_t;
'''+meta_type+r'''
static int s_mutex;
static quota_portable_service_hooks_t s_hooks;
static quota_portable_config_t s_config;
static quota_portable_view_t s_view;
static quota_snapshot_t s_snapshot;
static account_meta_t s_accounts[QUOTA_MAX_ACCOUNTS];
static quota_direct_t *s_direct;
static bool s_store_ready,s_open;
static uint64_t s_next_refresh;
static const char *TAG="test";
#define ESP_LOGI(tag,fmt,...) do {(void)(tag);(void)(fmt);}while(0)
void lock(void) {}
void unlock(void) {}
static int xSemaphoreCreateMutex(void) {return 1;}
static uint64_t millis(void) {return 100;}
static void copy(char *out,size_t cap,const char *in) {snprintf(out,cap,"%s",in?in:"");}
static bool quota_store_init(void) {return true;}
static bool quota_store_load_config(quota_portable_config_t *out) {
    memset(out,0,sizeof(*out));out->mode=QUOTA_MODE_DIRECT;out->refresh_seconds=300;return true;
}
static bool quota_store_save_config(const quota_portable_config_t *out) {(void)out;return true;}
static bool quota_store_load_credential(uint8_t slot,quota_direct_credential_t *out) {
    if(slot>1)return false;memset(out,0,sizeof(*out));out->slot=slot;out->generation=1;
    out->provider=slot?QUOTA_PROVIDER_DEEPSEEK:QUOTA_PROVIDER_CODEX;
    snprintf(out->id,sizeof(out->id),"%032x",slot+1);out->auth_state=QUOTA_PORTABLE_AUTH_PENDING;
    out->refresh_inflight=slot==0;return true;
}
static void free_credential(quota_direct_credential_t **out) {
    if(*out){quota_portable_clear_secret(*out,sizeof(**out));free(*out);*out=NULL;}
}
static void publish_credential(const quota_direct_credential_t *value) {
    s_accounts[value->slot].used=true;s_accounts[value->slot].ref.generation=value->generation;
    s_accounts[value->slot].ref.provider=value->provider;strcpy(s_accounts[value->slot].ref.id,value->id);
    s_accounts[value->slot].auth=value->refresh_inflight?QUOTA_PORTABLE_AUTH_REAUTH:value->auth_state;
    unsigned index=s_snapshot.account_count++;strcpy(s_snapshot.accounts[index].id,value->id);
    s_snapshot.accounts[index].provider=value->provider;s_snapshot.accounts[index].status=QUOTA_STATUS_WAITING;
}
static int snapshot_index(const char *id){return quota_find_account_by_id(&s_snapshot,id);}
static int slot_for(const char *id){for(int i=0;i<8;i++)if(s_accounts[i].used&&!strcmp(id,s_accounts[i].ref.id))return i;return -1;}
static bool quota_store_load_snapshot(const quota_portable_account_ref_t *refs,size_t count,uint64_t now,quota_snapshot_t *out) {
    assert(count==2&&refs[0].generation==1);(void)now;memset(out,0,sizeof(*out));
    out->refresh_seconds=300;out->account_count=1;strcpy(out->accounts[0].id,refs[0].id);
    out->accounts[0].provider=QUOTA_PROVIDER_CODEX;out->accounts[0].has_observed_at=true;
    out->accounts[0].observed_at=1800000000;out->accounts[0].five_hour.present=true;return true;
}
static bool admit(void *ctx,const char *id,uint32_t generation){(void)ctx;(void)id;(void)generation;return true;}
static bool account_current(void *ctx,const char *id,uint32_t generation){(void)ctx;(void)id;(void)generation;return true;}
static bool persist(void *ctx,const quota_direct_credential_t *value){(void)ctx;(void)value;return true;}
quota_direct_t *quota_direct_create(const quota_direct_hooks_t *hooks,quota_direct_transport_t transport,void *ctx) {
    assert(hooks&&hooks->persist);(void)transport;(void)ctx;return (quota_direct_t *)1;
}
'''+init+r'''
int main(void) {
    quota_portable_service_hooks_t hooks={0};assert(quota_portable_service_init(NULL,&hooks));
    assert(s_snapshot.account_count==2);
    int old=snapshot_index("00000000000000000000000000000001");
    int added=snapshot_index("00000000000000000000000000000002");
    assert(old>=0&&added>=0&&s_snapshot.accounts[old].has_observed_at);
    assert(s_snapshot.accounts[old].status==QUOTA_STATUS_EXPIRED);
    assert(!s_snapshot.accounts[added].has_observed_at&&s_snapshot.accounts[added].status==QUOTA_STATUS_WAITING);
    puts("portable controller cold boot merge runtime checks passed");
}
'''
        compile_and_run(harness, "quota-portable-controller-boot-",
                        ("main/quota_logic.c", "tests/cjson/cJSON.c"))

    def test_pending_jobs_do_not_block_close_and_duplicate_is_coalesced(self):
        source = (ROOT / "main/quota_portable_service.c").read_text()
        job_type = re.search(r"typedef struct \{[^{}]*\} job_t;", source)[0]
        functions = "\n".join((extract_function(source, "command_hash", "static uint32_t"),
                                 extract_function(source, "submit", "static quota_portable_submit_result_t")))
        harness = r'''
#include "quota_portable.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define QUEUE_DEPTH 4
#define portMAX_DELAY 0xffffffffU
#define pdTRUE 1
typedef int SemaphoreHandle_t;
static SemaphoreHandle_t s_mutex=1;
static bool unavailable;
static int xSemaphoreTake(int mutex,unsigned wait) {
    assert(mutex==1); assert(wait==0); return unavailable?0:pdTRUE;
}
static void unlock(void) {}
static void wake(void) {}
static void copy(char *out,size_t cap,const char *in) {snprintf(out,cap,"%s",in?in:"");}
static uint64_t now;
static uint64_t millis(void) {return now;}
static quota_portable_view_t s_view;
static bool s_sleeping;
static uint64_t s_setup_deadline=10000;
static quota_portable_command_t s_queue[QUEUE_DEPTH];
static unsigned s_head,s_count,s_job_count;
'''+job_type+r'''
static job_t s_jobs[QUEUE_DEPTH];
'''+functions+r'''
int main(void) {
    s_view.setup_ready=true;
    s_job_count=4;
    for (unsigned i=0;i<4;i++) {snprintf(s_jobs[i].id,9,"%08x",i);s_jobs[i].state=i?2:1;}
    quota_portable_command_t command={.op=QUOTA_PORTABLE_OP_SETUP_CLOSE};strcpy(command.request_id,"abcdef01");
    assert(submit(&command,NULL)==QUOTA_PORTABLE_SUBMIT_ACCEPTED);
    assert(s_count==1&&s_job_count==4);
    bool retained=false;for (unsigned i=0;i<4;i++) if (!strcmp(s_jobs[i].id,"00000000")) retained=true;
    assert(retained); /* A pending network job survives, a completed job yields its slot. */
    assert(submit(&command,NULL)==QUOTA_PORTABLE_SUBMIT_ACCEPTED&&s_count==1);
    strcpy(command.label,"different");assert(submit(&command,NULL)==QUOTA_PORTABLE_SUBMIT_CONFLICT&&s_count==1);
    strcpy(command.request_id,"abcdef02");s_count=4;
    assert(submit(&command,NULL)==QUOTA_PORTABLE_SUBMIT_BUSY&&s_count==4);
    s_count=0;for (unsigned i=0;i<4;i++) s_jobs[i].state=1;
    assert(submit(&command,NULL)==QUOTA_PORTABLE_SUBMIT_BUSY);
    unavailable=true;assert(submit(&command,NULL)==QUOTA_PORTABLE_SUBMIT_BUSY);unavailable=false;
    now=10000;assert(submit(&command,NULL)==QUOTA_PORTABLE_SUBMIT_CLOSED);
    puts("portable controller queue runtime checks passed");
}
'''
        compile_and_run(harness, "quota-portable-controller-queue-")

    def test_deferred_work_and_optional_details_preserve_source_contract(self):
        source = (ROOT / "main/quota_portable_service.c").read_text()
        meta_type = re.search(r"typedef struct \{[^{}]*\} account_meta_t;", source)[0]
        functions = "\n".join(extract_function(source, name, "static void")
                              for name in ("apply_result", "source_tick"))
        harness = r'''
#include "quota_direct.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#define ESP_LOGI(...) ((void)0)
'''+meta_type+r'''
static account_meta_t s_accounts[QUOTA_MAX_ACCOUNTS];
static quota_snapshot_t s_snapshot;
static quota_portable_view_t s_view;
static quota_portable_config_t s_config;
static bool s_cycle,s_refresh,s_failed,s_refreshing,s_cache_dirty,s_key_new;
static uint8_t s_refresh_slot;
static uint64_t s_next_refresh;
static quota_direct_t *s_direct=(quota_direct_t *)1;
static quota_direct_credential_t *s_key;
static char s_key_job[9];
static uint64_t now;
static unsigned queries,stores;
static uint32_t stored_generation;
static quota_direct_result_code_t next_code;
static void lock(void) {}
static void unlock(void) {}
static void changed(void) {}
static uint64_t millis(void) {return now;}
static uint64_t epoch(void) {return 1800000000;}
static int fake_settimeofday(const struct timeval *value,void *zone) {(void)zone;assert(value->tv_sec==1800000000);return 0;}
#define settimeofday fake_settimeofday
static void copy(char *out,size_t cap,const char *in) {snprintf(out,cap,"%s",in?in:"");}
static int snapshot_index(const char *id) {return quota_find_account_by_id(&s_snapshot,id);}
static const char *result_error(quota_direct_result_code_t code) {(void)code;return "fake_error";}
static void free_credential(quota_direct_credential_t **value) {
    if (*value) {quota_portable_clear_secret(*value,sizeof(**value));free(*value);*value=NULL;}
}
static void cache_tick(void) {}
static void finish_job(const char *id,const char *error) {(void)id;(void)error;}
static bool quota_store_load_credential(uint8_t slot,quota_direct_credential_t *out) {
    memset(out,0,sizeof(*out));out->slot=slot;out->provider=QUOTA_PROVIDER_DEEPSEEK;
    out->generation=s_accounts[slot].ref.generation;strcpy(out->id,s_accounts[slot].ref.id);
    strcpy(out->api_key,"old-key");return true;
}
static bool quota_store_save_credential(uint8_t slot,const quota_direct_credential_t *value) {
    assert(slot==value->slot);stores++;stored_generation=value->generation;return true;
}
static void publish_credential(const quota_direct_credential_t *value) {
    s_accounts[value->slot].ref.generation=value->generation;
}
bool persist(void *ctx,const quota_direct_credential_t *value) {
    (void)ctx;bool ok=quota_store_save_credential(value->slot,value);if(ok)publish_credential(value);return ok;
}
quota_direct_result_t quota_direct_query(quota_direct_t *direct,quota_direct_credential_t *value,uint64_t utc) {
    assert(direct==s_direct&&utc==epoch());queries++;
    quota_direct_result_t result={.code=next_code,.source_epoch=epoch(),.source_valid=next_code==QUOTA_DIRECT_OK};
    strcpy(result.account.id,value->id);result.account.provider=value->provider;result.account.status=QUOTA_STATUS_OK;
    result.balance.present=true;result.balance.currency_count=1;
    strcpy(result.balance.balance_infos[0].currency,"CNY");strcpy(result.balance.balance_infos[0].total_balance,"1.2345");
    return result;
}
quota_direct_result_t quota_direct_refresh(quota_direct_t *direct,quota_direct_credential_t *value,uint64_t utc) {
    (void)direct;(void)value;(void)utc;assert(0);quota_direct_result_t result={0};return result;
}
'''+functions+r'''
int main(void) {
    s_view.network_state=QUOTA_PORTABLE_NETWORK_READY;s_config.refresh_seconds=300;
    s_accounts[0].used=true;s_accounts[0].ref.generation=1;s_accounts[0].auth=QUOTA_PORTABLE_AUTH_READY;
    strcpy(s_accounts[0].ref.id,"0123456789abcdef0123456789abcdef");s_accounts[0].ref.provider=QUOTA_PROVIDER_DEEPSEEK;
    s_snapshot.account_count=1;strcpy(s_snapshot.accounts[0].id,s_accounts[0].ref.id);
    s_snapshot.accounts[0].provider=QUOTA_PROVIDER_DEEPSEEK;
    s_refresh=true;next_code=QUOTA_DIRECT_DEFERRED;source_tick();
    assert(queries==1&&s_cycle&&s_refresh_slot==0); /* Sleep before admission cannot consume manual work. */
    now=5000;next_code=QUOTA_DIRECT_OK;source_tick();
    assert(queries==2&&s_refresh_slot==1);
    now=7000;source_tick();assert(!s_cycle&&s_next_refresh==307000); /* Completion starts the interval. */
    s_snapshot.codex_extras[0].has_credits=true;strcpy(s_snapshot.codex_extras[0].credits_balance,"old");
    s_snapshot.codex_extras[0].has_next_reset_expiry=true;s_snapshot.codex_extras[0].next_reset_expires_at=epoch()+100;
    quota_direct_result_t result={.code=QUOTA_DIRECT_OK,.source_valid=true,.source_epoch=epoch()};
    strcpy(result.account.id,s_accounts[0].ref.id);result.extras.has_banked_reset=true;result.extras.available_resets=2;
    apply_result(0,&result);
    assert(!s_snapshot.codex_extras[0].has_credits&&!s_snapshot.codex_extras[0].has_next_reset_expiry);
    assert(s_snapshot.codex_extras[0].available_resets==2); /* Latest usage survives failed optional details. */
    s_key=calloc(1,sizeof(*s_key));assert(s_key);quota_store_load_credential(0,s_key);
    strcpy(s_key->api_key,"candidate-key");s_key_new=false;next_code=QUOTA_DIRECT_AUTH_REQUIRED;
    unsigned old_stores=stores;source_tick();assert(s_key==NULL&&stores==old_stores);
    assert(s_accounts[0].ref.generation==1); /* Failed candidate preserves the old credential. */
    s_key=calloc(1,sizeof(*s_key));assert(s_key);quota_store_load_credential(0,s_key);
    strcpy(s_key->api_key,"candidate-key");next_code=QUOTA_DIRECT_OK;source_tick();
    assert(s_key==NULL&&stored_generation==2&&s_accounts[0].ref.generation==2);
    puts("portable controller source runtime checks passed");
}
'''
        compile_and_run(harness, "quota-portable-controller-source-",
                        ("main/quota_logic.c", "tests/cjson/cJSON.c"))


if __name__ == "__main__":
    unittest.main()
