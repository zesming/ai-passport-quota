#pragma once
#include "freertos/FreeRTOS.h"

#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
void vTaskDelay(TickType_t ticks);
