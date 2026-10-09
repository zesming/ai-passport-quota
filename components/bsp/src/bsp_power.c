// components/bsp/src/bsp_power.c
// 息屏浅睡眠（POLL 唤醒）：把按键轮询、LVGL 定时器和引脚状态收拢到一个可回滚的进入/退出流程。
// 与 ESP_PM_CPU_FREQ_MAX 锁的顺序由调用方（main.c set_display_power）负责，见 bsp_power.h。
#include "bsp_power.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#if BSP_BTN_WAKE_MODE != BSP_BTN_WAKE_POLL
#error "BSP_BTN_WAKE_GPIO (P5b) is not implemented; use BSP_BTN_WAKE_POLL"
#endif

#ifdef CONFIG_BSP_SLEEP_PROFILE
#include "esp_pm.h"
#endif

static const char *TAG = "bsp_power";

// "Released" means ADC >= BSP_BTN_PRESSED_MAX_MV. The wake gesture ends once the ADC has stayed
// released for the debounce time plus one polling period: by then the button state machine has
// either emitted its release event (dropped like the rest of the gesture) or never will.
#define RELEASE_SETTLE_US                                                                          \
    ((int64_t)(CONFIG_BUTTON_DEBOUNCE_TICKS + 2) * CONFIG_BUTTON_PERIOD_TIME_MS * 1000)

static esp_timer_handle_t s_poll_timer;
static bsp_power_wake_cb_t s_wake_cb;
static void *s_wake_user;
static bool s_off;
static bool
    s_degraded; // Screen is off but no light-sleep steps were taken (buttons never came up).
static bool s_degraded_logged;
// Written by the application task, read by the poll callback in the esp_timer task.
static volatile bool s_polling;
static volatile bool s_released_seen;
// Wake gesture state: written at arm time before the button timer restarts, then only by the
// button timer task (sample and event callbacks run serially there).
static volatile bool s_gesture_active;
static volatile int64_t s_gesture_start_us;
static volatile int64_t s_release_start_us = -1;

void bsp_power_set_wake_callback(bsp_power_wake_cb_t cb, void *user)
{
    s_wake_user = user;
    s_wake_cb = cb;
}

// 50 ms 单次定时器：每次醒来读一次按键 ADC（iot_button 已停，没有并发访问）。
// 先要看到一次松开：长按下键息屏时键还按着。随后低于按键上界即唤醒，通知成功后不再重启定时器。
static void wake_poll(void *arg)
{
    (void)arg;
    if (!s_polling)
        return;
    const int mv = bsp_button_read_mv();
    const bool pressed = mv >= 0 && mv < BSP_BTN_PRESSED_MAX_MV;
    if (mv >= 0 && !pressed)
        s_released_seen = true;
    if (pressed && s_released_seen && s_wake_cb != NULL) {
        s_polling = false;
        if (s_wake_cb(s_wake_user))
            return;
        s_polling = true; // Could not notify (queue full): sample again next cycle.
    }
    if (esp_timer_start_once(s_poll_timer, (uint64_t)BSP_BTN_WAKE_POLL_MS * 1000) != ESP_OK)
        ESP_LOGE(TAG, "唤醒采样定时器重启失败");
}

