#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef enum { LEDC_LOW_SPEED_MODE = 0 } ledc_mode_t;
typedef enum { LEDC_TIMER_0 = 0 } ledc_timer_t;
typedef enum { LEDC_CHANNEL_0 = 0 } ledc_channel_t;
typedef enum { LEDC_TIMER_10_BIT = 10 } ledc_timer_bit_t;
esp_err_t ledc_stop(ledc_mode_t, ledc_channel_t, uint32_t idle_level);
