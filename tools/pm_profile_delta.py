#!/usr/bin/env python3
"""Light-sleep share of a screen-off run, from the profiling build's serial output.

The profiling firmware (sdkconfig.profiling) keeps two snapshots of the PM and esp_timer statistics
in RAM, one when the screen turns off and one 60 s later, and prints both repeatedly after the next
wake. Measure on battery with USB unplugged, wake the screen, then plug USB in and capture the log:

    python3 tools/pm_profile_delta.py console.log
"""

import re
import sys

START = "=== screen-off profile: start ==="
LATER = "=== screen-off profile: +60 s ==="
END = "=== end of screen-off profile ==="

# IDF prints the frequency as "%-3uM": "160M" but "40 M" at 40 MHz.
MODE_ROW = re.compile(r"^(SLEEP|APB_MIN|APB_MAX|CPU_MAX)\s+\d+\s*M\s+(\d+)\s", re.M)
SLEEP_COUNTS = re.compile(r"light_sleep_counts:(\d+)\s+light_sleep_reject_counts:(\d+)")
# esp_timer_dump with profiling: name, period, alarm, armed, triggered, skipped, callback time.
TIMER_ROW = re.compile(r"^(\S.{0,19}?)\s+(\d+)\s+(-?\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*$", re.M)


def parse(block):
    modes = {name: int(us) for name, us in MODE_ROW.findall(block)}
    counts = SLEEP_COUNTS.search(block)
    timers = {
        m[0].strip(): {"armed": int(m[3]), "triggered": int(m[4]), "callback_us": int(m[6])}
        for m in TIMER_ROW.findall(block)
    }
    return {
        "modes": modes,
        "sleeps": int(counts[1]) if counts else None,
        "rejects": int(counts[2]) if counts else None,
        "timers": timers,
    }


def split(text):
    """Return (start block, later block) of the last complete print in the log."""
    ends = text.rfind(END)
    if ends < 0:
        raise ValueError("no complete screen-off profile in the log")
    begin = text.rfind(START, 0, ends)
    middle = text.find(LATER, begin)
    if begin < 0 or middle < 0:
        raise ValueError("incomplete screen-off profile in the log")
    return text[begin + len(START) : middle], text[middle + len(LATER) : ends]


def delta(text):
    first, second = (parse(block) for block in split(text))
    modes = {m: second["modes"][m] - first["modes"].get(m, 0) for m in second["modes"]}
    elapsed = sum(modes.values())
    if elapsed <= 0 or "SLEEP" not in modes:
        raise ValueError("no light-sleep time recorded (is CONFIG_PM_PROFILING on?)")
    timers = {}
    for name, now in second["timers"].items():
        before = first["timers"].get(name, {"triggered": 0, "callback_us": 0})
        timers[name] = {
            "triggered": now["triggered"] - before["triggered"],
            "callback_us": now["callback_us"] - before["callback_us"],
        }
    return {
        "elapsed_us": elapsed,
        "modes_us": modes,
        "sleep_ratio": modes["SLEEP"] / elapsed,
        "light_sleeps": None if second["sleeps"] is None else second["sleeps"] - first["sleeps"],
        "rejected": None if second["rejects"] is None else second["rejects"] - first["rejects"],
        "timers": timers,
    }


def main(argv):
    text = open(argv[1]).read() if len(argv) > 1 else sys.stdin.read()
    result = delta(text)
    print(f"window: {result['elapsed_us'] / 1e6:.1f} s")
    for mode, us in result["modes_us"].items():
        print(f"  {mode:8s} {us / 1e6:8.2f} s  {100 * us / result['elapsed_us']:5.1f}%")
    print(f"light sleep share: {100 * result['sleep_ratio']:.1f}% (acceptance: >= 90%)")
    print(f"light sleeps: {result['light_sleeps']}  rejected: {result['rejected']}")
    print("timers that fired in the window:")
    for name, row in sorted(result["timers"].items(), key=lambda kv: -kv[1]["triggered"]):
        if row["triggered"]:
            rate = row["triggered"] / (result["elapsed_us"] / 1e6)
            print(f"  {name:20s} {row['triggered']:6d} ({rate:.1f}/s)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
