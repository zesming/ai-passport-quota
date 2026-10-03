"""Extract real firmware functions and execute their host C harnesses."""
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def extract_function(source, name, declaration=None):
    start = r"^static\s+(?:void|bool|uint32_t|uint16_t)\s+" if declaration is None else \
        r"^" + re.escape(declaration) + r"\s+"
    match = re.search(start + re.escape(name) + r"\s*\([^;]*?\)\s*\n\{.*?^\}",
                      source, re.M | re.S)
    if match is None:
        raise AssertionError(f"missing firmware function: {name}")
    return match[0]


def compile_and_run(harness, prefix, sources=()):
    with tempfile.TemporaryDirectory(prefix=prefix) as directory:
        path = Path(directory)
        (path / "test.c").write_text(harness)
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                        "-Werror", "-I" + str(ROOT / "main"), "-I" + str(ROOT / "tests/cjson"),
                        str(path / "test.c"), *(str(ROOT / name) for name in sources),
                        "-lm", "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)
