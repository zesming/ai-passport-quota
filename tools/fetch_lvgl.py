#!/usr/bin/env python3
"""Print the path of an LVGL 9.5.0 source tree for the host UI preview.

Uses the locked component in managed_components/ when it is there; otherwise downloads the
GitHub release archive into build/lvgl/ and checks its SHA-256 before unpacking."""

import hashlib
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VERSION = "9.5.0"
URL = f"https://github.com/lvgl/lvgl/archive/refs/tags/v{VERSION}.tar.gz"
# GitHub publishes no checksum for tag archives. This value was computed by the repository's own
# download of the v9.5.0 archive; src/lv_conf_internal.h in it is identical to the one in the
# locked component managed_components/lvgl__lvgl (9.5.0, commit 85aa60d). So it pins "the archive
# that was reviewed", it is not an upstream-signed digest. Changing LVGL means recomputing it.
SHA256 = "34a955cdf3a2d005507b704e87357af669a114523b6d3f77b5344fdc68717bc6"
COMPONENT = ROOT / "managed_components" / "lvgl__lvgl"


def has_version(path):
    header = path / "lv_version.h"
    if not header.is_file():
        return False
    text = header.read_text(encoding="utf-8")
    return all(
        f"#define LVGL_VERSION_{name} {value}" in text
        for name, value in zip(("MAJOR", "MINOR", "PATCH"), VERSION.split("."))
    )


def fetch(destination):
    destination.mkdir(parents=True, exist_ok=True)
    archive = destination / f"lvgl-{VERSION}.tar.gz"
    if not archive.is_file() or hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        print(f"downloading {URL}", file=sys.stderr)
        with urllib.request.urlopen(URL, timeout=120) as response:
            data = response.read()
        digest = hashlib.sha256(data).hexdigest()
        if digest != SHA256:
            raise SystemExit(f"LVGL archive checksum mismatch: {digest}")
        archive.write_bytes(data)
    with tarfile.open(archive) as bundle:
        for member in bundle.getmembers():  # refuse paths that leave the destination
            target = (destination / member.name).resolve()
            if destination.resolve() not in target.parents and target != destination.resolve():
                raise SystemExit(f"unsafe path in LVGL archive: {member.name}")
        bundle.extractall(destination, filter="data")
    return destination / f"lvgl-{VERSION}"


def lvgl_path():
    if has_version(COMPONENT):
        return COMPONENT
    unpacked = ROOT / "build" / "lvgl" / f"lvgl-{VERSION}"
    if has_version(unpacked):
        return unpacked
    return fetch(ROOT / "build" / "lvgl")


if __name__ == "__main__":
    print(lvgl_path())
