#!/usr/bin/env python3
"""Dependency-free repository checks shared by local development and CI."""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
import unicodedata
from pathlib import Path
from urllib.parse import unquote


ROOT = Path(__file__).resolve().parent.parent
FULL_SHA_RE = re.compile(r"^[0-9a-f]{40}$")
MARKDOWN_LINK_RE = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
HEADING_RE = re.compile(r"^ {0,3}#{1,6}[ \t]+(.+?)[ \t]*#*[ \t]*$", re.M)
LANGUAGE_LINK_RE = re.compile(r"\]\([^)]*\.(?:zh_CN|en_US)\.md|href=\"[^\"]*\.(?:zh_CN|en_US)\.md|\[English\]|\[简体中文\]", re.I)
LINE_LIMIT = 100
LINE_LIMIT_ROOTS = ("main/", "components/", "tests/")
# Third-party code and generated files.
LINE_LIMIT_EXEMPT = (
    "tests/cjson/",
    "main/quota_brand_assets.c",
    "main/setup_page.html",
)
SECRET_PATTERNS = {
    "GitHub token": re.compile(r"(?:ghp_|github_pat_)[A-Za-z0-9_]{20,}"),
    "AWS access key": re.compile(r"AKIA[0-9A-Z]{16}"),
    "private key": re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
}

def git_files() -> list[Path]:
    result = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard"],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    return [ROOT / line for line in result.stdout.splitlines() if line]


def text_files() -> list[Path]:
    files: list[Path] = []
    for path in git_files():
        if not path.is_file() or path.stat().st_size > 2 * 1024 * 1024:
            continue
        try:
            path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        files.append(path)
    return files


def check_required_files(errors: list[str]) -> None:
    for name in ("README.md", "AGENTS.md", "docs/development/README.md",
                 "docs/applications/ai-quota-monitor.md", "docs/CHANGELOG.md",
                 "dependencies.lock", "sdkconfig.defaults", "partitions.csv"):
        if not (ROOT / name).is_file():
            errors.append(f"missing required file: {name}")
    ignored = subprocess.run(["git", "check-ignore", "-q", "dependencies.lock"], cwd=ROOT)
    if ignored.returncode == 0:
        errors.append("dependencies.lock must be tracked, not ignored")


def heading_anchors(text: str) -> set[str]:
    """GitHub-style heading slugs, including the numeric suffix of repeated headings."""
    anchors: set[str] = set()
    seen: dict[str, int] = {}
    for heading in HEADING_RE.findall(re.sub(r"```.*?```", "", text, flags=re.S)):
        slug = re.sub(r"[^\w\- ]", "", re.sub(r"[`*_]", "", heading).strip().lower()).replace(" ", "-")
        count = seen.get(slug, 0)
        seen[slug] = count + 1
        anchors.add(slug if count == 0 else f"{slug}-{count}")
    return anchors


def check_markdown_links(files: list[Path], errors: list[str]) -> None:
    for path in files:
        if path.suffix.lower() != ".md":
            continue
        text = path.read_text(encoding="utf-8")
        for raw_target in MARKDOWN_LINK_RE.findall(text):
            target = raw_target.strip().split(maxsplit=1)[0].strip("<>")
            if not target or target.startswith(("http://", "https://", "mailto:")):
                continue
            local, _, fragment = unquote(target).partition("#")
            if local:
                resolved = (ROOT / local.lstrip("/")) if local.startswith("/") else (path.parent / local)
            else:
                resolved = path
            if not resolved.resolve().exists():
                errors.append(f"{path.relative_to(ROOT)}: missing link target {target}")
            elif fragment and resolved.suffix.lower() == ".md":
                if fragment.lower() not in heading_anchors(resolved.read_text(encoding="utf-8")):
                    errors.append(f"{path.relative_to(ROOT)}: missing anchor {target}")


def check_chinese_only_documents(files: list[Path], errors: list[str]) -> None:
    """Documentation is Simplified Chinese only: no language peers, no language links."""
    for path in sorted(files):
        if path.suffix.lower() != ".md":
            continue
        if path.name.endswith((".zh_CN.md", ".en_US.md")):
            errors.append(f"{path.relative_to(ROOT)}: language-specific Markdown files are not allowed")
        opening = "\n".join(path.read_text(encoding="utf-8").splitlines()[:8])
        if LANGUAGE_LINK_RE.search(opening):
            errors.append(f"{path.relative_to(ROOT)}: remove the top language link")


def check_action_pins(errors: list[str]) -> None:
    workflow_dir = ROOT / ".github" / "workflows"
    for path in sorted(workflow_dir.glob("*.y*ml")):
        for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            match = re.match(r"\s*-?\s*uses:\s*['\"]?([^'\"\s]+)", line)
            if not match:
                continue
            action = match.group(1)
            if action.startswith("./"):
                continue
            if action.startswith("docker://"):
                if "@sha256:" not in action:
                    errors.append(f"{path.relative_to(ROOT)}:{line_number}: unpinned Docker action {action}")
                continue
            if "@" not in action or not FULL_SHA_RE.fullmatch(action.rsplit("@", 1)[1]):
                errors.append(f"{path.relative_to(ROOT)}:{line_number}: action must use a full commit SHA: {action}")


def check_sensitive_content(files: list[Path], errors: list[str]) -> None:
    for path in files:
        text = path.read_text(encoding="utf-8")
        for label, pattern in SECRET_PATTERNS.items():
            if pattern.search(text):
                errors.append(f"{path.relative_to(ROOT)}: possible {label}")


def check_conflict_markers(files: list[Path], errors: list[str]) -> None:
    marker = re.compile(r"(?m)^(<<<<<<< |=======\s*$|>>>>>>> )")
    for path in files:
        if marker.search(path.read_text(encoding="utf-8")):
            errors.append(f"{path.relative_to(ROOT)}: unresolved merge conflict marker")


def display_width(line: str) -> int:
    """Columns as clang-format counts them: wide East Asian characters take two."""
    return sum(2 if unicodedata.east_asian_width(character) in "WF" else 1 for character in line)


def check_line_length(files: list[Path], errors: list[str]) -> None:
    for path in files:
        name = path.relative_to(ROOT).as_posix()
        if not name.startswith(LINE_LIMIT_ROOTS) or name.startswith(LINE_LIMIT_EXEMPT):
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if display_width(line) > LINE_LIMIT:
                errors.append(f"{name}:{number}: longer than {LINE_LIMIT} columns")


def check_setup_page_is_current(errors: list[str]) -> None:
    """main/setup_page.html is generated from main/setup/ and is committed."""
    node = shutil.which("node")
    if node is None:
        errors.append("node is required to check that main/setup_page.html is up to date")
        return
    result = subprocess.run(
        [node, "tools/build_setup_page.mjs", "--check"], cwd=ROOT, capture_output=True, text=True
    )
    if result.returncode != 0:
        errors.append(
            "main/setup_page.html is out of date (run npm run build:setup): "
            + (result.stderr.strip() or result.stdout.strip())
        )


def main() -> int:
    errors: list[str] = []
    files = text_files()
    check_required_files(errors)
    check_markdown_links(files, errors)
    check_chinese_only_documents(files, errors)
    check_action_pins(errors)
    check_sensitive_content(files, errors)
    check_conflict_markers(files, errors)
    check_line_length(files, errors)
    check_setup_page_is_current(errors)

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1

    print(f"Repository checks: PASS ({len(files)} text files scanned)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
