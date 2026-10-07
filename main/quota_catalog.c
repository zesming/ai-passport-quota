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
    if (!model || !logical_id || model->entry_count > QUOTA_CATALOG_STORED_ROWS) return -1;
    for (unsigned i = 0; i < model->entry_count; i++)
        if (!strcmp(model->entries[i].logical_id, logical_id)) return (int)i;
    return -1;
}

bool quota_catalog_binding_equal(const quota_catalog_entry_t *a, const quota_catalog_entry_t *b)
{
    if (!a || !b || a->provider != b->provider || a->source != b->source ||
        a->row_generation != b->row_generation || strcmp(a->logical_id, b->logical_id)) return false;
    return a->binding.native.slot == b->binding.native.slot &&
        a->binding.native.credential_generation == b->binding.native.credential_generation &&
        !strcmp(a->binding.native.credential_id, b->binding.native.credential_id);
}

bool quota_catalog_native_matches(const quota_catalog_entry_t *entry,
                                  const quota_portable_credential_t *credential)
{
    return entry && credential && !credential->tombstone &&
        entry->provider == credential->provider && entry->binding.native.slot == credential->slot &&
        entry->binding.native.credential_generation == credential->generation &&
        !strcmp(entry->binding.native.credential_id, credential->id);
}

static bool all_zero(const uint8_t *bytes, size_t length)
{
    while (length--) if (*bytes++) return false;
    return true;
}

bool quota_catalog_scrub_removed(quota_model_t *model, quota_catalog_removed_t *removed)
{
    if (removed) memset(removed, 0, sizeof(*removed));
    if (!model || model->entry_count > QUOTA_CATALOG_STORED_ROWS) return false;
    bool changed = false, selected_removed = false;
    unsigned kept = 0;
    for (unsigned i = 0; i < model->entry_count; i++) {
        quota_catalog_entry_t *entry = &model->entries[i];
        bool keep = (entry->provider == QUOTA_PROVIDER_CODEX ||
                     entry->provider == QUOTA_PROVIDER_DEEPSEEK) &&
                    entry->source == QUOTA_ACCOUNT_DEVICE && entry->activity == QUOTA_ACCOUNT_ACTIVE;
        if (keep) {
            if (kept != i) model->entries[kept] = *entry;
            kept++;
            continue;
        }
        changed = true;
        if (model->selected_account_id[0] &&
            !strncmp(entry->logical_id, model->selected_account_id, sizeof(entry->logical_id)))
            selected_removed = true;
        if (removed && removed->count < QUOTA_CATALOG_STORED_ROWS + 1) {
            memcpy(removed->rows[removed->count].logical_id, entry->logical_id, sizeof(entry->logical_id));
            /* Only a native binding owns a credential slot; the legacy bytes alias it. */
            if (entry->source == QUOTA_ACCOUNT_DEVICE) {
                removed->rows[removed->count].owns_slot = true;
                removed->rows[removed->count].slot = entry->binding.native.slot;
                memcpy(removed->rows[removed->count].credential_id, entry->binding.native.credential_id,
                       sizeof(entry->binding.native.credential_id));
                removed->rows[removed->count].credential_generation = entry->binding.native.credential_generation;
            }
            removed->count++;
        }
        /* An interrupted upsert of a dropped row would never finish; free its target slot too. */
        quota_model_intent_t *intent = &model->intent;
        if (intent->kind != QUOTA_INTENT_NONE && !intent->new_row &&
            !strncmp(intent->logical_id, entry->logical_id, sizeof(intent->logical_id))) {
            if (removed && removed->count < QUOTA_CATALOG_STORED_ROWS + 1) {
                memcpy(removed->rows[removed->count].credential_id, intent->target_credential_id,
                       sizeof(intent->target_credential_id));
                removed->rows[removed->count].owns_slot = true;
                removed->rows[removed->count].slot = intent->slot;
                removed->rows[removed->count++].credential_generation = intent->target_credential_generation;
            }
            memset(intent, 0, sizeof(*intent));
        }
    }
    if (kept != model->entry_count) {
        memset(&model->entries[kept], 0, (model->entry_count - kept) * sizeof(model->entries[0]));
        model->entry_count = (uint8_t)kept;
    }
    if (selected_removed) {
        memset(model->selected_account_id, 0, sizeof(model->selected_account_id));
        if (kept) memcpy(model->selected_account_id, model->entries[0].logical_id, sizeof(model->entries[0].logical_id));
    }
    if (!all_zero(model->reserved_pending_network, sizeof(model->reserved_pending_network))) {
        memset(model->reserved_pending_network, 0, sizeof(model->reserved_pending_network));
        changed = true;
    }
    if (!all_zero(model->reserved_legacy_endpoint, sizeof(model->reserved_legacy_endpoint))) {
        memset(model->reserved_legacy_endpoint, 0, sizeof(model->reserved_legacy_endpoint));
        changed = true;
    }
    return changed;
}

