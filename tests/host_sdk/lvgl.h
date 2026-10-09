#pragma once
/* Only the LVGL memory monitor, which the resource-log build of main/main.c reads. The real UI
 * runs against the real LVGL in tools/ui_preview/. */
#include <stdint.h>

typedef struct {
    uint32_t total_size;
    uint32_t free_cnt;
    uint32_t free_size;
    uint32_t free_biggest_size;
    uint32_t used_cnt;
    uint32_t max_used;
    uint8_t used_pct;
    uint8_t frag_pct;
} lv_mem_monitor_t;

void lv_mem_monitor(lv_mem_monitor_t *monitor);
