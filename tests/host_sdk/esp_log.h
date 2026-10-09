#pragma once
#include <stdio.h>

/* Formats into a scratch buffer, so the arguments are evaluated and the format is checked like the
 * real macros do, without printing. */
extern char host_log_line[512];
#define ESP_LOG_STUB(tag, ...)                                                                     \
    do {                                                                                           \
        (void)(tag);                                                                               \
        (void)snprintf(host_log_line, sizeof(host_log_line), __VA_ARGS__);                         \
    } while (0)
#define ESP_LOGE(tag, ...) ESP_LOG_STUB(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) ESP_LOG_STUB(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) ESP_LOG_STUB(tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) ESP_LOG_STUB(tag, __VA_ARGS__)
