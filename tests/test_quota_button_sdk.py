"""Run the locked button state machine with BSP callbacks and real wake logic."""
import re
import unittest
from runtime_helpers import ROOT, compile_and_run, extract_function


class ButtonSdk(unittest.TestCase):
    def test_rapid_taps_long_press_and_wake(self):
        vendor = (ROOT / "managed_components/espressif__button/iot_button.c").read_text()
        header = (ROOT / "managed_components/espressif__button/include/iot_button.h").read_text()
        bsp = (ROOT / "components/bsp/src/bsp_button.c").read_text()
        bsp_header = (ROOT / "components/bsp/include/bsp_button.h").read_text()
        defaults = (ROOT / "sdkconfig.defaults").read_text()
        for setting, value in (("BUTTON_PERIOD_TIME_MS", 5), ("BUTTON_DEBOUNCE_TICKS", 2),
                               ("BUTTON_LONG_PRESS_HOLD_SERIAL_TIME_MS", 20)):
            self.assertRegex(defaults, rf"(?m)^CONFIG_{setting}={value}$")
        def block(source, start, end):
            return re.search(re.escape(start) + r".*?" + re.escape(end), source, re.S)[0]
        types = "\n".join((
            block(header, "typedef enum {\n    BUTTON_PRESS_DOWN", "} button_event_t;"),
            block(header, "typedef union {", "} button_event_args_t;"),
            block(vendor, "enum {\n    PRESS_DOWN_CHECK", "};"),
            block(vendor, "typedef struct {\n    button_cb_t cb;", "} button_cb_info_t;"),
            block(vendor, "typedef struct button_dev_t {", "} button_dev_t;"),
            block(bsp_header, "typedef enum {\n    BSP_BTN_UP", "} bsp_btn_t;"),
            block(bsp_header, "typedef enum {\n    BSP_BTN_PRESS", "} bsp_btn_ev_t;")))
        callbacks = "\n".join(re.search(r"^static void " + name + r"\([^;]*?\)\s*\{.*?^\}",
            bsp, re.M | re.S)[0] for name in ("on_event", "cb_press", "cb_long", "cb_release"))
        harness = r'''
#include "quota_logic.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void (*button_cb_t)(void *,void *);
typedef struct button_driver_t { uint8_t (*get_key_level)(struct button_driver_t *); } button_driver_t;
'''+types+r'''
#define BUTTON_ACTIVE 1
#define TICKS_INTERVAL 5
#define DEBOUNCE_TICKS 2
#define SERIAL_TICKS 4
#define TOLERANCE 20
#define BSP_BTN_LONG_PRESS_MS 500
#define CALL_EVENT_CB(ev) do { for(size_t j=0;j<btn->size[ev];j++) btn->cb_info[ev][j].cb(btn,btn->cb_info[ev][j].usr_data); } while(0)
static uint32_t iot_button_get_pressed_time(button_dev_t *b) { return b->ticks*TICKS_INTERVAL; }
static bool s_ready=true, s_long_pressed[3];
static void (*s_cb)(bsp_btn_t,bsp_btn_ev_t,void *);
static void *s_user;
'''+callbacks+'\n#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored "-Wsign-compare"\n'+extract_function(vendor, "button_handler")+r'''
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
    button_cb_info_t down[1]={{.cb=cb_press,.usr_data=(void *)(intptr_t)key}};
    button_cb_info_t up[1]={{.cb=cb_release,.usr_data=(void *)(intptr_t)key}};
    button_cb_info_t hold[1]={{.cb=cb_long,.usr_data=(void *)(intptr_t)key}};
    button_dev_t b={.driver=&driver,.long_press_ticks=100,.short_press_ticks=0};
    b.cb_info[BUTTON_PRESS_DOWN]=down; b.size[BUTTON_PRESS_DOWN]=1;
    b.cb_info[BUTTON_PRESS_UP]=up; b.size[BUTTON_PRESS_UP]=1;
    b.cb_info[BUTTON_LONG_PRESS_HOLD]=hold; b.size[BUTTON_LONG_PRESS_HOLD]=1;
    display=(quota_display_state_t){0}; presses=clicks=longs=navigations=0;
    gesture(&b,2); assert(clicks==1); /* CLICK emitted on release, before any wait. */
    gesture(&b,2); assert(clicks==2 && presses==2 && longs==0);
    gesture(&b,110); assert(longs==1 && clicks==2); /* Fast following hold remains LONG. */
    if(key==BSP_BTN_DOWN) {
        assert(display.sleeping);
        gesture(&b,2); assert(!display.sleeping && !display.consume_wake_gesture);
    }
    ticks(&b,0,40);
    display=(quota_display_state_t){.sleeping=true}; navigations=0;
    gesture(&b,2); assert(!display.sleeping && navigations==0);
    gesture(&b,2); assert(navigations==1);
    ticks(&b,0,40);
    display=(quota_display_state_t){.sleeping=true}; navigations=0;
    gesture(&b,110); assert(!display.sleeping && navigations==0);
    gesture(&b,2); assert(navigations==1);
    ticks(&b,0,40); unsigned before=clicks;
    ticks(&b,1,1); ticks(&b,0,2); assert(clicks==before); /* bounce */
    unsigned durations[]={99,100,103,140}; /* release at 495/500/515/700 ms */
    for(unsigned i=0;i<4;i++) {
        ticks(&b,0,40); display=(quota_display_state_t){0};
        unsigned old_clicks=clicks,old_longs=longs;
        gesture(&b,durations[i]);
        assert(clicks==old_clicks+(i==0) && longs==old_longs+(i!=0));
        assert(display.sleeping==(key==BSP_BTN_DOWN && i!=0));
    }
}
int main(void) {
    s_cb=receive;
    run(BSP_BTN_UP); run(BSP_BTN_DOWN); run(BSP_BTN_OK);
    puts("locked button rapid taps, second hold, sleep/wake and bounce passed");
}
'''
        compile_and_run(harness, "quota-button-sdk-", ("main/quota_logic.c", "tests/cjson/cJSON.c"),
                        flags=("-Wno-deprecated-declarations", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"))


if __name__ == "__main__":
    unittest.main()
