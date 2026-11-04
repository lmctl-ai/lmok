#!/usr/bin/env python3
"""Review regressions: text-free previews and safe credential identities."""
from contextlib import closing
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest

HOST = Path(sys.argv.pop(1)).resolve()
ROOT = Path(__file__).resolve().parents[1]

class Security(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.work = Path(self.tmp.name)
        self.secret = 'AK' + 'IA' + 'A1B2C3D4E5F6G7H8'
        self.src = self.work / 'secret.c'
        self.src.write_text('const char *key="' + self.secret + '";\r\nint n=01;\r\n', encoding='utf-8')
        pack = json.loads((ROOT / 'rules/secrets-c.json').read_text(encoding='utf-8'))
        pack['rules'] += json.loads((ROOT / 'rules/c-autofix.json').read_text(encoding='utf-8'))['rules']
        self.pack = self.work / 'combined.json'
        self.pack.write_text(json.dumps(pack), encoding='utf-8')

    def lint(self, pack=None, src=None, *args):
        return subprocess.run([str(HOST), 'lint', str(pack or self.pack), str(src or self.src), *map(str, args)], capture_output=True)

    def test_preview_never_displays_source_even_without_secret_rules(self):
        for pack in (self.pack, ROOT / 'rules/c-autofix.json'):
            before = self.src.read_bytes()
            p = self.lint(pack, None, '--diff')
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertNotIn(self.secret.encode(), p.stdout + p.stderr)
            self.assertIn(b'source omitted', p.stdout)
            self.assertEqual(before, self.src.read_bytes())

    def test_diff_gate_and_sarif_combinations_are_refused(self):
        baseline = self.work / 'empty.txt'
        baseline.write_bytes(b'')
        for args in (('--baseline', baseline, '--fail-on-new'), ('--sarif', self.work / 'diff.sarif')):
            p = self.lint(None, None, '--diff', *args)
            self.assertEqual(p.returncode, 2, p.stdout + p.stderr)
            self.assertNotIn(self.secret.encode(), p.stdout + p.stderr)

    def test_java_secret_never_persists_plaintext(self):
        src = self.work / 'Secret.java'
        src.write_text('class Secret { String password="' + self.secret + '"; }', encoding='utf-8')
        db = self.work / 'java.db'
        sarif = self.work / 'java.sarif'
        p = self.lint(ROOT / 'rules/java-tier1.json', src, '--db', db, '--sarif', sarif)
        self.assertEqual(p.returncode, 0, p.stderr)
        with closing(sqlite3.connect(db)) as con:
            rows = con.execute("select snippet from findings where rule='LJV005'").fetchall()
        self.assertEqual(len(rows), 1)
        self.assertTrue(rows[0][0].startswith('sha256:'), rows)
        self.assertNotIn(self.secret.encode(), db.read_bytes() + sarif.read_bytes() + p.stdout + p.stderr)

    def test_redacted_replacement_never_leaves_rule_boundary(self):
        pack = self.work / 'unsafe-fix.json'
        pack.write_text(json.dumps({'lang': 'c', 'rules': [{
            'id': 'CREDENTIAL', 'on': 'string_literal', 'redact': True,
            'message': 'credential {text}', 'fix': '"' + self.secret + '"'}]}), encoding='utf-8')
        p = self.lint(pack)
        self.assertEqual(p.returncode, 2, p.stdout + p.stderr)
        self.assertNotIn(self.secret.encode(), p.stdout + p.stderr)

    def test_credential_rule_inventory_uses_safe_identity(self):
        for name, ids in [('java-tier1.json', {'LJV005'}),
                          ('json-tier1.json', {'LBJ001', 'LBJ104', 'LBJ006'}),
                          ('../examples/rules/json-config.json', {'LBJ001'})]:
            rules = json.loads((ROOT / 'rules' / name).read_text(encoding='utf-8'))['rules']
            for rule in rules:
                if rule['id'] in ids:
                    self.assertTrue(rule.get('redact'), (name, rule['id']))

    def test_plain_source_cannot_masquerade_as_safe_digest(self):
        pack, source = self.work / 'plain.json', self.work / 'data.json'
        pack.write_text(json.dumps({'lang': 'json', 'rules': [{
            'id': 'PLAIN', 'on': 'string_content', 'message': 'constant'}]}), encoding='utf-8')
        source.write_text(json.dumps({'k': 'sha256:' + self.secret}), encoding='utf-8')
        p = self.lint(pack, source)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertNotIn(self.secret.encode(), p.stdout + p.stderr)
        self.assertNotIn(b'"snippet"', p.stdout)

    def test_file_baseline_detects_credential_rotation(self):
        p = self.lint(ROOT / 'rules/secrets-c.json')
        self.assertEqual(p.returncode, 0, p.stderr)
        baseline = self.work / 'baseline.txt'
        baseline.write_bytes(p.stdout)
        unchanged = self.lint(ROOT / 'rules/secrets-c.json', None, '--baseline', baseline, '--fail-on-new')
        self.assertEqual(unchanged.returncode, 0, unchanged.stdout + unchanged.stderr)
        self.src.write_bytes(self.src.read_bytes().replace(self.secret.encode(), (self.secret[:-1] + '9').encode()))
        changed = self.lint(ROOT / 'rules/secrets-c.json', None, '--baseline', baseline, '--fail-on-new')
        self.assertEqual(changed.returncode, 4, changed.stdout + changed.stderr)
        self.assertIn(b'sha256:', p.stdout)
        self.assertNotIn(self.secret.encode(), p.stdout + changed.stdout + changed.stderr)

if __name__ == '__main__':
    unittest.main()
