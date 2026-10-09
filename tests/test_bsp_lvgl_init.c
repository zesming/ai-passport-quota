// No LVGL task may see the display until the rounding callback is registered.
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "../components/bsp/src/bsp_display_lvgl.c"

static lv_display_t display;
static int panel_present = 1, lock_depth, port_live, display_live, callback_live;
static int fail_lock, fail_port, fail_display, fail_event, init_calls, unlocked_flushes;
static int panel_token, io_token;
static lv_timer_t refresh;
static lv_obj_t screen;
static int fail_sleep, fail_on, panel_sleeping, panel_on = 1, invalidations, wakes;
static int refresh_calls, pixels_pending;
esp_lcd_panel_handle_t bsp_display_panel(void)
{
    return panel_present ? &panel_token : NULL;
}
esp_lcd_panel_io_handle_t bsp_display_io(void)
{
    return &io_token;
}
esp_err_t lvgl_port_init(const lvgl_port_cfg_t *cfg)
{
    assert(cfg->task_max_sleep_ms == 60000);
    ++init_calls;
    assert(!port_live);
    if (fail_port)
        return ESP_ERR_NO_MEM;
    port_live = 1;
    return ESP_OK;
}
esp_err_t lvgl_port_deinit(void)
{
    // The real API is asynchronous: returning does not release the context.
    assert(false && "Display rollback must not deinit/reinitialize the live port");
    return ESP_OK;
}
bool lvgl_port_lock(uint32_t timeout)
{
    (void)timeout;
    assert(port_live);
    if (fail_lock)
        return false;
    ++lock_depth;
    return true;
}
void lvgl_port_unlock(void)
{
    assert(lock_depth > 0);
    --lock_depth;
    if (!lock_depth && display_live) {
        assert(callback_live); // Simulate rendering as soon as the lock is free.
        ++unlocked_flushes;
    }
}
lv_display_t *lvgl_port_add_disp(const lvgl_port_display_cfg_t *cfg)
{
    assert(lock_depth > 0 && cfg->panel_handle == &panel_token && !display_live);
    assert(lvgl_port_lock(0)); // Real port takes and releases a recursive lock.
    if (!fail_display)
        display_live = 1;
    lvgl_port_unlock();
    return fail_display ? NULL : &display;
}
esp_err_t lvgl_port_remove_disp(lv_display_t *disp)
{
    assert(disp == &display && lock_depth && display_live);
    display_live = callback_live = 0;
    return ESP_OK;
}
void lv_display_add_event_cb(lv_display_t *disp, void (*cb)(lv_event_t *), int code, void *user)
{
    (void)user;
    assert(lock_depth && disp == &display && code == LV_EVENT_FLUSH_START &&
           cb == rounded_flush_event);
    if (!fail_event)
        callback_live = 1;
}
uint32_t lv_display_get_event_count(lv_display_t *disp)
{
    assert(disp == &display && lock_depth);
    return 1 + callback_live; // The display already owns an internal callback.
}
lv_timer_t *lv_display_get_refr_timer(lv_display_t *disp)
{
    assert(disp == &display && lock_depth);
    return &refresh;
}
lv_obj_t *lv_display_get_screen_active(lv_display_t *disp)
{
    assert(disp == &display && lock_depth);
    return &screen;
}
/* Fake LVGL timer list: refresh is always the head, test timers hang off it. */
static int handler_enabled = 1, tick_running = 1, fail_port_stop, fail_port_resume;
static char event_log[64];
static void log_event(char code)
{
    size_t length = strlen(event_log);
    assert(length + 1 < sizeof(event_log));
    event_log[length] = code;
    event_log[length + 1] = '\0';
}
static int timer_listed(const lv_timer_t *timer)
{
    for (const lv_timer_t *t = &refresh; t; t = t->next) {
        if (t == timer)
            return 1;
    }
    return 0;
}
lv_timer_t *lv_timer_get_next(lv_timer_t *timer)
{
    assert(lock_depth);
    return timer ? timer->next : &refresh;
}
bool lv_timer_get_paused(lv_timer_t *timer)
{
    assert(lock_depth && timer_listed(timer));
    return timer->paused;
}
void lv_timer_pause(lv_timer_t *timer)
{
    assert(timer_listed(timer) && lock_depth);
    timer->paused = true;
    log_event('p');
}
void lv_timer_resume(lv_timer_t *timer)
{
    assert(timer_listed(timer) && lock_depth);
    if (timer == &refresh)
        assert(!panel_sleeping && panel_on);
    timer->paused = false;
    log_event('r');
}
void lv_timer_enable(bool enable)
{
    handler_enabled = enable;
    log_event(enable ? 'E' : 'D');
}
esp_err_t lvgl_port_stop(void)
{
    assert(lock_depth);
    if (fail_port_stop)
        return ESP_ERR_INVALID_STATE;
    /* The real call disables the handler first, then stops the 2 ms tick timer. */
    handler_enabled = 0;
    tick_running = 0;
    log_event('S');
    return ESP_OK;
}
esp_err_t lvgl_port_resume(void)
{
    assert(lock_depth);
    if (fail_port_resume)
        return ESP_ERR_INVALID_STATE;
    handler_enabled = 1;
    tick_running = 1;
    log_event('T');
    return ESP_OK;
}
void lv_obj_invalidate(lv_obj_t *obj)
{
    assert(obj == &screen && lock_depth && !refresh.paused);
    ++invalidations;
}
void lv_refr_now(lv_display_t *disp)
{
    assert(disp == &display && lock_depth && !refresh.paused);
    ++refresh_calls;
    pixels_pending = 1;
}
esp_err_t lvgl_port_task_wake(int event, lv_display_t *disp)
{
    assert(event == LVGL_PORT_EVENT_DISPLAY && disp == &display && !lock_depth);
    ++wakes;
    return ESP_OK;
}
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t panel, bool on)
{
    assert(panel == &panel_token && lock_depth);
    if (!on)
        assert(refresh.paused);
    if (fail_on)
        return ESP_ERR_NO_MEM;
    panel_on = on;
    pixels_pending = 0;
    return ESP_OK;
}
esp_err_t esp_lcd_panel_disp_sleep(esp_lcd_panel_handle_t panel, bool sleeping)
{
    assert(panel == &panel_token && lock_depth && refresh.paused);
    if (fail_sleep)
        return ESP_ERR_NO_MEM;
    panel_sleeping = sleeping;
    return ESP_OK;
}
static void expect_failure(void)
{
    assert(bsp_lvgl_init() == NULL);
    assert(!s_disp && !lock_depth && !display_live);
    assert(!bsp_lvgl_lock(0));
}

