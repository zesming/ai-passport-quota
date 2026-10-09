#!/usr/bin/env python3
"""Build the host UI preview and draw every fixture to a PNG.

    python3 tools/ui_preview/build.py [--out build/ui-preview] [--scale 2] [--no-render]

The preview compiles main/quota_ui.c, main/quota_logic.c, the subset fonts and the brand images
against LVGL (managed_components/lvgl__lvgl, or the pinned download of tools/fetch_lvgl.py) with
the firmware's LVGL options (tools/ui_preview/sdkconfig.h). It prints one line per fixture with
the LVGL memory in use; see docs/development/README.md for how to read the peak."""

import argparse
import concurrent.futures
import hashlib
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))
import fetch_lvgl  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pngtools  # noqa: E402

CC = os.environ.get("CC", "cc")
COMMON = [
    "-std=gnu11",
    "-O1",
    "-g0",
    "-DLV_LVGL_H_INCLUDE_SIMPLE",
    # The firmware's LVGL options come from its sdkconfig, as in the ESP-IDF build.
    f'-DLV_CONF_KCONFIG_EXTERNAL_INCLUDE="{HERE / "sdkconfig.h"}"',
    f"-I{HERE}",
]
# Our own sources are built strictly; LVGL is third-party code and only has to compile.
OWN_FLAGS = ["-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-DQUOTA_HOST_TEST"]
OWN_SOURCES = [
    "main/quota_ui.c",
    "main/quota_logic.c",
    "main/quota_brand_assets.c",
    "assets/fonts/quota_font_12.c",
    "assets/fonts/quota_font_16.c",
    "components/bsp/src/bsp_display_rounding.c",
    "tests/cjson/cJSON.c",
    "tools/ui_preview/main.c",
]


def compile_one(job):
    source, obj, flags = job
    stamp = obj.with_suffix(".key")
    key = hashlib.sha256(" ".join([CC, *flags, str(source)]).encode()).hexdigest()
    newest = max(source.stat().st_mtime, *(p.stat().st_mtime for p in HERE.glob("*.h")))
    if (
        obj.is_file()
        and stamp.is_file()
        and stamp.read_text() == key
        and obj.stat().st_mtime >= newest
    ):
        return None
    obj.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [CC, *flags, "-c", str(source), "-o", str(obj)], capture_output=True, text=True
    )
    if result.returncode:
        return f"{source}:\n{result.stderr}"
    stamp.write_text(key)
    return None


def build(out):
    lvgl = fetch_lvgl.lvgl_path()
    headers = [f"-I{lvgl}"]
    own_includes = [
        f"-I{ROOT / 'main'}",
        f"-I{ROOT / 'tests/cjson'}",
        f"-I{ROOT / 'components/bsp/src'}",
        f"-I{ROOT / 'components/bsp/include'}",
        f"-I{ROOT / 'tests/bsp_stubs'}",
        f"-I{ROOT / 'tests/host_sdk'}",
    ]
    obj_dir = out / "obj"
    jobs = []
    for source in sorted((lvgl / "src").rglob("*.c")):
        jobs.append(
            (
                source,
                obj_dir / "lvgl" / source.relative_to(lvgl).with_suffix(".o"),
                [*COMMON, *headers, "-w"],
            )
        )
    for name in OWN_SOURCES:
        flags = [*COMMON, *headers, *own_includes, *OWN_FLAGS]
        if name.startswith("tests/cjson"):
            flags = [*COMMON, *headers, *own_includes, "-w"]
        elif name.startswith("assets/"):
            flags = [*COMMON, *headers, *own_includes, "-w"]
        jobs.append((ROOT / name, obj_dir / "own" / Path(name).with_suffix(".o"), flags))
    errors = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for error in pool.map(compile_one, jobs):
            if error:
                errors.append(error)
    if errors:
        raise SystemExit("\n".join(errors))
    binary = out / "ui_preview"
    objects = sorted(str(job[1]) for job in jobs)
    link = subprocess.run([CC, *objects, "-lm", "-o", str(binary)], capture_output=True, text=True)
    if link.returncode:
        raise SystemExit(link.stderr)
    return binary


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", default=str(ROOT / "build" / "ui-preview"))
    parser.add_argument("--scale", type=int, default=2)
    parser.add_argument("--fixtures", default=str(HERE / "fixtures.json"))
    parser.add_argument("--no-render", action="store_true")
    parser.add_argument(
        "--sheets", action="store_true", help="compress the PNGs and tile them into sheet-N.png"
    )
    args = parser.parse_args()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    binary = build(out)
    if args.no_render:
        return 0
    shots = out / "png"
    shots.mkdir(exist_ok=True)
    for stale in shots.glob("*.png"):
        stale.unlink()
    status = subprocess.run([str(binary), args.fixtures, str(shots), str(args.scale)]).returncode
    if status == 0 and args.sheets:
        pngtools.shrink(shots)
        for sheet, names in pngtools.contact_sheets(shots):
            print(f"{sheet.name}: {', '.join(names)}")
    return status


if __name__ == "__main__":
    sys.exit(main())
