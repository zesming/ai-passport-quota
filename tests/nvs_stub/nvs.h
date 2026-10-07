#pragma once
/* In-memory NVS for host tests: partitions, namespaces and blobs, plus a power-cut switch. */
#include <stddef.h>
#include <stdint.h>

typedef int nvs_handle_t;
typedef int esp_err_t;
enum {
    ESP_OK = 0, ESP_FAIL = 1, ESP_ERR_NVS_NOT_FOUND = 2, ESP_ERR_NO_MEM = 3,
    ESP_ERR_NVS_TYPE_MISMATCH = 4, ESP_ERR_NVS_PART_NOT_FOUND = 5,
    NVS_READONLY = 0, NVS_READWRITE = 1,
};

esp_err_t nvs_open(const char *name_space, int mode, nvs_handle_t *handle);
esp_err_t nvs_open_from_partition(const char *partition, const char *name_space, int mode,
                                  nvs_handle_t *handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);
esp_err_t nvs_erase_all(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);

#define NVS_STUB_ITEMS 24
#define NVS_STUB_BLOB 16384
typedef struct {
    int used;
    char partition[16], name_space[16], key[16];
    size_t length;
    unsigned char bytes[NVS_STUB_BLOB];
} nvs_stub_item_t;
typedef struct {
    nvs_stub_item_t items[NVS_STUB_ITEMS];
    char spaces[8][2][16]; /* partition, namespace pairs that exist */
    unsigned space_count;
    unsigned mutations;   /* applied set/erase operations */
    long cut_after;       /* >= 0: operations beyond this many fail, as after a power cut */
} nvs_stub_state_t;

extern nvs_stub_state_t nvs_stub;
void nvs_stub_put(const char *partition, const char *name_space, const char *key,
                  const void *bytes, size_t length);
const nvs_stub_item_t *nvs_stub_find(const char *partition, const char *name_space, const char *key);
unsigned nvs_stub_count(const char *partition, const char *name_space);
