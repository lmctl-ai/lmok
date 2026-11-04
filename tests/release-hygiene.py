#!/usr/bin/env python3
"""Offline release input controls; tags exist only in disposable repositories."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GUARD = ROOT / 'scripts/release-needed.py'

class ReleaseTests(unittest.TestCase):
    def test_workflow_gates_all_release_side_effects(self):
        release = (ROOT / '.github/workflows/ci.yml').read_text(encoding='utf-8').split('\n  release:', 1)[1]
        self.assertIn('fetch-depth: 0', release)
        self.assertIn('id: release_gate', release)
        for title in ('actions/download-artifact@', 'name: Check five release assets', 'name: Publish release'):
            block = next(b for b in release.split('\n      - ') if title in b)
            self.assertIn("if: steps.release_gate.outputs.publish == 'true'", block)

    def test_changes_since_latest_tag(self):
        with tempfile.TemporaryDirectory(prefix='lmok release inputs ') as tmp:
            root = Path(tmp)
            env = dict(os.environ, GIT_CONFIG_NOSYSTEM='1')
            def git(*args):
                return subprocess.run(['git', '-c', 'user.name=fixture', '-c', 'user.email=fixture@example.invalid', '-c', 'commit.gpgsign=false', '-C', str(root), *args], env=env, check=True, capture_output=True)
            def commit(path):
                file = root / path; file.parent.mkdir(parents=True, exist_ok=True)
                file.write_text(file.read_text(encoding='utf-8') + 'change\n' if file.exists() else 'change\n', encoding='utf-8')
                git('add', '--', path); git('commit', '-qm', 'fixture')
            def check(expected, ref='refs/heads/main'):
                output = root / 'outputs'
                output.write_text('', encoding='utf-8')
                run = subprocess.run([sys.executable, str(GUARD), '--repo', str(root)], env=dict(env, GITHUB_REF=ref, GITHUB_REF_TYPE='tag' if '/tags/' in ref else 'branch', GITHUB_OUTPUT=str(output)), capture_output=True, text=True, encoding='utf-8', errors='replace')
                self.assertEqual(run.returncode, 0, run.stderr)
                self.assertIn('publish=' + str(expected).lower(), output.read_text(encoding='utf-8'))
                self.assertIn('PUBLISH' if expected else 'SKIP release', run.stdout)
            git('init', '-q'); commit('cpp/main.cpp'); check(True)  # bootstrap
            git('tag', 'v1.0.0'); check(False)
            for path in ['README.md', 'tests/new.py', 'test.sh', 'scripts/release-needed.py', '.github/workflows/ci.yml']:
                commit(path); check(False)
            check(True, 'refs/tags/v1.0.0')
            for i, path in enumerate(['cpp/main.cpp', 'rules/a.json', 'schema/a.json', 'CMakeLists.txt', 'cmake/a.cmake', 'queries/go/facts.scm', 'deps.lock', 'sdk.lock', 'scripts/sync-deps.sh', 'scripts/sync-sdk.sh', 'scripts/next-version.sh', 'THIRD_PARTY_NOTICES.md'], 1):
                commit(path); check(True); git('tag', 'v1.0.' + str(i)); check(False)
            git('rm', 'cpp/main.cpp'); git('commit', '-qm', 'delete binary input'); check(True)

    def test_notices_contain_vendored_license_text(self):
        notices = (ROOT / 'THIRD_PARTY_NOTICES.md').read_text(encoding='utf-8')
        lock = (ROOT / 'deps.lock').read_text(encoding='utf-8').splitlines()
        names = [line.split('.', 1)[0].replace('_', '-') for line in lock
                 if line.startswith('tree_sitter') and '.sha256=' in line]
        # Every tree-sitter archive in deps.lock has a vendored license and
        # its exact license text must be present in the embedded notices.
        names = sorted(set(names))
        for name in names:
            path = ROOT / 'third_party' / name / 'LICENSE'
            self.assertTrue(path.exists(), name)
            self.assertIn(path.read_text(encoding='utf-8').strip(), notices, name)

    def test_notices_cover_non_grammar_dependencies(self):
        notices = (ROOT / 'THIRD_PARTY_NOTICES.md').read_text(encoding='utf-8')
        sdk = (ROOT / 'sdk.lock').read_text(encoding='utf-8')
        self.assertTrue(sdk.strip(), 'sdk.lock must identify the embedded SDK')
        self.assertIn('lmcode-cpp SDK', notices)
        self.assertIn('SQLite', notices)
        cmake = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
        for grammar in ('tree-sitter-python', 'tree-sitter-bash', 'tree-sitter-java',
                        'tree-sitter-go', 'tree-sitter-javascript', 'tree-sitter-typescript'):
            self.assertIn(grammar, cmake, grammar)

if __name__ == '__main__':
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ReleaseTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    print(('ok' if result.wasSuccessful() else 'FAIL') + ' - release input policy')
    sys.exit(not result.wasSuccessful())
