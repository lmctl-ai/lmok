#!/usr/bin/env python3
"""Portable lmok smoke tests for the lmscript and lmcode Tree-sitter grammars.

These tests intentionally exercise the public command boundary.  The S5
injection work will reuse the named lmcode_body ranges; this file keeps the
standalone language contracts independently gated.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import shutil

ROOT = Path(__file__).resolve().parents[1]
HOST = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 and not sys.argv[1].startswith('--') else ROOT / 'build' / ('lmok.exe' if os.name == 'nt' else 'lmok')
PROVE = '--prove-controls' in sys.argv
if PROVE:
    sys.argv.remove('--prove-controls')
CORRUPT = False
COMPILER = os.environ.get('LMSCRIPT_BIN', 'lmscript')
if os.name == 'nt' and not COMPILER.lower().endswith(('.cmd', '.exe', '.bat')):
    COMPILER = shutil.which(COMPILER + '.cmd') or COMPILER + '.cmd'


class ScriptLanguages(unittest.TestCase):
    def setUp(self):
        (ROOT / 'build').mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='script languages ', dir=ROOT / 'build')
        self.work = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def run_host(self, *args):
        result = subprocess.run([str(HOST), *map(str, args)], cwd=ROOT,
                                capture_output=True, timeout=30)
        if CORRUPT:
            result.stdout += b'\r'
        # Public output is LF-framed even on Windows; every test uses this oracle.
        self.assertFalse(result.stdout.endswith(b'\r'), 'unexpected CR after terminal LF')
        return result

    def assert_clean_parse(self, lang, source):
        parsed = self.run_host('parse', '--lang', lang, source)
        self.assertEqual(parsed.returncode, 0, parsed.stderr.decode('utf-8', 'replace'))
        self.assertEqual(parsed.stderr, b'')
        self.assertNotIn(b'ERROR', parsed.stdout)
        self.assertNotIn(b'MISSING', parsed.stdout)
        return parsed.stdout

    def test_lmscript_parse_query_facts_extension_and_utf8(self):
        source = self.work / 'entrée 雪.lms'
        text = 'import "dep.lms";\nfn greet(name) { return name; }\n'
        for newline in ('\n', '\r\n'):
            source.write_bytes(text.replace('\n', newline).encode('utf-8'))
            tree = self.assert_clean_parse('lmscript', source)
            self.assertIn(b'function_declaration', tree)
            query = self.run_host('query', '--lang', 'lmscript',
                                  '(function_declaration (identifier) @name)', source)
            self.assertEqual(query.returncode, 0, query.stderr)
            expected_row = b'1:3-1:8' if newline == '\n' else b'1:3-1:8'
            self.assertEqual(query.stdout, b'identifier ' + expected_row + b'\n')
            facts = self.run_host('facts', source)  # .lms inference
            self.assertEqual(facts.returncode, 0, facts.stderr)
            rows = [json.loads(line) for line in facts.stdout.splitlines()]
            self.assertTrue(any(row.get('kind') == 'def.function' and row.get('name') == 'greet' for row in rows), rows)
            self.assertTrue(any(row.get('kind') == 'ref.import' and row.get('name') == 'dep.lms' for row in rows), rows)
            self.assertTrue(all(row.get('lang') == 'lmscript' for row in rows), rows)
            self.assertEqual(facts.stdout, self.run_host('facts', source).stdout)

    def test_lmcode_parse_query_facts_and_lint(self):
        source = self.work / 'lmcode fixture.lmcode'
        source.write_text('fn greet(name: string) -> string { return name; }\n', encoding='utf-8')
        tree = self.assert_clean_parse('lmcode', source)
        self.assertIn(b'function_declaration', tree)
        query = self.run_host('query', '--lang', 'lmcode',
                              '(function_declaration name: (identifier) @name)', source)
        self.assertEqual(query.returncode, 0, query.stderr)
        self.assertIn(b'identifier', query.stdout)
        facts = self.run_host('facts', '--lang', 'lmcode', source)
        self.assertEqual(facts.returncode, 0, facts.stderr)
        rows = [json.loads(line) for line in facts.stdout.splitlines()]
        self.assertTrue(any(row.get('kind') == 'def.function' and row.get('name') == 'greet' for row in rows), rows)
        self.assertTrue(all(row.get('lang') == 'lmcode' for row in rows), rows)
        inferred = self.run_host('facts', source)
        self.assertEqual(inferred.returncode, 0, inferred.stderr)
        self.assertEqual(inferred.stdout, facts.stdout)

        pack = self.work / 'lmcode-pack.json'
        pack.write_text(json.dumps({'lang': 'lmcode', 'rules': [
            {'id': 'LM001', 'on': 'function_declaration', 'message': 'function'}]}), encoding='utf-8')
        lint = self.run_host('lint', pack, source)
        self.assertEqual(lint.returncode, 0, lint.stderr)
        findings = [json.loads(line) for line in lint.stdout.splitlines() if line.startswith(b'{')]
        self.assertTrue(findings and findings[0]['rule'] == 'LM001', lint.stdout)

        # check is the compiled-rule path. Keep it here in addition to lint so
        # registration errors cannot hide behind JSON-pack-only coverage.
        rule = self.work / 'rule.lms'
        binary = self.work / 'rule.dslb'
        rule.write_text('p = _lmcode_ schema=' + json.dumps(str(ROOT / 'schema/lmok.dsl.json')) + ';\n'
                        '@function_declaration fn check(n: Node) -> void implements Rule { '
                        'ts.report(n, "function", "LM001", "minor"); }\n'
                        'end _lmcode_\n', encoding='utf-8')
        compiled = subprocess.run(["node",str(ROOT/"scripts/compile-fixture.mjs"), str(rule), str(binary)], cwd=ROOT,
                                  capture_output=True, timeout=30)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        checked = self.run_host('check', '--lang', 'lmcode', binary, source)
        self.assertEqual(checked.returncode, 0, checked.stderr)
        self.assertIn(b'LM001', checked.stdout)

    def test_located_facts_and_opaque_body(self):
        fixture = ROOT / 'tests/fixtures/script-languages/workflow.lms'
        tree = self.assert_clean_parse('lmscript', fixture)
        self.assertIn(b'lmcode_body', tree)
        self.assertEqual(self.run_host('parse', fixture).stdout, tree)
        result = self.run_host('facts', fixture)
        self.assertEqual((result.returncode, result.stderr), (0, b''))
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual([(r['kind'], r['name'], r['name_row'], r['name_col']) for r in rows], [
            ('def.package', 'demo', 0, 8), ('ref.import', 'dep.lms', 1, 7),
            ('def.schema', 'host', 2, 0), ('ref.schema', 'host.json', 2, 26),
            ('def.function', 'greet', 3, 3), ('ref.call', 'emit', 3, 29),
            ('ref.call', 'greet', 4, 9), ('ref.marker', '_lmcode_', 5, 4),
            ('ref.schema', 'host.json', 5, 20)])
        # The nested function is deliberately absent: injection belongs to S5.
        self.assertNotIn('inner', [r['name'] for r in rows])
        fixture = ROOT / 'tests/fixtures/script-languages/rule.lmcode'
        self.assert_clean_parse('lmcode', fixture)
        result = self.run_host('facts', fixture)
        self.assertEqual((result.returncode, result.stderr), (0, b''))
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual([(r['kind'], r['name'], r['name_row'], r['name_col']) for r in rows], [
            ('def.function', 'greet', 0, 3), ('ref.implements', 'Render', 0, 44),
            ('ref.call', 'helper', 0, 60)])

    def test_pinned_upstream_fixtures(self):
        counts = {'lmscript': 4, 'lmcode': 131}
        for lang, count in counts.items():
            root = ROOT / 'third_party' / ('tree-sitter-' + lang) / 'test/fixtures'
            fixtures = sorted([*root.rglob('*.lms'), *root.rglob('*.lmcode')])
            self.assertEqual(len(fixtures), count, 'review corpus count when updating grammar pin')
            for fixture in fixtures:
                with self.subTest(lang=lang, fixture=fixture.name):
                    self.assert_clean_parse(lang, fixture)

    def test_invalid_syntax_is_visible(self):
        source = self.work / 'bad.lms'
        source.write_text('fn broken(x) { return x }\n', encoding='utf-8')
        parsed = self.run_host('parse', '--lang', 'lmscript', source)
        self.assertEqual(parsed.returncode, 0)
        self.assertTrue(b'ERROR' in parsed.stdout or b'MISSING' in parsed.stdout, parsed.stdout)

    def test_lmscript_lint_and_compiled_check(self):
        source = self.work / 'lint.lms'
        source.write_text('fn greet(name) { return name; }\n', encoding='utf-8')
        pack = self.work / 'lmscript-pack.json'
        pack.write_text(json.dumps({'lang': 'lmscript', 'rules': [
            {'id': 'LS001', 'on': 'function_declaration', 'message': 'function'}]}), encoding='utf-8')
        lint = self.run_host('lint', pack, source)
        self.assertEqual(lint.returncode, 0, lint.stderr)
        self.assertIn(b'LS001', lint.stdout)

        rule = self.work / 'lmscript-rule.lms'
        binary = self.work / 'lmscript-rule.dslb'
        rule.write_text('p = _lmcode_ schema=' + json.dumps(str(ROOT / 'schema/lmok.dsl.json')) + ';\n'
                        '@function_declaration fn check(n: Node) -> void implements Rule { '
                        'ts.report(n, "function", "LS001", "minor"); }\n'
                        'end _lmcode_\n', encoding='utf-8')
        compiled = subprocess.run(["node",str(ROOT/"scripts/compile-fixture.mjs"), str(rule), str(binary)], cwd=ROOT,
                                  capture_output=True, timeout=30)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        checked = self.run_host('check', '--lang', 'lmscript', binary, source)
        self.assertEqual(checked.returncode, 0, checked.stderr)
        self.assertIn(b'LS001', checked.stdout)

    def test_lmcode_scanner_heredoc_and_quoted_literals(self):
        source = self.work / 'scanner.lmcode'
        source.write_text('fn render() -> string {\n'
                          '  let words = qw(one two);\n'
                          '  let body = <<END;\nhello ${name}\nEND\n'
                          '  return body;\n}\n', encoding='utf-8')
        tree = self.assert_clean_parse('lmcode', source)
        self.assertIn(b'heredoc_literal', tree)
        self.assertIn(b'qw_literal', tree)


if __name__ == '__main__':
    if PROVE:
        CORRUPT = True
        names = unittest.defaultTestLoader.getTestCaseNames(ScriptLanguages)
        for name in names:
            result = unittest.TestResult()
            ScriptLanguages(name).run(result)
            if result.errors or not result.failures:
                raise SystemExit('output-corruption control survived: ' + name + str(result.errors))
        print('CAUGHT script-language output corruption in %d native oracles' % len(names))
        CORRUPT = False
    unittest.main()