static lv_timer_t ui[18];
static int running_timers(void)
{
    int running = 0;
    for (lv_timer_t *t = &refresh; t; t = t->next)
        running += !t->paused;
    return running;
}
static void link_timers(int count)
{
    refresh.next = count ? &ui[0] : NULL;
    for (int i = 0; i < count; ++i)
        ui[i].next = i + 1 < count ? &ui[i + 1] : NULL;
}
static void check_suspend_resume_bookkeeping(void)
{
    /* Screen-off order: panel Sleep In first (refresh timer paused), then suspend. */
    assert(bsp_lvgl_set_sleeping(true) && refresh.paused);
    link_timers(4);
    ui[2].paused = true; /* Paused by the UI itself: must stay paused after the wake. */
    wakes = 0;
    event_log[0] = '\0';

    fail_lock = 1;
    assert(!bsp_lvgl_suspend() && !s_suspended && tick_running && running_timers() == 3);
    fail_lock = 0;

    fail_port_stop = 1; /* Tick stop failure rolls the paused timers back. */
    assert(!bsp_lvgl_suspend() && !s_suspended && tick_running && running_timers() == 3);
    assert(!s_suspended_count && !lock_depth);
    fail_port_stop = 0;

    event_log[0] = '\0';
    assert(bsp_lvgl_suspend() && s_suspended && !lock_depth);
    assert(running_timers() == 0 && !tick_running && handler_enabled);
    /* Pause every running timer, stop the tick, then re-enable the handler (never before). */
    assert(!strcmp(event_log, "pppSE"));
    assert(s_suspended_count == 3);
    assert(bsp_lvgl_suspend() && !strcmp(event_log, "pppSE")); /* Idempotent. */

    /* A recorded timer deleted while suspended is skipped, never touched. */
    ui[0].next = &ui[2];
    fail_port_resume = 1;
    assert(!bsp_lvgl_resume() && s_suspended && !tick_running && wakes == 0);
    fail_port_resume = 0;
    event_log[0] = '\0';
    assert(bsp_lvgl_resume() && !s_suspended && !s_suspended_count && !lock_depth);
    assert(tick_running && handler_enabled && wakes == 1);
    /* Tick restarts before any timer runs again; ui[0], ui[3] resumed, ui[1] gone, ui[2] stays. */
    assert(!strcmp(event_log, "Trr") && ui[2].paused && !ui[0].paused && !ui[3].paused);
    assert(refresh.paused); /* Only bsp_lvgl_set_sleeping(false) resumes the refresh timer. */
    assert(bsp_lvgl_resume() && wakes == 1); /* Idempotent. */

    /* More running timers than slots: the surplus keeps running and nothing is lost. */
    for (int i = 0; i < 18; ++i)
        ui[i].paused = false;
    link_timers(18);
    assert(bsp_lvgl_suspend() && s_suspended_count == BSP_LVGL_SUSPEND_MAX_TIMERS);
    assert(running_timers() == 2 && !tick_running);
    assert(bsp_lvgl_resume() && running_timers() == 18);
    link_timers(0);
    assert(bsp_lvgl_set_sleeping(false) && !refresh.paused);
}
int main(void)
{
    panel_present = 0;
    expect_failure();
    panel_present = 1;
    fail_port = 1;
    expect_failure();
    fail_port = 0;
    const int failed_init_calls = init_calls;
    expect_failure(); // A partially initialized port cannot safely be re-created.
    assert(init_calls == failed_init_calls);
    s_port_init_failed = false; // Simulate reboot for remaining scenarios.
    fail_lock = 1;
    expect_failure();
    fail_lock = 0;
    const int retained_init_calls = init_calls;
    fail_display = 1;
    expect_failure();
    fail_display = 0;
    fail_event = 1;
    expect_failure();
    fail_event = 0;
    assert(bsp_lvgl_init() == &display);
    assert(init_calls == retained_init_calls); // Display retries reuse the port.
    assert(callback_live && !lock_depth && unlocked_flushes == 1);
    const int before = init_calls;
    assert(bsp_lvgl_init() == &display && init_calls == before);
    assert(bsp_lvgl_lock(5));
    bsp_lvgl_unlock();
    uint16_t pixels[BSP_LCD_W] = {0};
    for (int x = 0; x < BSP_LCD_W; ++x)
        pixels[x] = 0xffff;
    display.buffer = (lv_draw_buf_t){.data = (uint8_t *)pixels, .header.stride = sizeof(pixels)};
    lv_area_t area = {.x1 = 0, .y1 = 0, .x2 = BSP_LCD_W - 1, .y2 = 0};
    lv_event_t ev = {.target = &display, .area = &area};
    rounded_flush_event(&ev);
    assert(pixels[0] == 0 && pixels[BSP_LCD_W - 1] == 0 && pixels[BSP_LCD_W / 2] == 0xffff);
    fail_lock = 1;
    assert(!bsp_lvgl_set_sleeping(true) && !refresh.paused && panel_on);
    fail_lock = 0;
    assert(bsp_lvgl_set_sleeping(true) && refresh.paused && panel_sleeping && !panel_on);
    assert(wakes == 0);
    fail_sleep = 1;
    assert(!bsp_lvgl_set_sleeping(false) && refresh.paused && panel_sleeping && !panel_on);
    assert(wakes == 0 && invalidations == 0);
    fail_sleep = 0;
    fail_on = 1;
    assert(!bsp_lvgl_set_sleeping(false) && refresh.paused && !panel_sleeping);
    assert(wakes == 0 && invalidations == 0);
    fail_on = 0;
    assert(bsp_lvgl_set_sleeping(false) && !refresh.paused && !panel_sleeping && panel_on);
    assert(wakes == 1 && invalidations == 1 && !lock_depth);
    assert(bsp_lvgl_refresh() && refresh_calls == 1 && !pixels_pending && !lock_depth);
    fail_on = 1;
    assert(!bsp_lvgl_refresh() && !lock_depth);
    fail_on = 0;
    check_suspend_resume_bookkeeping();
    puts("BSP LVGL initialization tests: PASS");
}
