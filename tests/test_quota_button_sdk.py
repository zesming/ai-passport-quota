"""Run the locked button state machine with BSP callbacks and real wake logic."""
import re
import unittest
from runtime_helpers import ROOT, compile_and_run, vendor_function


class ButtonSdk(unittest.TestCase):
    def vendor_prefix(self):
        """Fake button component and ADC around the vendor state machine."""
        vendor = (ROOT / "managed_components/espressif__button/iot_button.c").read_text()
        header = (ROOT / "managed_components/espressif__button/include/iot_button.h").read_text()
        defaults = (ROOT / "sdkconfig.defaults").read_text()
        for setting, value in (
            ("BUTTON_PERIOD_TIME_MS", 5),
            ("BUTTON_DEBOUNCE_TICKS", 2),
            ("BUTTON_LONG_PRESS_HOLD_SERIAL_TIME_MS", 20),
        ):
            self.assertRegex(defaults, rf"(?m)^CONFIG_{setting}={value}$")

        def block(source, start, end):
            return re.search(re.escape(start) + r".*?" + re.escape(end), source, re.S)[0]

        types = "\n".join(
            (
                block(header, "typedef enum {\n    BUTTON_PRESS_DOWN", "} button_event_t;"),
                block(header, "typedef union {", "} button_event_args_t;"),
                block(vendor, "enum {\n    PRESS_DOWN_CHECK", "};"),
                block(vendor, "typedef struct {\n    button_cb_t cb;", "} button_cb_info_t;"),
                block(vendor, "typedef struct button_dev_t {", "} button_dev_t;"),
            )
        )
        # The vendor state machine is third-party text; the firmware's own bsp_button.c is
        # compiled whole and registers its callbacks with this fake component.
        prefix = (
            r"""
#include "bsp_button.h"
#include "bsp_pins.h"
#include "quota_logic.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void (*button_cb_t)(void *, void *);
/* Layout of the driver struct bsp_button.c fills in (tests/bsp_stubs/iot_button.h). */
typedef struct button_driver_t button_driver_t;
struct button_driver_t {
    bool enable_power_save;
    uint8_t (*get_key_level)(button_driver_t *);
    int (*enter_power_save)(button_driver_t *);
    int (*exit_power_save)(button_driver_t *);
    int32_t (*get_gpio_num)(button_driver_t *);
    int (*del)(button_driver_t *);
};
"""
            + types
            + r"""
#define BUTTON_ACTIVE 1
#define TICKS_INTERVAL 5
#define DEBOUNCE_TICKS 2
#define SERIAL_TICKS 4
#define TOLERANCE 20
#define CALL_EVENT_CB(ev)                                                                          \
    do {                                                                                           \
        for (size_t j = 0; j < btn->size[ev]; j++)                                                 \
            btn->cb_info[ev][j].cb(btn, btn->cb_info[ev][j].usr_data);                             \
    } while (0)
/* bsp_button.c is built against tests/bsp_stubs/iot_button.h; these are its event numbers. */
enum { STUB_PRESS_DOWN = 0, STUB_PRESS_UP = 1, STUB_LONG_PRESS_HOLD = 5 };

static button_dev_t devs[BSP_BTN_COUNT];
static button_cb_info_t infos[BSP_BTN_COUNT][BUTTON_EVENT_MAX];
static unsigned created;
static const button_driver_t *created_drivers[BSP_BTN_COUNT];
/* The vendor timer is driven by the harness, so stop/resume only track the running flag here. */
static int timer_running = 1;
int iot_button_stop(void)
{
    assert(timer_running);
    timer_running = 0;
    return 0;
}
int iot_button_resume(void)
{
    assert(!timer_running);
    timer_running = 1;
    return 0;
}
uint32_t iot_button_get_pressed_time(button_dev_t *b)
{
    return b->ticks * TICKS_INTERVAL;
}
int iot_button_create(const void *config, const button_driver_t *driver, button_dev_t **handle)
{
    (void)config;
    assert(created < BSP_BTN_COUNT);
    created_drivers[created] = driver;
    *handle = &devs[created++];
    return 0;
}
int iot_button_delete(button_dev_t *handle)
{
    (void)handle;
    return 0;
}
int iot_button_set_param(button_dev_t *handle, int param, void *value)
{
    (void)handle;
    (void)param;
    (void)value;
    return 0;
}
int iot_button_register_cb(button_dev_t *handle, int event, void *args, button_cb_t cb, void *user)
{
    (void)args;
    assert(event == STUB_PRESS_DOWN || event == STUB_PRESS_UP || event == STUB_LONG_PRESS_HOLD);
    unsigned key = (unsigned)(handle - devs);
    unsigned mapped = event == STUB_PRESS_DOWN ? BUTTON_PRESS_DOWN
                      : event == STUB_PRESS_UP ? BUTTON_PRESS_UP
                                               : BUTTON_LONG_PRESS_HOLD;
    infos[key][mapped] = (button_cb_info_t){.cb = cb, .usr_data = user};
    handle->cb_info[mapped] = &infos[key][mapped];
    handle->size[mapped] = 1;
    return 0;
}
int adc_oneshot_new_unit(const void *config, void **handle)
{
    (void)config;
    *handle = &created;
    return 0;
}
int adc_oneshot_del_unit(void *handle)
{
    (void)handle;
    return 0;
}
int adc_oneshot_config_channel(void *handle, int channel, const void *config)
{
    (void)handle;
    (void)channel;
    (void)config;
    return 0;
}
int adc_oneshot_read(void *handle, int channel, int *raw)
{
    (void)handle;
    (void)channel;
    *raw = 0;
    return 0;
}
int adc_cali_create_scheme_curve_fitting(const void *config, void **handle)
{
    (void)config;
    *handle = &created;
    return 0;
}
int adc_cali_delete_scheme_curve_fitting(void *handle)
{
    (void)handle;
    return 0;
}
int adc_cali_raw_to_voltage(void *handle, int raw, int *mv)
{
    (void)handle;
    *mv = raw;
    return 0;
}
int64_t esp_timer_get_time(void)
{
    return 0;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
"""
        )
        return vendor, prefix

    def test_rapid_taps_long_press_and_wake(self):
        vendor, prefix = self.vendor_prefix()
        harness = (
            prefix
            + vendor_function(vendor, "button_handler")
            + r"""
#pragma GCC diagnostic pop
static unsigned presses, clicks, longs, navigations;
static uint64_t now;
static quota_display_state_t display;
static uint8_t level;
static uint8_t read_level(button_driver_t *driver) { (void)driver; return level; }
static button_driver_t driver={.get_key_level=read_level};
static void receive(bsp_btn_t key,bsp_btn_ev_t event,void *user) {
    (void)user;
    if(event==BSP_BTN_PRESS) presses++;
    if(event==BSP_BTN_CLICK) clicks++;
    if(event==BSP_BTN_LONG) longs++;
    quota_key_event_t mapped=event==BSP_BTN_PRESS?QUOTA_KEY_PRESS:
        event==BSP_BTN_LONG?QUOTA_KEY_LONG:QUOTA_KEY_CLICK;
    if(quota_display_handle_key(&display,now,mapped,key==BSP_BTN_DOWN)) navigations++;
}
static void ticks(button_dev_t *b,uint8_t value,unsigned count) {
    level=value;
    for(unsigned i=0;i<count;i++) { now+=TICKS_INTERVAL; button_handler(b); }
}
static void gesture(button_dev_t *b,unsigned held) { ticks(b,1,held); ticks(b,0,2); }
static void run(bsp_btn_t key) {
    /* Fresh state machine, keeping the callbacks bsp_button_init() registered. */
    button_dev_t *b=&devs[key];
    button_cb_info_t *kept[BUTTON_EVENT_MAX]; size_t sizes[BUTTON_EVENT_MAX];
    memcpy(kept,b->cb_info,sizeof(kept)); memcpy(sizes,b->size,sizeof(sizes));
    memset(b,0,sizeof(*b));
    memcpy(b->cb_info,kept,sizeof(kept)); memcpy(b->size,sizes,sizeof(sizes));
    b->driver=&driver; b->long_press_ticks=100; b->short_press_ticks=0;
    display=(quota_display_state_t){0}; presses=clicks=longs=navigations=0;
    gesture(b,2); assert(clicks==1); /* CLICK emitted on release, before any wait. */
    gesture(b,2); assert(clicks==2 && presses==2 && longs==0);
    gesture(b,110); assert(longs==1 && clicks==2); /* Fast following hold remains LONG. */
    if(key==BSP_BTN_DOWN) {
        assert(display.sleeping);
        gesture(b,2); assert(!display.sleeping && !display.consume_wake_gesture);
    }
    ticks(b,0,40);
    display=(quota_display_state_t){.sleeping=true}; navigations=0;
    gesture(b,2); assert(!display.sleeping && navigations==0);
    gesture(b,2); assert(navigations==1);
    ticks(b,0,40);
    display=(quota_display_state_t){.sleeping=true}; navigations=0;
    gesture(b,110); assert(!display.sleeping && navigations==0);
    gesture(b,2); assert(navigations==1);
    ticks(b,0,40); unsigned before=clicks;
    ticks(b,1,1); ticks(b,0,2); assert(clicks==before); /* bounce */
    unsigned durations[]={99,100,103,140}; /* release at 495/500/515/700 ms */
    for(unsigned i=0;i<4;i++) {
        ticks(b,0,40); display=(quota_display_state_t){0};
        unsigned old_clicks=clicks,old_longs=longs;
        gesture(b,durations[i]);
        assert(clicks==old_clicks+(i==0) && longs==old_longs+(i!=0));
        assert(display.sleeping==(key==BSP_BTN_DOWN && i!=0));
    }
}
int main(void) {
    assert(bsp_button_init(receive,NULL)==0 && created==BSP_BTN_COUNT);
    run(BSP_BTN_UP); run(BSP_BTN_DOWN); run(BSP_BTN_OK);
    assert(bsp_button_suspend()==0 && !timer_running && bsp_button_suspend()==0);
    assert(bsp_button_resume()==0 && timer_running && bsp_button_resume()==0);
    puts("locked button rapid taps, second hold, sleep/wake and bounce passed");
}
"""
        )
        compile_and_run(
            harness,
            "quota-button-sdk-",
            ("main/quota_logic.c", "components/bsp/src/bsp_button.c", "tests/cjson/cJSON.c"),
            (
                "-DQUOTA_HOST_TEST",
                "-I" + str(ROOT / "tests/bsp_stubs"),
                "-I" + str(ROOT / "components/bsp/include"),
                "-Wno-deprecated-declarations",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
            ),
        )


    def test_wake_gesture_with_real_state_machine(self):
        """The waking key press only wakes, however the state machine was left by the stop."""
        vendor, prefix = self.vendor_prefix()
        # The BSP's own level function reads this ADC; time moves with the 5 ms ticks.
        prefix = prefix.replace(
            "    *raw = 0;\n    return 0;", "    *raw = adc_mv;\n    return 0;"
        ).replace(
            "int adc_oneshot_read(", "static int adc_mv = 3300;\nint adc_oneshot_read(", 1
        ).replace(
            "int64_t esp_timer_get_time(void)\n{\n    return 0;\n}",
            "static uint64_t now;\nint64_t esp_timer_get_time(void)\n"
            "{\n    return (int64_t)now * 1000;\n}",
        )
        self.assertIn("now * 1000", prefix)
        self.assertIn("adc_mv;", prefix)
        harness = (
            prefix
            + vendor_function(vendor, "button_handler")
            + r"""
#pragma GCC diagnostic pop
#include "bsp_power.h"
#include "esp_timer.h"
struct esp_timer { void (*cb)(void *); int armed; };
static struct esp_timer poll_timer;
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h)
{ poll_timer.cb = a->callback; *h = &poll_timer; return 0; }
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us)
{ (void)us; if (t->armed) return 0x103; t->armed = 1; return 0; }
esp_err_t esp_timer_stop(esp_timer_handle_t t)
{ if (!t->armed) return 0x103; t->armed = 0; return 0; }
bool bsp_lvgl_suspend(void) { return true; }
bool bsp_lvgl_resume(void) { return true; }
esp_err_t bsp_display_enter_light_sleep(void) { return 0; }
esp_err_t bsp_display_exit_light_sleep(void) { return 0; }
static void fire(void) { assert(poll_timer.armed); poll_timer.armed = 0; poll_timer.cb(NULL); }

static unsigned passed, dropped, woke;
static const int key_mv[BSP_BTN_COUNT] = {0, 300, 595};
static void receive(bsp_btn_t key, bsp_btn_ev_t ev, void *user) {
    (void)key; (void)ev; (void)user;
    if (bsp_power_wake_gesture_drop(key, ev)) dropped++; else passed++;
}
static bool on_wake(void *u) { (void)u; woke++; return true; }
static void tick(unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        now += TICKS_INTERVAL;
        if (timer_running) for (int k = 0; k < BSP_BTN_COUNT; k++) button_handler(&devs[k]);
    }
}
static void fresh_state_machines(void) {
    for (int k = 0; k < BSP_BTN_COUNT; k++) {
        button_dev_t *b = &devs[k];
        button_cb_info_t *kept[BUTTON_EVENT_MAX]; size_t sizes[BUTTON_EVENT_MAX];
        memcpy(kept, b->cb_info, sizeof(kept)); memcpy(sizes, b->size, sizeof(sizes));
        memset(b, 0, sizeof(*b));
        memcpy(b->cb_info, kept, sizeof(kept)); memcpy(b->size, sizes, sizeof(sizes));
        b->driver = (button_driver_t *)created_drivers[k];
        b->long_press_ticks = 100; b->short_press_ticks = 0;
    }
}
static void real_click(int key) {
    adc_mv = key_mv[key]; tick(10); adc_mv = 3300; tick(10);
}
/* Screen off, then the wake key is pressed for held_ticks (the sampler sees it, exit reads the
 * ADC while it is down, the state machine restarts), then released. */
static void wake_with(int wake_key, unsigned held_ticks) {
    adc_mv = 3300; now += 100; fire(); assert(!woke);
    adc_mv = key_mv[wake_key]; now += 50; fire(); assert(woke == 1);
    assert(bsp_power_exit_screen_off() == 0);
    tick(held_ticks);
    adc_mv = 3300; tick(10);
}
static void scenario(int sleeping_long_press_key, int wake_key, int click_key,
                     unsigned held_ticks) {
    passed = dropped = woke = 0; adc_mv = 3300; poll_timer.armed = 0; timer_running = 1;
    fresh_state_machines(); tick(40);
    if (sleeping_long_press_key >= 0) {
        adc_mv = key_mv[sleeping_long_press_key]; tick(110);
        assert(passed == 2); /* PRESS + LONG reached the application. */
    }
    assert(bsp_power_enter_screen_off() == 0 && !timer_running);
    adc_mv = 3300; now += 2000; /* Let go while the button timer is stopped. */
    unsigned before = passed;
    wake_with(wake_key, held_ticks);
    assert(passed == before); /* The waking press produced no application event. */
    assert(bsp_power_screen_off() == false);
    now += 1000;
    before = passed;
    real_click(click_key);
    assert(passed - before == 2); /* PRESS + CLICK: the first real press is not swallowed. */
}
/* A key pressed just before the stop stays "pressed" inside the button state machine; its
 * residual release must neither reach the application nor end the wake gesture early. */
static void race_scenario(int pre_key, int wake_key, int click_key, int held_at_exit,
                          unsigned held_ticks) {
    passed = dropped = woke = 0; adc_mv = 3300; poll_timer.armed = 0; timer_running = 1;
    fresh_state_machines(); tick(40);
    adc_mv = key_mv[pre_key]; tick(15);
    assert(bsp_power_enter_screen_off() == 0 && !timer_running);
    adc_mv = 3300; now += 2000;
    fire(); assert(!woke);
    adc_mv = key_mv[wake_key]; now += 50; fire(); assert(woke == 1);
    if (!held_at_exit) adc_mv = 3300;
    assert(bsp_power_exit_screen_off() == 0);
    unsigned before = passed;
    tick(held_ticks); adc_mv = 3300; tick(10);
    assert(passed == before); /* Neither the residual release nor the wake press got through. */
    now += 1000;
    before = passed;
    real_click(click_key);
    assert(passed - before == 2);
}
int main(void) {
    bsp_power_set_wake_callback(on_wake, NULL);
    assert(bsp_button_init(receive, NULL) == 0);
    now = 1000;
    scenario(-1, BSP_BTN_OK, BSP_BTN_UP, 20);                /* Timeout sleep, normal wake tap. */
    scenario(-1, BSP_BTN_OK, BSP_BTN_UP, 1);                 /* Tap shorter than the debounce. */
    scenario(-1, BSP_BTN_UP, BSP_BTN_OK, 140);               /* Held past the long-press time. */
    scenario(BSP_BTN_DOWN, BSP_BTN_DOWN, BSP_BTN_UP, 20);    /* Sleep with DOWN, wake with DOWN. */
    scenario(BSP_BTN_DOWN, BSP_BTN_DOWN, BSP_BTN_UP, 1);
    scenario(BSP_BTN_DOWN, BSP_BTN_UP, BSP_BTN_DOWN, 20);    /* Sleep with DOWN, wake with UP. */
    scenario(BSP_BTN_DOWN, BSP_BTN_OK, BSP_BTN_DOWN, 140);
    race_scenario(BSP_BTN_OK, BSP_BTN_UP, BSP_BTN_DOWN, 1, 20);   /* Press cut by the stop. */
    race_scenario(BSP_BTN_OK, BSP_BTN_UP, BSP_BTN_DOWN, 0, 0);    /* Wake tap already released. */
    race_scenario(BSP_BTN_OK, BSP_BTN_OK, BSP_BTN_DOWN, 1, 20);
    race_scenario(BSP_BTN_DOWN, BSP_BTN_UP, BSP_BTN_OK, 1, 20);
    puts("wake gesture with the locked state machine passed");
}
"""
        )
        compile_and_run(
            harness,
            "quota-wake-gesture-",
            (
                "components/bsp/src/bsp_button.c",
                "components/bsp/src/bsp_power.c",
                "main/quota_logic.c",
                "tests/cjson/cJSON.c",
            ),
            (
                "-DQUOTA_HOST_TEST",
                "-I" + str(ROOT / "tests/bsp_stubs"),
                "-I" + str(ROOT / "components/bsp/include"),
                "-I" + str(ROOT / "components/bsp/src"),
                "-Wno-deprecated-declarations",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
            ),
        )


if __name__ == "__main__":
    unittest.main()
