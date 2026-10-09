"""Draw every device screen with the real LVGL on the host and check what comes out.

tools/ui_preview/ compiles main/quota_ui.c and main/quota_logic.c against LVGL 9.5.0 with the
firmware's LVGL options (RGB565, 240 x 20 partial buffer) and renders
tools/ui_preview/fixtures.json.
This test builds it, runs it and checks that every wireframe screen renders from the one fixed
object pool. It cannot see how the fonts look on the panel or how the QR codes scan: those need the
device."""

import hashlib
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
PREVIEW = ROOT / "tools" / "ui_preview"
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(PREVIEW))
import fetch_lvgl  # noqa: E402
import pngtools  # noqa: E402

# Every wireframe of the redesign, by fixture name.
REQUIRED = {
    "home-chatgpt",
    "home-deepseek",
    "home-welcome",
    "home-clock-unsynced",
    "home-sleep-notice",
    "status-1-storage-error",
    "status-2-authorizing",
    "status-3-reauth",
    "status-4-unverified",
    "status-5-wifi-failed",
    "status-6-rate-limited",
    "status-7-update-failed",
    "status-8-refreshing",
    "status-9-updated",
    "status-10-no-data",
    "menu",
    "options-refresh",
    "options-sleep",
    "hotspot-1",
    "hotspot-2",
    "hotspot-3",
    "hotspot-result-failed",
    "usb-waiting",
    "usb-connected",
    "auth-waiting",
    "info",
    "confirm-factory-reset",
    "confirm-cancel-auth",
    "home-deepseek-chinese",
    "info-chinese-ssid",
    "hotspot-result-chinese",
    "home-deepseek-rare",
    "info-rare-ssid",
    "hotspot-result-rare",
}
STRING_SOURCES = (
    "main/quota_ui.c",
    "main/quota_logic.c",
    "main/setup/app.mjs",
    "main/setup/page.html",
)
FORBIDDEN = (
    "账户管理",
    "立即刷新",
    "网络信息",
    "配对窗口",
    "USB 配对",
    "设置密钥",
    "历史账户",
    "来源",
    "电脑",
)
OUT = ROOT / "build" / "ui-preview"
# The host pool holds 64-bit objects, styles and pointers, which come out about 1.4 times larger
# than on the ESP32-C3 (an allocation-by-allocation 32-bit estimate of this UI gave ~0.7). So the
# device budget of 20 KB of its 24 KB pool corresponds to roughly 28 KB on the host. The cap is a
# regression alarm with real margin, not a measurement: the device number decides.
HOST_PEAK_CAP = 28 * 1024


