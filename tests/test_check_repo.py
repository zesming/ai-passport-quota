#!/usr/bin/env python3
"""Check the repository guards with small synthetic fixtures."""
import importlib.util
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('check_repo', Path(__file__).resolve().parents[1] / 'tools/check_repo.py')
CHECKS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKS)


class RepositoryChecks(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='ai-quota-repo-test-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        root_patch = patch.object(CHECKS, 'ROOT', self.root)
        root_patch.start()
        self.addCleanup(root_patch.stop)

    def write(self, name, content):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return path

    def test_local_assets_and_anchors(self):
        files = [self.write('guide.md', '# 指南\n[图](image.svg) [章节](other.md#刷新显示与验证) [重复](other.md#重复-1) [本页](#指南)\n'),
                 self.write('other.md', '# 其他\n## 刷新、显示与验证\n## 重复\n## 重复\n[回](guide.md#指南)\n')]
        self.write('image.svg', '<svg/>')
        errors = []
        CHECKS.check_markdown_links(files, errors)
        self.assertEqual(errors, [])

    def test_missing_link_target_and_anchor(self):
        page = self.write('guide.md', '[图](missing.svg)\n[章节](other.md#没有)\n')
        self.write('other.md', '# 其他\n')
        errors = []
        CHECKS.check_markdown_links([page], errors)
        self.assertEqual(len(errors), 2)
        self.assertIn('missing link target', errors[0])
        self.assertIn('missing anchor', errors[1])

    def test_language_peers_and_links_are_rejected(self):
        files = [self.write('guide.md', '[简体中文](guide.zh_CN.md) · English\n# 指南\n'),
                 self.write('guide.zh_CN.md', '[English](guide.md)\n# 指南\n'),
                 self.write('html.md', '<p><a href="html.zh_CN.md">x</a></p>\n'),
                 self.write('clean.md', '# 指南\n正文提到 guide.zh_CN.md 的历史。\n' * 1)]
        errors = []
        CHECKS.check_chinese_only_documents(files, errors)
        self.assertEqual(sorted(errors), [
            'guide.md: remove the top language link',
            'guide.zh_CN.md: language-specific Markdown files are not allowed',
            'guide.zh_CN.md: remove the top language link',
            'html.md: remove the top language link'])

    def test_chinese_only_documents_pass(self):
        files = [self.write('guide.md', '# 指南\n只有中文。\n')]
        errors = []
        CHECKS.check_chinese_only_documents(files, errors)
        self.assertEqual(errors, [])

    def test_sensitive_content_and_conflicts(self):
        samples = ['ghp_' + 'x' * 24, 'AKIA' + '0' * 16,
                   '-----BEGIN ' + 'PRIVATE KEY-----', '<' * 7 + ' HEAD\n']
        files = [self.write(f'sample{index}.txt', value) for index, value in enumerate(samples)]
        errors = []
        CHECKS.check_sensitive_content(files, errors)
        CHECKS.check_conflict_markers(files, errors)
        self.assertEqual(len(errors), 4)
        clean = self.write('normal.c', 'if (remaining < limit) return;\n')
        errors = []
        CHECKS.check_sensitive_content([clean], errors)
        CHECKS.check_conflict_markers([clean], errors)
        self.assertEqual(errors, [])

    def test_action_pins(self):
        self.write('.github/workflows/check.yml', '\n'.join([
            'uses: actions/checkout@v6', 'uses: docker://alpine:3',
            'uses: actions/checkout@' + 'a' * 40,
            'uses: docker://alpine@sha256:' + 'b' * 64, 'uses: ./local-action']))
        errors = []
        CHECKS.check_action_pins(errors)
        self.assertEqual(len(errors), 2)
        self.assertIn('full commit SHA', errors[0])
        self.assertIn('unpinned Docker', errors[1])


if __name__ == '__main__':
    unittest.main()
