"""Execute historical loaders: v1 records are migration input, never a second authority."""
import re
import unittest
from runtime_helpers import ROOT, extract_function, compile_and_run


class StorageRuntime(unittest.TestCase):
    def test_optional_sidecar_typed_config_and_aged_inventory(self):
        source = (ROOT / "main/quota_service.c").read_text()
        definitions = "\n".join(re.findall(
            r"^#define (?:NVS_NAMESPACE|NVS_CONFIG_KEY|NVS_SNAPSHOT_KEY|NVS_SCREEN_TIMEOUT_KEY|NVS_BALANCE_KEY|"
            r"STORED_CONFIG_MAGIC|STORED_CONFIG_VERSION|STORED_SNAPSHOT_MAGIC|STORED_SNAPSHOT_VERSION|STORED_BALANCE_MAGIC) .*", source, re.M))
        declarations = "\n".join(re.search(r"typedef struct \{[^{}]*\} " + name + ";", source)[0]
                                  for name in ("stored_config_t", "cached_snapshot_t", "stored_snapshot_t", "stored_balance_t"))
        functions = "\n".join(extract_function(source, name, declaration) for name, declaration in (
            ("crc32_update", "static uint32_t"), ("crc32_bytes", "static uint32_t"),
            ("nvs_read_config_result", "static quota_store_read_result_t"),
            ("nvs_load_screen_timeout", "static uint16_t"),
            ("nvs_load_snapshot_inventory_result", "static quota_store_read_result_t"),
            ("nvs_load_balance_snapshot", "static void")))
        harness = r'''
#include "quota_store.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int nvs_handle_t;
typedef int esp_err_t;
enum {ESP_OK,ESP_FAIL,ESP_ERR_NVS_NOT_FOUND,ESP_ERR_NVS_INVALID_LENGTH,NVS_READONLY};
''' + definitions + "\n" + declarations + r'''
static bool s_nvs_ready=true,s_balance_cache_present,timeout_present;
static uint16_t s_saved_screen_timeout_seconds,fake_timeout;
static uint64_t s_snapshot_cache_saved_at;
static stored_snapshot_t s_stored_snapshot,fake_snapshot;
static stored_balance_t s_stored_balances,fake_balance;
static stored_config_t fake_config;
static esp_err_t open_error=ESP_OK,config_error=ESP_ERR_NVS_NOT_FOUND,snapshot_error=ESP_ERR_NVS_NOT_FOUND,balance_error=ESP_ERR_NVS_NOT_FOUND;
static bool config_valid=true,snapshot_valid=true,config_oversized,snapshot_oversized;
static uint32_t config_cache_identity(const quota_device_config_t *config){(void)config;return 123;}
static bool config_is_well_formed(const quota_device_config_t *config){return config&&config_valid;}
static bool cached_snapshot_is_well_formed(const cached_snapshot_t *snapshot){return snapshot&&snapshot_valid;}
static esp_err_t nvs_open(const char *space,int mode,nvs_handle_t *handle){assert(!strcmp(space,NVS_NAMESPACE)&&mode==NVS_READONLY);*handle=1;return open_error;}
static void nvs_close(nvs_handle_t handle){assert(handle==1);}
static esp_err_t nvs_get_u16(nvs_handle_t handle,const char *key,uint16_t *value){assert(handle==1&&!strcmp(key,NVS_SCREEN_TIMEOUT_KEY));if(!timeout_present)return ESP_ERR_NVS_NOT_FOUND;*value=fake_timeout;return ESP_OK;}
static esp_err_t nvs_get_blob(nvs_handle_t handle,const char *key,void *value,size_t *length){assert(handle==1);const void *record;size_t bytes;esp_err_t result;if(!strcmp(key,NVS_CONFIG_KEY)){record=&fake_config;bytes=sizeof(fake_config);result=config_error;if(config_oversized){*length=bytes+1;return ESP_ERR_NVS_INVALID_LENGTH;}}else if(!strcmp(key,NVS_SNAPSHOT_KEY)){record=&fake_snapshot;bytes=sizeof(fake_snapshot);result=snapshot_error;if(snapshot_oversized){*length=bytes+1;return ESP_ERR_NVS_INVALID_LENGTH;}}else{assert(!strcmp(key,NVS_BALANCE_KEY));record=&fake_balance;bytes=sizeof(fake_balance);result=balance_error;}if(result!=ESP_OK)return result;assert(*length>=bytes);memcpy(value,record,bytes);*length=bytes;return ESP_OK;}
''' + functions + r'''
int main(void){
    quota_device_config_t config={0};
    assert(nvs_load_screen_timeout()==120);timeout_present=true;fake_timeout=0;assert(nvs_load_screen_timeout()==0);fake_timeout=31;assert(nvs_load_screen_timeout()==120);
    assert(nvs_read_config_result(&config)==QUOTA_STORE_READ_MISSING);open_error=ESP_FAIL;assert(nvs_read_config_result(&config)==QUOTA_STORE_READ_IO_ERROR);open_error=ESP_OK;config_error=ESP_OK;assert(nvs_read_config_result(&config)==QUOTA_STORE_READ_INVALID);
    fake_config.magic=STORED_CONFIG_MAGIC;fake_config.version=STORED_CONFIG_VERSION;fake_config.config_size=sizeof(config);fake_config.config.refresh_seconds=300;fake_config.crc32=crc32_bytes(&fake_config.config,sizeof(config));assert(nvs_read_config_result(&config)==QUOTA_STORE_READ_OK&&config.refresh_seconds==300);
    config_oversized=true;assert(nvs_read_config_result(&config)==QUOTA_STORE_READ_INVALID);config_oversized=false;fake_config.crc32^=1;assert(nvs_read_config_result(&config)==QUOTA_STORE_READ_INVALID);
    assert(nvs_load_snapshot_inventory_result(&config)==QUOTA_STORE_READ_MISSING);snapshot_error=ESP_OK;fake_snapshot.magic=STORED_SNAPSHOT_MAGIC;fake_snapshot.version=STORED_SNAPSHOT_VERSION;fake_snapshot.snapshot_size=sizeof(fake_snapshot.snapshot);fake_snapshot.config_identity=123;fake_snapshot.stored_at=1600000000;fake_snapshot.snapshot.account_count=1;fake_snapshot.snapshot.revision=7;fake_snapshot.crc32=crc32_bytes(&fake_snapshot,offsetof(stored_snapshot_t,crc32));
    snapshot_oversized=true;assert(nvs_load_snapshot_inventory_result(&config)==QUOTA_STORE_READ_INVALID);snapshot_oversized=false;assert(nvs_load_snapshot_inventory_result(&config)==QUOTA_STORE_READ_OK);assert(s_stored_snapshot.snapshot.account_count==1&&s_snapshot_cache_saved_at==1600000000); /* No age limit erases identity inventory. */
    nvs_load_balance_snapshot(&config);assert(!s_balance_cache_present);
    balance_error=ESP_OK;fake_balance.magic=STORED_BALANCE_MAGIC;fake_balance.config_identity=123;fake_balance.stored_at=1600000000;fake_balance.revision=7;
    quota_balance_t *balance=&fake_balance.balances[0];balance->present=true;balance->is_available=true;balance->currency_count=1;strcpy(balance->balance_infos[0].currency,"CNY");strcpy(balance->balance_infos[0].total_balance,"12.3456789");strcpy(balance->balance_infos[0].granted_balance,"2.3456789");strcpy(balance->balance_infos[0].topped_up_balance,"10.00");fake_balance.crc32=crc32_bytes(&fake_balance,offsetof(stored_balance_t,crc32));
    nvs_load_balance_snapshot(&config);assert(s_balance_cache_present&&!strcmp(s_stored_balances.balances[0].balance_infos[0].total_balance,"12.3456789"));
    fake_balance.revision++;fake_balance.crc32=crc32_bytes(&fake_balance,offsetof(stored_balance_t,crc32));nvs_load_balance_snapshot(&config);assert(!s_balance_cache_present);
    fake_snapshot.crc32^=1;assert(nvs_load_snapshot_inventory_result(&config)==QUOTA_STORE_READ_INVALID);puts("historical typed storage and inventory passed");
}
'''
        compile_and_run(harness, "ai-quota-legacy-storage-", ("main/quota_logic.c", "tests/cjson/cJSON.c"))


if __name__ == "__main__":
    unittest.main()
