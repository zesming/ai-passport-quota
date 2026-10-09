#include "host_sdk.h"

#define WEAK __attribute__((weak))

int64_t host_time_us;
char host_log_line[512];
const char *const WIFI_EVENT = "WIFI_EVENT";
const char *const IP_EVENT = "IP_EVENT";

WEAK int64_t esp_timer_get_time(void)
{
    return host_time_us;
}
WEAK uint32_t esp_random(void)
{
    return 4;
}
WEAK UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    (void)task;
    return 4096;
}
WEAK QueueHandle_t xQueueCreate(unsigned depth, unsigned item_size)
{
    (void)depth;
    (void)item_size;
    return (void *)1;
}
WEAK BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait)
{
    (void)queue;
    (void)item;
    (void)wait;
    return pdTRUE;
}
WEAK BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait)
{
    (void)queue;
    (void)item;
    (void)wait;
    return pdFALSE;
}
WEAK SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return (void *)1;
}
WEAK BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait)
{
    (void)semaphore;
    (void)wait;
    return pdTRUE;
}
WEAK BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    (void)semaphore;
    return pdTRUE;
}
WEAK BaseType_t xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *argument,
                            unsigned priority, TaskHandle_t *handle)
{
    (void)task;
    (void)name;
    (void)stack;
    (void)argument;
    (void)priority;
    *handle = (void *)1;
    return pdPASS;
}
WEAK BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
    (void)task;
    return pdTRUE;
}
WEAK uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    (void)clear;
    (void)wait;
    return 0;
}
WEAK esp_err_t esp_event_loop_create_default(void)
{
    return ESP_OK;
}
WEAK esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
                                                   esp_event_handler_t handler, void *argument,
                                                   esp_event_handler_instance_t *instance)
{
    (void)base;
    (void)id;
    (void)handler;
    (void)argument;
    (void)instance;
    return ESP_OK;
}
WEAK esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, int32_t id,
                                                     esp_event_handler_instance_t instance)
{
    (void)base;
    (void)id;
    (void)instance;
    return ESP_OK;
}
WEAK esp_err_t esp_netif_init(void)
{
    return ESP_OK;
}
WEAK esp_netif_t *esp_netif_new(const esp_netif_config_t *config)
{
    static esp_netif_t netif;
    (void)config;
    return &netif;
}
WEAK esp_err_t esp_netif_attach_wifi_station(esp_netif_t *netif)
{
    (void)netif;
    return ESP_OK;
}
WEAK void esp_netif_destroy_default_wifi(void *netif)
{
    (void)netif;
}
WEAK esp_err_t esp_wifi_set_default_wifi_sta_handlers(void)
{
    return ESP_OK;
}
WEAK esp_err_t esp_wifi_init(const wifi_init_config_t *config)
{
    (void)config;
    return ESP_OK;
}
WEAK esp_err_t esp_wifi_deinit(void)
{
    return ESP_OK;
}
WEAK esp_err_t esp_wifi_set_storage(int storage)
{
    (void)storage;
    return ESP_OK;
}
WEAK esp_err_t esp_wifi_set_mode(int mode)
{
    (void)mode;
    return ESP_OK;
}
WEAK esp_err_t esp_wifi_start(void)
{
    return ESP_OK;
}
WEAK esp_err_t esp_wifi_stop(void)
{
    return ESP_OK;
}
WEAK esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *record)
{
    (void)record;
    return ESP_OK;
}
WEAK esp_err_t esp_pm_lock_create(int type, int argument, const char *name,
                                  esp_pm_lock_handle_t *lock)
{
    (void)type;
    (void)argument;
    (void)name;
    *lock = (void *)1;
    return ESP_OK;
}
WEAK esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t lock)
{
    (void)lock;
    return ESP_OK;
}
WEAK esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t lock)
{
    (void)lock;
    return ESP_OK;
}
WEAK esp_err_t esp_pm_lock_delete(esp_pm_lock_handle_t lock)
{
    (void)lock;
    return ESP_OK;
}
WEAK esp_err_t esp_pm_configure(const void *config)
{
    (void)config;
    return ESP_OK;
}
WEAK void usb_serial_jtag_vfs_use_nonblocking(void) {}
WEAK esp_err_t nvs_flash_init(void)
{
    return ESP_OK;
}
WEAK esp_err_t nvs_flash_deinit_partition(const char *partition)
{
    (void)partition;
    return ESP_OK;
}
WEAK esp_err_t nvs_flash_erase_partition(const char *partition)
{
    (void)partition;
    return ESP_OK;
}
WEAK esp_err_t nvs_flash_init_partition(const char *partition)
{
    (void)partition;
    return ESP_OK;
}
WEAK void esp_restart(void) {}

#include "esp_http_server.h"

WEAK esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config)
{
    (void)config;
    *handle = (void *)1;
    return ESP_OK;
}
WEAK esp_err_t httpd_stop(httpd_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}
WEAK esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri)
{
    (void)handle;
    (void)uri;
    return ESP_OK;
}
WEAK esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status)
{
    (void)request;
    (void)status;
    return ESP_OK;
}
WEAK esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type)
{
    (void)request;
    (void)type;
    return ESP_OK;
}
WEAK esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *field, const char *value)
{
    (void)request;
    (void)field;
    (void)value;
    return ESP_OK;
}
WEAK esp_err_t httpd_resp_send(httpd_req_t *request, const char *body, ssize_t length)
{
    (void)request;
    (void)body;
    (void)length;
    return ESP_OK;
}
WEAK size_t httpd_req_get_hdr_value_len(httpd_req_t *request, const char *field)
{
    (void)request;
    (void)field;
    return 0;
}
WEAK esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *request, const char *field, char *value,
                                           size_t size)
{
    (void)request;
    (void)field;
    (void)value;
    (void)size;
    return ESP_FAIL;
}
WEAK int httpd_req_recv(httpd_req_t *request, char *buffer, size_t length)
{
    (void)request;
    (void)buffer;
    (void)length;
    return 0;
}
WEAK int httpd_req_to_sockfd(httpd_req_t *request)
{
    (void)request;
    return 1;
}

WEAK esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *timer)
{
    (void)args;
    *timer = (void *)1;
    return ESP_OK;
}
WEAK esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us)
{
    (void)timer;
    (void)period_us;
    return ESP_OK;
}
WEAK uint32_t esp_get_free_heap_size(void)
{
    return 100000;
}
WEAK uint32_t esp_get_minimum_free_heap_size(void)
{
    return 50000;
}
