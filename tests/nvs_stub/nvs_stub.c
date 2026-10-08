#include "nvs.h"
#include "nvs_flash.h"
#include <assert.h>
#include <string.h>

nvs_stub_state_t nvs_stub = {.cut_after = -1};
#define HANDLES 8
static struct {
    int used, mode;
    char partition[16], name_space[16];
} handles[HANDLES];

static int space_exists(const char *partition, const char *name_space)
{
    for (unsigned i = 0; i < nvs_stub.space_count; i++)
        if (!strcmp(nvs_stub.spaces[i][0], partition) && !strcmp(nvs_stub.spaces[i][1], name_space))
            return 1;
    return 0;
}

static void create_space(const char *partition, const char *name_space)
{
    if (space_exists(partition, name_space))
        return;
    assert(nvs_stub.space_count < 8);
    strcpy(nvs_stub.spaces[nvs_stub.space_count][0], partition);
    strcpy(nvs_stub.spaces[nvs_stub.space_count++][1], name_space);
}

static nvs_stub_item_t *find(const char *partition, const char *name_space, const char *key)
{
    for (unsigned i = 0; i < NVS_STUB_ITEMS; i++) {
        nvs_stub_item_t *item = &nvs_stub.items[i];
        if (item->used && !strcmp(item->partition, partition) &&
            !strcmp(item->name_space, name_space) && (!key || !strcmp(item->key, key)))
            return item;
    }
    return NULL;
}

/* A mutation is refused once the simulated power is gone. */
static int power_ok(void)
{
    if (nvs_stub.cut_after >= 0 && (long)nvs_stub.mutations >= nvs_stub.cut_after)
        return 0;
    nvs_stub.mutations++;
    return 1;
}

esp_err_t nvs_flash_init_partition(const char *partition)
{
    (void)partition;
    return ESP_OK;
}

esp_err_t nvs_open_from_partition(const char *partition, const char *name_space, int mode,
                                  nvs_handle_t *handle)
{
    if (mode == NVS_READONLY && !space_exists(partition, name_space))
        return ESP_ERR_NVS_NOT_FOUND;
    if (mode == NVS_READWRITE)
        create_space(partition, name_space);
    for (int i = 0; i < HANDLES; i++) {
        if (handles[i].used)
            continue;
        handles[i].used = 1;
        handles[i].mode = mode;
        strcpy(handles[i].partition, partition);
        strcpy(handles[i].name_space, name_space);
        *handle = i + 1;
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t nvs_open(const char *name_space, int mode, nvs_handle_t *handle)
{
    return nvs_open_from_partition("nvs", name_space, mode, handle);
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle >= 1 && handle <= HANDLES && handles[handle - 1].used);
    handles[handle - 1].used = 0;
}

static int handle_ok(nvs_handle_t handle)
{
    return handle >= 1 && handle <= HANDLES && handles[handle - 1].used;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length)
{
    assert(handle_ok(handle));
    nvs_stub_item_t *item =
        find(handles[handle - 1].partition, handles[handle - 1].name_space, key);
    if (!item)
        return ESP_ERR_NVS_NOT_FOUND;
    if (!value) {
        *length = item->length;
        return ESP_OK;
    }
    assert(*length >= item->length);
    memcpy(value, item->bytes, item->length);
    *length = item->length;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    assert(handle_ok(handle) && handles[handle - 1].mode == NVS_READWRITE &&
           length <= NVS_STUB_BLOB);
    if (!power_ok())
        return ESP_FAIL;
    const char *partition = handles[handle - 1].partition,
               *name_space = handles[handle - 1].name_space;
    nvs_stub_item_t *item = find(partition, name_space, key);
    if (!item) {
        for (unsigned i = 0; i < NVS_STUB_ITEMS && !item; i++)
            if (!nvs_stub.items[i].used)
                item = &nvs_stub.items[i];
        assert(item);
        memset(item, 0, sizeof(*item));
        item->used = 1;
        strcpy(item->partition, partition);
        strcpy(item->name_space, name_space);
        strcpy(item->key, key);
    }
    item->length = length;
    memcpy(item->bytes, value, length);
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    assert(handle_ok(handle) && handles[handle - 1].mode == NVS_READWRITE);
    nvs_stub_item_t *item =
        find(handles[handle - 1].partition, handles[handle - 1].name_space, key);
    if (!item)
        return ESP_ERR_NVS_NOT_FOUND;
    if (!power_ok())
        return ESP_FAIL;
    memset(item, 0, sizeof(*item));
    return ESP_OK;
}

esp_err_t nvs_erase_all(nvs_handle_t handle)
{
    assert(handle_ok(handle) && handles[handle - 1].mode == NVS_READWRITE);
    nvs_stub_item_t *item;
    while ((item = find(handles[handle - 1].partition, handles[handle - 1].name_space, NULL)) !=
           NULL) {
        if (!power_ok())
            return ESP_FAIL;
        memset(item, 0, sizeof(*item));
    }
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle_ok(handle));
    return nvs_stub.cut_after >= 0 && (long)nvs_stub.mutations >= nvs_stub.cut_after ? ESP_FAIL
                                                                                     : ESP_OK;
}

void nvs_stub_put(const char *partition, const char *name_space, const char *key, const void *bytes,
                  size_t length)
{
    nvs_handle_t handle;
    long cut = nvs_stub.cut_after;
    nvs_stub.cut_after = -1;
    assert(nvs_open_from_partition(partition, name_space, NVS_READWRITE, &handle) == ESP_OK);
    assert(nvs_set_blob(handle, key, bytes, length) == ESP_OK);
    nvs_close(handle);
    nvs_stub.cut_after = cut;
}

const nvs_stub_item_t *nvs_stub_find(const char *partition, const char *name_space, const char *key)
{
    return find(partition, name_space, key);
}

unsigned nvs_stub_count(const char *partition, const char *name_space)
{
    unsigned count = 0;
    for (unsigned i = 0; i < NVS_STUB_ITEMS; i++)
        if (nvs_stub.items[i].used && !strcmp(nvs_stub.items[i].partition, partition) &&
            !strcmp(nvs_stub.items[i].name_space, name_space))
            count++;
    return count;
}
