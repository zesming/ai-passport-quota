"""Execute the firmware's refresh paths with a fake transport, without ESP-IDF."""
import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


class RefreshRuntime(unittest.TestCase):
    def test_cache_polls_are_quiet_and_source_refreshes_are_busy(self):
        source = (ROOT / "main/quota_service.c").read_text()
        functions = []
        for name in ("finish_refresh", "perform_refresh", "perform_snapshot_fetch"):
            match = re.search(r"^static void " + name + r"\(.*?^\}", source, re.M | re.S)
            self.assertIsNotNone(match, name)
            functions.append(match[0])
        harness = r'''
#include "quota_logic.h"
#include <assert.h>
#include <stdio.h>
typedef struct {
    quota_snapshot_t snapshot;
    bool snapshot_valid, refreshing, request_failed;
    uint64_t now_epoch;
} view_t;
static view_t s_view;
static quota_snapshot_t s_snapshot_work;
enum { QUOTA_APP_EVENT_SNAPSHOT };
static bool transport_ok, refresh_accepted, expect_busy, current_generation;
static unsigned fetches, posts, events;
static void mutex_lock(void) {}
static void mutex_unlock(void) {}
static uint64_t current_epoch(void) { return 1700000000; }
static void post_simple_event(int kind) { (void)kind; events++; }
static bool config_generation_is_current(uint32_t generation) {
    (void)generation; return current_generation;
}
static bool fetch_snapshot(const quota_device_config_t *config, quota_snapshot_t *snapshot) {
    (void)config; assert(s_view.refreshing == expect_busy); fetches++;
    snapshot->revision = 2; return transport_ok;
}
static bool publish_snapshot(const quota_snapshot_t *snapshot, uint32_t generation) {
    if (!config_generation_is_current(generation)) return false;
    s_view.snapshot = *snapshot; s_view.snapshot_valid = true;
    s_view.request_failed = false; post_simple_event(QUOTA_APP_EVENT_SNAPSHOT); return true;
}
static bool request_refresh(const quota_device_config_t *config) {
    (void)config; assert(s_view.refreshing); posts++; return refresh_accepted;
}
#define pdMS_TO_TICKS(ms) (ms)
static void vTaskDelay(unsigned ticks) { (void)ticks; }
'''
        harness += "\n".join(functions)
        harness += r'''
int main(void) {
    quota_device_config_t config = {0};
    current_generation = true; transport_ok = true; refresh_accepted = true;
    s_view.snapshot_valid = true; s_view.snapshot.revision = 1;
    for (unsigned i = 0; i < 3; i++) perform_snapshot_fetch(&config, 1);
    assert(fetches == 3 && posts == 0 && events == 6);
    assert(!s_view.refreshing && !s_view.request_failed && s_view.snapshot.revision == 2);
    transport_ok = false;
    perform_snapshot_fetch(&config, 1);
    assert(s_view.request_failed && s_view.snapshot_valid && s_view.snapshot.revision == 2);
    transport_ok = true;
    perform_snapshot_fetch(&config, 1);
    assert(!s_view.request_failed);

    expect_busy = true; s_view.snapshot.revision = 1;
    perform_refresh(&config, 1);
    assert(posts == 1 && !s_view.refreshing && !s_view.request_failed);
    unsigned previous_fetches = fetches;
    refresh_accepted = false;
    perform_refresh(&config, 1);
    assert(posts == 2 && s_view.request_failed && !s_view.refreshing);
    assert(fetches == previous_fetches && s_view.snapshot.revision == 2);
    /* Rejected old-generation poll cannot replace the cache or clear its failure. */
    current_generation = false; expect_busy = false;
    perform_snapshot_fetch(&config, 1);
    assert(s_view.request_failed && s_view.snapshot.revision == 2);
    puts("quota refresh runtime tests passed");
}
'''
        with tempfile.TemporaryDirectory(prefix="ai-quota-refresh-test-") as directory:
            path = Path(directory)
            (path / "test.c").write_text(harness)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                            "-Werror", "-I" + str(ROOT / "main"), str(path / "test.c"),
                            "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