// Runs for every fresh button ADC sample once the button timer is back. It ends the wake gesture
// when the key is released, including releases the button state machine never reports (a long
// press interrupted by the stop, a tap shorter than the debounce time).
static void gesture_sample(int mv)
{
    if (!s_gesture_active)
        return;
    if (mv < BSP_BTN_PRESSED_MAX_MV) { // Pressed, or an unreadable ADC (-1): not released.
        s_release_start_us = -1;
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (s_release_start_us < 0)
        s_release_start_us = now;
    else if (now - s_release_start_us >= RELEASE_SETTLE_US)
        s_gesture_active = false;
}

#ifdef CONFIG_BSP_SLEEP_PROFILE
// Light-sleep measurement without a console during the test: the chip is on battery with USB
// unplugged, so stats are kept in RAM (at screen-off start and 60 s later) and printed
// repeatedly after the wake; plug USB in after waking to read them (tools/pm_profile_delta.py).
#define PROFILE_DELAY_US (60LL * 1000 * 1000)
#define PROFILE_PRINT_PERIOD_US (15LL * 1000 * 1000)
#define PROFILE_PRINT_COUNT 20

static esp_timer_handle_t s_profile_timer;
static esp_timer_handle_t s_profile_print_timer;
static char *s_profile_snap[2];
static unsigned s_profile_prints;

static char *profile_capture(void)
{
    char *text = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&text, &size);
    if (stream == NULL)
        return NULL;
    esp_pm_dump_locks(stream);
    esp_timer_dump(stream);
    fclose(stream);
    return text;
}

static void profile_store(int slot)
{
    free(s_profile_snap[slot]);
    s_profile_snap[slot] = profile_capture();
}

static void profile_capture_60s(void *arg)
{
    (void)arg;
    profile_store(1);
}

static void profile_print(void *arg)
{
    (void)arg;
    if (s_profile_snap[0] == NULL || s_profile_snap[1] == NULL || s_off ||
        ++s_profile_prints > PROFILE_PRINT_COUNT) {
        if (s_profile_print_timer != NULL)
            (void)esp_timer_stop(s_profile_print_timer);
        return;
    }
    printf("=== screen-off profile: start ===\n%s\n=== screen-off profile: +60 s ===\n%s\n"
           "=== end of screen-off profile ===\n",
           s_profile_snap[0], s_profile_snap[1]);
}

static void profile_enter(void)
{
    if (s_profile_timer == NULL) {
        const esp_timer_create_args_t args = {.callback = profile_capture_60s,
                                              .name = "bsp_profile"};
        if (esp_timer_create(&args, &s_profile_timer) != ESP_OK)
            s_profile_timer = NULL; // Debug aid only: never blocks screen-off.
    }
    if (s_profile_print_timer != NULL)
        (void)esp_timer_stop(s_profile_print_timer);
    if (s_profile_timer != NULL)
        (void)esp_timer_stop(s_profile_timer); // Left armed when an earlier exit failed midway.
    free(s_profile_snap[1]);
    s_profile_snap[1] = NULL;
    profile_store(0);
    s_profile_prints = 0;
    if (s_profile_timer != NULL)
        (void)esp_timer_start_once(s_profile_timer, PROFILE_DELAY_US);
}

static void profile_exit(void)
{
    if (s_profile_timer != NULL)
        (void)esp_timer_stop(s_profile_timer);
    if (s_profile_snap[1] == NULL)
        return; // Woken before the 60 s mark: nothing comparable to print.
    if (s_profile_print_timer == NULL) {
        const esp_timer_create_args_t args = {.callback = profile_print,
                                              .name = "bsp_profile_print"};
        if (esp_timer_create(&args, &s_profile_print_timer) != ESP_OK) {
            s_profile_print_timer = NULL;
            return;
        }
    }
    (void)esp_timer_stop(s_profile_print_timer);
    (void)esp_timer_start_periodic(s_profile_print_timer, PROFILE_PRINT_PERIOD_US);
}
#endif

static esp_err_t ensure_timers(void)
{
    if (s_poll_timer == NULL) {
        const esp_timer_create_args_t args = {.callback = wake_poll, .name = "bsp_wake_poll"};
        esp_err_t e = esp_timer_create(&args, &s_poll_timer);
        if (e != ESP_OK) {
            s_poll_timer = NULL;
            return e;
        }
    }
    bsp_button_set_sample_callback(gesture_sample);
    return ESP_OK;
}

