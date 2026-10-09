// Execute the real BSP with public-interface stubs and a fake ADC (no SDK needed).
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include "../components/bsp/src/bsp_button.c"

struct button_dev_t {
    button_driver_t *driver;
    bool live;
};
static struct button_dev_t buttons[BSP_BTN_COUNT];
static int adc_token, cal_token, adc_live, cal_live, live_buttons;
static int create_calls, callback_calls, param_calls, fail_create, fail_callback, fail_param;
static int fail_adc, fail_channel, fail_cal, fail_read, fail_convert, fail_delete;
static int raw_mv, reads, events;
static int timer_running = 1, fail_stop, fail_resume, stop_calls, resume_calls;
static int64_t clock_us;

esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *cfg, adc_oneshot_unit_handle_t *h)
{
    assert(cfg->unit_id == BSP_BTN_ADC_UNIT && !adc_live);
    if (fail_adc)
        return ESP_ERR_NO_MEM;
    adc_live = 1;
    *h = &adc_token;
    return ESP_OK;
}
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t h)
{
    assert(h == &adc_token && adc_live && !live_buttons);
    adc_live = 0;
    return ESP_OK;
}
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t h, int channel,
                                     const adc_oneshot_chan_cfg_t *cfg)
{
    assert(h == &adc_token && channel == BSP_BTN_ADC_CHANNEL && cfg->atten == ADC_ATTEN_DB_12);
    return fail_channel ? ESP_FAIL : ESP_OK;
}
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t h, int channel, int *raw)
{
    assert(h == &adc_token && adc_live && channel == BSP_BTN_ADC_CHANNEL);
    ++reads;
    if (fail_read)
        return ESP_FAIL;
    *raw = raw_mv;
    return ESP_OK;
}
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *cfg,
                                               adc_cali_handle_t *h)
{
    assert(adc_live && !cal_live && cfg->atten == ADC_ATTEN_DB_12);
    if (fail_cal)
        return ESP_ERR_NO_MEM;
    cal_live = 1;
    *h = &cal_token;
    return ESP_OK;
}
esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t h)
{
    assert(h == &cal_token && cal_live && !live_buttons);
    cal_live = 0;
    return ESP_OK;
}
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t h, int raw, int *mv)
{
    assert(h == &cal_token && cal_live);
    if (fail_convert) {
        *mv = 0;
        return ESP_FAIL;
    }
    *mv = raw;
    return ESP_OK;
}
int64_t esp_timer_get_time(void)
{
    return clock_us;
}
esp_err_t iot_button_create(const button_config_t *cfg, const button_driver_t *driver,
                            button_handle_t *h)
{
    // 判定门限必须由 BSP 显式下发(bsp_pins.h),不能退回组件默认的 180 / 1500ms。
    assert(cfg->short_press_time == BSP_BTN_SHORT_PRESS_MS);
    assert(cfg->long_press_time == BSP_BTN_LONG_PRESS_MS);
    if (++create_calls == fail_create)
        return ESP_ERR_NO_MEM;
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        if (buttons[i].live)
            continue;
        buttons[i] = (struct button_dev_t){.driver = (button_driver_t *)driver, .live = true};
        *h = &buttons[i];
        ++live_buttons;
        assert(driver->get_key_level((button_driver_t *)driver) == BUTTON_INACTIVE);
        return ESP_OK;
    }
    assert(false);
    return ESP_FAIL;
}
esp_err_t iot_button_delete(button_handle_t h)
{
    assert(h && h->live && adc_live && cal_live);
    if (fail_delete)
        return ESP_FAIL;
    assert(h->driver->del(h->driver) == ESP_OK);
    h->live = false;
    --live_buttons;
    return ESP_OK;
}
esp_err_t iot_button_register_cb(button_handle_t h, button_event_t ev, button_event_args_t *args,
                                 button_cb_t cb, void *u)
{
    (void)args;
    (void)ev;
    assert(h->live);
    cb(h, u); // No user callbacks may escape a partial initialization.
    return ++callback_calls == fail_callback ? ESP_ERR_NO_MEM : ESP_OK;
}
esp_err_t iot_button_set_param(button_handle_t h, button_param_t param, void *value)
{
    assert(h->live && param == BUTTON_SHORT_PRESS_TIME_MS && value == NULL);
    return ++param_calls == fail_param ? ESP_ERR_NO_MEM : ESP_OK;
}
uint32_t iot_button_get_pressed_time(button_handle_t h)
{
    (void)h;
    return 0;
}
esp_err_t iot_button_stop(void)
{
    ++stop_calls;
    if (fail_stop)
        return ESP_FAIL;
    assert(timer_running); // The real driver rejects a stop while the timer is not running.
    timer_running = 0;
    return ESP_OK;
}
esp_err_t iot_button_resume(void)
{
    ++resume_calls;
    if (fail_resume)
        return ESP_FAIL;
    assert(!timer_running);
    timer_running = 1;
    return ESP_OK;
}
static void event_cb(bsp_btn_t btn, bsp_btn_ev_t ev, void *u)
{
    assert(btn == BSP_BTN_OK && ev == BSP_BTN_CLICK && u == &events);
    ++events;
}
static void reset_faults(void)
{
    fail_adc = fail_channel = fail_cal = fail_read = fail_convert = fail_delete = 0;
    fail_create = fail_callback = fail_param = create_calls = callback_calls = param_calls = 0;
}
static void assert_clean(void)
{
    assert(!adc_live && !cal_live && !live_buttons && !s_ready && !s_adc && !s_cali);
    for (int i = 0; i < BSP_BTN_COUNT; ++i)
        assert(!s_btn[i]);
}
static void retry_success(void)
{
    assert_clean();
    reset_faults();
    assert(bsp_button_init(event_cb, &events) == ESP_OK);
    assert(create_calls == BSP_BTN_COUNT && live_buttons == BSP_BTN_COUNT);
    assert(bsp_button_init(event_cb, &events) == ESP_OK);
    assert(create_calls == BSP_BTN_COUNT);
    button_cleanup();
    assert_clean();
}
static void check_voltage(int mv, int expected)
{
    raw_mv = mv;
    clock_us += 2000;
    const int before = reads;
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        assert(s_drivers[i].base.get_key_level(&s_drivers[i].base) == (i == expected));
    }
    assert(reads - before == CONFIG_ADC_BUTTON_SAMPLE_TIMES);
}
int main(void)
{
    for (int i = 1; i <= BSP_BTN_COUNT; ++i) {
        reset_faults();
        fail_create = i;
        assert(bsp_button_init(event_cb, &events) != ESP_OK);
        retry_success();
    }
    for (int i = 1; i <= BSP_BTN_COUNT * 3; ++i) {
        reset_faults();
        fail_callback = i;
        assert(bsp_button_init(event_cb, &events) != ESP_OK);
        retry_success();
    }
    for (int i = 1; i <= BSP_BTN_COUNT; ++i) {
        reset_faults();
        fail_param = i;
        assert(bsp_button_init(event_cb, &events) != ESP_OK);
        retry_success();
    }
    reset_faults();
    fail_adc = 1;
    assert(bsp_button_init(event_cb, &events) != ESP_OK);
    retry_success();
    fail_channel = 1;
    assert(bsp_button_init(event_cb, &events) != ESP_OK);
    retry_success();
    fail_cal = 1;
    assert(bsp_button_init(event_cb, &events) != ESP_OK);
    retry_success();
    assert(events == 0);
    assert(bsp_button_init(event_cb, &events) == ESP_OK);
    s_long_pressed[BSP_BTN_OK] = false;
    cb_release(NULL, (void *)(intptr_t)BSP_BTN_OK);
    assert(events == 1);
    s_long_pressed[BSP_BTN_OK] = true;
    cb_release(NULL, (void *)(intptr_t)BSP_BTN_OK);
    assert(events == 1);
    check_voltage(0, BSP_BTN_UP);
    check_voltage(149, BSP_BTN_UP);
    check_voltage(150, BSP_BTN_DOWN);
    check_voltage(446, BSP_BTN_DOWN);
    check_voltage(447, BSP_BTN_OK);
    check_voltage(1899, BSP_BTN_OK);
    check_voltage(1900, -1);
    check_voltage(3300, -1);
    assert(bsp_button_read_mv() == 3300);
    fail_read = 1;
    clock_us += 2000;
    for (int i = 0; i < BSP_BTN_COUNT; ++i)
        assert(!button_level(&s_drivers[i].base));
    assert(bsp_button_read_mv() == -1);
    fail_read = 0;
    fail_convert = 1;
    clock_us += 2000;
    for (int i = 0; i < BSP_BTN_COUNT; ++i)
        assert(!button_level(&s_drivers[i].base));
    assert(bsp_button_read_mv() == -1);
    /* Screen-off suspend/resume: idempotent, failures keep state so the caller can retry. */
    s_sample_valid = true;
    fail_stop = 1;
    assert(bsp_button_suspend() == ESP_FAIL && !s_suspended && timer_running);
    fail_stop = 0;
    assert(bsp_button_suspend() == ESP_OK && s_suspended && !timer_running && stop_calls == 2);
    assert(bsp_button_suspend() == ESP_OK && stop_calls == 2); /* Already stopped. */
    fail_resume = 1;
    assert(bsp_button_resume() == ESP_FAIL && s_suspended && !timer_running);
    fail_resume = 0;
    assert(bsp_button_resume() == ESP_OK && !s_suspended && timer_running && !s_sample_valid);
    assert(bsp_button_resume() == ESP_OK && resume_calls == 2); /* Not suspended: no driver call. */
    fail_delete = 1;
    button_cleanup();
    assert(adc_live && cal_live && live_buttons == BSP_BTN_COUNT);
    assert(bsp_button_init(event_cb, &events) == ESP_ERR_INVALID_STATE);
    fail_delete = 0;
    button_cleanup();
    retry_success();
    assert(bsp_button_suspend() == ESP_ERR_INVALID_STATE); /* Buttons never became ready. */
    assert(bsp_button_resume() == ESP_ERR_INVALID_STATE);
    puts("BSP button fault-injection tests: PASS");
}
