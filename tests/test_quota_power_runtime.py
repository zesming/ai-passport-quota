"""Execute power transitions and sleeping workers without hardware or transport."""
import unittest
from runtime_helpers import ROOT, extract_function, compile_and_run


class PowerRuntime(unittest.TestCase):
    def test_display_retry_cpu_lock_and_application_wait(self):
        source = (ROOT / "main/main.c").read_text()
        functions = "\n".join(extract_function(source, name) for name in (
            "configure_cpu_power_management", "set_display_power",
            "render_application", "application_task"))
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
typedef int esp_err_t;
typedef void *esp_pm_lock_handle_t;
typedef struct { int max_freq_mhz, min_freq_mhz; bool light_sleep_enable; } esp_pm_config_t;
typedef unsigned TickType_t;
typedef void *QueueHandle_t;
typedef struct { int unused; } quota_app_event_t;
#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 160
#define ESP_PM_CPU_FREQ_MAX 1
#define ESP_OK 0
#define ESP_LOGW(...) ((void)0)
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY UINT32_MAX
static esp_pm_lock_handle_t s_cpu_lock;
static bool s_cpu_lock_held, s_display_power_sleeping, s_display_power_pending;
static uint8_t s_backlight_percent = 100;
static struct { bool sleeping; } s_display;
static struct { unsigned screen_timeout_seconds; bool pairing_active; } s_view_work;
static int s_navigation, s_battery_percent;
static uint64_t s_battery_read_ms;
static int cpu_depth, acquires, releases, power_calls, renders, brightness = 100;
static bool fail_power, fail_refresh, fail_release, sleep_gate;
static unsigned wait_expected, queue_calls;
static jmp_buf done;
esp_err_t esp_pm_lock_create(int type, int arg, const char *name, void **lock) {
    (void)arg; (void)name; assert(type == ESP_PM_CPU_FREQ_MAX); *lock = (void *)1; return ESP_OK;
}
esp_err_t esp_pm_configure(const esp_pm_config_t *cfg) {
    assert(cfg->max_freq_mhz == 160 && cfg->min_freq_mhz == 40 && !cfg->light_sleep_enable);
    return ESP_OK;
}
esp_err_t esp_pm_lock_acquire(void *lock) {
    assert(lock); ++acquires; assert(++cpu_depth == 1); return ESP_OK;
}
esp_err_t esp_pm_lock_release(void *lock) {
    assert(lock); ++releases; if (fail_release) return 1; assert(--cpu_depth == 0); return ESP_OK;
}
esp_err_t esp_pm_lock_delete(void *lock) { (void)lock; return ESP_OK; }
void bsp_display_backlight(uint8_t level) { brightness = level; }
bool bsp_lvgl_set_sleeping(bool sleeping) {
    (void)sleeping; assert(brightness == 0); ++power_calls; return !fail_power;
}
bool bsp_lvgl_refresh(void) { assert(renders && brightness == 0); return !fail_refresh; }
void quota_service_get_view(void *view) { (void)view; }
int64_t esp_timer_get_time(void) { return 1000000; }
void quota_display_tick(void *display, uint64_t now, unsigned timeout, bool pairing) {
    (void)display; (void)now; (void)timeout; (void)pairing;
}
void quota_service_set_display_sleeping(bool sleeping) { sleep_gate = sleeping; }
int bsp_battery_soc(void) { return 50; }
bool bsp_lvgl_lock(int timeout) { (void)timeout; return true; }
void bsp_lvgl_unlock(void) {}
void quota_ui_render(void *nav, void *view, int battery) {
    (void)nav; (void)view; (void)battery; ++renders;
}
QueueHandle_t quota_service_event_queue(void) { return (void *)1; }
int xQueueReceive(QueueHandle_t queue, quota_app_event_t *event, TickType_t wait) {
    (void)queue; (void)event; assert(wait == wait_expected);
    if (++queue_calls == 2) longjmp(done, 1);
    return pdTRUE;
}
void process_event(const quota_app_event_t *event) { (void)event; }
'''
        harness += functions
        harness += r'''
