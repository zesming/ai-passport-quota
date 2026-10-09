"""The firmware has one common owner; provider cadence is in the full controller harness."""
import unittest
from runtime_helpers import compile_and_run


class RefreshRuntime(unittest.TestCase):
    def test_one_owner_sleep_and_usb_cleanup_order(self):
        harness = r'''
#include "quota_service.h"
#include "quota_usb.h"
#include "quota_wifi.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

void network_task(void *arg);

static jmp_buf done;
static char order[64];
static unsigned owner_ticks, stops, waits;
static bool wifi_running = true;

static void note(char step) { size_t n = strlen(order); order[n] = step; order[n + 1] = 0; }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait) {
    assert(clear == pdTRUE);
    /* Awake, or asleep with the radio still up: bounded wait. Radio stopped: wait for a wake. */
    if (owner_ticks < 2) assert(wait == 500);
    else assert(wait == portMAX_DELAY && !wifi_running);
    waits++;
    if (owner_ticks == 3) longjmp(done, 1);
    return 0;
}
bool quota_wifi_started(void) { return wifi_running; }
bool quota_wifi_stop(void) {
    assert(quota_service_display_sleeping());
    note('S'); stops++; wifi_running = false; return true;
}
void quota_usb_poll(bool sleeping) { (void)sleeping; note('U'); }
void quota_usb_fill_view_locked(quota_service_view_t *view) { (void)view; note('V'); }
void quota_portable_service_tick(bool sleeping, uint32_t generation) {
    assert(generation >= 1 && sleeping == quota_service_display_sleeping());
    note('P'); owner_ticks++;
    if (owner_ticks == 1) quota_service_set_display_sleeping(true);
}

int main(void) {
    if (!setjmp(done)) network_task(NULL);
    /* Iteration 1 is awake, 2 and 3 asleep: USB before and after the controller, radio last. */
    assert(owner_ticks == 3 && stops == 2 && waits == 4);
    assert(!strcmp(order, "UPUVUPSUVUPSUV"));
    puts("one common owner sleep/USB order passed");
}
'''
        compile_and_run(harness, "ai-quota-common-owner-", ("main/quota_service.c",), host_sdk=True)

    def test_sleeping_closes_the_usb_window_once_per_transition(self):
        harness = r"""
#include "quota_service.h"
#include <assert.h>
#include <stdio.h>

static unsigned closes;
void quota_usb_close_window(void) { closes++; }

int main(void) {
    assert(quota_service_init());
    quota_service_set_display_sleeping(false);
    assert(closes == 0); /* Awake already: nothing changes. */
    quota_service_set_display_sleeping(true);
    assert(closes == 1); /* Screen off ends the USB window. */
    quota_service_set_display_sleeping(true);
    assert(closes == 1); /* Repeats are ignored. */
    quota_service_set_display_sleeping(false);
    assert(closes == 1);
    quota_service_set_display_sleeping(true);
    assert(closes == 2);
    puts("sleep closes USB window passed");
}
"""
        compile_and_run(harness, "ai-quota-sleep-usb-", ("main/quota_service.c",), host_sdk=True)


if __name__ == "__main__":
    unittest.main()
