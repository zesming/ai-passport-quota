"""The firmware has one common owner; provider cadence is in the full controller harness."""
import unittest
from runtime_helpers import ROOT, extract_function, compile_and_run


class RefreshRuntime(unittest.TestCase):
    def test_one_owner_sleep_and_pairing_cleanup_order(self):
        source = (ROOT / "main/quota_service.c").read_text()
        function = extract_function(source, "network_task")
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#define ESP_PLATFORM 1
#define pdTRUE 1
#define QUOTA_PAIRING_WINDOW_MS 120000
static jmp_buf done;
typedef struct {bool sleeping; uint32_t generation;} display_state_t;
static struct {bool pairing_active;uint32_t pairing_seconds_left;} s_view;
static int64_t s_pairing_opened_at_ms;
static bool sleeping,pairing;
static unsigned owner_ticks,pairing_ticks,stops,phase;
static display_state_t display_state_snapshot(void){return (display_state_t){.sleeping=sleeping,.generation=7};}
static unsigned network_wait(bool value){return value?999:500;}
static void ulTaskNotifyTake(int clear,unsigned wait){assert(clear==1);assert(wait==(sleeping?999U:500U));if(owner_ticks==3)longjmp(done,1);phase=0;}
static bool pairing_requested(void){return pairing;}
static uint64_t monotonic_ms(void){return 1000;}
static bool pairing_active_locked(uint64_t now){(void)now;return false;}
static void mutex_lock(void){}
static void mutex_unlock(void){}
static void service_pairing_tick(bool value){(void)value;pairing_ticks++;if(!pairing)assert(phase==0||phase==1);}
static void stop_wifi_for_sleep(void){assert(sleeping);stops++;}
static void quota_portable_service_tick(bool value,uint32_t generation){assert(generation==7&&value==sleeping);assert(pairing||pairing_ticks);phase=1;owner_ticks++;if(owner_ticks==1)sleeping=true;if(owner_ticks==2)pairing=true;}
'''
        harness += function
        harness += r'''
int main(void){if(!setjmp(done))network_task(NULL);assert(owner_ticks==3&&pairing_ticks==5&&stops==2);puts("one common owner sleep/pairing order passed");}
'''
        compile_and_run(harness, "ai-quota-common-owner-")


if __name__ == "__main__":
    unittest.main()
