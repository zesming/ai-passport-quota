#pragma once

#include "lvgl.h"

/* A8 images of the status bar Wi-Fi glyph, made by tools/gen_wifi_icons.py. quota_wifi_lit[n - 1]
 * is the dot and the first n arcs; quota_wifi_dim[n] is what stays unlit at signal n (0: all). */
extern const lv_image_dsc_t quota_wifi_lit[3];
extern const lv_image_dsc_t quota_wifi_dim[4];
extern const lv_image_dsc_t quota_wifi_slash;
