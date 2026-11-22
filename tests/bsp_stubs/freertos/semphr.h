#pragma once
#include "freertos/FreeRTOS.h"

typedef struct {
    int locked;
} StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    if (storage)
        storage->locked = 0;
    return storage;
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks)
{
    (void)ticks;
    if (!handle || handle->locked)
        return pdFALSE;
    handle->locked = 1;
    return pdTRUE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t handle)
{
    if (!handle || !handle->locked)
        return pdFALSE;
    handle->locked = 0;
    return pdTRUE;
}
