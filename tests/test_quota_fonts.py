"""Verify the generated font tables cover actual quota interface literals."""

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import font_glyphs  # noqa: E402

# What a person is likely to type as a remark or Wi-Fi name: it must be drawn, not replaced.
SAMPLE_TEXT = (
    "工作笔记 家里的网络 办公室 我的手机热点 王小明的账户 测试用密钥 "
    "备用（二）《学习》、，。：；？！“好”‘行’—…·"
)


def font_codepoints(filename):
    source = filename.read_text(encoding="utf-8")
    arrays = {}
    for name, values in re.findall(
        r"static const uint16_t (unicode_list_\w+)\[\]\s*=\s*\{(.*?)\};", source, re.S
    ):
        values = re.sub(r"/\*.*?\*/", "", values, flags=re.S)
        arrays[name] = [
            int(value, 0) for value in re.findall(r"\b(?:0x[0-9a-fA-F]+|[0-9]+)\b", values)
        ]
    result = set()
    for block in re.findall(r"\{\s*\.range_start\s*=.*?\}", source, re.S):
        start = int(re.search(r"\.range_start\s*=\s*(\d+)", block)[1])
        length = int(re.search(r"\.range_length\s*=\s*(\d+)", block)[1])
        name = re.search(r"\.unicode_list\s*=\s*(\w+)", block)[1]
        if name == "NULL":
            result.update(range(start, start + length))
        else:
            result.update(start + offset for offset in arrays[name])
    return result


class QuotaFonts(unittest.TestCase):
    def test_every_interface_character_is_in_both_fonts(self):
        ui = "\n".join(
            (ROOT / filename).read_text(encoding="utf-8")
            for filename in ("main/quota_ui.c", "main/quota_logic.c")
        )
        literals = re.findall(r'"(?:\\.|[^"\\])*"', ui)
        required = {
            ord(character) for literal in literals for character in literal if ord(character) > 127
        }
        self.assertTrue(required)
        for size in (12, 16):
            with self.subTest(size=size):
                available = font_codepoints(ROOT / f"assets/fonts/quota_font_{size}.c")
                missing = required - available
                self.assertEqual(
                    missing, set(), "Missing glyphs: " + "".join(map(chr, sorted(missing)))
                )
                self.assertTrue(set(range(32, 127)).issubset(available))

    def test_common_chinese_and_the_glyph_list_are_in_both_fonts(self):
        # The source list is the GB 2312 level-1 set, and the list the fonts were built from is
        # exactly what the generator produces now (nothing added or dropped by hand).
        self.assertEqual(font_glyphs.hanzi(), font_glyphs.gb2312_level1())
        self.assertEqual(len(font_glyphs.hanzi()), 3755)
        listed = (ROOT / "assets/fonts/quota-font-glyphs.txt").read_text(encoding="utf-8").strip()
        self.assertEqual(listed, "".join(font_glyphs.glyphs()))
        required = set(listed) | {c for c in SAMPLE_TEXT if c != " "}
        for size in (12, 16):
            with self.subTest(size=size):
                available = font_codepoints(ROOT / f"assets/fonts/quota_font_{size}.c")
                missing = {ord(c) for c in required} - available
                self.assertEqual(
                    missing, set(), "Missing glyphs: " + "".join(map(chr, sorted(missing)))
                )

    def test_rare_characters_stay_outside_the_fonts(self):
        # Characters the fonts do not have are what the device shows as "DeepSeek N" / "Wi-Fi N".
        available = font_codepoints(ROOT / "assets/fonts/quota_font_12.c")
        for rare in "龘靐\U0001f600":
            self.assertNotIn(ord(rare), available)


if __name__ == "__main__":
    unittest.main()
