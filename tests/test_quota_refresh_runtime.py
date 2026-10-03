"""Execute the firmware refresh paths and network worker with fake dependencies."""
import re
import unittest
from runtime_helpers import ROOT, extract_function, compile_and_run


class RefreshRuntime(unittest.TestCase):
    def test_refresh_generation_gate_and_display_sleep_api(self):
        source = (ROOT / "main/quota_service.c").read_text()
        names = ("display_generation_is_current", "config_generation_is_current",
                 "operation_is_current", "network_operation_is_current",
                 "finish_wake_fetch",
                 "clear_stale_refreshing", "begin_refresh", "finish_refresh",
                 "perform_refresh", "perform_snapshot_fetch", "restore_pending_settings",
                 "perform_settings_update")
        functions = [extract_function(source, name) for name in names]
        # Inject a link change at the outer GET gate, before the transport is called.
        index = names.index("network_operation_is_current")
        functions[index] = functions[index].replace(
            "network_operation_is_current(", "network_operation_is_current_actual(", 1)
        functions[index] += r'''
static bool network_operation_is_current(uint32_t config_generation,
                                         uint32_t display_generation) {
    if (defer_wake_get_gate && s_display_scheduler.wake_fetch_pending) {
        defer_wake_get_gate = false;
        s_view.connected = false;
        bool admitted = network_operation_is_current_actual(config_generation, display_generation);
        s_view.connected = true;
        return admitted;
    }
    return network_operation_is_current_actual(config_generation, display_generation);
}
'''
        functions.append(extract_function(source, "quota_service_set_display_sleeping",
                                          "void"))
        harness = r'''
#include "quota_logic.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    quota_snapshot_t snapshot;
    bool snapshot_valid, refreshing, request_failed, connected;
    uint16_t refresh_seconds;
    bool auto_refresh;
    uint16_t screen_timeout_seconds;
    uint64_t now_epoch;
} view_t;
typedef struct {
    bool sleeping, wake_fetch_pending;
    uint32_t generation;
} display_scheduler_t;
typedef int portMUX_TYPE;
typedef int http_request_outcome_t;
enum { HTTP_REQUEST_NOT_ADMITTED, HTTP_REQUEST_ADMITTED, HTTP_REQUEST_DEFERRED };
enum { QUOTA_APP_EVENT_SNAPSHOT, QUOTA_APP_EVENT_SETTINGS_RESULT };
typedef struct {
    int kind;
    bool success;
    uint16_t refresh_seconds;
    bool auto_refresh;
    uint16_t screen_timeout_seconds;
} quota_app_event_t;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
view_t s_view;
display_scheduler_t s_display_scheduler;
portMUX_TYPE s_display_state_mux;
uint32_t s_refreshing_display_generation;
bool s_has_config;
uint32_t s_config_generation;
void *s_mutex = (void *)1;
void *s_network_task = (void *)1;
quota_snapshot_t s_snapshot_work;
bool transport_ok, refresh_accepted, expect_busy;
bool sleep_on_post, sleep_wake_on_post, sleep_on_get, lose_link_on_post;
bool defer_get_fast_reconnect, defer_wake_get_gate;
bool defer_post_fast_reconnect, local_post_setup_failure;
bool s_settings_pending, s_pending_auto_refresh;
uint16_t s_pending_refresh_seconds, s_pending_screen_timeout_seconds;
quota_device_config_t s_config;
int settings_behavior, settings_events, nvs_saves;
unsigned fetches, posts, events, notifications;
static bool operation_is_current(uint32_t config_generation,
                                 uint32_t display_generation);
void quota_service_set_display_sleeping(bool sleeping);
void mutex_lock(void) {}
void mutex_unlock(void) {}
int xSemaphoreTake(void *mutex, unsigned ticks) {
    (void)mutex; (void)ticks; return pdTRUE;
}
int xSemaphoreGive(void *mutex) { (void)mutex; return pdTRUE; }
void xTaskNotifyGive(void *task) { (void)task; notifications++; }
uint64_t current_epoch(void) { return 1700000000; }
void post_simple_event(int kind) { (void)kind; events++; }
void post_event(const quota_app_event_t *event, unsigned ticks) {
    (void)event; (void)ticks; settings_events++;
}
bool nvs_save_screen_timeout_locked(uint16_t seconds) { (void)seconds; nvs_saves++; return true; }
bool nvs_save_config_locked(const quota_device_config_t *config) {
    (void)config; nvs_saves++; return true;
}
void vTaskDelay(unsigned ticks) { (void)ticks; }
bool publish_snapshot(const quota_snapshot_t *snapshot, uint32_t config_generation,
                      uint32_t display_generation) {
    if (!operation_is_current(config_generation, display_generation)) return false;
    s_view.snapshot = *snapshot;
    s_view.snapshot_valid = true;
    s_view.refresh_seconds = snapshot->refresh_seconds;
    s_view.auto_refresh = snapshot->auto_refresh;
    s_view.request_failed = false;
    post_simple_event(QUOTA_APP_EVENT_SNAPSHOT);
    return true;
}
bool fetch_snapshot(const quota_device_config_t *config, quota_snapshot_t *snapshot,
                    uint32_t config_generation, uint32_t display_generation,
                    http_request_outcome_t *outcome) {
    (void)config; (void)config_generation; (void)display_generation;
    assert(s_view.refreshing == expect_busy);
    fetches++;
    if (defer_get_fast_reconnect) {
        defer_get_fast_reconnect = false;
        s_view.connected = false;
        s_view.connected = true;
        if (outcome != NULL) *outcome = HTTP_REQUEST_DEFERRED;
        return false;
    }
    if (outcome != NULL) *outcome = HTTP_REQUEST_ADMITTED;
    if (sleep_on_get) {
        sleep_on_get = false;
        quota_service_set_display_sleeping(true);
    }
    snapshot->revision = 2;
    snapshot->refresh_seconds = s_view.refresh_seconds;
    snapshot->auto_refresh = s_view.auto_refresh;
    return transport_ok;
}
bool request_refresh(const quota_device_config_t *config, uint32_t config_generation,
                     uint32_t display_generation, http_request_outcome_t *outcome) {
    (void)config; (void)config_generation; (void)display_generation;
    assert(s_view.refreshing);
    if (defer_post_fast_reconnect || local_post_setup_failure) {
        if (outcome != NULL) *outcome = defer_post_fast_reconnect
            ? HTTP_REQUEST_DEFERRED : HTTP_REQUEST_NOT_ADMITTED;
        defer_post_fast_reconnect = false;
        return false;
    }
    posts++;
    if (outcome != NULL) *outcome = HTTP_REQUEST_ADMITTED;
    if (lose_link_on_post) {
        lose_link_on_post = false;
        s_view.connected = false;
        return false;
    }
    if (sleep_on_post) {
        sleep_on_post = false;
        quota_service_set_display_sleeping(true);
    } else if (sleep_wake_on_post) {
        sleep_wake_on_post = false;
        quota_service_set_display_sleeping(true);
        quota_service_set_display_sleeping(false);
    }
    return refresh_accepted;
}
bool apply_settings(const quota_device_config_t *config, uint16_t seconds, bool automatic,
                   uint16_t timeout, quota_settings_t *applied, uint32_t config_generation,
                   uint32_t display_generation, http_request_outcome_t *outcome) {
    (void)config; (void)config_generation; (void)display_generation;
    if (settings_behavior == 1 || settings_behavior == 4) {
        s_view.connected = false;
        s_view.connected = true;
        if (settings_behavior == 4) {
            s_settings_pending = true;
            s_pending_refresh_seconds = 900;
            s_pending_auto_refresh = false;
            s_pending_screen_timeout_seconds = 120;
        }
        if (outcome != NULL) *outcome = HTTP_REQUEST_DEFERRED;
        return false;
    }
    if (settings_behavior == 2) {
        quota_service_set_display_sleeping(true);
        quota_service_set_display_sleeping(false);
    }
    if (settings_behavior == 3) {
        s_settings_pending = true;
        s_pending_refresh_seconds = 900;
        s_pending_auto_refresh = false;
        s_pending_screen_timeout_seconds = 120;
    }
    if (outcome != NULL) *outcome = HTTP_REQUEST_ADMITTED;
    applied->refresh_seconds = seconds;
    applied->auto_refresh = automatic;
    applied->has_screen_timeout_seconds = true;
    applied->screen_timeout_seconds = timeout;
    return true;
}
'''
        harness += "\n".join(functions)
        harness += r'''
void reset_state(void) {
    memset(&s_view, 0, sizeof(s_view));
    memset(&s_display_scheduler, 0, sizeof(s_display_scheduler));
    s_display_scheduler.generation = 1;
    s_refreshing_display_generation = 0;
    s_has_config = true; s_config_generation = 1; s_view.connected = true;
    s_view.refresh_seconds = 60; s_view.auto_refresh = false;
    s_view.screen_timeout_seconds = 30;
    memset(&s_config, 0, sizeof(s_config));
    s_config.refresh_seconds = 60; s_config.auto_refresh = false;
    s_settings_pending = false; s_pending_refresh_seconds = 0;
    s_pending_auto_refresh = false; s_pending_screen_timeout_seconds = 0;
    settings_behavior = 0; settings_events = 0; nvs_saves = 0;
    transport_ok = true; refresh_accepted = true; expect_busy = false;
    sleep_on_post = false; sleep_wake_on_post = false; sleep_on_get = false;
    lose_link_on_post = false;
    defer_get_fast_reconnect = false; defer_wake_get_gate = false;
    defer_post_fast_reconnect = false; local_post_setup_failure = false;
    fetches = 0; posts = 0; events = 0; notifications = 0;
}
int main(void) {
    quota_device_config_t config = {0};
    reset_state(); defer_wake_get_gate = true;
    s_display_scheduler.wake_fetch_pending = true;
    perform_snapshot_fetch(&config, 1, 1, true);
    assert(posts == 0 && fetches == 0 && s_view.connected && !s_view.refreshing);
    assert(s_display_scheduler.wake_fetch_pending);
    perform_snapshot_fetch(&config, 1, 1, true);
    assert(posts == 0 && fetches == 1 && !s_display_scheduler.wake_fetch_pending);

    reset_state(); quota_service_set_display_sleeping(true);
    assert(!perform_refresh(&config, 1, 1));
    assert(posts == 0 && fetches == 0 && !s_view.refreshing);

    reset_state(); expect_busy = true; defer_post_fast_reconnect = true;
    assert(!perform_refresh(&config, 1, 1));
    assert(posts == 0 && fetches == 0 && !s_view.refreshing);

    reset_state(); expect_busy = true; local_post_setup_failure = true;
    assert(perform_refresh(&config, 1, 1));
    assert(posts == 0 && fetches == 0 && !s_view.refreshing && s_view.request_failed);

    reset_state(); s_view.snapshot_valid = true; s_view.snapshot.revision = 1;
    for (unsigned i = 0; i < 3; i++) perform_snapshot_fetch(&config, 1, 1, false);
    assert(fetches == 3 && posts == 0 && events == 6);
    assert(!s_view.refreshing && !s_view.request_failed && s_view.snapshot.revision == 2);
    transport_ok = false;
    perform_snapshot_fetch(&config, 1, 1, false);
    assert(s_view.request_failed && s_view.snapshot_valid && s_view.snapshot.revision == 2);
    transport_ok = true;
    perform_snapshot_fetch(&config, 1, 1, false);
    assert(!s_view.request_failed);

    reset_state(); s_view.snapshot_valid = true; s_view.snapshot.revision = 1;
    expect_busy = true;
    assert(perform_refresh(&config, 1, 1));
    assert(posts == 1 && fetches == 1 && !s_view.refreshing && !s_view.request_failed);
    unsigned previous_fetches = fetches;
    refresh_accepted = false;
    assert(perform_refresh(&config, 1, 1));
    assert(posts == 2 && fetches == previous_fetches);
    assert(s_view.request_failed && !s_view.refreshing && s_view.snapshot.revision == 2);

    reset_state(); s_view.snapshot_valid = true; s_view.snapshot.revision = 1;
    expect_busy = true;
    lose_link_on_post = true;
    assert(perform_refresh(&config, 1, 1));
    assert(posts == 1 && fetches == 0 && s_view.request_failed);
    assert(!s_view.connected && !s_view.refreshing && s_view.snapshot.revision == 1);
    s_view.connected = true;
    assert(perform_refresh(&config, 1, 1));
    assert(posts == 2 && fetches == 1 && !s_view.request_failed);

    reset_state(); s_view.snapshot_valid = true; s_view.snapshot.revision = 1;
    s_display_scheduler.wake_fetch_pending = true;
    defer_get_fast_reconnect = true;
    perform_snapshot_fetch(&config, 1, 1, true);
    assert(posts == 0 && fetches == 1 && s_display_scheduler.wake_fetch_pending);
    assert(!s_view.refreshing && s_view.snapshot.revision == 1);
    perform_snapshot_fetch(&config, 1, 1, true);
    assert(posts == 0 && fetches == 2 && !s_display_scheduler.wake_fetch_pending);
    assert(s_view.snapshot.revision == 2);

    reset_state(); settings_behavior = 1;
    perform_settings_update(&config, 1, 1, 300, true, 60);
    assert(s_settings_pending && s_pending_refresh_seconds == 300);
    assert(s_pending_auto_refresh && s_pending_screen_timeout_seconds == 60);
    assert(settings_events == 0 && nvs_saves == 0);

    reset_state(); settings_behavior = 4;
    perform_settings_update(&config, 1, 1, 300, true, 60);
    assert(s_settings_pending && s_pending_refresh_seconds == 900);
    assert(!s_pending_auto_refresh && s_pending_screen_timeout_seconds == 120);
    assert(settings_events == 0 && nvs_saves == 0);

    reset_state(); settings_behavior = 2;
    perform_settings_update(&config, 1, 1, 300, true, 60);
    assert(s_settings_pending && s_pending_refresh_seconds == 300);
    assert(s_config.refresh_seconds == 60 && !s_config.auto_refresh);
    assert(s_view.refresh_seconds == 60 && !s_view.auto_refresh);
    assert(settings_events == 0 && nvs_saves == 0);

    reset_state(); settings_behavior = 3;
    perform_settings_update(&config, 1, 1, 300, true, 60);
    assert(s_settings_pending && s_pending_refresh_seconds == 900);
    assert(!s_pending_auto_refresh && s_pending_screen_timeout_seconds == 120);
    assert(s_config.refresh_seconds == 300 && s_config.auto_refresh);
    assert(settings_events == 1 && nvs_saves == 2);

    reset_state(); s_view.snapshot_valid = true; s_view.snapshot.revision = 1;
    expect_busy = true;
    sleep_on_post = true;
    assert(perform_refresh(&config, 1, 1));
    assert(posts == 1 && fetches == 0 && s_view.snapshot.revision == 1);
    assert(!s_view.refreshing && s_display_scheduler.sleeping);

    reset_state(); s_view.snapshot_valid = true; s_view.snapshot.revision = 1;
    expect_busy = true;
    sleep_wake_on_post = true;
    assert(perform_refresh(&config, 1, 1));
    assert(posts == 1 && fetches == 0 && s_view.snapshot.revision == 1);
    assert(!s_view.refreshing && !s_display_scheduler.sleeping);
    assert(s_display_scheduler.wake_fetch_pending);

    reset_state(); s_view.snapshot_valid = true; s_view.snapshot.revision = 1;
    expect_busy = true;
    sleep_on_get = true;
    assert(perform_refresh(&config, 1, 1));
    assert(posts == 1 && fetches == 1 && s_view.snapshot.revision == 1);
    assert(!s_view.refreshing && s_display_scheduler.sleeping);

    reset_state(); s_view.refreshing = true; s_refreshing_display_generation = 1;
    quota_service_set_display_sleeping(true);
    uint32_t sleeping_generation = s_display_scheduler.generation;
    quota_service_set_display_sleeping(true);
    assert(sleeping_generation == 2 && s_display_scheduler.generation == 2);
    assert(!s_view.refreshing && s_display_scheduler.sleeping && notifications == 1);
    quota_service_set_display_sleeping(false);
    assert(!s_display_scheduler.sleeping && s_display_scheduler.generation == 3);
    assert(s_display_scheduler.wake_fetch_pending && notifications == 2);
    puts("quota refresh generation tests passed");
}
'''
        compile_and_run(harness, "ai-quota-refresh-test-")

    def test_network_worker_pauses_wakes_and_preserves_provider_deadline(self):
        source = (ROOT / "main/quota_service.c").read_text()
        function = extract_function(source, "network_task")
        harness = r'''
#include "quota_logic.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    bool connected, auto_refresh, request_failed, pairing_active;
    uint16_t refresh_seconds;
    uint32_t pairing_seconds_left;
    uint64_t now_epoch;
} view_t;
typedef struct {
    bool sleeping, wake_fetch_pending;
    uint32_t generation;
} display_scheduler_t;
typedef struct { bool sleeping; uint32_t generation; } display_state_t;
typedef int portMUX_TYPE;
typedef int esp_err_t;
enum { ESP_OK = 0, QUOTA_APP_EVENT_PAIRING_TICK, QUOTA_APP_EVENT_CONNECTION };
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define SNAPSHOT_POLL_MS 10000
#define SNAPSHOT_RETRY_MS 30000
#define SELECTION_PERSIST_RETRY_MS 30000
#define ESP_LOGW(...) ((void)0)
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
view_t s_view;
display_scheduler_t s_display_scheduler;
portMUX_TYPE s_display_state_mux;
bool s_has_config, s_wifi_started, s_settings_pending, s_selection_pending;
bool s_fetch_after_connect, s_refresh_requested, s_wifi_retry_pending;
uint32_t s_config_generation, s_wifi_retry_delay_ms;
int64_t s_wifi_retry_at_ms;
uint16_t s_pending_refresh_seconds, s_pending_screen_timeout_seconds;
bool s_pending_auto_refresh;
char s_pending_account_id[QUOTA_ACCOUNT_ID_BYTES + 1];
uint64_t s_selection_persist_retry_at_ms;
int64_t s_pairing_opened_at_ms;
quota_device_config_t s_config, s_network_config;
void *s_mutex = (void *)1;
static void network_task(void *arg);
uint64_t s_now_ms;
unsigned s_loop_calls, s_stop_after, s_init_calls, s_config_calls;
unsigned s_retry_calls, s_settings_calls, s_posts, s_gets, s_wake_gets;
unsigned s_refresh_calls;
uint64_t s_post_times[16], s_wake_fetch_duration_ms;
typedef struct { uint64_t time; bool sleep, wake, manual, cancel_source; } loop_step_t;
loop_step_t s_steps[16];
bool s_use_steps;
bool s_disconnect_after_view_copy, s_disconnected_by_hook;
bool s_inject_manual_during_refresh, s_inject_settings_during_refresh;
bool s_cancel_before_refresh, s_local_refresh_failure;
jmp_buf s_loop_exit;
void mutex_lock(void) {}
void mutex_unlock(void) {}
uint64_t monotonic_ms(void) { return s_now_ms; }
uint64_t current_epoch(void) { return 1700000000; }
int ulTaskNotifyTake(unsigned clear, unsigned ticks) {
    (void)clear; (void)ticks;
    if (++s_loop_calls > s_stop_after) longjmp(s_loop_exit, 1);
    if (s_use_steps) {
        loop_step_t step = s_steps[s_loop_calls - 1];
        s_now_ms = step.time;
        if (step.sleep || step.wake) {
            s_display_scheduler.sleeping = step.sleep;
            s_display_scheduler.generation++;
            s_display_scheduler.wake_fetch_pending = step.wake;
        }
        if (step.manual) s_refresh_requested = true;
        if (step.cancel_source) s_cancel_before_refresh = true;
    }
    return 0;
}
void vTaskDelay(unsigned ticks) { (void)ticks; }
void post_simple_event(int kind) { (void)kind; }
bool pairing_active_locked(uint64_t now_ms) {
    (void)now_ms;
    if (s_disconnect_after_view_copy && !s_disconnected_by_hook) {
        s_view.connected = false;
        s_disconnected_by_hook = true;
    }
    return false;
}
bool quota_id_is_valid(const char *id) { (void)id; return true; }
bool nvs_save_config_locked(const quota_device_config_t *config) { (void)config; return true; }
display_state_t display_state_snapshot(void) {
    display_state_t state = {s_display_scheduler.sleeping, s_display_scheduler.generation};
    return state;
}
bool display_generation_is_current(uint32_t generation) {
    return !s_display_scheduler.sleeping && s_display_scheduler.generation == generation;
}
bool init_wifi(void) { s_init_calls++; s_wifi_started = true; return true; }
bool apply_wifi_config(const quota_device_config_t *config, uint32_t generation) {
    (void)config; (void)generation; s_config_calls++; return true;
}
esp_err_t esp_wifi_connect(void) { s_retry_calls++; return ESP_OK; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "stub"; }
void perform_settings_update(const quota_device_config_t *config, uint32_t cfg,
                             uint32_t display, uint16_t seconds, bool automatic,
                             uint16_t timeout) {
    (void)config; (void)cfg; (void)display; (void)seconds; (void)automatic;
    (void)timeout; s_settings_calls++;
}
bool perform_refresh(const quota_device_config_t *config, uint32_t cfg,
                     uint32_t display) {
    (void)config; (void)cfg; (void)display;
    s_refresh_calls++;
    if (s_cancel_before_refresh) {
        s_cancel_before_refresh = false;
        s_display_scheduler.sleeping = true;
        s_display_scheduler.generation++;
        s_display_scheduler.wake_fetch_pending = false;
        return false;
    }
    if (s_local_refresh_failure) return true;
    assert(s_posts < 16);
    s_post_times[s_posts++] = s_now_ms;
    if (s_inject_manual_during_refresh) {
        s_inject_manual_during_refresh = false;
        s_refresh_requested = true;
    }
    if (s_inject_settings_during_refresh) {
        s_inject_settings_during_refresh = false;
        s_settings_pending = true;
        s_pending_refresh_seconds = 300;
        s_pending_auto_refresh = true;
        s_pending_screen_timeout_seconds = 60;
    }
    return true;
}
void perform_snapshot_fetch(const quota_device_config_t *config, uint32_t cfg,
                            uint32_t display, bool wake_fetch) {
    (void)config; (void)cfg; (void)display; s_gets++;
    if (wake_fetch) {
        s_wake_gets++;
        s_display_scheduler.wake_fetch_pending = false;
        s_now_ms += s_wake_fetch_duration_ms;
    }
    s_view.request_failed = false;
}
void reset_state(void) {
    memset(&s_view, 0, sizeof(s_view));
    memset(&s_display_scheduler, 0, sizeof(s_display_scheduler));
    memset(&s_config, 0, sizeof(s_config));
    memset(&s_network_config, 0, sizeof(s_network_config));
    s_display_scheduler.generation = 1;
    s_has_config = true; s_config_generation = 0;
    s_view.connected = true; s_view.refresh_seconds = 60;
    s_view.auto_refresh = false; s_wifi_started = true;
    s_settings_pending = false; s_selection_pending = false;
    s_fetch_after_connect = false; s_refresh_requested = false;
    s_wifi_retry_pending = false; s_wifi_retry_delay_ms = 2000;
    s_pending_refresh_seconds = 0; s_pending_screen_timeout_seconds = 0;
    s_pending_auto_refresh = false; s_pending_account_id[0] = '\0';
    s_selection_persist_retry_at_ms = 0; s_pairing_opened_at_ms = 0;
    s_now_ms = 0; s_loop_calls = 0; s_stop_after = 0;
    s_init_calls = 0; s_config_calls = 0; s_retry_calls = 0;
    s_settings_calls = 0; s_posts = 0; s_gets = 0; s_wake_gets = 0;
    s_refresh_calls = 0; s_cancel_before_refresh = false; s_local_refresh_failure = false;
    s_wake_fetch_duration_ms = 0; s_use_steps = false;
    memset(s_steps, 0, sizeof(s_steps));
    memset(s_post_times, 0, sizeof(s_post_times));
    s_disconnect_after_view_copy = false; s_disconnected_by_hook = false;
    s_inject_manual_during_refresh = false;
    s_inject_settings_during_refresh = false;
}
void run_worker(unsigned loops) {
    s_loop_calls = 0; s_stop_after = loops;
    if (setjmp(s_loop_exit) == 0) network_task(NULL);
}
'''
        harness += function
        harness += r'''
int main(void) {
    reset_state();
    s_display_scheduler.sleeping = true; s_display_scheduler.generation = 2;
    s_settings_pending = true; s_wifi_started = false;
    run_worker(3);
    assert(s_init_calls == 0 && s_config_calls == 0 && s_retry_calls == 0);
    assert(s_settings_calls == 0 && s_posts == 0 && s_gets == 0);
    assert(s_settings_pending);

    reset_state();
    s_view.auto_refresh = false;
    s_display_scheduler.generation = 3;
    s_display_scheduler.wake_fetch_pending = true;
    run_worker(4);
    assert(s_posts == 0 && s_wake_gets == 1 && s_gets == 1);
    assert(!s_display_scheduler.wake_fetch_pending);

    reset_state();
    s_display_scheduler.wake_fetch_pending = true;
    s_disconnect_after_view_copy = true;
    run_worker(1);
    assert(s_disconnected_by_hook && !s_view.connected);
    assert(s_posts == 0 && s_gets == 0 && s_display_scheduler.wake_fetch_pending);
    s_view.connected = true; s_disconnect_after_view_copy = false;
    run_worker(3);
    assert(s_posts == 0 && s_wake_gets == 1 && s_gets == 1);

    reset_state();
    s_view.auto_refresh = true; s_view.refresh_seconds = 60;
    s_display_scheduler.wake_fetch_pending = true;
    s_wake_fetch_duration_ms = 65000;
    run_worker(2);
    assert(s_gets == 1 && s_posts == 1 && s_post_times[0] == 65000);

    /* Repeated wake reads before the deadline cannot defer the source refresh. */
    reset_state(); s_view.auto_refresh = true; s_use_steps = true;
    s_steps[0] = (loop_step_t){.time = 0};
    s_steps[1] = (loop_step_t){.time = 20000, .sleep = true};
    s_steps[2] = (loop_step_t){.time = 25000, .wake = true};
    s_steps[3] = (loop_step_t){.time = 40000, .sleep = true};
    s_steps[4] = (loop_step_t){.time = 45000, .wake = true};
    s_steps[5] = (loop_step_t){.time = 60000};
    run_worker(6);
    assert(s_wake_gets == 2 && s_posts == 1 && s_post_times[0] == 60000);

    /* An overdue source refresh follows the wake read and anchors the next period. */
    reset_state(); s_view.auto_refresh = true; s_use_steps = true;
    s_steps[0] = (loop_step_t){.time = 0};
    s_steps[1] = (loop_step_t){.time = 1000, .sleep = true};
    s_steps[2] = (loop_step_t){.time = 70000, .wake = true};
    s_steps[3] = (loop_step_t){.time = 70500};
    s_steps[4] = (loop_step_t){.time = 100000, .sleep = true};
    s_steps[5] = (loop_step_t){.time = 105000, .wake = true};
    s_steps[6] = (loop_step_t){.time = 130499};
    s_steps[7] = (loop_step_t){.time = 130500};
    run_worker(8);
    assert(s_wake_gets == 2 && s_posts == 2);
    assert(s_post_times[0] == 70500 && s_post_times[1] == 130500);

    /* Wake reads do not consume an explicit manual request, even with auto off. */
    reset_state(); s_use_steps = true;
    s_steps[0] = (loop_step_t){.time = 0, .sleep = true};
    s_steps[1] = (loop_step_t){.time = 70000, .wake = true, .manual = true};
    s_steps[2] = (loop_step_t){.time = 70500};
    run_worker(3);
    assert(s_wake_gets == 1 && s_posts == 1 && s_post_times[0] == 70500);

    /* Cancellation before POST admission retains the overdue automatic deadline. */
    reset_state(); s_view.auto_refresh = true; s_use_steps = true;
    s_steps[0] = (loop_step_t){.time = 0};
    s_steps[1] = (loop_step_t){.time = 60000, .cancel_source = true};
    s_steps[2] = (loop_step_t){.time = 70000, .wake = true};
    s_steps[3] = (loop_step_t){.time = 70500};
    run_worker(4);
    assert(s_refresh_calls == 2 && s_wake_gets == 1);
    assert(s_posts == 1 && s_post_times[0] == 70500);

    /* With auto off, the explicit manual request survives cancelled admission. */
    reset_state(); s_use_steps = true;
    s_steps[0] = (loop_step_t){.time = 0, .manual = true, .cancel_source = true};
    s_steps[1] = (loop_step_t){.time = 1000, .wake = true};
    s_steps[2] = (loop_step_t){.time = 1500};
    run_worker(3);
    assert(s_refresh_calls == 2 && s_wake_gets == 1);
    assert(s_posts == 1 && s_post_times[0] == 1500 && !s_refresh_requested);

    /* Genuine client/setup failures keep their cadence rather than retrying each loop. */
    reset_state(); s_view.auto_refresh = true; s_use_steps = true;
    s_local_refresh_failure = true;
    s_steps[0] = (loop_step_t){.time = 0};
    s_steps[1] = (loop_step_t){.time = 60000};
    s_steps[2] = (loop_step_t){.time = 60500};
    s_steps[3] = (loop_step_t){.time = 119999};
    s_steps[4] = (loop_step_t){.time = 120000};
    run_worker(5);
    assert(s_refresh_calls == 2 && s_posts == 0);

    reset_state(); s_refresh_requested = true;
    s_inject_manual_during_refresh = true;
    run_worker(3);
    assert(s_posts == 2);

    reset_state(); s_refresh_requested = true;
    s_inject_settings_during_refresh = true;
    run_worker(3);
    assert(s_posts == 1 && s_settings_calls == 1 && !s_settings_pending);
    puts("quota network worker tests passed");
}
'''
        compile_and_run(harness, "ai-quota-network-test-")


if __name__ == "__main__":
    unittest.main()
