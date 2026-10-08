"""Verify the generated font tables cover actual quota interface literals."""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


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


if __name__ == "__main__":
    unittest.main()
