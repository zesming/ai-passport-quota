"""Check the screen-off profile delta against output laid out like the IDF dumps."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import pm_profile_delta  # noqa: E402


def snapshot(sleep_us, cpu_us, wake_poll, sleeps):
    return (
        "\nMode stats:\n"
        "Mode      CPU_freq    Time(us)    Time(%)   \n"
        f"SLEEP     160M        {sleep_us:<10d}  50%\n"
        "APB_MIN   80M         0           0 %\n"
        "APB_MAX   160M        0           0 %\n"
        f"CPU_MAX   160M        {cpu_us:<10d}  50%\n"
        "\nSleep stats:\n"
        f"light_sleep_counts:{sleeps}  light_sleep_reject_counts:2\n"
        "Timer stats:\n"
        "Name                  Period      Alarm         Times_armed   Times_trigg   Times_skip  "
        "  Cb_exec_time\n"
        f"bsp_wake_poll         0           123456        {wake_poll:<12d}  {wake_poll:<12d}  "
        "0             "
        f"{wake_poll * 30:<12d}\n"
        "button_timer          5000        0             10            10            0"
        "             100         \n"
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

    def test_missing_profile_is_an_error(self):
        with self.assertRaises(ValueError):
            pm_profile_delta.delta("no profile here")
        with self.assertRaises(ValueError):
            pm_profile_delta.delta(LOG.replace("=== end of screen-off profile ===", ""))


if __name__ == "__main__":
    unittest.main()
