#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef int gpio_num_t;
typedef enum {
    GPIO_PULLUP_ONLY,
    GPIO_PULLDOWN_ONLY,
    GPIO_PULLUP_PULLDOWN,
    GPIO_FLOATING
} gpio_pull_mode_t;
esp_err_t gpio_sleep_set_pull_mode(gpio_num_t, gpio_pull_mode_t);
