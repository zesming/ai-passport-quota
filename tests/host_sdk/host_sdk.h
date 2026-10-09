#pragma once
/* One-file host replacement for the ESP-IDF and FreeRTOS APIs used by the firmware sources. They
 * compile unmodified against it. host_sdk_defaults.c supplies weak default definitions; a test
 * defines its own strong version of any function it needs to observe or steer. */
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <stdint.h>

/* Simulated clock for esp_timer_get_time(). */
extern int64_t host_time_us;

/* FreeRTOS. */
typedef uint32_t TickType_t;
typedef unsigned UBaseType_t;
typedef int BaseType_t;
typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef struct {
    int unused;
} portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
QueueHandle_t xQueueCreate(unsigned depth, unsigned item_size);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
BaseType_t xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *argument,
                       unsigned priority, TaskHandle_t *handle);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);

/* esp_event, esp_netif, esp_wifi. */
typedef const char *esp_event_base_t;
typedef void *esp_event_handler_instance_t;
typedef void (*esp_event_handler_t)(void *argument, esp_event_base_t base, int32_t id, void *data);
extern const char *const WIFI_EVENT;
extern const char *const IP_EVENT;
enum { WIFI_EVENT_STA_DISCONNECTED = 5, IP_EVENT_STA_GOT_IP = 0 };
esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
                                              esp_event_handler_t handler, void *argument,
                                              esp_event_handler_instance_t *instance);
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, int32_t id,
                                                esp_event_handler_instance_t instance);
typedef struct {
    int unused;
} esp_netif_t;
typedef struct {
    int unused;
} esp_netif_config_t;
#define ESP_NETIF_DEFAULT_WIFI_STA() {0}
esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_new(const esp_netif_config_t *config);
esp_err_t esp_netif_attach_wifi_station(esp_netif_t *netif);
void esp_netif_destroy_default_wifi(void *netif);
typedef struct {
    int unused;
} wifi_init_config_t;
typedef struct {
    unsigned char ssid[33];
} wifi_ap_record_t;
typedef struct {
    uint8_t reason;
} wifi_event_sta_disconnected_t;
#define WIFI_INIT_CONFIG_DEFAULT() {0}
enum { WIFI_STORAGE_RAM = 1, WIFI_MODE_STA = 1 };
esp_err_t esp_wifi_set_default_wifi_sta_handlers(void);
esp_err_t esp_wifi_init(const wifi_init_config_t *config);
esp_err_t esp_wifi_deinit(void);
esp_err_t esp_wifi_set_storage(int storage);
esp_err_t esp_wifi_set_mode(int mode);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *record);

typedef void *i2c_master_bus_handle_t;

/* esp_pm, esp_random, USB serial console, nvs, I2C. */
typedef void *esp_pm_lock_handle_t;
typedef struct {
    int max_freq_mhz, min_freq_mhz;
    bool light_sleep_enable;
} esp_pm_config_t;
#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 160
enum { ESP_PM_CPU_FREQ_MAX = 1 };
esp_err_t esp_pm_lock_create(int type, int argument, const char *name, esp_pm_lock_handle_t *lock);
esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t lock);
esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t lock);
esp_err_t esp_pm_lock_delete(esp_pm_lock_handle_t lock);
esp_err_t esp_pm_configure(const void *config);
uint32_t esp_random(void);
void usb_serial_jtag_vfs_use_nonblocking(void);
esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_deinit_partition(const char *partition);
esp_err_t nvs_flash_erase_partition(const char *partition);
esp_err_t nvs_flash_init_partition(const char *partition);
void esp_restart(void);

/* esp_timer, esp_system. */
typedef void *esp_timer_handle_t;
typedef struct {
    void (*callback)(void *argument);
    void *arg;
    const char *name;
} esp_timer_create_args_t;
int64_t esp_timer_get_time(void);
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *timer);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us);
uint32_t esp_get_free_heap_size(void);
uint32_t esp_get_minimum_free_heap_size(void);
