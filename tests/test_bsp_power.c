// Execute the real screen-off power sequence against fakes that record the call order.
// Compiled twice: plain, and with -DCONFIG_BSP_SLEEP_PROFILE for the profiling hooks.
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "../components/bsp/src/bsp_power.c"

#ifdef CONFIG_BSP_SLEEP_PROFILE
#define PROFILE_START "P"
#else
#define PROFILE_START ""
#endif

static char order[64];
static int64_t clock_us;
static int mv = 3300;
static int fail_button_suspend, fail_lvgl_suspend, fail_display_enter, fail_display_exit;
static int fail_lvgl_resume, fail_button_resume, fail_timer_start, fail_timer_create;
static int wake_calls, wake_result = 1, dumps;
static void *wake_user_seen;

static void note(char code)
{
    size_t length = strlen(order);
    assert(length + 1 < sizeof(order));
    order[length] = code;
    order[length + 1] = '\0';
}
static void clear_order(void)
{
    order[0] = '\0';
}

struct esp_timer {
    void (*callback)(void *);
    const char *name;
    int armed;
    uint64_t period_us;
};
static struct esp_timer timers[2];
static int timer_count;

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *handle)
{
    if (fail_timer_create)
        return ESP_ERR_NO_MEM;
    assert(timer_count < 2);
    timers[timer_count] = (struct esp_timer){.callback = args->callback, .name = args->name};
    *handle = &timers[timer_count++];
    return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t period_us)
{
    if (timer == s_poll_timer && fail_timer_start)
        return ESP_ERR_NO_MEM;
    if (timer->armed)
        return ESP_ERR_INVALID_STATE;
    timer->armed = 1;
    timer->period_us = period_us;
    note(timer == s_poll_timer ? 't' : 'P');
    return ESP_OK;
}
esp_err_t esp_timer_stop(esp_timer_handle_t timer)
{
    if (!timer->armed)
        return ESP_ERR_INVALID_STATE;
    timer->armed = 0;
    note(timer == s_poll_timer ? 's' : 'Q');
    return ESP_OK;
}
int64_t esp_timer_get_time(void)
{
    return clock_us;
}
esp_err_t esp_timer_dump(FILE *out)
{
    (void)out;
    ++dumps;
    return ESP_OK;
}
esp_err_t esp_pm_dump_locks(FILE *out)
{
    (void)out;
    ++dumps;
    return ESP_OK;
}
static void fire(esp_timer_handle_t timer)
{
    assert(timer->armed);
    timer->armed = 0;
    timer->callback(NULL);
}

esp_err_t bsp_button_suspend(void)
{
    note('b');
    return fail_button_suspend ? ESP_FAIL : ESP_OK;
}
esp_err_t bsp_button_resume(void)
{
    note('B');
    return fail_button_resume ? ESP_FAIL : ESP_OK;
}
int bsp_button_read_mv(void)
{
    return mv;
}
bool bsp_lvgl_suspend(void)
{
    note('L');
    return !fail_lvgl_suspend;
}
bool bsp_lvgl_resume(void)
{
    note('l');
    return !fail_lvgl_resume;
}
esp_err_t bsp_display_enter_light_sleep(void)
{
    note('d');
    return fail_display_enter ? ESP_FAIL : ESP_OK;
}
esp_err_t bsp_display_exit_light_sleep(void)
{
    note('D');
    return fail_display_exit ? ESP_FAIL : ESP_OK;
}

static bool on_wake(void *user)
{
    wake_user_seen = user;
    ++wake_calls;
    return wake_result;
}

static void expect_order(const char *expected)
{
    if (strcmp(order, expected) != 0) {
        fprintf(stderr, "order %s, expected %s\n", order, expected);
        assert(false);
    }
    clear_order();
}

