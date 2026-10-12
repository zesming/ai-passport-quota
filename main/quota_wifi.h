#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_wifi.h"

typedef enum {
    QUOTA_WIFI_WAIT,
    QUOTA_WIFI_READY,
    QUOTA_WIFI_FAILED,
} quota_wifi_result_t;

/* Prepare one serialized Wi-Fi operation. READY means initialized/configured but not started. */
quota_wifi_result_t quota_wifi_prepare(uint32_t request_id, wifi_mode_t mode);
/* Idempotently begin and advance driver retirement. */
void quota_wifi_retire_begin(void);
quota_wifi_result_t quota_wifi_retire_poll(void);
uint64_t quota_wifi_next_deadline_ms(bool sleeping);

/* Start only a mode/configuration admitted by quota_wifi_prepare. */
bool quota_wifi_start(void);
/* Stop and request retirement. True means the radio stopped, not that event fences completed. */
bool quota_wifi_stop(void);
bool quota_wifi_started(void);
/* True only while this request owns a started driver in the admitted mode. */
bool quota_wifi_operation_active(uint32_t request_id, wifi_mode_t mode);
bool quota_wifi_scan_start(const wifi_scan_config_t *config);
esp_err_t quota_wifi_scan_get_ap_num(uint16_t *number);
esp_err_t quota_wifi_scan_get_ap_record(wifi_ap_record_t *record);
esp_err_t quota_wifi_scan_clear_ap_list(void);
