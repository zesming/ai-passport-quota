"""First boot after an upgrade: removed rows, credentials, cached observations and retired
storage are cleaned idempotently, with the real store and catalog C and an in-memory NVS."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from runtime_helpers import ROOT

HARNESS = r'''
#include "quota_store.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quota_store.c"

#define ID_OTHER "00000000000000000000000000000c00"
#define ID_LEGACY "00000000000000000000000000000c01"
#define ID_PENDING "00000000000000000000000000000c02"
#define ID_CODEX "00000000000000000000000000000c03"
#define ID_DEEPSEEK "00000000000000000000000000000c04"
#define CRED_PENDING "0000000000000000000000000000d002"
#define CRED_CODEX "0000000000000000000000000000d003"
#define CRED_DEEPSEEK "0000000000000000000000000000d004"
#define PENDING_SECRET "SECRET-PENDING-ACCESS-TOKEN"
#define OLD_PROVIDER_OTHER 1
#define OLD_SOURCE_LEGACY 1
#define OLD_ACTIVITY_PENDING 1
#define SEQUENCE 7

static quota_model_t model;
static quota_portable_credential_t credential;
static nvs_stub_state_t initial, reference;
static quota_model_t reference_model;
static uint64_t reference_sequence;

static void put_credential(uint8_t slot, quota_provider_t provider, const char *id, const char *secret)
{
    memset(&credential, 0, sizeof(credential));
    credential.slot = slot; credential.provider = provider; credential.generation = 1;
    strcpy(credential.id, id); strcpy(credential.label, "label");
    credential.auth_state = QUOTA_PORTABLE_AUTH_READY;
    if (provider == QUOTA_PROVIDER_CODEX) {
        strcpy(credential.access_token, secret); strcpy(credential.refresh_token, "refresh");
        strcpy(credential.server_account_id, "acct");
    } else strcpy(credential.api_key, secret);
    assert(quota_store_save_credential(slot, &credential));
}

static void put_model(const quota_model_t *value)
{
    model_record_t *record = calloc(1, sizeof(*record));
    record->magic = MODEL_MAGIC; record->version = MODEL_VERSION; record->bytes = sizeof(*record);
    record->sequence = SEQUENCE; record->value = *value;
    record->crc = store_crc(record, offsetof(model_record_t, crc));
    nvs_stub_put(STORE_PARTITION, STORE_NAMESPACE, "model_v2", record, sizeof(*record));
    free(record);
}

/* Written as the previous firmware did: raw values the current enums no longer name. */
static void set_entry(unsigned index, const char *id, int provider, int source, int activity,
                      uint8_t slot, const char *credential_id)
{
    quota_catalog_entry_t *entry = &model.entries[index];
    memset(entry, 0, sizeof(*entry));
    strcpy(entry->logical_id, id); strcpy(entry->label, "alias");
    entry->provider = (quota_provider_t)provider; entry->source = (quota_account_source_t)source;
    entry->activity = (quota_account_activity_t)activity; entry->row_generation = 1;
    if (source == 0) {
        entry->binding.native.slot = slot; entry->binding.native.credential_generation = 1;
        strcpy(entry->binding.native.credential_id, credential_id);
    } else {
        entry->binding.reserved_legacy[0] = 1; /* endpoint epoch */
        memcpy(&entry->binding.reserved_legacy[4], id, 33); /* remote id */
    }
}

static void put_observation(observation_record_t *record, unsigned index, const char *id, int provider,
                            int source, uint8_t slot, const char *credential_id, uint8_t percent)
{
    observation_t *row = &record->rows[index];
    memset(row, 0, sizeof(*row));
    strcpy(row->logical_id, id); row->provider = (quota_provider_t)provider;
    row->source = (quota_account_source_t)source; row->row_generation = 1;
    row->status = QUOTA_STATUS_OK; row->has_observed_at = true; row->observed_at = 1790000000;
    row->five_hour.present = true; row->five_hour.remaining_percent = percent;
    if (source == 0) {
        row->binding.native.slot = slot; row->binding.native.credential_generation = 1;
        strcpy(row->binding.native.credential_id, credential_id);
    } else {
        row->binding.reserved_legacy[0] = 1;
        memcpy(&row->binding.reserved_legacy[4], id, 33);
    }
}

static void build_old_device(bool with_intent)
{
    memset(&nvs_stub, 0, sizeof(nvs_stub)); nvs_stub.cut_after = -1;
    assert(quota_store_init());
    memset(&model, 0, sizeof(model));
    model.network_count = 3; model.selected_network = 1;
    for (unsigned i = 0; i < 3; i++) {
        snprintf(model.networks[i].ssid, sizeof(model.networks[i].ssid), "wifi-%u", i);
        snprintf(model.networks[i].password, sizeof(model.networks[i].password), "password-%u", i);
    }
    model.refresh_seconds = 900; model.screen_timeout_seconds = 60; model.auto_refresh = true;
    model.last_known_time = 1790000000;
    model.reserved_pending_network[0] = 1; /* pending network present */
    strcpy((char *)&model.reserved_pending_network[1], "Historical");
    model.reserved_legacy_endpoint[0] = 1; /* endpoint enabled */
    model.reserved_legacy_endpoint[4] = 1;
    strcpy((char *)&model.reserved_legacy_endpoint[8], "https://192.168.1.2:1234");
    set_entry(0, ID_OTHER, OLD_PROVIDER_OTHER, OLD_SOURCE_LEGACY, 0, 0, "");
    set_entry(1, ID_LEGACY, QUOTA_PROVIDER_CODEX, OLD_SOURCE_LEGACY, 0, 0, "");
    set_entry(2, ID_PENDING, QUOTA_PROVIDER_CODEX, 0, OLD_ACTIVITY_PENDING, 2, CRED_PENDING);
    set_entry(3, ID_CODEX, QUOTA_PROVIDER_CODEX, 0, 0, 0, CRED_CODEX);
    set_entry(4, ID_DEEPSEEK, QUOTA_PROVIDER_DEEPSEEK, 0, 0, 1, CRED_DEEPSEEK);
    model.entry_count = 5;
    strcpy(model.selected_account_id, ID_OTHER);
    if (with_intent) { /* An interrupted re-authorization of the pending row. */
        quota_model_intent_t *intent = &model.intent;
        intent->kind = QUOTA_INTENT_UPSERT_NATIVE; intent->provider = QUOTA_PROVIDER_CODEX; intent->slot = 2;
        strcpy(intent->logical_id, ID_PENDING); intent->expected_row_generation = 1;
        strcpy(intent->previous_credential_id, CRED_PENDING); intent->previous_credential_generation = 1;
        strcpy(intent->target_credential_id, CRED_PENDING); intent->target_credential_generation = 2;
    }
    put_model(&model);
    put_credential(0, QUOTA_PROVIDER_CODEX, CRED_CODEX, "codex-access");
    put_credential(1, QUOTA_PROVIDER_DEEPSEEK, CRED_DEEPSEEK, "deepseek-key");
    put_credential(2, QUOTA_PROVIDER_CODEX, CRED_PENDING, PENDING_SECRET);
    observation_record_t *cache = calloc(1, sizeof(*cache));
    cache->magic = OBSERVATION_MAGIC; cache->version = MODEL_VERSION; cache->bytes = sizeof(*cache);
    cache->stored_at = 1790000000; cache->count = 5;
    put_observation(cache, 0, ID_OTHER, OLD_PROVIDER_OTHER, OLD_SOURCE_LEGACY, 0, "", 11);
    put_observation(cache, 1, ID_LEGACY, QUOTA_PROVIDER_CODEX, OLD_SOURCE_LEGACY, 0, "", 22);
    put_observation(cache, 2, ID_PENDING, QUOTA_PROVIDER_CODEX, 0, 2, CRED_PENDING, 33);
    put_observation(cache, 3, ID_CODEX, QUOTA_PROVIDER_CODEX, 0, 0, CRED_CODEX, 44);
    put_observation(cache, 4, ID_DEEPSEEK, QUOTA_PROVIDER_DEEPSEEK, 0, 1, CRED_DEEPSEEK, 55);
    cache->crc = store_crc(cache, offsetof(observation_record_t, crc));
    nvs_stub_put(STORE_PARTITION, STORE_NAMESPACE, "observations_v2", cache, sizeof(*cache));
    free(cache);
    nvs_stub.mutations = 0;
}

static bool zero_bytes(const void *bytes, size_t length)
{
    const unsigned char *p = bytes;
    while (length--) if (*p++) return false;
    return true;
}

static bool contains(const nvs_stub_item_t *item, const char *text)
{
    size_t n = strlen(text);
    for (size_t i = 0; i + n <= item->length; i++) if (!memcmp(item->bytes + i, text, n)) return true;
    return false;
}

static void check_cleaned(const quota_model_t *loaded, uint64_t sequence)
{
    assert(sequence == SEQUENCE + 1 && quota_catalog_valid(loaded));
    assert(loaded->entry_count == 2);
    assert(!strcmp(loaded->entries[0].logical_id, ID_CODEX) && loaded->entries[0].provider == QUOTA_PROVIDER_CODEX);
    assert(!strcmp(loaded->entries[1].logical_id, ID_DEEPSEEK) && loaded->entries[1].provider == QUOTA_PROVIDER_DEEPSEEK);
    assert(!strcmp(loaded->selected_account_id, ID_CODEX)); /* Was the row of the removed provider. */
    assert(zero_bytes(&loaded->entries[2], (QUOTA_CATALOG_STORED_ROWS - 2) * sizeof(loaded->entries[0])));
    assert(zero_bytes(loaded->reserved_pending_network, sizeof(loaded->reserved_pending_network)));
    assert(zero_bytes(loaded->reserved_legacy_endpoint, sizeof(loaded->reserved_legacy_endpoint)));
    assert(loaded->intent.kind == QUOTA_INTENT_NONE);
    /* Everything else survives. */
    assert(loaded->network_count == 3 && loaded->selected_network == 1);
    assert(!strcmp(loaded->networks[2].ssid, "wifi-2") && !strcmp(loaded->networks[2].password, "password-2"));
    assert(loaded->refresh_seconds == 900 && loaded->screen_timeout_seconds == 60 && loaded->auto_refresh);
    assert(loaded->last_known_time == 1790000000);
}

static void check_storage(void)
{
    /* Credentials of kept rows are untouched; the pending row's slot is a tombstone with no token. */
    static quota_portable_credential_t loaded;
    assert(quota_store_load_credential_result(0, &loaded) == QUOTA_STORE_READ_OK && !loaded.tombstone &&
           !strcmp(loaded.access_token, "codex-access"));
    assert(quota_store_load_credential_result(1, &loaded) == QUOTA_STORE_READ_OK && !loaded.tombstone &&
           !strcmp(loaded.api_key, "deepseek-key"));
    assert(quota_store_load_credential_result(2, &loaded) == QUOTA_STORE_READ_OK && loaded.tombstone &&
           loaded.generation == 1 && !loaded.access_token[0] && !loaded.refresh_token[0] && !loaded.api_key[0]);
    assert(!contains(nvs_stub_find(STORE_PARTITION, STORE_NAMESPACE, "account2"), PENDING_SECRET));
    /* Cached observations of dropped rows are zeroed and the record stays well-formed. */
    const nvs_stub_item_t *item = nvs_stub_find(STORE_PARTITION, STORE_NAMESPACE, "observations_v2");
    observation_record_t *cache = (observation_record_t *)item->bytes;
    assert(item->length == sizeof(*cache) && cache->crc == store_crc(cache, offsetof(observation_record_t, crc)));
    assert(cache->count == 2 && !strcmp(cache->rows[0].logical_id, ID_CODEX) && cache->rows[0].five_hour.remaining_percent == 44);
    assert(!strcmp(cache->rows[1].logical_id, ID_DEEPSEEK) && cache->rows[1].five_hour.remaining_percent == 55);
    assert(zero_bytes(&cache->rows[2], (QUOTA_MAX_ACCOUNTS - 2) * sizeof(cache->rows[0])));
    quota_snapshot_t snapshot = {0};
    snapshot.account_count = 2;
    strcpy(snapshot.accounts[0].id, ID_CODEX); strcpy(snapshot.accounts[1].id, ID_DEEPSEEK);
    quota_model_t current; uint64_t sequence;
    assert(quota_store_load_model_result(&current, &sequence) == QUOTA_STORE_READ_OK);
    assert(quota_store_load_observations(&current, 1790000001, &snapshot) == QUOTA_STORE_READ_OK);
    assert(snapshot.accounts[0].five_hour.remaining_percent == 44 && snapshot.accounts[1].five_hour.remaining_percent == 55);
}

static void check_same_as_reference(void)
{
    for (unsigned i = 0; i < NVS_STUB_ITEMS; i++) {
        const nvs_stub_item_t *want = &reference.items[i];
        if (!want->used) continue;
        const nvs_stub_item_t *have = nvs_stub_find(want->partition, want->name_space, want->key);
        assert(have && have->length == want->length && !memcmp(have->bytes, want->bytes, want->length));
    }
    for (unsigned i = 0; i < NVS_STUB_ITEMS; i++) {
        const nvs_stub_item_t *have = &nvs_stub.items[i];
        if (!have->used) continue;
        bool known = false;
        for (unsigned j = 0; j < NVS_STUB_ITEMS; j++)
            known = known || (reference.items[j].used && !strcmp(reference.items[j].key, have->key));
        assert(known);
    }
}

static void upgrade_and_power_cut(bool with_intent)
{
    build_old_device(with_intent);
    initial = nvs_stub;
    quota_model_t loaded; uint64_t sequence = 0;
    assert(quota_store_load_model_result(&loaded, &sequence) == QUOTA_STORE_READ_OK);
    check_cleaned(&loaded, sequence);
    check_storage();
    unsigned total = nvs_stub.mutations;
    assert(total >= 3); /* slot, cache, model */
    reference = nvs_stub; reference_model = loaded; reference_sequence = sequence;
    /* A second boot finds nothing to remove and writes nothing. */
    nvs_stub.mutations = 0;
    quota_model_t again; uint64_t again_sequence = 0;
    assert(quota_store_load_model_result(&again, &again_sequence) == QUOTA_STORE_READ_OK);
    assert(again_sequence == sequence && !memcmp(&again, &loaded, sizeof(loaded)) && nvs_stub.mutations == 0);
    /* Power lost after any number of writes: the next boot repeats the cleanup to the same result. */
    for (unsigned cut = 0; cut <= total; cut++) {
        nvs_stub = initial; nvs_stub.mutations = 0; nvs_stub.cut_after = (long)cut;
        quota_model_t interrupted; uint64_t interrupted_sequence = 0;
        quota_store_read_result_t result = quota_store_load_model_result(&interrupted, &interrupted_sequence);
        assert(cut == total ? result == QUOTA_STORE_READ_OK : result != QUOTA_STORE_READ_OK);
        nvs_stub.cut_after = -1; nvs_stub.mutations = 0;
        quota_model_t rebooted; uint64_t rebooted_sequence = 0;
        assert(quota_store_load_model_result(&rebooted, &rebooted_sequence) == QUOTA_STORE_READ_OK);
        assert(rebooted_sequence == reference_sequence && !memcmp(&rebooted, &reference_model, sizeof(rebooted)));
        check_same_as_reference();
        assert(cut == total || nvs_stub.mutations > 0);
        nvs_stub.mutations = 0;
        assert(quota_store_load_model_result(&rebooted, &rebooted_sequence) == QUOTA_STORE_READ_OK && nvs_stub.mutations == 0);
    }
}

static void retired_storage(void)
{
    memset(&nvs_stub, 0, sizeof(nvs_stub)); nvs_stub.cut_after = -1;
    s_ready = false; assert(quota_store_init());
    /* A clean device: nothing to erase, nothing written, no namespace created. */
    assert(quota_store_erase_retired(true) && nvs_stub.mutations == 0 && nvs_stub.space_count == 0);
    /* A device that only has the retired data starts as a new device. */
    static const char *const keys[] = {"device_cfg", "quota_cache", "screen_to", "balance_cache"};
    for (unsigned i = 0; i < 4; i++) nvs_stub_put("nvs", "ai_quota", keys[i], "retired", 8);
    nvs_stub_put("nvs", "other_app", "keep", "me", 3);
    nvs_stub_put(STORE_PARTITION, STORE_NAMESPACE, "config", "wifi password", 14);
    nvs_stub_put(STORE_PARTITION, STORE_NAMESPACE, "snapshot", "old", 4);
    nvs_stub_put(STORE_PARTITION, STORE_NAMESPACE, "account3", "credential", 11);
    nvs_stub.mutations = 0;
    quota_model_t model_out; uint64_t sequence = 9;
    assert(quota_store_erase_retired(true));
    assert(nvs_stub_count("nvs", "ai_quota") == 0 && nvs_stub_count("nvs", "other_app") == 1);
    assert(!nvs_stub_find(STORE_PARTITION, STORE_NAMESPACE, "config") && !nvs_stub_find(STORE_PARTITION, STORE_NAMESPACE, "snapshot"));
    assert(nvs_stub_find(STORE_PARTITION, STORE_NAMESPACE, "account3")); /* Live records are never touched. */
    assert(quota_store_load_model_result(&model_out, &sequence) == QUOTA_STORE_READ_MISSING && sequence == 0);
    /* Idempotent, also with the default partition unavailable. */
    unsigned writes = nvs_stub.mutations;
    assert(quota_store_erase_retired(true) && quota_store_erase_retired(false) && nvs_stub.mutations == writes);
    /* A power cut mid-way is repeated at the next boot. */
    for (unsigned i = 0; i < 4; i++) nvs_stub_put("nvs", "ai_quota", keys[i], "retired", 8);
    nvs_stub_put(STORE_PARTITION, STORE_NAMESPACE, "config", "wifi password", 14);
    nvs_stub.mutations = 0; nvs_stub.cut_after = 2;
    assert(!quota_store_erase_retired(true) && nvs_stub_count("nvs", "ai_quota") > 0);
    nvs_stub.cut_after = -1;
    assert(quota_store_erase_retired(true) && nvs_stub_count("nvs", "ai_quota") == 0 &&
           !nvs_stub_find(STORE_PARTITION, STORE_NAMESPACE, "config"));
}

static void unusable_rows_fail_closed(void)
{
    /* A damaged catalog is still refused; cleanup never repairs what it cannot recognise. */
    build_old_device(false);
    model.entries[3].logical_id[0] = 'X';
    put_model(&model);
    quota_model_t out; uint64_t sequence;
    nvs_stub.mutations = 0;
    assert(quota_store_load_model_result(&out, &sequence) == QUOTA_STORE_READ_INVALID && nvs_stub.mutations == 0);
    /* An unreadable credential slot is overwritten with a tombstone and the cleanup still completes. */
    build_old_device(false);
    nvs_stub_put(STORE_PARTITION, STORE_NAMESPACE, "account2", "garbage", 8);
    nvs_stub.mutations = 0;
    assert(quota_store_load_model_result(&out, &sequence) == QUOTA_STORE_READ_OK && sequence == SEQUENCE + 1);
    assert(out.entry_count == 2); /* An unreadable slot is overwritten, not an obstacle. */
}

static void orphan_credentials_are_freed(void)
{
    /* No catalog: every readable credential slot becomes a tombstone, nothing else is touched. */
    memset(&nvs_stub, 0, sizeof(nvs_stub)); nvs_stub.cut_after = -1;
    s_ready = false; assert(quota_store_init());
    put_credential(0, QUOTA_PROVIDER_CODEX, CRED_CODEX, "codex-access");
    put_credential(1, QUOTA_PROVIDER_DEEPSEEK, CRED_DEEPSEEK, "deepseek-key");
    put_credential(2, QUOTA_PROVIDER_CODEX, CRED_PENDING, PENDING_SECRET);
    nvs_stub_put(STORE_PARTITION, STORE_NAMESPACE, "account3", "garbage", 8);
    quota_portable_credential_t *tomb = calloc(1, sizeof(*tomb));
    tomb->slot = 4; tomb->provider = QUOTA_PROVIDER_CODEX; tomb->generation = 5; tomb->tombstone = true;
    strcpy(tomb->id, CRED_CODEX);
    assert(quota_store_save_credential(4, tomb)); free(tomb);
    nvs_stub_state_t before = nvs_stub; nvs_stub.mutations = 0;
    assert(quota_store_release_orphan_credentials() == QUOTA_STORE_READ_OK);
    static quota_portable_credential_t loaded;
    for (uint8_t slot = 0; slot < 3; slot++) {
        assert(quota_store_load_credential_result(slot, &loaded) == QUOTA_STORE_READ_OK && loaded.tombstone &&
               loaded.generation == 1 && !loaded.access_token[0] && !loaded.api_key[0]);
    }
    assert(!contains(nvs_stub_find(STORE_PARTITION, STORE_NAMESPACE, "account2"), PENDING_SECRET));
    assert(!contains(nvs_stub_find(STORE_PARTITION, STORE_NAMESPACE, "account1"), "deepseek-key"));
    assert(quota_store_load_credential_result(3, &loaded) == QUOTA_STORE_READ_INVALID); /* Left as found. */
    assert(quota_store_load_credential_result(4, &loaded) == QUOTA_STORE_READ_OK && loaded.generation == 5);
    assert(nvs_stub.mutations == 3);
    nvs_stub.mutations = 0; /* A second boot has nothing left to free. */
    assert(quota_store_release_orphan_credentials() == QUOTA_STORE_READ_OK && nvs_stub.mutations == 0);
    /* Power lost part-way: the repeat finishes the job. */
    nvs_stub = before; nvs_stub.mutations = 0; nvs_stub.cut_after = 1;
    assert(quota_store_release_orphan_credentials() != QUOTA_STORE_READ_OK);
    nvs_stub.cut_after = -1;
    assert(quota_store_release_orphan_credentials() == QUOTA_STORE_READ_OK);
    for (uint8_t slot = 0; slot < 3; slot++)
        assert(quota_store_load_credential_result(slot, &loaded) == QUOTA_STORE_READ_OK && loaded.tombstone);
}

static void stale_cache_rows_do_not_discard_the_rest(void)
{
    /* A clean catalog needs no cleanup, but an old cache may still hold rows of removed
     * providers or sources; only those rows are ignored. */
    build_old_device(false);
    memset(&model, 0, sizeof(model));
    model.refresh_seconds = 300; model.screen_timeout_seconds = 120;
    set_entry(0, ID_CODEX, QUOTA_PROVIDER_CODEX, 0, 0, 0, CRED_CODEX);
    set_entry(1, ID_DEEPSEEK, QUOTA_PROVIDER_DEEPSEEK, 0, 0, 1, CRED_DEEPSEEK);
    model.entry_count = 2;
    put_model(&model);
    observation_record_t *cache = calloc(1, sizeof(*cache));
    cache->magic = OBSERVATION_MAGIC; cache->version = MODEL_VERSION; cache->bytes = sizeof(*cache);
    cache->stored_at = 1790000000; cache->count = 4;
    put_observation(cache, 0, ID_OTHER, OLD_PROVIDER_OTHER, OLD_SOURCE_LEGACY, 0, "", 11);
    put_observation(cache, 1, ID_LEGACY, QUOTA_PROVIDER_CODEX, OLD_SOURCE_LEGACY, 0, "", 22);
    put_observation(cache, 2, ID_CODEX, QUOTA_PROVIDER_CODEX, 0, 0, CRED_CODEX, 44);
    put_observation(cache, 3, ID_DEEPSEEK, QUOTA_PROVIDER_DEEPSEEK, 0, 1, CRED_DEEPSEEK, 55);
    cache->crc = store_crc(cache, offsetof(observation_record_t, crc));
    nvs_stub_put(STORE_PARTITION, STORE_NAMESPACE, "observations_v2", cache, sizeof(*cache));
    free(cache);
    nvs_stub.mutations = 0;
    quota_model_t current; uint64_t sequence;
    assert(quota_store_load_model_result(&current, &sequence) == QUOTA_STORE_READ_OK && sequence == SEQUENCE);
    quota_snapshot_t snapshot = {0};
    snapshot.account_count = 2;
    strcpy(snapshot.accounts[0].id, ID_CODEX); strcpy(snapshot.accounts[1].id, ID_DEEPSEEK);
    assert(quota_store_load_observations(&current, 1790000001, &snapshot) == QUOTA_STORE_READ_OK);
    assert(snapshot.accounts[0].five_hour.remaining_percent == 44 && snapshot.accounts[1].five_hour.remaining_percent == 55);
    assert(nvs_stub.mutations == 0);
    /* A damaged kept row still invalidates the cache. */
    observation_record_t *stored = (observation_record_t *)nvs_stub_find(STORE_PARTITION, STORE_NAMESPACE, "observations_v2")->bytes;
    stored->rows[2].row_generation = 0;
    stored->crc = store_crc(stored, offsetof(observation_record_t, crc));
    assert(quota_store_load_observations(&current, 1790000001, &snapshot) == QUOTA_STORE_READ_INVALID);
}

int main(void)
{
    upgrade_and_power_cut(false);
    upgrade_and_power_cut(true);
    retired_storage();
    unusable_rows_fail_closed();
    orphan_credentials_are_freed();
    stale_cache_rows_do_not_discard_the_rest();
    puts("upgrade cleanup passed");
}
'''


class StorageRuntime(unittest.TestCase):
    def test_upgrade_cleanup_is_idempotent_and_power_cut_safe(self):
        with tempfile.TemporaryDirectory(prefix="quota-storage-") as directory:
            path = Path(directory)
            (path / "test.c").write_text(HARNESS)
            executable = path / "test"
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I" + str(ROOT / "tests/nvs_stub"), "-I" + str(ROOT / "main"),
                str(path / "test.c"), str(ROOT / "main/quota_catalog.c"), str(ROOT / "main/quota_logic.c"),
                str(ROOT / "tests/nvs_stub/nvs_stub.c"), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_service_cleans_retired_storage_before_the_catalog_is_read(self):
        source = (ROOT / "main/quota_service.c").read_text()
        init = source[source.index("bool quota_service_init(void)"):source.index("bool quota_service_start(void)")]
        order = [init.index(name) for name in ("nvs_flash_init()", "quota_store_erase_retired(",
                                                "quota_portable_service_init(")]
        self.assertEqual(order, sorted(order))

    def test_stored_layout_is_frozen(self):
        """The firmware compiles these assertions; record the intent in one place for readers."""
        source = (ROOT / "main/quota_store.c").read_text()
        for needle in ("sizeof(model_record_t) == 4424", "sizeof(observation_record_t) == 3104"):
            self.assertIn(needle, source)
        catalog = (ROOT / "main/quota_catalog.h").read_text()
        for needle in ("sizeof(quota_model_t) == 4400", "reserved_legacy_endpoint[1728]",
                       "reserved_pending_network[99]", "reserved_legacy[40]"):
            self.assertIn(needle, catalog)


if __name__ == "__main__":
    unittest.main()