static void check_sequence_and_rollback(void)
{
    /* Normal enter: button poll off, LVGL quiet, pins, then the 50 ms wake sampler. */
    assert(!bsp_power_screen_off());
    assert(bsp_power_enter_screen_off() == ESP_OK && bsp_power_screen_off());
    expect_order("bLdt" PROFILE_START);
    assert(s_poll_timer->armed && s_poll_timer->period_us == BSP_BTN_WAKE_POLL_MS * 1000ULL);
    assert(bsp_power_enter_screen_off() == ESP_OK && order[0] == '\0'); /* Idempotent. */

    /* Exit: stop the sampler first, restore pins and LVGL, button polling last. */
    mv = 3300;
    assert(bsp_power_exit_screen_off() == ESP_OK && !bsp_power_screen_off());
#ifdef CONFIG_BSP_SLEEP_PROFILE
    expect_order("sQDlB");
#else
    expect_order("sDlB");
#endif
    assert(!s_poll_timer->armed && bsp_power_exit_screen_off() == ESP_OK && order[0] == '\0');

    /* Each failing step rolls the earlier ones back to the awake state. */
    fail_timer_create = 1;
    s_poll_timer = NULL;
    timer_count = 0;
    assert(bsp_power_enter_screen_off() == ESP_ERR_NO_MEM && order[0] == '\0');
    fail_timer_create = 0;
    fail_button_suspend = 1;
    assert(bsp_power_enter_screen_off() == ESP_FAIL && !bsp_power_screen_off());
    expect_order("b");
    fail_button_suspend = 0;
    fail_lvgl_suspend = 1;
    assert(bsp_power_enter_screen_off() == ESP_FAIL && !bsp_power_screen_off());
    expect_order("bLDlB");
    fail_lvgl_suspend = 0;
    fail_display_enter = 1;
    assert(bsp_power_enter_screen_off() == ESP_FAIL && !bsp_power_screen_off());
    expect_order("bLdDlB");
    fail_display_enter = 0;
    fail_timer_start = 1;
    assert(bsp_power_enter_screen_off() == ESP_ERR_NO_MEM && !bsp_power_screen_off());
    expect_order("bLdDlB");
    assert(!s_polling);
    fail_timer_start = 0;

    /* A failed exit stays "off" so the caller retries; steps are repeated, none is skipped. */
    assert(bsp_power_enter_screen_off() == ESP_OK);
    clear_order();
    fail_display_exit = 1;
    assert(bsp_power_exit_screen_off() == ESP_FAIL && bsp_power_screen_off());
    assert(strstr(order, "D") && strstr(order, "l") && strstr(order, "B"));
    fail_display_exit = 0;
    fail_lvgl_resume = 1;
    assert(bsp_power_exit_screen_off() == ESP_FAIL && bsp_power_screen_off());
    fail_lvgl_resume = 0;
    fail_button_resume = 1;
    assert(bsp_power_exit_screen_off() == ESP_FAIL && bsp_power_screen_off());
    fail_button_resume = 0;
    assert(bsp_power_exit_screen_off() == ESP_OK && !bsp_power_screen_off());
    clear_order();
    s_gesture_active = false;
}

static void check_wake_poll(void)
{
    mv = 300; /* Long-press DOWN screen-off: the key is still held when the screen goes off. */
    bsp_power_set_wake_callback(on_wake, &mv);
    assert(bsp_power_enter_screen_off() == ESP_OK);
    wake_calls = 0;
    fire(s_poll_timer);
    assert(wake_calls == 0 && s_poll_timer->armed); /* Held key never wakes. */
    mv = -1;
    fire(s_poll_timer);
    assert(wake_calls == 0 && s_poll_timer->armed); /* Failed ADC read is not a press. */
    mv = 3300;
    fire(s_poll_timer);
    assert(wake_calls == 0 && s_poll_timer->armed && s_released_seen);
    mv = BSP_BTN_PRESSED_MAX_MV; /* The window is half open: 1900 mV is still released. */
    fire(s_poll_timer);
    assert(wake_calls == 0 && s_poll_timer->armed);
    mv = BSP_BTN_PRESSED_MAX_MV - 1;
    wake_result = 0; /* Queue full: keep sampling until the wake is delivered. */
    fire(s_poll_timer);
    assert(wake_calls == 1 && s_poll_timer->armed && s_polling);
    wake_result = 1;
    mv = 150; /* Any of the three keys wakes. */
    fire(s_poll_timer);
    assert(wake_calls == 2 && wake_user_seen == &mv);
    assert(!s_poll_timer->armed && !s_polling); /* One wake per sleep; no more sampling. */

    /* The wake press is still down at exit: it only wakes. */
    clear_order();
    assert(bsp_power_exit_screen_off() == ESP_OK);
    assert(s_gesture_active);
    /* A sampler tick that was already queued is a no-op after exit. */
    s_poll_timer->armed = 1;
    mv = 100;
    fire(s_poll_timer);
    assert(wake_calls == 2 && !s_poll_timer->armed);
    s_gesture_active = false;
}