// Undo the steps of a failed enter; each call is idempotent and tolerates a partial state.
static void rollback_enter(void)
{
    s_polling = false;
    s_off = false;
    if (s_poll_timer != NULL)
        (void)esp_timer_stop(s_poll_timer);
    (void)bsp_display_exit_light_sleep();
    (void)bsp_lvgl_resume();
    (void)bsp_button_resume();
}

esp_err_t bsp_power_enter_screen_off(void)
{
    // Already fully armed. After a failed exit s_off is still set but polling is stopped, so the
    // steps below run again (all of them are idempotent) and re-arm the wake sampler.
    if (s_off && (s_degraded || s_polling))
        return ESP_OK;
    if (!bsp_button_ready()) {
        // No key can ever wake the chip: skip every light-sleep step, report success so the
        // caller does not retry the panel sleep once a second, and let it keep the awake lock.
        if (!s_degraded_logged) {
            s_degraded_logged = true;
            ESP_LOGW(TAG, "按键未就绪，息屏不进入浅睡眠");
        }
        s_degraded = true;
        s_off = true;
        return ESP_OK;
    }
    esp_err_t e = ensure_timers();
    if (e != ESP_OK)
        return e;
    e = bsp_button_suspend();
    if (e != ESP_OK)
        return e;
    if (!bsp_lvgl_suspend()) {
        rollback_enter();
        return ESP_FAIL;
    }
    e = bsp_display_enter_light_sleep();
    if (e != ESP_OK) {
        rollback_enter();
        return e;
    }
    // A key that is already up counts as the required release, so a press within the first
    // sampling period still wakes. A held key (long-press DOWN) waits for its release.
    const int mv = bsp_button_read_mv();
    s_released_seen = mv >= BSP_BTN_PRESSED_MAX_MV;
    s_gesture_active = false;
    s_polling = true;
    e = esp_timer_start_once(s_poll_timer, (uint64_t)BSP_BTN_WAKE_POLL_MS * 1000);
    if (e != ESP_OK) {
        rollback_enter();
        return e;
    }
#ifdef CONFIG_BSP_SLEEP_PROFILE
    profile_enter();
#endif
    s_degraded = false;
    s_off = true;
    return ESP_OK;
}

esp_err_t bsp_power_exit_screen_off(void)
{
    if (!s_off)
        return ESP_OK;
    if (s_degraded) {
        s_degraded = false;
        s_off = false;
        return ESP_OK;
    }
    s_polling = false;
    (void)esp_timer_stop(s_poll_timer); // INVALID_STATE when it already fired: fine.
    esp_err_t first = bsp_display_exit_light_sleep();
    if (!bsp_lvgl_resume() && first == ESP_OK)
        first = ESP_FAIL;
    // The waking press only wakes. If the key is still down (or unreadable), its events are
    // dropped until the ADC shows a release; armed before the button timer can produce any.
    const int mv = bsp_button_read_mv();
    if (mv < 0 || mv < BSP_BTN_PRESSED_MAX_MV) {
        s_gesture_start_us = esp_timer_get_time();
        s_release_start_us = -1;
        s_gesture_active = true;
    }
    esp_err_t resumed = bsp_button_resume();
    if (first == ESP_OK)
        first = resumed;
    if (first == ESP_OK) {
        s_off = false;
#ifdef CONFIG_BSP_SLEEP_PROFILE
        profile_exit();
#endif
    }
    return first;
}

bool bsp_power_screen_off(void)
{
    return s_off;
}

bool bsp_power_light_sleep_armed(void)
{
    return s_off && !s_degraded;
}

bool bsp_power_wake_gesture_drop(bsp_btn_t button, bsp_btn_ev_t event)
{
    (void)button;
    if (!s_gesture_active)
        return false;
    if (esp_timer_get_time() - s_gesture_start_us >= (int64_t)BSP_BTN_WAKE_GESTURE_MS * 1000) {
        s_gesture_active = false;
        return false;
    }
    if (event != BSP_BTN_PRESS)
        s_gesture_active = false; // Release (or a long press) ends the waking gesture.
    return true;
}
