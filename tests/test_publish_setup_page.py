"""Rehearse the gh-pages publication: earlier /pN/ pages must survive every release."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tools/publish_setup_page.sh"


def git(*arguments, cwd):
    return subprocess.run(
        ["git", "-c", "user.name=test", "-c", "user.email=test@example.invalid", *arguments],
        cwd=cwd,
        check=True,
        capture_output=True,
        text=True,
    ).stdout


class PublishSetupPage(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ai-quota-pages-test-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.remote = self.directory / "remote.git"
        git("init", "--bare", "--initial-branch=gh-pages", str(self.remote), cwd=self.directory)
        seed = self.directory / "seed"
        git("clone", str(self.remote), str(seed), cwd=self.directory)
        (seed / "p2").mkdir()
        (seed / "p2/index.html").write_text("protocol 2 page")
        git("add", "-A", cwd=seed)
        git("commit", "-m", "first", cwd=seed)
        git("push", "origin", "HEAD:gh-pages", cwd=seed)

    def release(self, name, protocol, page_text):
        """One tag: clone the branch, publish, push, as pages.yml does."""
        site = self.directory / name
        git("clone", "--branch", "gh-pages", str(self.remote), str(site), cwd=self.directory)
        header = self.directory / f"{name}.h"
        header.write_text(f"#define QUOTA_PROTOCOL_VERSION {protocol}\n")
        page = self.directory / f"{name}.html"
        page.write_text(page_text)
        result = subprocess.run(
            [str(SCRIPT), str(site), str(page), str(header)],
            check=True,
            capture_output=True,
            text=True,
        )
        git("push", "origin", "HEAD:gh-pages", cwd=site)
        return site, result.stdout

    def published(self, name):
        shown = self.directory / f"{name}-check"
        git("clone", "--branch", "gh-pages", str(self.remote), str(shown), cwd=self.directory)
        return shown

    def test_two_releases_keep_every_earlier_protocol_page(self):
        self.release("tag-a", 3, "page A")
        second, _ = self.release("tag-b", 3, "page B")  # a second version of protocol 3
        shown = self.published("after-two")
        self.assertEqual((shown / "p2/index.html").read_text(), "protocol 2 page")
        self.assertEqual((shown / "p3/index.html").read_text(), "page B")
        self.assertEqual((shown / "index.html").read_text(), "page B")
        self.assertTrue((shown / ".nojekyll").exists())
        self.release("tag-c", 4, "page C")  # a newer protocol leaves p3 as it was
        shown = self.published("after-three")
        self.assertEqual(
            [(path.name, (path / "index.html").read_text()) for path in sorted(shown.glob("p*/"))],
            [("p2", "protocol 2 page"), ("p3", "page B"), ("p4", "page C")],
        )
        self.assertEqual((shown / "index.html").read_text(), "page C")
        self.assertEqual(len(git("log", "--oneline", cwd=shown).splitlines()), 4)

    def test_a_fix_of_an_older_protocol_does_not_take_the_root_back(self):
        self.release("tag-a", 3, "page A")
        self.release("tag-b", 4, "page B")
        self.release("tag-c", 3, "page A fixed")
        shown = self.published("after-fix")
        self.assertEqual((shown / "p3/index.html").read_text(), "page A fixed")
        self.assertEqual((shown / "p4/index.html").read_text(), "page B")
        self.assertEqual((shown / "index.html").read_text(), "page B")
        self.assertEqual((shown / "p2/index.html").read_text(), "protocol 2 page")
        self.release("tag-d", 4, "page B fixed")  # the same protocol again does move the root
        shown = self.published("after-same")
        self.assertEqual((shown / "index.html").read_text(), "page B fixed")

    def test_publishing_the_same_page_again_adds_nothing(self):
        self.release("tag-a", 3, "page A")
        _, output = self.release("tag-b", 3, "page A")
        self.assertIn("already up to date", output)

    def test_a_header_without_a_protocol_is_refused(self):
        site = self.directory / "site"
        git("clone", "--branch", "gh-pages", str(self.remote), str(site), cwd=self.directory)
        header = self.directory / "empty.h"
        header.write_text("#define OTHER 1\n")
        page = self.directory / "page.html"
        page.write_text("page")
        result = subprocess.run([str(SCRIPT), str(site), str(page), str(header)])
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((site / "index.html").exists())


if __name__ == "__main__":
    unittest.main()