static void check_wake_gesture_filter(void)
{
    /* Wake by a held key: PRESS then CLICK are swallowed, the next press is a real one. */
    mv = 300;
    clock_us = 10000000;
    s_off = true;
    assert(bsp_power_exit_screen_off() == ESP_OK);
    assert(bsp_power_wake_gesture_drop(BSP_BTN_OK, BSP_BTN_PRESS));
    clock_us += 100000;
    assert(bsp_power_wake_gesture_drop(BSP_BTN_OK, BSP_BTN_PRESS));
    assert(bsp_power_wake_gesture_drop(BSP_BTN_OK, BSP_BTN_CLICK));
    assert(!bsp_power_wake_gesture_drop(BSP_BTN_OK, BSP_BTN_PRESS));
    assert(!bsp_power_wake_gesture_drop(BSP_BTN_OK, BSP_BTN_CLICK));

    /* A long hold ends the gesture at LONG (the release after it produces no event). */
    s_off = true;
    assert(bsp_power_exit_screen_off() == ESP_OK);
    assert(bsp_power_wake_gesture_drop(BSP_BTN_DOWN, BSP_BTN_PRESS));
    assert(bsp_power_wake_gesture_drop(BSP_BTN_DOWN, BSP_BTN_LONG));
    assert(!bsp_power_wake_gesture_drop(BSP_BTN_DOWN, BSP_BTN_CLICK));

    /* Nothing is dropped after the 3 s cap, even without a release event. */
    s_off = true;
    assert(bsp_power_exit_screen_off() == ESP_OK);
    clock_us += BSP_BTN_WAKE_GESTURE_MS * 1000LL - 1;
    assert(bsp_power_wake_gesture_drop(BSP_BTN_UP, BSP_BTN_PRESS));
    clock_us += 1;
    assert(!bsp_power_wake_gesture_drop(BSP_BTN_UP, BSP_BTN_PRESS));
    assert(!bsp_power_wake_gesture_drop(BSP_BTN_UP, BSP_BTN_CLICK));

    /* A tap already released before the button timer resumes leaves nothing to swallow. */
    mv = 3300;
    s_off = true;
    assert(bsp_power_exit_screen_off() == ESP_OK);
    assert(!bsp_power_wake_gesture_drop(BSP_BTN_OK, BSP_BTN_PRESS));
    /* An unreadable ADC is treated as still pressed (bounded by the 3 s cap). */
    mv = -1;
    s_off = true;
    assert(bsp_power_exit_screen_off() == ESP_OK && s_gesture_active);
    s_gesture_active = false;
}

#ifdef CONFIG_BSP_SLEEP_PROFILE
static void check_profile_hook(void)
{
    mv = 3300;
    assert(bsp_power_enter_screen_off() == ESP_OK && s_profile_timer->armed);
    assert(s_profile_timer->period_us == PROFILE_DELAY_US);
    fire(s_profile_timer);
    assert(dumps == 2); /* PM locks and esp_timer table. */
    assert(bsp_power_exit_screen_off() == ESP_OK && !s_profile_timer->armed);
}
#endif

int main(void)
{
    check_sequence_and_rollback();
    check_wake_poll();
    check_wake_gesture_filter();
#ifdef CONFIG_BSP_SLEEP_PROFILE
    check_profile_hook();
#else
    assert(timer_count == 1); /* Only the wake sampler exists without the profiling option. */
#endif
    puts("BSP power sequence tests: PASS");
}
