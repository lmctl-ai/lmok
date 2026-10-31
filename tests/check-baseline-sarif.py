#!/usr/bin/env python3
"""Baseline/new-code-only (lint --new-since/--baseline/--changed/--fail-on-new)
and host SARIF (--sarif) contracts, executed on each native Actions runner.

The failing controls are semantic, not byte corruption: a position-keyed diff
of the two fixture revisions MUST flag the moved finding (proving the fixture
really moves it and only position-independent keying passes), a baseline file
with the key fields stripped MUST misclassify everything as new, and the SARIF
validator MUST reject a 0-based (unconverted) region row."""
import copy
import re
from urllib.parse import quote
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HOST = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else ROOT / 'build' / ('lmok.exe' if os.name == 'nt' else 'lmok')
COMPILER = os.environ.get('LMSCRIPT_BIN', 'lmscript')
PACK = ROOT / 'examples/rules/c-security.json'

COMMIT1 = b'#include <stdio.h>\nvoid f(void) {\n  char buf[16];\n  gets(buf);\n}\n'
COMMIT2 = (b'#include <stdio.h>\n#include <string.h>\nvoid f(void) {\n  char buf[16];\n'
           b'\n  /* moved below */\n  gets(buf);\n  strcpy(buf, "x");\n}\n')
GETS_V1 = '{"row":3,"col":2,"end_row":3,"end_col":6,"rule":"LBC001","severity":"critical","message":"unsafe call \'gets\'"}'
GETS_V2 = '{"row":6,"col":2,"end_row":6,"end_col":6,"rule":"LBC001","severity":"critical","message":"unsafe call \'gets\'"}'
STRCPY = '{"row":7,"col":2,"end_row":7,"end_col":8,"rule":"LBC001","severity":"critical","message":"unsafe call \'strcpy\'"}'


def compiler_argv():
    # On Windows npm installs `lmscript` as an extension-less sh script plus a
    # `lmscript.cmd` shim; CreateProcess can run only the shim (WinError 193 otherwise).
    if os.name == 'nt' and not COMPILER.lower().endswith(('.cmd', '.exe', '.bat')):
        for candidate in (COMPILER + '.cmd', shutil.which('lmscript.cmd')):
            if candidate and Path(candidate).exists():
                return [candidate]
    return [COMPILER]


def validate_sarif(doc, want_rules, want_results):
    """Structural assertions shared by the lint and check SARIF tests.
    want_results entries: (ruleId, level, message, uri, row, col, end_row, end_col)
    with 0-based rows/cols (the validator expects the 1-based conversion);
    uri None means the result must carry no locations (check body findings)."""
    assert doc['version'] == '2.1.0', doc['version']
    assert doc['$schema'] == 'https://json.schemastore.org/sarif-2.1.0.json'
    assert len(doc['runs']) == 1
    run = doc['runs'][0]
    driver = run['tool']['driver']
    assert driver['name'] == 'lmok', driver
    assert 'version' in driver, driver
    assert [r['id'] for r in driver['rules']] == want_rules, driver['rules']
    results = run['results']
    assert isinstance(results, list)
    assert len(results) == len(want_results), (len(results), len(want_results))
    for result, want in zip(results, want_results):
        rule, level, message, uri, row, col, end_row, end_col = want
        assert result['ruleId'] == rule, result
        assert result['level'] == level, result
        assert result['message']['text'] == message, result
        if uri is None:
            assert 'locations' not in result, result
            continue
        location = result['locations'][0]['physicalLocation']
        expected_uri = quote(uri, safe='/')
        if re.match(r'^[A-Za-z]:/', uri):
            expected_uri = 'file:///' + uri[:2] + quote(uri[2:], safe='/')
        assert location['artifactLocation']['uri'] == expected_uri, location
        region = location['region']
        assert (region['startLine'], region['startColumn']) == (row + 1, col + 1), region
        assert (region['endLine'], region['endColumn']) == (end_row + 1, end_col + 1), region