bool quota_catalog_valid(const quota_model_t *model)
{
    if (!model || model->network_count > QUOTA_PORTABLE_NETWORKS ||
        (model->network_count ? model->selected_network >= model->network_count : model->selected_network != 0) ||
        !quota_refresh_seconds_is_valid(model->refresh_seconds) ||
        !quota_screen_timeout_is_valid(model->screen_timeout_seconds) ||
        model->entry_count > QUOTA_MAX_ACCOUNTS ||
        !all_zero(model->reserved_pending_network, sizeof(model->reserved_pending_network)) ||
        !all_zero(model->reserved_legacy_endpoint, sizeof(model->reserved_legacy_endpoint)) ||
        !text(model->selected_account_id, sizeof(model->selected_account_id), true, true)) return false;
    if (model->selected_account_id[0] && quota_catalog_find(model, model->selected_account_id) < 0) return false;
    for (unsigned i = 0; i < model->network_count; i++) if (!network_valid(&model->networks[i])) return false;
    for (unsigned i = 0; i < model->entry_count; i++) {
        const quota_catalog_entry_t *entry = &model->entries[i];
        if (!text(entry->logical_id, sizeof(entry->logical_id), false, true) ||
            !quota_id_is_valid(entry->logical_id) || !entry->row_generation ||
            (entry->provider != QUOTA_PROVIDER_CODEX && entry->provider != QUOTA_PROVIDER_DEEPSEEK) ||
            entry->source != QUOTA_ACCOUNT_DEVICE || entry->activity != QUOTA_ACCOUNT_ACTIVE ||
            !text(entry->label, sizeof(entry->label), true, false) ||
            entry->binding.native.slot >= QUOTA_MAX_ACCOUNTS ||
            !entry->binding.native.credential_generation ||
            !text(entry->binding.native.credential_id, sizeof(entry->binding.native.credential_id), false, true) ||
            !quota_id_is_valid(entry->binding.native.credential_id)) return false;
        for (unsigned j = 0; j < i; j++) {
            const quota_catalog_entry_t *previous = &model->entries[j];
            if (!strcmp(previous->logical_id, entry->logical_id) ||
                entry->binding.native.slot == previous->binding.native.slot) return false;
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
        if (model->entries[i].binding.native.slot == intent->slot && (int)i != row) return false;
    if (intent->kind == QUOTA_INTENT_DELETE_NATIVE)
        return row < 0 && !intent->previous_missing && !intent->previous_tombstone &&
               !strcmp(intent->target_credential_id, intent->previous_credential_id);
    if (intent->new_row)
        return row < 0 && intent->expected_row_generation == 0 &&
               model->entry_count < QUOTA_MAX_ACCOUNTS &&
               (intent->previous_missing || intent->previous_tombstone);
    if (row < 0 || intent->expected_row_generation == UINT32_MAX ||
        model->entries[row].row_generation != intent->expected_row_generation ||
        model->entries[row].provider != intent->provider) return false;
    const quota_catalog_entry_t *previous = &model->entries[row];
    return previous->binding.native.slot == intent->slot && !intent->previous_missing &&
           !intent->previous_tombstone &&
           previous->binding.native.credential_generation == intent->previous_credential_generation &&
           !strcmp(previous->binding.native.credential_id, intent->previous_credential_id);
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
