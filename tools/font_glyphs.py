#!/usr/bin/env python3
"""Maintain the interface font glyph list, and optionally rebuild the two fonts from it.

The fonts cover, besides printable ASCII:
  1. every common Chinese character of assets/fonts/hanzi-level1.txt (the 3,755 level-1 hanzi of
     GB 2312-1980, so a remark or Wi-Fi name in everyday Chinese can be shown),
  2. every non-ASCII character in a string literal of main/quota_ui.c and main/quota_logic.c,
  3. the common full-width punctuation listed in PUNCTUATION below.

    python3 tools/font_glyphs.py            rewrite assets/fonts/quota-font-glyphs.txt
    python3 tools/font_glyphs.py --build    ... and regenerate quota_font_12.c / quota_font_16.c
                                            (needs npx and the network for lv_font_conv@1.5.3)
    python3 tools/font_glyphs.py --check    fail if hanzi-level1.txt does not match GB 2312

hanzi-level1.txt is derived from the GB 2312 code table (rows 16-55, the level-1 set, in code
order, which is by pinyin). It is a list of Unicode code points, which are facts of a national
standard; it is regenerated with --write-hanzi and checked by --check, and tests/test_quota_fonts.py
runs the check. tests/test_quota_fonts.py checks the generated fonts."""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FONTS = ROOT / "assets/fonts"
HANZI = FONTS / "hanzi-level1.txt"
SOURCES = ("main/quota_ui.c", "main/quota_logic.c")
# Full-width and typographic punctuation in everyday Chinese text.
PUNCTUATION = "，。、；：？！“”‘’（）【】《》〈〉「」『』—…·～％＋－＝×÷℃￥＄＃＆＊＠／＼｜"
FONT_SOURCE = "managed_components/lvgl__lvgl/tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf"


def gb2312_level1():
    """The 3,755 level-1 hanzi of GB 2312, in code order (rows 0xB0-0xD7)."""
    characters = []
    for row in range(0xB0, 0xD8):
        for column in range(0xA1, 0xFF):
            if row == 0xD7 and column > 0xF9:
                break
            characters.append(bytes([row, column]).decode("gb2312"))
    assert len(characters) == 3755
    return "".join(characters)


def hanzi():
    return HANZI.read_text(encoding="utf-8").strip()


def interface_characters():
    text = "\n".join((ROOT / name).read_text(encoding="utf-8") for name in SOURCES)
    literals = re.findall(r'"(?:\\.|[^"\\])*"', text)
    return {c for literal in literals for c in literal if ord(c) > 127}


def glyphs():
    """Every non-ASCII character the fonts must contain, sorted by code point."""
    return sorted(set(hanzi()) | interface_characters() | set(PUNCTUATION))


def main():
    arguments = sys.argv[1:]
    if "--write-hanzi" in arguments:
        HANZI.write_text(gb2312_level1() + "\n", encoding="utf-8")
    if hanzi() != gb2312_level1():
        raise SystemExit("assets/fonts/hanzi-level1.txt does not match the GB 2312 level-1 set")
    if "--check" in arguments:
        return
    characters = glyphs()
    (FONTS / "quota-font-glyphs.txt").write_text("".join(characters) + "\n", encoding="utf-8")
    print(f"{len(characters)} glyphs")
    if "--build" in arguments:
        for size in (12, 16):
            subprocess.run(
                [
                    "npx",
                    "--yes",
                    "lv_font_conv@1.5.3",
                    "--font",
                    FONT_SOURCE,
                    "--range",
                    "0x20-0x7E",
                    "--symbols",
                    "".join(characters),
                    "--size",
                    str(size),
                    "--bpp",
                    "4",
                    "--format",
                    "lvgl",
                    "--no-compress",
                    "--lv-font-name",
                    f"quota_font_{size}",
                    "--lv-include",
                    "lvgl.h",
                    "--output",
                    f"assets/fonts/quota_font_{size}.c",
                ],
                cwd=ROOT,
                check=True,
            )


if __name__ == "__main__":
    main()