void check_wait(unsigned expected) {
    wait_expected = expected; queue_calls = 0;
    if (!setjmp(done)) application_task(NULL);
}
int main(void) {
    configure_cpu_power_management();
    assert(s_cpu_lock_held && cpu_depth == 1);
    s_display.sleeping = true; fail_power = true;
    render_application();
    assert(sleep_gate && brightness == 0 && renders == 0);
    assert(s_display_power_pending && !s_cpu_lock_held && cpu_depth == 0);
    check_wait(1000); /* Failed transitions retry instead of becoming permanent. */
    render_application(); assert(releases == 1);
    fail_power = false; render_application();
    assert(!s_display_power_pending && s_display_power_sleeping);
    check_wait(portMAX_DELAY);
    int calls = power_calls; render_application(); assert(power_calls == calls);
    s_display.sleeping = false; fail_power = true; render_application();
    assert(!sleep_gate && s_cpu_lock_held && brightness == 0 && renders == 0);
    render_application(); assert(acquires == 2 && cpu_depth == 1);
    fail_power = false; fail_refresh = true; render_application();
    assert(!s_display_power_pending && !s_display_power_sleeping && brightness == 0);
    fail_refresh = false; render_application(); assert(brightness == 100 && renders == 2);
    check_wait(1000);
    s_display.sleeping = true; fail_release = true; render_application();
    assert(s_display_power_pending && s_cpu_lock_held);
    fail_release = false; render_application();
    assert(!s_display_power_pending && !s_cpu_lock_held && cpu_depth == 0);
    check_wait(portMAX_DELAY);
    puts("display power and application wait tests passed");
}
'''
        compile_and_run(harness, "ai-quota-display-power-")

    def test_wifi_stop_restart_and_late_events(self):
        source = (ROOT / "main/quota_service.c").read_text()
        functions = "\n".join(extract_function(source, name) for name in (
            "wifi_event_handler", "ip_event_handler", "init_wifi", "stop_wifi_for_sleep"))
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef int esp_err_t;
typedef int esp_event_base_t;
#define ESP_OK 0
#define WIFI_EVENT 1
#define IP_EVENT 2
#define WIFI_EVENT_STA_DISCONNECTED 3
#define IP_EVENT_STA_GOT_IP 4
#define WIFI_STORAGE_RAM 1
#define WIFI_MODE_STA 1
#define WIFI_RETRY_MIN_MS 2000
#define WIFI_RETRY_MAX_MS 30000
#define QUOTA_APP_EVENT_CONNECTION 1
#define ESP_LOGW(...) ((void)0)
static bool s_wifi_started, s_handlers_registered, s_wifi_retry_pending, s_fetch_after_connect;
static int s_wifi_handler, s_ip_handler;
static int64_t s_wifi_retry_at_ms;
static uint32_t s_wifi_retry_delay_ms = WIFI_RETRY_MIN_MS;
static struct { bool connected; } s_view;
static void *s_network_task = (void *)1;
static bool sleeping, fail_start, fail_stop;
static bool associated;
typedef struct { int unused; } wifi_ap_record_t;
static esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap) {
    (void)ap; return associated ? ESP_OK : 1;
}
static unsigned registrations, starts, stops, settings, events, notifications;
static uint64_t now_ms;
static bool prepare_network_stack(void) { return true; }
static void mutex_lock(void) {}
static void mutex_unlock(void) {}
static uint64_t monotonic_ms(void) { return now_ms; }
typedef struct { bool sleeping; } display_state_t;
static display_state_t display_state_snapshot(void) {
    return (display_state_t){.sleeping = sleeping};
}
static void post_simple_event(int kind) { (void)kind; ++events; }
static void xTaskNotifyGive(void *task) { (void)task; ++notifications; }
static esp_err_t esp_event_handler_instance_register(int base, int event,
    void (*handler)(void *, int, int32_t, void *), void *arg, int *instance) {
    (void)base; (void)event; (void)handler; (void)arg; (void)instance; ++registrations; return ESP_OK;
}
static esp_err_t esp_event_handler_instance_unregister(int base, int event, int instance) {
    (void)base; (void)event; (void)instance; return ESP_OK;
}
static esp_err_t esp_wifi_set_storage(int value) { (void)value; ++settings; return ESP_OK; }
static esp_err_t esp_wifi_set_mode(int value) { (void)value; ++settings; return ESP_OK; }
static esp_err_t esp_wifi_start(void) { ++starts; return fail_start ? 1 : ESP_OK; }
static esp_err_t esp_wifi_stop(void) {
    ++stops; if (fail_stop) return 1; associated = false; return ESP_OK;
}
'''
        harness += functions
        harness += r'''
int main(void) {
    assert(init_wifi() && s_wifi_started && s_handlers_registered);
    assert(registrations == 2 && settings == 2 && starts == 1 && s_wifi_retry_pending);
    associated = true;
    ip_event_handler(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(s_view.connected && s_fetch_after_connect && !s_wifi_retry_pending);
    sleeping = true;
    fail_stop = true; stop_wifi_for_sleep(); assert(s_wifi_started && s_view.connected);
    associated = false;
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    assert(!s_view.connected && s_wifi_retry_pending);
    sleeping = false; assert(init_wifi() && s_wifi_retry_pending && starts == 1);
    /* Failed stop plus asleep disconnect must retain reconnect work on wake. */
    sleeping = true;
    fail_stop = false; stop_wifi_for_sleep();
    assert(!s_wifi_started && !s_view.connected && !s_fetch_after_connect && !s_wifi_retry_pending);
    unsigned before = events;
    ip_event_handler(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    assert(!s_view.connected && !s_wifi_retry_pending && events == before);
    stop_wifi_for_sleep(); assert(stops == 2); /* No repeated stop after success. */
    sleeping = false;
    ip_event_handler(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(!s_view.connected); /* A queued old IP event cannot revive a stopped radio. */
    now_ms = 70000; assert(init_wifi());
    assert(starts == 2 && registrations == 2 && settings == 2 && s_wifi_retry_at_ms == 70000);
    before = events;
    ip_event_handler(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(!s_view.connected && s_wifi_retry_pending && events == before);
    /* The late old IP event after restart cannot cancel the next connect. */
    associated = true;
    ip_event_handler(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(s_view.connected && s_fetch_after_connect);
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    assert(!s_view.connected && s_wifi_retry_pending && s_wifi_retry_at_ms == 72000);
    sleeping = true; stop_wifi_for_sleep();
    sleeping = false; fail_start = true; assert(!init_wifi());
    assert(!s_wifi_started && !s_handlers_registered);
    fail_start = false; assert(init_wifi() && s_wifi_started);
    assert(registrations == 4 && settings == 4);
    puts("Wi-Fi sleep lifecycle tests passed");
}
'''
        compile_and_run(harness, "ai-quota-wifi-power-")

    def test_serial_blocks_asleep_and_discards_partial_frame(self):
        source = (ROOT / "main/quota_service.c").read_text()
        harness = r'''
#include "quota_logic.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY UINT32_MAX
typedef struct { bool sleeping; } display_state_t;
static quota_frame_decoder_t s_frame_decoder;
static bool sleeping;
static unsigned reads, waits, frames;
static jmp_buf done;
static display_state_t display_state_snapshot(void) { return (display_state_t){sleeping}; }
static int fake_fgetc(FILE *stream) {
    (void)stream; ++reads;
    if (reads == 1) return 'x';
    if (reads == 2) { sleeping = true; return 'y'; }
    if (reads == 3) return 'a';
    if (reads == 4) return '\n';
    longjmp(done, 1);
}
#define fgetc fake_fgetc
static unsigned ulTaskNotifyTake(unsigned clear, unsigned ticks) {
    assert(clear == pdTRUE && ticks == portMAX_DELAY);
    ++waits; assert(reads == 2);
    if (waits == 3) sleeping = false;
    return 1;
}
static void vTaskDelay(unsigned ticks) { (void)ticks; assert(false); }
static void handle_serial_frame(const char *frame, size_t size) {
    assert(size == 1 && memcmp(frame, "a", 1) == 0); ++frames;
}
static bool pairing_active(void) { return false; }
static void send_pairing_result(const char *id, bool success, const char *error) {
    (void)id; (void)success; (void)error; assert(false);
}
'''
        harness += extract_function(source, "serial_task")
        harness += r'''
int main(void) {
    if (!setjmp(done)) serial_task(NULL);
    assert(waits == 3 && reads == 5 && frames == 1);
    puts("serial sleep and frame reset tests passed");
}
'''
        compile_and_run(harness, "ai-quota-serial-power-", (
            "main/quota_logic.c", "tests/cjson/cJSON.c"))


if __name__ == "__main__":
    unittest.main()
