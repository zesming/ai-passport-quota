#include "quota_catalog.h"
#include <string.h>

static bool text(const char *value, size_t capacity, bool empty, bool ascii)
{
    const char *end = memchr(value, 0, capacity);
    if (!end || (!empty && end == value)) return false;
    size_t bytes = (size_t)(end - value);
    if (!quota_utf8_is_valid(value, bytes)) return false;
    for (size_t i = 0; i < bytes; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < 32 || c == 127 || (ascii && c > 126)) return false;
    }
    return true;
}

static bool network_valid(const quota_portable_network_t *network)
{
    return text(network->ssid, sizeof(network->ssid), false, false) &&
           text(network->password, sizeof(network->password), true, false);
}

int quota_catalog_find(const quota_model_t *model, const char *logical_id)
{
    if (!model || !logical_id || model->entry_count > QUOTA_CATALOG_CAPACITY) return -1;
    for (unsigned i = 0; i < model->entry_count; i++)
        if (!strcmp(model->entries[i].logical_id, logical_id)) return (int)i;
    return -1;
}

size_t quota_catalog_count(const quota_model_t *model, quota_account_activity_t activity)
{
    size_t count = 0;
    if (!model || model->entry_count > QUOTA_CATALOG_CAPACITY) return 0;
    for (unsigned i = 0; i < model->entry_count; i++)
        if (model->entries[i].activity == activity) count++;
    return count;
}

bool quota_catalog_binding_equal(const quota_catalog_entry_t *a, const quota_catalog_entry_t *b)
{
    if (!a || !b || a->provider != b->provider || a->source != b->source ||
        a->row_generation != b->row_generation || strcmp(a->logical_id, b->logical_id)) return false;
    if (a->source == QUOTA_ACCOUNT_DEVICE)
        return a->binding.native.slot == b->binding.native.slot &&
            a->binding.native.credential_generation == b->binding.native.credential_generation &&
            !strcmp(a->binding.native.credential_id, b->binding.native.credential_id);
    return a->binding.legacy.endpoint_epoch == b->binding.legacy.endpoint_epoch &&
           !strcmp(a->binding.legacy.remote_id, b->binding.legacy.remote_id);
}

bool quota_catalog_native_matches(const quota_catalog_entry_t *entry,
                                  const quota_portable_credential_t *credential)
{
    return entry && credential && entry->source == QUOTA_ACCOUNT_DEVICE && !credential->tombstone &&
        entry->provider == credential->provider && entry->binding.native.slot == credential->slot &&
        entry->binding.native.credential_generation == credential->generation &&
        !strcmp(entry->binding.native.credential_id, credential->id);
}

