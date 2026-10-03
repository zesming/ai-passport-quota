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

    def test_paired_documents_and_local_assets(self):
        files = [self.write('guide.md', '[简体中文](guide.zh_CN.md)\n[Image](image.svg)\n'),
                 self.write('guide.zh_CN.md', '[English](guide.md)\n# 指南\n')]
        self.write('image.svg', '<svg/>')
        errors = []
        CHECKS.check_document_languages(files, errors)
        CHECKS.check_markdown_links(files, errors)
        self.assertEqual(errors, [])

    def test_missing_peer_and_link(self):
        page = self.write('guide.md', '[Image](missing.svg)\n')
        errors = []
        CHECKS.check_document_languages([page], errors)
        CHECKS.check_markdown_links([page], errors)
        self.assertEqual(len(errors), 2)
        self.assertIn('missing Simplified Chinese peer', errors[0])
        self.assertIn('missing link target', errors[1])

    def test_language_links_and_default_prose(self):
        files = [self.write('guide.md', '# English\n中文\n'),
                 self.write('guide.zh_CN.md', '# 指南\n')]
        errors = []
        CHECKS.check_document_languages(files, errors)
        self.assertEqual(len(errors), 3)
        self.assertTrue(any('English prose' in item for item in errors))
        self.assertEqual(sum('top language link' in item for item in errors), 2)

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
