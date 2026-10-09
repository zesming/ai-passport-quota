"""Run the locked button state machine with BSP callbacks and real wake logic."""
import re
import unittest
from runtime_helpers import ROOT, compile_and_run, vendor_function


class ButtonSdk(unittest.TestCase):
    def test_rapid_taps_long_press_and_wake(self):
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
        harness = (
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
    (void)driver;
    assert(created < BSP_BTN_COUNT);
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


if __name__ == "__main__":
    unittest.main()
