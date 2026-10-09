"""The status bar Wi-Fi images are generated: the committed C file must be what the script makes."""

import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


class WifiIcons(unittest.TestCase):
    def test_committed_file_matches_the_generator(self):
        run = subprocess.run(
            [sys.executable, str(ROOT / "tools/gen_wifi_icons.py"), "--check"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(run.returncode, 0, run.stderr)

    def test_lit_and_dim_layers_do_not_overlap(self):
        sys.path.insert(0, str(ROOT / "tools"))
        import gen_wifi_icons as icons

        for level in (1, 2, 3):
            lit = icons.render(icons.lit_shape(level))
            dim = icons.render(icons.dim_shape(level))
            self.assertEqual(len(lit), icons.HEIGHT)
            for lit_row, dim_row in zip(lit, dim):
                for a, b in zip(lit_row, dim_row):
                    self.assertLessEqual(a + b, 255 + 16, "a pixel is lit and dim at once")
        self.assertFalse(any(any(row) for row in icons.render(icons.dim_shape(3))))


if __name__ == "__main__":
    unittest.main()
