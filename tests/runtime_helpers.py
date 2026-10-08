"""Compile firmware sources whole against host stubs and run their C harnesses."""

import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HOST_SDK_FLAGS = (
    "-DQUOTA_HOST_TEST",
    "-I" + str(ROOT / "tests/host_sdk"),
    "-I" + str(ROOT / "tests/bsp_stubs"),
    "-I" + str(ROOT / "components/bsp/include"),
)
HOST_SDK_SOURCES = ("tests/host_sdk/host_sdk_defaults.c", "tests/host_sdk/module_defaults.c")


def vendor_function(source, name, declaration=None):
    """Copy one function out of third-party (ESP-IDF or managed component) source text.

    Firmware sources never use this: they export their internals with QUOTA_TESTABLE and are
    compiled whole. Vendor files cannot carry the macro and need their full SDK to compile."""
    start = (
        r"^static\s+(?:void|bool|uint32_t|uint16_t)\s+"
        if declaration is None
        else r"^" + re.escape(declaration) + r"\s+"
    )
    match = re.search(start + re.escape(name) + r"\s*\([^;]*?\)\s*\n\{.*?^\}", source, re.M | re.S)
    if match is None:
        raise AssertionError(f"missing vendor function: {name}")
    return match[0]


def compile_and_run(harness, prefix, sources=(), flags=(), host_sdk=False):
    """Build harness plus whole source files.

    host_sdk adds the ESP-IDF/FreeRTOS stubs, QUOTA_HOST_TEST
    (which exports QUOTA_TESTABLE symbols) and the weak default stub implementations."""
    if host_sdk:
        flags = (*HOST_SDK_FLAGS, *flags)
        sources = (*sources, *HOST_SDK_SOURCES)
    with tempfile.TemporaryDirectory(prefix=prefix) as directory:
        path = Path(directory)
        (path / "test.c").write_text(harness)
        subprocess.run(
            [
                os.environ.get("CC", "cc"),
                "-std=c11",
                "-D_DEFAULT_SOURCE",  # glibc hides setenv, tzset and localtime_r under -std=c11
                "-Wall",
                "-Wextra",
                "-Werror",
                *flags,
                "-I" + str(ROOT / "main"),
                "-I" + str(ROOT / "tests/cjson"),
                str(path / "test.c"),
                *(str(ROOT / name) for name in sources),
                "-lm",
                "-o",
                str(path / "test"),
            ],
            check=True,
        )
        subprocess.run([str(path / "test")], check=True)
