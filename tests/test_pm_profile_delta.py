"""Check the screen-off profile delta against output laid out like the IDF dumps."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import pm_profile_delta  # noqa: E402


def mode_row(name, mhz, time_us, percent):
    # esp_pm_impl_dump_stats: "%-8s  %-3uM%-7s %-10lld  %-2d%%" ("40 M" at 40 MHz).
    return f"{name:<8}  {mhz:<3}M{'':<7} {time_us:<10d}  {percent:<2d}%\n"


def timer_row(name, period, alarm, armed, triggered, skipped, callback_us):
    # esp_timer_dump with profiling: "%-20.20s  %-10lld  %-12lld  %-12d  %-12d  %-12d  %-12lld".
    return (
        f"{name:<20.20}  {period:<10d}  {alarm:<12d}  {armed:<12d}  {triggered:<12d}  "
        f"{skipped:<12d}  {callback_us:<12d}\n"
    )


def snapshot(sleep_us, cpu_us, wake_poll, sleeps):
    return (
        "\nMode stats:\n"
        f"{'Mode':<8}  {'CPU_freq':<10}  {'Time(us)':<10}  {'Time(%)':<10}\n"
        + mode_row("SLEEP", 160, sleep_us, 50)
        + mode_row("APB_MIN", 40, 0, 0)
        + mode_row("APB_MAX", 160, 0, 0)
        + mode_row("CPU_MAX", 160, cpu_us, 50)
        + "\nSleep stats:\n"
        + f"light_sleep_counts:{sleeps}  light_sleep_reject_counts:2\n"
        + "Timer stats:\n"
        + f"{'Name':<20}  {'Period':<10}  {'Alarm':<12}  {'Times_armed':<12}  {'Times_trigg':<12}  "
        + f"{'Times_skip':<12}  {'Cb_exec_time':<12}\n"
        + timer_row("bsp_wake_poll", 0, 123456, wake_poll, wake_poll, 0, wake_poll * 30)
        + timer_row("button_timer", 5000, 0, 10, 10, 0, 100)
    )


LOG = (
    "I (1) boot: noise\n=== screen-off profile: start ===\n"
    + snapshot(1_000_000, 2_000_000, 10, 4)
    + "\n=== screen-off profile: +60 s ===\n"
    + snapshot(55_000_000, 2_300_000, 1210, 1214)
    + "\n=== end of screen-off profile ===\n"
)


class ProfileDelta(unittest.TestCase):
    def test_sleep_share_and_timer_rates(self):
        result = pm_profile_delta.delta(LOG)
        self.assertEqual(result["modes_us"]["SLEEP"], 54_000_000)
        self.assertEqual(result["modes_us"]["CPU_MAX"], 300_000)
        self.assertAlmostEqual(result["sleep_ratio"], 54_000_000 / 54_300_000)
        self.assertEqual(result["light_sleeps"], 1210)
        self.assertEqual(result["rejected"], 0)
        self.assertEqual(result["timers"]["bsp_wake_poll"]["triggered"], 1200)
        self.assertEqual(result["timers"]["button_timer"]["triggered"], 0)

    def test_repeated_prints_use_the_last_complete_one(self):
        result = pm_profile_delta.delta(LOG + LOG[:60])
        self.assertEqual(result["light_sleeps"], 1210)

    def test_40_mhz_row_has_a_space_before_the_unit(self):
        self.assertIn("APB_MIN   40 M", LOG)
        modes = pm_profile_delta.parse(snapshot(1, 2, 3, 4))["modes"]
        self.assertEqual(sorted(modes), ["APB_MAX", "APB_MIN", "CPU_MAX", "SLEEP"])

    def test_missing_profile_is_an_error(self):
        with self.assertRaises(ValueError):
            pm_profile_delta.delta("no profile here")
        with self.assertRaises(ValueError):
            pm_profile_delta.delta(LOG.replace("=== end of screen-off profile ===", ""))


if __name__ == "__main__":
    unittest.main()
