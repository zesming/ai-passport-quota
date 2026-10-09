// components/bsp/src/bsp_display_pins.h
#pragma once

#include "esp_err.h"
#include <stdbool.h>

// 背光脚睡眠下拉（backlight_pwm 为真时先 ledc_stop，空闲低电平），LCD CS 睡眠上拉。
esp_err_t bsp_display_sleep_pins_set(bool backlight_pwm);
// 把两个脚的睡眠配置恢复为默认的浮空（PM_SLP_DISABLE_GPIO 的隔离配置）。
esp_err_t bsp_display_sleep_pins_restore(void);