def literals(text):
    return re.findall(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|>[^<>]+<', text)


class UiPreview(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        run = subprocess.run(
            [sys.executable, str(PREVIEW / "build.py"), "--scale", "1", "--out", str(OUT)],
            capture_output=True,
            text=True,
        )
        cls.log = run.stdout + run.stderr
        cls.returncode = run.returncode
        cls.fixtures = {
            f["name"]: f for f in json.loads((PREVIEW / "fixtures.json").read_text())["fixtures"]
        }
        cls.lines = [l.split() for l in run.stdout.splitlines() if l.startswith("fixture ")]
        cls.summary = dict(
            part.split("=")
            for part in next(
                (l for l in run.stdout.splitlines() if l.startswith("summary ")), "summary"
            ).split()[1:]
        )

    def test_every_wireframe_renders_from_one_pool(self):
        self.assertEqual(self.returncode, 0, self.log)
        self.assertTrue(REQUIRED <= set(self.fixtures), REQUIRED - set(self.fixtures))
        rendered = {line[1] for line in self.lines}
        self.assertEqual(rendered, set(self.fixtures), self.log)
        counts = {dict(p.split("=") for p in line[2:])["objects"] for line in self.lines}
        self.assertEqual(len(counts), 1, "the object count must not change between screens")
        for name in self.fixtures:
            width, height, rows = pngtools.read_png(OUT / "png" / f"{name}.png")
            self.assertEqual((width, height), (240, 320), name)
            colors = {
                rows[y][x * 3 : x * 3 + 3] for y in range(0, height, 3) for x in range(0, width, 3)
            }
            self.assertGreater(len(colors), 6, f"{name} is blank")
            # The device masks the corners black.
            self.assertEqual(rows[0][0:3], b"\x00\x00\x00", name)

    def test_lvgl_memory_is_reported_and_inside_the_pool(self):
        self.assertEqual(self.returncode, 0, self.log)
        pool, peak = int(self.summary["pool"]), int(self.summary["max_used"])
        self.assertGreater(peak, 0)
        self.assertLess(peak, pool)
        # See HOST_PEAK_CAP: a scaled budget, so that a regression shows up here before the device
        # ever runs out.
        self.assertLessEqual(peak, HOST_PEAK_CAP, "UI peak on the host is above the budget")

    def test_sleep_hint_is_on_every_screen(self):
        self.assertEqual(self.returncode, 0, self.log)
        footers = set()
        names = [n for n in self.fixtures if n.endswith("sleep-notice")]
        self.assertGreaterEqual(len(names), 6)
        for name in names:
            _, _, rows = pngtools.read_png(OUT / "png" / f"{name}.png")
            footers.add(b"".join(rows[296:320]))
        self.assertEqual(len(footers), 1, "the hint must look the same on every screen")
        _, _, plain = pngtools.read_png(OUT / "png" / "menu.png")
        self.assertNotEqual(b"".join(plain[296:320]), footers.copy().pop())

    def test_screens_differ_where_they_should(self):
        def pixels(name):
            return (OUT / "png" / f"{name}.png").read_bytes()

        for group in (
            ("hotspot-1", "hotspot-2", "hotspot-3"),
            ("confirm-factory-reset", "confirm-factory-reset-delete"),
            ("menu", "menu-usb-open"),
            ("home-chatgpt", "home-deepseek", "home-welcome"),
            ("usb-waiting", "usb-connected", "usb-ended"),
        ):
            digests = {hashlib.sha256(pixels(name)).hexdigest() for name in group}
            self.assertEqual(len(digests), len(group), group)

    def test_no_forbidden_words_in_interface_text(self):
        for name in STRING_SOURCES:
            text = (ROOT / name).read_text(encoding="utf-8")
            if name.endswith(".c"):
                found = literals(text)
            else:
                found = [text]
            for word in FORBIDDEN:
                for literal in found:
                    self.assertNotIn(word, literal, f"{word} in {name}")

    def test_preview_lvgl_options_are_the_firmware_options(self):
        defaults = (ROOT / "sdkconfig.defaults").read_text()
        preview = (PREVIEW / "sdkconfig.h").read_text()
        for key, value in re.findall(r"^CONFIG_(LV_\w+)=(\w+)", defaults, re.M):
            value = "1" if value == "y" else value
            if value == "n":
                self.assertNotIn(f"#define CONFIG_{key} ", preview)
            elif key == "LV_MEM_SIZE_KILOBYTES":
                self.assertIn(f"#define CONFIG_{key} 64", preview)  # the one host difference
            else:
                self.assertIn(f"#define CONFIG_{key} {value}\n", preview, key)
        self.assertIn("CONFIG_LV_COLOR_DEPTH 16", preview)
        main = (PREVIEW / "main.c").read_text()
        self.assertIn("LV_COLOR_FORMAT_RGB565", main)
        self.assertIn("#define DRAW_LINES 20", main)
        bsp = (ROOT / "components/bsp/src/bsp_display_lvgl.c").read_text()
        self.assertIn("#define BSP_LVGL_DRAW_BUFFER_LINES 20", bsp)

    def test_lvgl_download_is_checked_against_the_pinned_digest(self):
        self.assertEqual(len(fetch_lvgl.SHA256), 64)
        self.assertTrue(
            fetch_lvgl.has_version(fetch_lvgl.COMPONENT) or fetch_lvgl.COMPONENT.exists() is False
        )

        class Response:
            def __enter__(self):
                return self

            def __exit__(self, *args):
                return False

            def read(self):
                return b"not the archive"

        with tempfile.TemporaryDirectory() as directory, mock.patch.object(
            fetch_lvgl.urllib.request, "urlopen", return_value=Response()
        ):
            with self.assertRaises(SystemExit) as raised:
                fetch_lvgl.fetch(Path(directory))
            self.assertIn("checksum mismatch", str(raised.exception))
            self.assertFalse(list(Path(directory).glob("*.tar.gz")))


if __name__ == "__main__":
    unittest.main()
