// components/bsp/src/bsp_power.c
// 息屏浅睡眠（POLL 唤醒）：把按键轮询、LVGL 定时器和引脚状态收拢到一个可回滚的进入/退出流程。
// 与 ESP_PM_CPU_FREQ_MAX 锁的顺序由调用方（main.c set_display_power）负责，见 bsp_power.h。
#include "bsp_power.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <stddef.h>
#include <stdio.h>

#if BSP_BTN_WAKE_MODE != BSP_BTN_WAKE_POLL
#error "BSP_BTN_WAKE_GPIO (P5b) is not implemented; use BSP_BTN_WAKE_POLL"
#endif

#ifdef CONFIG_BSP_SLEEP_PROFILE
#include "esp_pm.h"
#endif

static const char *TAG = "bsp_power";

#define PROFILE_DELAY_US (60LL * 1000 * 1000)

static esp_timer_handle_t s_poll_timer;
#ifdef CONFIG_BSP_SLEEP_PROFILE
static esp_timer_handle_t s_profile_timer;
#endif
static bsp_power_wake_cb_t s_wake_cb;
static void *s_wake_user;
static bool s_off;
// Written by the application task, read by the poll callback in the esp_timer task.
static volatile bool s_polling;
static volatile bool s_released_seen;
static volatile bool s_gesture_active;
static volatile int64_t s_gesture_start_us;

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

#ifdef CONFIG_BSP_SLEEP_PROFILE
// 息屏 60 秒后打印 PM 锁与 esp_timer 统计，用于确认息屏期间唯一周期性唤醒源是 wake_poll。
static void profile_dump(void *arg)
{
    (void)arg;
    printf("=== screen-off profile (60 s) ===\n");
    esp_pm_dump_locks(stdout);
    esp_timer_dump(stdout);
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
#ifdef CONFIG_BSP_SLEEP_PROFILE
    if (s_profile_timer == NULL) {
        const esp_timer_create_args_t args = {.callback = profile_dump, .name = "bsp_profile"};
        if (esp_timer_create(&args, &s_profile_timer) != ESP_OK)
            s_profile_timer = NULL; // Debug aid only: never blocks screen-off.
    }
#endif
    return ESP_OK;
}

// Undo the steps of a failed enter; each call is idempotent and tolerates a partial state.
static void rollback_enter(void)
{
    s_polling = false;
    if (s_poll_timer != NULL)
        (void)esp_timer_stop(s_poll_timer);
    (void)bsp_display_exit_light_sleep();
    (void)bsp_lvgl_resume();
    (void)bsp_button_resume();
}

esp_err_t bsp_power_enter_screen_off(void)
{
    if (s_off)
        return ESP_OK;
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
    s_released_seen = false;
    s_polling = true;
    e = esp_timer_start_once(s_poll_timer, (uint64_t)BSP_BTN_WAKE_POLL_MS * 1000);
    if (e != ESP_OK) {
        rollback_enter();
        return e;
    }
#ifdef CONFIG_BSP_SLEEP_PROFILE
    if (s_profile_timer != NULL)
        (void)esp_timer_start_once(s_profile_timer, PROFILE_DELAY_US);
#endif
    s_off = true;
    return ESP_OK;
}

esp_err_t bsp_power_exit_screen_off(void)
{
    if (!s_off)
        return ESP_OK;
    s_polling = false;
    (void)esp_timer_stop(s_poll_timer); // INVALID_STATE when it already fired: fine.
#ifdef CONFIG_BSP_SLEEP_PROFILE
    if (s_profile_timer != NULL)
        (void)esp_timer_stop(s_profile_timer);
#endif
    esp_err_t first = bsp_display_exit_light_sleep();
    if (!bsp_lvgl_resume() && first == ESP_OK)
        first = ESP_FAIL;
    // The waking press only wakes. If the key is still down, its PRESS/CLICK/LONG are dropped
    // until the release; the arm happens before the button timer can produce any event.
    const int mv = bsp_button_read_mv();
    if (mv < 0 || mv < BSP_BTN_PRESSED_MAX_MV) {
        s_gesture_start_us = esp_timer_get_time();
        s_gesture_active = true;
    }
    esp_err_t resumed = bsp_button_resume();
    if (first == ESP_OK)
        first = resumed;
    if (first == ESP_OK)
        s_off = false;
    return first;
}

bool bsp_power_screen_off(void)
{
    return s_off;
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
