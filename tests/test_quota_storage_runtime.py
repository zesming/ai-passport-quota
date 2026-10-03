"""Run actual NVS loaders against fake public records and missing legacy keys."""
import re
import unittest
from runtime_helpers import ROOT, extract_function, compile_and_run


class StorageRuntime(unittest.TestCase):
    def test_optional_keys_and_balance_cache_binding(self):
        source = (ROOT / "main/quota_service.c").read_text()
        declarations = re.search(r"typedef struct \{[^{}]*\} stored_balance_t;", source)[0]
        functions = []
        for kind, name in (("uint32_t", "crc32_update"), ("uint32_t", "crc32_bytes"),
                           ("uint16_t", "nvs_load_screen_timeout"),
                           ("void", "nvs_load_balance_snapshot")):
            functions.append(extract_function(source, name, "static " + kind))
        definitions = "\n".join(re.findall(r"^#define (?:NVS_NAMESPACE|NVS_SCREEN_TIMEOUT_KEY|NVS_BALANCE_KEY|STORED_BALANCE_MAGIC) .*", source, re.M))
        harness = r'''
#include "quota_logic.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef int nvs_handle_t;
typedef int esp_err_t;
enum { ESP_OK = 0, NVS_READONLY = 1, ESP_ERR_NVS_NOT_FOUND = 2 };
'''
        harness += definitions + "\n" + declarations
        harness += r'''
static bool s_nvs_ready = true, s_balance_cache_present, blob_present, timeout_present;
static uint16_t s_saved_screen_timeout_seconds, fake_timeout;
static stored_balance_t s_stored_balances, fake_blob;
static size_t fake_length;
static struct { uint64_t stored_at; struct { uint64_t revision; } snapshot; } s_stored_snapshot;
static struct { quota_snapshot_t snapshot; } s_view;
static uint32_t config_cache_identity(const quota_device_config_t *config) {
    (void)config; return 123;
}
static esp_err_t nvs_open(const char *space, int mode, nvs_handle_t *handle) {
    assert(strcmp(space, NVS_NAMESPACE) == 0); assert(mode == NVS_READONLY);
    *handle = 1; return ESP_OK;
}
static void nvs_close(nvs_handle_t handle) { assert(handle == 1); }
static esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *value) {
    assert(handle == 1 && strcmp(key, NVS_SCREEN_TIMEOUT_KEY) == 0);
    if (!timeout_present) return ESP_ERR_NVS_NOT_FOUND;
    *value = fake_timeout; return ESP_OK;
}
static esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length) {
    assert(handle == 1 && strcmp(key, NVS_BALANCE_KEY) == 0);
    if (!blob_present) return ESP_ERR_NVS_NOT_FOUND;
    assert(*length >= fake_length); memcpy(value, &fake_blob, fake_length);
    *length = fake_length; return ESP_OK;
}
'''
        harness += "\n".join(functions)
        harness += r'''
static void checksum(void) {
    fake_blob.crc32 = crc32_bytes(&fake_blob, offsetof(stored_balance_t, crc32));
}
static void assert_rejected(const quota_device_config_t *config) {
    memset(&s_view, 0, sizeof(s_view)); nvs_load_balance_snapshot(config);
    assert(!s_balance_cache_present && !s_view.snapshot.balances[0].present);
}
int main(void) {
    quota_device_config_t config = {0};
    assert(nvs_load_screen_timeout() == 120);  /* Legacy pairing has no new key. */
    timeout_present = true; fake_timeout = 0; assert(nvs_load_screen_timeout() == 0);
    fake_timeout = 600; assert(nvs_load_screen_timeout() == 600);
    fake_timeout = 31; assert(nvs_load_screen_timeout() == 120);
    assert_rejected(&config); /* Legacy quota cache has no balance sidecar. */
    s_stored_snapshot.stored_at = 1700000000; s_stored_snapshot.snapshot.revision = 7;
    fake_blob.magic = STORED_BALANCE_MAGIC; fake_blob.config_identity = 123;
    fake_blob.stored_at = 1700000000; fake_blob.revision = 7;
    quota_balance_t *balance = &fake_blob.balances[0];
    balance->present = true; balance->is_available = true; balance->currency_count = 1;
    strcpy(balance->label, "Local API");
    strcpy(balance->balance_infos[0].currency, "CNY");
    strcpy(balance->balance_infos[0].total_balance, "12.3456789");
    strcpy(balance->balance_infos[0].granted_balance, "2.3456789");
    strcpy(balance->balance_infos[0].topped_up_balance, "10.00");
    checksum(); blob_present = true; fake_length = sizeof(fake_blob);
    nvs_load_balance_snapshot(&config);
    assert(s_balance_cache_present && s_view.snapshot.balances[0].present);
    assert(strcmp(s_view.snapshot.balances[0].balance_infos[0].total_balance, "12.3456789") == 0);
    fake_blob.revision++; checksum(); assert_rejected(&config); fake_blob.revision--;
    fake_blob.config_identity++; checksum(); assert_rejected(&config); fake_blob.config_identity--;
    fake_blob.stored_at++; checksum(); assert_rejected(&config); fake_blob.stored_at--;
    checksum(); fake_blob.crc32 ^= 1; assert_rejected(&config);
    checksum(); fake_length--; assert_rejected(&config); fake_length++;
    strcpy(balance->balance_infos[0].total_balance, "NaN"); checksum(); assert_rejected(&config);
    puts("quota storage runtime tests passed");
}
'''
        compile_and_run(harness, "ai-quota-storage-test-",
                        ("main/quota_logic.c", "tests/cjson/cJSON.c"))


if __name__ == "__main__":
    unittest.main()
