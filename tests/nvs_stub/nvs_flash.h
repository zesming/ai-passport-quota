#pragma once
#include "nvs.h"
esp_err_t nvs_flash_init_partition(const char *partition);
esp_err_t nvs_flash_deinit_partition(const char *partition);
/* Drops every namespace and blob of the partition. */
esp_err_t nvs_flash_erase_partition(const char *partition);
extern unsigned nvs_stub_partition_erases;
extern int nvs_stub_fail_partition_erase; /* the partition erase fails, other writes do not */