bool quota_catalog_valid(const quota_model_t *model)
{
    if (!model || model->network_count > QUOTA_PORTABLE_NETWORKS ||
        (model->network_count ? model->selected_network >= model->network_count : model->selected_network != 0) ||
        !quota_refresh_seconds_is_valid(model->refresh_seconds) ||
        !quota_screen_timeout_is_valid(model->screen_timeout_seconds) ||
        model->entry_count > QUOTA_CATALOG_CAPACITY ||
        quota_catalog_count(model, QUOTA_ACCOUNT_ACTIVE) > QUOTA_MAX_ACCOUNTS ||
        quota_catalog_count(model, QUOTA_ACCOUNT_PENDING) > QUOTA_MAX_ACCOUNTS ||
        !text(model->selected_account_id, sizeof(model->selected_account_id), true, true)) return false;
    if (model->selected_account_id[0]) {
        int selected = quota_catalog_find(model, model->selected_account_id);
        if (selected < 0 || model->entries[selected].activity != QUOTA_ACCOUNT_ACTIVE) return false;
    }
    for (unsigned i = 0; i < model->network_count; i++) if (!network_valid(&model->networks[i])) return false;
    if (model->pending_network_present && !network_valid(&model->pending_network)) return false;
    const quota_legacy_endpoint_t *endpoint = &model->legacy;
    if (endpoint->enabled) {
        char host[16];
        if (!endpoint->epoch || !text(endpoint->base_url, sizeof(endpoint->base_url), false, true) ||
            !text(endpoint->pair_token, sizeof(endpoint->pair_token), false, true) ||
            !quota_url_is_private_ipv4(endpoint->base_url, host) ||
            !quota_pair_token_is_valid(endpoint->pair_token) ||
            !memchr(endpoint->server_cert_pem, 0, sizeof(endpoint->server_cert_pem)) ||
            strncmp(endpoint->server_cert_pem, "-----BEGIN CERTIFICATE-----", 27) ||
            !strstr(endpoint->server_cert_pem, "-----END CERTIFICATE-----")) return false;
    }
    for (unsigned i = 0; i < model->entry_count; i++) {
        const quota_catalog_entry_t *entry = &model->entries[i];
        if (!text(entry->logical_id, sizeof(entry->logical_id), false, true) ||
            !quota_id_is_valid(entry->logical_id) || !entry->row_generation ||
            (unsigned)entry->provider > QUOTA_PROVIDER_DEEPSEEK ||
            (unsigned)entry->source > QUOTA_ACCOUNT_LEGACY ||
            (unsigned)entry->activity > QUOTA_ACCOUNT_PENDING ||
            !text(entry->label, sizeof(entry->label), true, false)) return false;
        if (entry->source == QUOTA_ACCOUNT_DEVICE) {
            if (entry->provider == QUOTA_PROVIDER_CLAUDE || entry->binding.native.slot >= QUOTA_MAX_ACCOUNTS ||
                !entry->binding.native.credential_generation ||
                !text(entry->binding.native.credential_id, sizeof(entry->binding.native.credential_id), false, true) ||
                !quota_id_is_valid(entry->binding.native.credential_id)) return false;
        } else if (!entry->binding.legacy.endpoint_epoch ||
                   !text(entry->binding.legacy.remote_id, sizeof(entry->binding.legacy.remote_id), false, true) ||
                   !quota_id_is_valid(entry->binding.legacy.remote_id)) return false;
        for (unsigned j = 0; j < i; j++) {
            const quota_catalog_entry_t *previous = &model->entries[j];
            if (!strcmp(previous->logical_id, entry->logical_id)) return false;
            if (entry->source == QUOTA_ACCOUNT_DEVICE && previous->source == QUOTA_ACCOUNT_DEVICE &&
                entry->binding.native.slot == previous->binding.native.slot) return false;
            if (entry->source == QUOTA_ACCOUNT_LEGACY && previous->source == QUOTA_ACCOUNT_LEGACY &&
                entry->binding.legacy.endpoint_epoch == previous->binding.legacy.endpoint_epoch &&
                !strcmp(entry->binding.legacy.remote_id, previous->binding.legacy.remote_id)) return false;
        }
    }
    const quota_model_intent_t *intent = &model->intent;
    if (intent->kind == QUOTA_INTENT_NONE) return true;
    if ((unsigned)intent->kind > QUOTA_INTENT_DELETE_NATIVE || intent->slot >= QUOTA_MAX_ACCOUNTS ||
        (intent->provider != QUOTA_PROVIDER_CODEX && intent->provider != QUOTA_PROVIDER_DEEPSEEK) ||
        !text(intent->logical_id, sizeof(intent->logical_id), false, true) || !quota_id_is_valid(intent->logical_id) ||
        !text(intent->target_credential_id, sizeof(intent->target_credential_id), false, true) ||
        !quota_id_is_valid(intent->target_credential_id) || !intent->target_credential_generation ||
        !text(intent->desired_label, sizeof(intent->desired_label), true, false)) return false;
    if (intent->previous_missing) {
        if (intent->previous_tombstone || intent->previous_credential_generation ||
            intent->previous_credential_id[0] || intent->target_credential_generation != 1) return false;
    } else if (!text(intent->previous_credential_id, sizeof(intent->previous_credential_id), false, true) ||
               !quota_id_is_valid(intent->previous_credential_id) || !intent->previous_credential_generation ||
               intent->previous_credential_generation == UINT32_MAX ||
               intent->target_credential_generation != intent->previous_credential_generation + 1) return false;
    int row = quota_catalog_find(model, intent->logical_id);
    for (unsigned i = 0; i < model->entry_count; i++)
        if (model->entries[i].source == QUOTA_ACCOUNT_DEVICE &&
            model->entries[i].binding.native.slot == intent->slot && (int)i != row) return false;
    if (intent->kind == QUOTA_INTENT_DELETE_NATIVE)
        return row < 0 && !intent->previous_missing && !intent->previous_tombstone &&
               !strcmp(intent->target_credential_id, intent->previous_credential_id);
    if (intent->new_row)
        return row < 0 && intent->expected_row_generation == 0 &&
               model->entry_count < QUOTA_CATALOG_CAPACITY &&
               quota_catalog_count(model, QUOTA_ACCOUNT_ACTIVE) < QUOTA_MAX_ACCOUNTS &&
               (intent->previous_missing || intent->previous_tombstone);
    if (row < 0 || intent->expected_row_generation == UINT32_MAX ||
        model->entries[row].row_generation != intent->expected_row_generation ||
        model->entries[row].provider != intent->provider) return false;
    const quota_catalog_entry_t *previous = &model->entries[row];
    if (previous->source == QUOTA_ACCOUNT_DEVICE)
        return previous->binding.native.slot == intent->slot && !intent->previous_missing &&
               !intent->previous_tombstone &&
               previous->binding.native.credential_generation == intent->previous_credential_generation &&
               !strcmp(previous->binding.native.credential_id, intent->previous_credential_id);
    return intent->previous_missing || intent->previous_tombstone;
}

void quota_catalog_copy_observation(quota_snapshot_t *target, size_t output,
                                   const quota_snapshot_t *source, size_t input)
{
    if (!target || !source || output >= QUOTA_MAX_ACCOUNTS || input >= source->account_count) return;
    quota_account_t *to = &target->accounts[output];
    const quota_account_t *from = &source->accounts[input];
    to->status = from->status; to->has_observed_at = from->has_observed_at;
    to->observed_at = from->observed_at; to->five_hour = from->five_hour; to->seven_day = from->seven_day;
    char label[QUOTA_PLAN_MAX_BYTES + 1];
    memcpy(label, target->balances[output].label, sizeof(label));
    target->balances[output] = source->balances[input];
    memcpy(target->balances[output].label, label, sizeof(label));
    target->codex_extras[output] = source->codex_extras[input];
}