class Baseline(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='baseline sarif ', dir=ROOT / 'build')
        self.work = Path(self.temp.name)
        self.repo = self.work / 'repo'
        self.repo.mkdir()
        self.assertTrue(shutil.which('git'), 'git is required for the --changed contracts')
        self.git('init', '-q', '.')
        self.git('config', 'user.email', 'baseline@test')
        self.git('config', 'user.name', 'baseline test')
        self.git('config', 'core.autocrlf', 'false')
        (self.repo / 'a.c').write_bytes(COMMIT1)
        (self.repo / 'b.c').write_bytes(b'int g(void) { return 0; }\n')
        self.git('add', 'a.c', 'b.c')
        self.git('commit', '-qm', 'one')
        self.db = self.work / 'lmok.db'

    def tearDown(self):
        self.temp.cleanup()

    def git(self, *args):
        result = subprocess.run(['git', '-C', str(self.repo), *args],
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, (args, result.stderr))
        return result

    def run_host(self, *args, cwd=None, env=None):
        merged = dict(os.environ)
        merged.update(env or {})
        return subprocess.run([str(HOST), *map(str, args)], cwd=cwd or self.repo,
                              env=merged, capture_output=True, timeout=30)

    def advance(self):
        (self.repo / 'a.c').write_bytes(COMMIT2)
        self.git('add', 'a.c')
        self.git('commit', '-qm', 'move gets down, add strcpy')

    def test_new_since_db(self):
        # commit1 recorded, then commit2 moves gets() down and adds strcpy:
        # the moved finding is NOT new, the added one IS.
        base = self.run_host('lint', PACK, 'a.c', '--db', self.db)
        self.assertEqual((base.returncode, base.stderr), (0, b''), base)
        self.assertEqual(base.stdout, (GETS_V1 + '\n1 findings in a.c\n').encode())
        self.advance()
        result = self.run_host('lint', PACK, 'a.c', '--db', self.db, '--new-since', 'last')
        self.assertEqual((result.returncode, result.stderr), (0, b''), result)
        self.assertEqual(result.stdout, (STRCPY + '\n1 findings in a.c\n1 new, 1 baseline\n').encode())
        # The snippet column exists from now on and is populated.
        import sqlite3
        con = sqlite3.connect(self.db)
        try:
            snippets = con.execute('select rule, snippet from findings where superseded = 0 order by id').fetchall()
        finally:
            con.close()
        self.assertEqual(snippets, [('LBC001', 'gets'), ('LBC001', 'strcpy')])
        # An explicit run id selects the same baseline; --fail-on-new exits 4.
        gated = self.run_host('lint', PACK, 'a.c', '--db', self.db, '--new-since', '1', '--fail-on-new')
        self.assertEqual(gated.returncode, 4, gated)
        self.assertEqual(gated.stdout, result.stdout)
        # A re-lint of unchanged content has nothing new: exit 0 even gated.
        quiet = self.run_host('lint', PACK, 'a.c', '--db', self.db, '--new-since', 'last', '--fail-on-new')
        self.assertEqual((quiet.returncode, quiet.stderr), (0, b''), quiet)
        self.assertEqual(quiet.stdout, b'0 findings in a.c\n0 new, 2 baseline\n')
        # Without baseline flags the output shape is unchanged (no summary).
        plain = self.run_host('lint', PACK, 'a.c')
        self.assertEqual(plain.stdout, (GETS_V2 + '\n' + STRCPY + '\n2 findings in a.c\n').encode())
        # Bad selectors and flag combinations are usage errors.
        for args, fragment in [
                (('--new-since', '99'), b'no recorded run with id 99'),
                (('--new-since', 'bogus'), b"bad --new-since value 'bogus'"),
                (('--new-since', 'last', '--baseline', 'x'), b'mutually exclusive'),
                (('--fail-on-new',), b'--fail-on-new needs --new-since or --baseline')]:
            refused = self.run_host('lint', PACK, 'a.c', '--db', self.db, *args)
            self.assertEqual(refused.returncode, 2, (args, refused))
            self.assertIn(fragment, refused.stderr, (args, refused.stderr))
        # A never-recorded DB is not an implicit empty baseline.
        missing = self.run_host('lint', PACK, 'a.c', '--db', self.work / 'no-such.db', '--new-since', 'last')
        self.assertEqual(missing.returncode, 2, missing)
        self.assertIn(b'DB not found', missing.stderr)
        self.assertFalse((self.work / 'no-such.db').exists())

    def test_position_keyed_control_fails(self):
        # FAILING CONTROL: keying on (rule, file, message, row, col) instead of
        # the snippet MUST flag the moved gets() finding as new — if it does
        # not, the fixture is broken and test_new_since_db proves nothing.
        self.run_host('lint', PACK, 'a.c', '--db', self.db)
        self.advance()
        self.run_host('lint', PACK, 'a.c', '--db', self.db)
        import sqlite3
        con = sqlite3.connect(self.db)
        try:
            rows = con.execute('select run_id, rule, message, start_row, start_col from findings order by id').fetchall()
        finally:
            con.close()
        by_run = {}
        for run_id, rule, message, row, col in rows:
            by_run.setdefault(run_id, set()).add((rule, message, row, col))
        naive_new = by_run[2] - by_run[1]
        self.assertEqual(naive_new,
                         {('LBC001', "unsafe call 'gets'", 6, 2), ('LBC001', "unsafe call 'strcpy'", 7, 2)},
                         'position-keyed diff must flag BOTH the moved and the added finding')
        actual = self.run_host('lint', PACK, 'a.c', '--db', self.db, '--new-since', '1')
        self.assertEqual(actual.stdout, (STRCPY + '\n1 findings in a.c\n1 new, 1 baseline\n').encode())

    def test_baseline_file(self):
        # --baseline FILE: a previous lint JSON-lines output; no DB involved.
        (self.work / 'base.lintout').write_bytes(
            self.run_host('lint', PACK, 'a.c', cwd=self.repo).stdout)
        self.advance()
        result = self.run_host('lint', PACK, 'a.c', '--baseline', self.work / 'base.lintout')
        self.assertEqual((result.returncode, result.stderr), (0, b''), result)
        self.assertEqual(result.stdout, (STRCPY + '\n1 findings in a.c\n1 new, 1 baseline\n').encode())
        gated = self.run_host('lint', PACK, 'a.c', '--baseline', self.work / 'base.lintout', '--fail-on-new')
        self.assertEqual(gated.returncode, 4, gated)
        # FAILING CONTROL: a baseline whose finding lines lack the key fields
        # (rule/message) must misclassify everything as new — entries that
        # cannot be keyed never match.
        broken = []
        for line in (self.work / 'base.lintout').read_text().splitlines():
            if line.startswith('{'):
                entry = json.loads(line)
                del entry['rule'], entry['message']
                broken.append(json.dumps(entry))
            else:
                broken.append(line)
        (self.work / 'broken.lintout').write_text('\n'.join(broken) + '\n')
        mis = self.run_host('lint', PACK, 'a.c', '--baseline', self.work / 'broken.lintout')
        self.assertEqual(mis.stdout,
                         (GETS_V2 + '\n' + STRCPY + '\n2 findings in a.c\n2 new, 0 baseline\n').encode())
        unreadable = self.run_host('lint', PACK, 'a.c', '--baseline', self.work / 'no-such.lintout')
        self.assertEqual(unreadable.returncode, 3, unreadable)
        self.assertIn(b'cannot read', unreadable.stderr)

    def test_baseline_db_without_snippet_column_falls_back(self):
        # A DB recorded before the snippet column existed: drop the column and
        # the baseline must still match on (rule, file, message) alone.
        result = self.run_host('lint', PACK, 'a.c', '--db', self.db)
        self.assertEqual(result.returncode, 0, result)
        import sqlite3
        con = sqlite3.connect(self.db)
        try:
            con.execute('alter table findings drop column snippet')
            con.commit()
        finally:
            con.close()
        result = self.run_host('lint', PACK, 'a.c', '--db', self.db, '--new-since', 'last', '--fail-on-new')
        self.assertEqual((result.returncode, result.stderr), (0, b''), result)
        self.assertEqual(result.stdout, b'0 findings in a.c\n0 new, 1 baseline\n')

    def test_changed(self):
        # Bare --changed = working tree vs HEAD: only the touched file lints.
        with (self.repo / 'a.c').open('ab') as handle:
            handle.write(b'// touched\n')
        result = self.run_host('lint', PACK, 'a.c', 'b.c', '--changed')
        self.assertEqual((result.returncode, result.stderr), (0, b''), result)
        self.assertEqual(result.stdout, (GETS_V1 + '\n1 findings in a.c\n').encode())
        self.git('add', 'a.c')
        self.git('commit', '-qm', 'touch')
        self.advance()
        rev = self.run_host('lint', PACK, 'a.c', 'b.c', '--changed=HEAD~1')
        self.assertEqual(rev.returncode, 0, rev)
        self.assertNotIn(b'b.c', rev.stdout)
        # Unknown revision and injection-shaped revisions are refused (exit 2).
        unknown = self.run_host('lint', PACK, 'a.c', '--changed=HEAD~9')
        self.assertEqual(unknown.returncode, 2, unknown)
        self.assertIn(b'git diff --name-only HEAD~9 failed', unknown.stderr)
        bad = self.run_host('lint', PACK, 'a.c', '--changed=HE;AD')
        self.assertEqual(bad.returncode, 2, bad)
        self.assertIn(b'bad --changed revision', bad.stderr)
        # Outside a git repository: fail fast, exit 2 with a clear message.
        outside = self.work / 'outside'
        outside.mkdir()
        (outside / 'x.c').write_bytes(COMMIT1)
        refused = self.run_host('lint', PACK, 'x.c', '--changed', cwd=outside,
                                env={'GIT_CEILING_DIRECTORIES': self.work.as_posix()})
        self.assertEqual(refused.returncode, 2, refused)
        self.assertIn(b'--changed needs a git repository', refused.stderr)

    def test_lint_sarif(self):
        # A pack with all three mapped severities plus a top-level url (SARIF helpUri).
        pack = self.work / 'pack.json'
        pack.write_text(json.dumps({'lang': 'c', 'rules': [
            {'id': 'C1', 'on': 'comment', 'message': 'crit {text}', 'severity': 'critical'},
            {'id': 'M1', 'on': 'comment', 'message': 'maj {text}', 'severity': 'major'},
            {'id': 'N1', 'on': 'comment', 'message': 'min {text}', 'severity': 'minor',
             'url': 'https://example.test/N1'}]}))
        source = self.repo / 'input.c'
        source.write_bytes(b'// hi\nint x;\n')
        sarif = self.work / 'lint.sarif'
        result = self.run_host('lint', pack, 'input.c', '--sarif', sarif)
        self.assertEqual((result.returncode, result.stderr), (0, b''), result)
        # stdout stays the normal JSON-lines output.
        self.assertEqual(result.stdout.count(b'\n'), 4, result.stdout)  # 3 findings + count line
        doc = json.loads(sarif.read_bytes())
        self.assertEqual([r['id'] for r in doc['runs'][0]['tool']['driver']['rules']], ['C1', 'M1', 'N1'])
        rules = {r['id']: r for r in doc['runs'][0]['tool']['driver']['rules']}
        self.assertEqual(rules['C1']['shortDescription']['text'], 'crit {text}')
        self.assertEqual(rules['N1']['shortDescription']['text'], 'min {text}')
        self.assertEqual(rules['N1']['helpUri'], 'https://example.test/N1')
        self.assertNotIn('helpUri', rules['C1'])
        validate_sarif(doc, ['C1', 'M1', 'N1'], [
            ('C1', 'error', 'crit // hi', 'input.c', 0, 0, 0, 5),
            ('M1', 'warning', 'maj // hi', 'input.c', 0, 0, 0, 5),
            ('N1', 'note', 'min // hi', 'input.c', 0, 0, 0, 5)])
        # Two files: results aggregate in argument order, uris as printed.
        other = self.repo / 'other.c'
        other.write_bytes(b'int y; // hey\n')
        multi = self.work / 'multi.sarif'
        result = self.run_host('lint', pack, 'other.c', 'input.c', '--sarif', multi)
        self.assertEqual(result.returncode, 0, result)
        doc = json.loads(multi.read_bytes())
        self.assertEqual([r['locations'][0]['physicalLocation']['artifactLocation']['uri']
                          for r in doc['runs'][0]['results']],
                         ['other.c'] * 3 + ['input.c'] * 3)
        # Zero findings still writes the file with an empty results array.
        empty = self.work / 'empty.sarif'
        result = self.run_host('lint', pack, 'b.c', '--sarif', empty)
        self.assertEqual(result.returncode, 0, result)
        doc = json.loads(empty.read_bytes())
        validate_sarif(doc, ['C1', 'M1', 'N1'], [])
        # An unwritable SARIF path fails the run (exit 3), after lint output.
        failed = self.run_host('lint', pack, 'input.c', '--sarif', self.work / 'no-dir/x.sarif')
        self.assertEqual(failed.returncode, 3, failed)
        self.assertIn(b'cannot write', failed.stderr)

    def test_sarif_control_rejects_unconverted_region(self):
        # FAILING CONTROL: the validator must reject a region whose startLine
        # was left 0-based (the port forgetting the +1).
        doc = {'version': '2.1.0', '$schema': 'https://json.schemastore.org/sarif-2.1.0.json',
               'runs': [{'tool': {'driver': {'name': 'lmok', 'version': '0', 'rules': [{'id': 'C1'}]}},
                         'results': [{'ruleId': 'C1', 'level': 'error', 'message': {'text': 'm'},
                                      'locations': [{'physicalLocation': {'artifactLocation': {'uri': 'a.c'},
                                                     'region': {'startLine': 1, 'startColumn': 1,
                                                                'endLine': 1, 'endColumn': 2}}}]}]}]}
        want = [('C1', 'error', 'm', 'a.c', 0, 0, 0, 1)]
        validate_sarif(copy.deepcopy(doc), ['C1'], want)  # sane baseline passes
        broken = copy.deepcopy(doc)
        broken['runs'][0]['results'][0]['locations'][0]['physicalLocation']['region']['startLine'] = 0
        with self.assertRaises(AssertionError):
            validate_sarif(broken, ['C1'], want)
        broken = copy.deepcopy(doc)
        broken['runs'][0]['results'][0]['level'] = 'warning'
        with self.assertRaises(AssertionError):
            validate_sarif(broken, ['C1'], want)

    def test_check_sarif(self):
        # lmcode rules through check: one body finding (unlocated) plus one
        # entry finding per file.
        source = self.work / 'rule.lms'
        binary = self.work / 'rule.dslb'
        source.write_text(
            'p = _lmcode_ schema=' + json.dumps(str(ROOT / 'schema/lmok.dsl.json')) + ';\n'
            'ts.report(ts.parse_c("int body;").root(), "body finding", "B001", "major");\n'
            '@comment fn fixme(n: Node) -> void implements Rule { ts.report(n, "FIXME left", "X001", "minor"); }\n'
            'end _lmcode_\n')
        compiled = subprocess.run(["node",str(ROOT/"scripts/compile-fixture.mjs"), str(source), str(binary)], capture_output=True, text=True)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        target = self.repo / 'input.c'
        target.write_bytes(b'// FIXME: later\nint x;\n')
        sarif = self.work / 'check.sarif'
        result = self.run_host('check', '--lang', 'c', binary, 'input.c', '--sarif', sarif)
        self.assertEqual((result.returncode, result.stderr), (0, b''), result)
        doc = json.loads(sarif.read_bytes())
        validate_sarif(doc, ['B001', 'X001'], [
            ('B001', 'warning', 'body finding', None, 0, 0, 0, 0),
            ('X001', 'note', 'FIXME left', 'input.c', 0, 0, 0, 15)])
        rules = {r['id']: r for r in doc['runs'][0]['tool']['driver']['rules']}
        self.assertEqual(rules['X001']['shortDescription']['text'], 'rule X001')
        # Zero findings: the file is still written, with empty rules/results.
        clean = self.repo / 'b.c'
        empty = self.work / 'check-empty.sarif'
        result = self.run_host('check', '--lang', 'c', binary, clean, '--sarif', empty)
        self.assertEqual(result.returncode, 0, result)
        validate_sarif(json.loads(empty.read_bytes()), ['B001'],
                       [('B001', 'warning', 'body finding', None, 0, 0, 0, 0)])

    def test_ci_sarif(self):
        # The wp18a ci verb landed first; --sarif hangs off it additively.
        pack = self.work / 'pack.json'
        pack.write_text(json.dumps({'lang': 'c', 'rules': [
            {'id': 'C1', 'on': 'comment', 'message': 'crit {text}', 'severity': 'critical',
             'url': 'https://example.test/C1'}]}))
        project = self.work / 'proj'
        (project / '.lmok').mkdir(parents=True)
        (project / '.lmok/config.json').write_text(json.dumps(
            {'version': 1, 'packs': {'c': ['pack.json']}, 'severity_floor': 'critical', 'fail_on': 'findings'}))
        shutil.copy(pack, project / 'pack.json')
        (project / 'live.c').write_bytes(b'// hi\nint x;\n')
        sarif = self.work / 'ci.sarif'
        result = self.run_host('ci', '--sarif', sarif, cwd=project)
        self.assertEqual(result.returncode, 1, result)  # critical finding trips the floor
        doc = json.loads(sarif.read_bytes())
        validate_sarif(doc, ['C1'], [('C1', 'error', 'crit // hi', './live.c', 0, 0, 0, 5)])
        rules = {r['id']: r for r in doc['runs'][0]['tool']['driver']['rules']}
        self.assertEqual(rules['C1']['helpUri'], 'https://example.test/C1')
        # A clean project still writes the file (empty results), exit 0.
        (project / 'live.c').write_bytes(b'int x;\n')
        result = self.run_host('ci', '--sarif', sarif, cwd=project)
        self.assertEqual(result.returncode, 0, result)
        validate_sarif(json.loads(sarif.read_bytes()), ['C1'], [])


if __name__ == '__main__':
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(Baseline))
    if not result.wasSuccessful():
        sys.exit(1)
    print('ok - baseline/new-code keying, --changed, fail-on-new and host SARIF (lint + check + ci), controls proven')
