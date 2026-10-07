"""Exercise unified durability and observation fencing with real catalog/store C."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from runtime_helpers import ROOT


class CatalogRuntime(unittest.TestCase):
    def test_model_write_and_cache_fencing(self):
        harness = r'''
#include "quota_store.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bool reject_alloc;
static void *test_calloc(size_t count, size_t bytes) { return reject_alloc ? NULL : calloc(count,bytes); }
#define calloc test_calloc
#include "quota_store.c"
#undef calloc
typedef struct {char key[16]; size_t length; unsigned char bytes[8192];} item_t;
static item_t items[3]; static unsigned count, writes;
static bool write_fails, commit_fails, error_after_write;
static esp_err_t read_error;
static item_t *find(const char *key) {
    for(unsigned i=0;i<count;i++)if(!strcmp(key,items[i].key))return &items[i];
    return NULL;
}
esp_err_t nvs_flash_init_partition(const char *p) {(void)p;return ESP_OK;}
esp_err_t nvs_open_from_partition(const char *p,const char *s,int mode,nvs_handle_t *h) {
    (void)p;(void)s;(void)mode;*h=1;return ESP_OK;
}
void nvs_close(nvs_handle_t h) {(void)h;}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *bytes) {
    (void)h;if(read_error)return read_error;
    item_t *item=find(key);if(!item)return ESP_ERR_NVS_NOT_FOUND;
    if(!out){*bytes=item->length;return ESP_OK;}
    assert(*bytes>=item->length);memcpy(out,item->bytes,item->length);*bytes=item->length;return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *data,size_t bytes) {
    (void)h;writes++;if(write_fails)return ESP_FAIL;
    item_t *item=find(key);if(!item){assert(count<3);item=&items[count++];strcpy(item->key,key);}
    assert(bytes<=sizeof(item->bytes));item->length=bytes;memcpy(item->bytes,data,bytes);
    if(error_after_write)read_error=ESP_FAIL;
    return ESP_OK; /* SDK persists set_blob before commit. */
}
esp_err_t nvs_commit(nvs_handle_t h) {(void)h;return commit_fails?ESP_FAIL:ESP_OK;}
esp_err_t nvs_open(const char *s,int mode,nvs_handle_t *h) {(void)s;(void)mode;(void)h;return ESP_ERR_NVS_NOT_FOUND;}
esp_err_t nvs_erase_all(nvs_handle_t h) {(void)h;return ESP_OK;}
esp_err_t nvs_erase_key(nvs_handle_t h,const char *key) {(void)h;(void)key;return ESP_ERR_NVS_NOT_FOUND;}
static quota_model_t model, candidate, restored;
static quota_snapshot_t snapshot;
static void add_native(unsigned index) {
    quota_catalog_entry_t *e=&model.entries[index];
    snprintf(e->logical_id,sizeof(e->logical_id),"%032u",index+1);
    strcpy(e->binding.native.credential_id,e->logical_id);
    e->provider=QUOTA_PROVIDER_CODEX;e->source=QUOTA_ACCOUNT_DEVICE;
    e->activity=QUOTA_ACCOUNT_ACTIVE;e->row_generation=1;
    e->binding.native.slot=index;e->binding.native.credential_generation=1;
    strcpy(e->label,"Fresh alias");model.entry_count=index+1;
}
int main(void) {
    assert(quota_store_init());model.refresh_seconds=300;model.screen_timeout_seconds=120;
    add_native(0);assert(quota_catalog_valid(&model));uint64_t seq=9;
    {   /* Only Codex and DeepSeek device rows exist; retired values and bytes are refused. */
        quota_catalog_entry_t saved=model.entries[0];
        model.entries[0].provider=(quota_provider_t)1;assert(!quota_catalog_valid(&model));model.entries[0]=saved;
        model.entries[0].source=(quota_account_source_t)1;assert(!quota_catalog_valid(&model));model.entries[0]=saved;
        model.entries[0].activity=(quota_account_activity_t)1;assert(!quota_catalog_valid(&model));model.entries[0]=saved;
        model.reserved_pending_network[3]=1;assert(!quota_catalog_valid(&model));model.reserved_pending_network[3]=0;
        model.reserved_legacy_endpoint[5]=1;assert(!quota_catalog_valid(&model));model.reserved_legacy_endpoint[5]=0;
        assert(quota_catalog_valid(&model));
    }
    assert(quota_store_load_model_result(&restored,&seq)==QUOTA_STORE_READ_MISSING&&seq==0);
    write_fails=true;
    assert(quota_store_save_model_verified(0,&model,1)==QUOTA_MODEL_NOT_APPLIED);
    write_fails=false;commit_fails=true;
    assert(quota_store_save_model_verified(0,&model,1)==QUOTA_MODEL_APPLIED);
    assert(quota_store_load_model_result(&restored,&seq)==QUOTA_STORE_READ_OK&&seq==1);
    candidate=model;candidate.refresh_seconds=900;commit_fails=false;write_fails=true;
    assert(quota_store_save_model_verified(1,&candidate,2)==QUOTA_MODEL_NOT_APPLIED);
    write_fails=false;error_after_write=true;
    assert(quota_store_save_model_verified(1,&candidate,2)==QUOTA_MODEL_WRITE_UNKNOWN);
    unsigned before=writes;
    assert(quota_store_save_model_verified(1,&candidate,2)==QUOTA_MODEL_WRITE_UNKNOWN&&writes==before);
    read_error=ESP_OK;error_after_write=false;reject_alloc=true;
    assert(quota_store_save_model_verified(1,&candidate,2)==QUOTA_MODEL_WRITE_UNKNOWN&&writes==before);
    reject_alloc=false;
    assert(quota_store_save_model_verified(1,&candidate,2)==QUOTA_MODEL_APPLIED&&writes==before);
    assert(quota_store_save_model_verified(1,&model,2)==QUOTA_MODEL_WRITE_UNKNOWN&&writes==before);
    assert(quota_store_load_model_result(&restored,&seq)==QUOTA_STORE_READ_OK&&seq==2);
    item_t *item=find("model_v2");item->bytes[offsetof(model_record_t,crc)]^=1;
    assert(quota_store_load_model_result(&restored,&seq)==QUOTA_STORE_READ_INVALID);
    assert(quota_store_save_model_verified(2,&candidate,3)==QUOTA_MODEL_WRITE_UNKNOWN&&writes==before);
    item->bytes[offsetof(model_record_t,crc)]^=1;
    snapshot.account_count=1;strcpy(snapshot.accounts[0].id,model.entries[0].logical_id);
    snapshot.accounts[0].provider=QUOTA_PROVIDER_CODEX;
    strcpy(snapshot.accounts[0].email,"old@example.test");strcpy(snapshot.accounts[0].plan,"Old");
    snapshot.accounts[0].status=QUOTA_STATUS_OK;snapshot.accounts[0].has_observed_at=true;
    snapshot.accounts[0].observed_at=1800000000;snapshot.accounts[0].seven_day.present=true;
    snapshot.accounts[0].seven_day.remaining_percent=37;
    snapshot.codex_extras[0].has_credits=true;strcpy(snapshot.codex_extras[0].credits_balance,"100");
    assert(quota_store_save_observations(&model,&snapshot,1800000000));
    strcpy(snapshot.accounts[0].email,"fresh@example.test");strcpy(snapshot.accounts[0].plan,"Pro");
    strcpy(snapshot.balances[0].label,"Fresh alias");snapshot.accounts[0].seven_day.remaining_percent=0;
    assert(quota_store_load_observations(&model,1800000001,&snapshot)==QUOTA_STORE_READ_OK);
    assert(snapshot.accounts[0].seven_day.remaining_percent==37);
    assert(!strcmp(snapshot.accounts[0].email,"fresh@example.test"));
    assert(!strcmp(snapshot.accounts[0].plan,"Pro")&&!strcmp(snapshot.balances[0].label,"Fresh alias"));
    assert(!strcmp(snapshot.codex_extras[0].credits_balance,"100"));
    model.entries[0].row_generation++;snapshot.accounts[0].seven_day.remaining_percent=99;
    assert(quota_store_load_observations(&model,1800000001,&snapshot)==QUOTA_STORE_READ_OK);
    assert(snapshot.accounts[0].seven_day.remaining_percent==99);
    model.entries[0].row_generation--;model.entries[0].binding.native.credential_generation++;
    assert(quota_store_load_observations(&model,1800000001,&snapshot)==QUOTA_STORE_READ_OK);
    assert(snapshot.accounts[0].seven_day.remaining_percent==99);
    model.entries[0].binding.native.credential_generation--;
    assert(quota_store_load_observations(&model,1800000000+31ULL*86400,&snapshot)==QUOTA_STORE_READ_OK);
    assert(snapshot.accounts[0].seven_day.remaining_percent==99);
    quota_model_intent_t *intent=&model.intent;
    intent->kind=QUOTA_INTENT_UPSERT_NATIVE;intent->provider=QUOTA_PROVIDER_CODEX;intent->slot=1;
    strcpy(intent->logical_id,"00000000000000000000000000000099");
    strcpy(intent->target_credential_id,intent->logical_id);
    intent->target_credential_generation=1;intent->previous_missing=true;intent->new_row=true;
    assert(quota_catalog_valid(&model));
    intent->slot=0;assert(!quota_catalog_valid(&model));intent->slot=1;
    memset(intent,0,sizeof(*intent));
    intent->kind=QUOTA_INTENT_UPSERT_NATIVE;intent->provider=QUOTA_PROVIDER_CODEX;intent->slot=0;
    strcpy(intent->logical_id,model.entries[0].logical_id);intent->expected_row_generation=1;
    strcpy(intent->previous_credential_id,model.entries[0].binding.native.credential_id);
    intent->previous_credential_generation=1;strcpy(intent->target_credential_id,intent->previous_credential_id);
    intent->target_credential_generation=2;assert(quota_catalog_valid(&model));
    intent->previous_missing=true;assert(!quota_catalog_valid(&model));intent->previous_missing=false;
    intent->previous_credential_generation=UINT32_MAX;assert(!quota_catalog_valid(&model));
    memset(intent,0,sizeof(*intent));
    for(unsigned i=1;i<8;i++)add_native(i);
    assert(quota_catalog_valid(&model)&&model.entry_count==8);
    intent->kind=QUOTA_INTENT_UPSERT_NATIVE;intent->provider=QUOTA_PROVIDER_CODEX;intent->slot=0;
    strcpy(intent->logical_id,"00000000000000000000000000000099");
    strcpy(intent->target_credential_id,intent->logical_id);intent->target_credential_generation=1;
    intent->new_row=true;intent->previous_missing=true;
    assert(!quota_catalog_valid(&model));
    memset(intent,0,sizeof(*intent));
    /* A full catalog refuses a new row, but an existing row can still be upgraded. */
    intent->kind=QUOTA_INTENT_UPSERT_NATIVE;intent->provider=QUOTA_PROVIDER_CODEX;intent->slot=0;
    strcpy(intent->logical_id,"00000000000000000000000000000099");
    strcpy(intent->target_credential_id,intent->logical_id);intent->target_credential_generation=1;
    intent->new_row=true;intent->previous_missing=true;
    assert(!quota_catalog_valid(&model));
    strcpy(intent->logical_id,model.entries[0].logical_id);intent->new_row=false;
    intent->previous_missing=false;strcpy(intent->previous_credential_id,model.entries[0].binding.native.credential_id);
    strcpy(intent->target_credential_id,intent->previous_credential_id);
    intent->previous_credential_generation=1;intent->target_credential_generation=2;
    intent->expected_row_generation=1;assert(quota_catalog_valid(&model));
    model.entries[0].row_generation=UINT32_MAX;intent->expected_row_generation=UINT32_MAX;
    assert(!quota_catalog_valid(&model));model.entries[0].row_generation=1;
    memset(intent,0,sizeof(*intent));
    model.entry_count=0;intent->kind=QUOTA_INTENT_DELETE_NATIVE;intent->provider=QUOTA_PROVIDER_DEEPSEEK;
    strcpy(intent->logical_id,"00000000000000000000000000000001");
    strcpy(intent->previous_credential_id,intent->logical_id);strcpy(intent->target_credential_id,intent->logical_id);
    intent->previous_credential_generation=1;intent->target_credential_generation=2;
    assert(quota_catalog_valid(&model));intent->previous_tombstone=true;assert(!quota_catalog_valid(&model));
    memset(intent,0,sizeof(*intent));model.entry_count=8;
    model.entries[8]=model.entries[7];model.entry_count=9;
    assert(!quota_catalog_valid(&model)); /* Cannot silently exceed capacity or duplicate slots. */
    puts("catalog durability/cache checks passed");
}
'''
        nvs = '''#pragma once
#include <stddef.h>
typedef int nvs_handle_t; typedef int esp_err_t;
enum {ESP_OK=0, ESP_FAIL=1, ESP_ERR_NVS_NOT_FOUND=2, ESP_ERR_NO_MEM=3,
ESP_ERR_NVS_TYPE_MISMATCH=4, ESP_ERR_NVS_PART_NOT_FOUND=5, NVS_READONLY=0, NVS_READWRITE=1};
esp_err_t nvs_open_from_partition(const char*,const char*,int,nvs_handle_t*);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);
esp_err_t nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);
esp_err_t nvs_commit(nvs_handle_t);
esp_err_t nvs_open(const char*,int,nvs_handle_t*);
esp_err_t nvs_erase_all(nvs_handle_t);
esp_err_t nvs_erase_key(nvs_handle_t,const char*);
'''
        with tempfile.TemporaryDirectory(prefix="quota-catalog-") as directory:
            path = Path(directory)
            (path / "nvs.h").write_text(nvs)
            (path / "nvs_flash.h").write_text('#pragma once\n#include "nvs.h"\nesp_err_t nvs_flash_init_partition(const char*);\n')
            (path / "test.c").write_text(harness)
            executable=path / "test"
            command=[os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                     "-Wno-deprecated-declarations", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                     "-I"+str(path), "-I"+str(ROOT/"main"), "-I"+str(ROOT/"tests/cjson"),
                     str(path/"test.c"),str(ROOT/"main/quota_catalog.c"),str(ROOT/"main/quota_logic.c"),
                     str(ROOT/"tests/cjson/cJSON.c"),"-lm","-o",str(executable)]
            subprocess.run(command,check=True)
            subprocess.run([str(executable)],check=True)


if __name__ == "__main__":
    unittest.main()
