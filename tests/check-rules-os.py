#!/usr/bin/env python3
"""Rule-pack dispatch oracles per native OS: every converted rules/lmcode pack,
line-ending/unicode edge fixtures, anonymous-kind annotations and multi-file
ordering, byte-exact on each Actions runner. Complements check-entries.py
(dispatch semantics) and check-native-os.py (host byte/path/linkage contracts);
nothing here repeats their fixtures."""
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HOST = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else ROOT / 'build' / ('lmok.exe' if os.name == 'nt' else 'lmok')
PROVE = '--prove-controls' in sys.argv
if PROVE: sys.argv.remove('--prove-controls')
MUTATE = False
COMPILER = os.environ.get('LMSCRIPT_BIN', 'lmscript')

def compiler_argv():
    # On Windows npm installs `lmscript` as an extension-less sh script plus a
    # `lmscript.cmd` shim; CreateProcess can run only the shim (WinError 193 otherwise).
    if os.name == 'nt' and not COMPILER.lower().endswith(('.cmd', '.exe', '.bat')):
        for candidate in (COMPILER + '.cmd', shutil.which('lmscript.cmd')):
            if candidate and Path(candidate).exists():
                return [candidate]
    return [COMPILER]

class RulePacksOS(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='rules os ', dir=ROOT / 'build')
        self.work = Path(self.temp.name)
    def tearDown(self):
        self.temp.cleanup()

    def test_lmscript_lmcode_compiled_check_dispatch(self):
        # Exercise the same compiler-to-check boundary as a real rule pack for
        # both registrations; the host must preserve the selected language.
        body = ('@function_declaration fn check(n: Node) -> void implements Rule { '
                'ts.report(n, "function", "LANG001", "minor"); }')
        binary = self.compile(body)
        for lang, suffix, source_text in (
                ('lmcode', '.lmcode', 'fn f(x: string) -> string { return x; }\n'),
                ('lmscript', '.lms', 'fn f(x) { return x; }\n')):
            source = self.work / ('dispatch' + suffix)
            source.write_text(source_text, encoding='utf-8')
            result = self.run_host('check', '--lang', lang, binary, source, cwd=ROOT)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(b'LANG001', result.stdout)
            self.assertTrue(result.stdout.endswith(b'LMCODE_RETURN_END\n'), result.stdout)
    def run_host(self, *args, cwd=None):
        result = subprocess.run([str(HOST), *map(str, args)], cwd=cwd, capture_output=True, timeout=30)
        if MUTATE: result.stdout += b'\r'  # Deliberate output corruption: every byte oracle must reject it.
        return result
    def compile(self, body):
        src = self.work / 'rule.lms'
        binary = self.work / 'rule.dslb'
        src.write_text('p = _lmcode_ schema=' + json.dumps(str(ROOT / 'schema/lmok.dsl.json')) + ';\n' + body + '\nend _lmcode_\n')
        r = subprocess.run(["node",str(ROOT/"scripts/compile-fixture.mjs"), str(src), str(binary)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return binary

    def test_every_converted_rule_pack_runs_on_this_os(self):
        # Every pack in rules/lmcode compiles with the platform compiler and
        # dispatches natively: violations fixture yields N>0 findings with the
        # full finding shape, and the count line matches the array length.
        packs = sorted((ROOT / 'rules/lmcode').glob('*.lms'))
        self.assertEqual(len(packs), 14, packs)  # a new pack must be wired in here by glob, so pin the count
        for pack in packs:
            with self.subTest(pack=pack.name):
                header = [line[9:].split() for line in pack.read_text().splitlines() if line.startswith('# check: ')]
                r = subprocess.run(["node",str(ROOT/"scripts/compile-fixture.mjs"), str(pack), str(self.work / (pack.stem + '.dslb'))], capture_output=True, text=True, cwd=ROOT)
                self.assertEqual(r.returncode, 0, r.stderr)
                # Use this test invocation's explicit fixture output, never a stale build artifact.
                binary = self.work / (pack.stem + '.dslb')
                self.assertTrue(binary.exists(), r.stdout + r.stderr)
                if header:
                    lang, fixture = header[0]
                    result = self.run_host('check', '--lang', lang, binary, fixture, cwd=ROOT)
                    self.assertEqual(result.stderr, b'', result)
                    self.assertEqual(result.returncode, 0, result.stdout)
                    lines = result.stdout.split(b'\n')
                    self.assertEqual(lines.pop(), b'')
                    self.assertEqual(lines.pop(), b'LMCODE_RETURN_END')
                    self.assertEqual(lines.pop(), b'{"ok":true,"result":null}')
                    self.assertEqual(lines.pop(), b'LMCODE_RETURN_BEGIN')
                    self.assertEqual(json.loads(lines.pop()), {"host_summary":{"input":Path(fixture).as_posix()}})
                    count = lines.pop().decode()
                    findings = json.loads(lines[0])
                    self.assertEqual(len(lines), 1, lines)  # one findings group, one count line
                    self.assertEqual(count, f'{len(findings)} findings in {Path(fixture).as_posix()}')
                    self.assertGreater(len(findings), 0, pack.name)
                    for finding in findings:
                        self.assertEqual(sorted(finding), ['col', 'end_col', 'end_row', 'message', 'row', 'rule', 'severity'], finding)
                else:
                    # Packs without a '# check:' header keep the body-only --lmcode path.
                    result = self.run_host('--lmcode', binary, cwd=ROOT)
                    self.assertEqual(result.stderr, b'', result)
                    self.assertEqual(result.returncode, 0, result.stdout)
                    self.assertTrue(result.stdout.endswith(b'LMCODE_RETURN_BEGIN\n{"ok":true,"result":null}\nLMCODE_RETURN_END\n'), result.stdout)

    def test_crlf_and_missing_trailing_newline_are_byte_stable(self):
        # Findings are computed on bytes: CRLF and a missing final newline must
        # not move a row, column or message on any OS.
        binary = self.compile('@pair fn key(n: Node) -> void implements Rule { ts.report(n, n.child(0).text(), "K", "minor"); }')
        variants = {'lf': b'{"a": 1,\n "b": 2}\n', 'crlf': b'{"a": 1,\r\n "b": 2}\r\n', 'nonewline': b'{"a": 1,\n "b": 2}'}
        groups = []
        for name, data in variants.items():
            source = self.work / (name + '.json')
            source.write_bytes(data)
            result = self.run_host('check', '--lang', 'json', binary, source)
            self.assertEqual((result.returncode, result.stderr), (0, b''), result)
            lines = result.stdout.split(b'\n')
            self.assertEqual(lines.pop(), b'')
            self.assertEqual(lines.pop(), b'LMCODE_RETURN_END')
            self.assertEqual(lines.pop(), b'{"ok":true,"result":null}')
            self.assertEqual(lines.pop(), b'LMCODE_RETURN_BEGIN')
            self.assertEqual(json.loads(lines.pop()), {"host_summary":{"input":source.as_posix()}})
            count = lines.pop()
            self.assertEqual(count, ('2 findings in ' + source.as_posix()).encode())
            groups.append(lines[0])
        self.assertEqual(groups[0], groups[1], 'CRLF moved the findings')
        self.assertEqual(groups[0], groups[2], 'missing trailing newline moved the findings')
        findings = json.loads(groups[0])
        self.assertEqual([(f['row'], f['col'], f['end_row'], f['end_col'], f['message']) for f in findings],
                         [(0, 1, 0, 7, '"a"'), (1, 1, 1, 7, '"b"')])

    def test_unicode_identifiers_have_byte_columns(self):
        # Columns are byte offsets on every platform; a UTF-16-code-unit host
        # would compute different end columns for non-ASCII identifiers.
        binary = self.compile('@identifier fn name(n: Node) -> void implements Rule { ts.report(n, n.text(), "U", "minor"); }')
        for lang, text in [('python', 'def héllo_雪():\n    return 1\n'), ('c', 'int café_水;\n'), ('go', 'package main\nfunc café_水() {}\n'), ('javascript', 'function café_水() {}\n'), ('typescript', 'function café_水(): string { return "ok"; }\n'), ('tsx', 'function café_水(): string { return "ok"; }\n')]:
            with self.subTest(lang=lang):
                source = self.work / ('unicode.' + {'python':'py','c':'c','go':'go','javascript':'js','typescript':'ts','tsx':'tsx'}[lang])
                source.write_bytes(text.encode('utf-8'))
                result = self.run_host('check', '--lang', lang, binary, source)
                self.assertEqual((result.returncode, result.stderr), (0, b''), result)
                findings = json.loads(result.stdout.split(b'\n')[0])
                name = 'héllo_雪' if lang == 'python' else 'café_水'
                match = [f for f in findings if f['message'] == name]
                self.assertEqual(len(match), 1, findings)
                self.assertEqual(match[0]['end_col'] - match[0]['col'], len(name.encode('utf-8')), match)
                self.assertTrue(result.stdout.endswith((' findings in ' + source.as_posix() + '\n'+json.dumps({'host_summary':{'input':source.as_posix()}},ensure_ascii=False,separators=(',',':'))+'\nLMCODE_RETURN_BEGIN\n{"ok":true,"result":null}\nLMCODE_RETURN_END\n').encode()), result.stdout)

    def test_anonymous_kind_annotations_dispatch_on_every_grammar(self):
        # Anonymous (unnamed) node kinds are legal annotation values (R47 keeps
        # keyword spellings); pin one per grammar so a grammar-table regression
        # fails on the OS that saw it.
        # json has no usable anonymous kind (its only unnamed token is `"`,
        # which is not a legal annotation identifier) — one case per grammar that has one.
        cases = [
            ('c', '@return', b'int f(void) { return 1; }\n', 1, 'return'),
            ('python', '@def', b'def f():\n    return 1\n', 1, 'def'),
            ('bash', '@if', b'if true; then echo x; fi\n', 1, 'if'),
            ('go', '@return', b'package p\nfunc f() { return }\n', 1, 'return'),
            ('javascript', '@return_statement', b'function f() { return 1; }\n', 1, 'return 1;'),
            ('typescript', '@return_statement', b'function f(): number { return 1; }\n', 1, 'return 1;'),
            ('tsx', '@return_statement', b'function f(): number { return <div />; }\n', 1, 'return <div />;'),
        ]
        for lang, annotation, data, want_count, want_text in cases:
            with self.subTest(lang=lang):
                binary = self.compile(annotation + ' fn kw(n: Node) -> void implements Rule { ts.report(n, n.text(), "KW", "minor"); }')
                source = self.work / ('anon.' + lang)
                source.write_bytes(data)
                result = self.run_host('check', '--lang', lang, binary, source)
                self.assertEqual((result.returncode, result.stderr), (0, b''), result)
                findings = json.loads(result.stdout.split(b'\n')[0])
                self.assertEqual([(f['message'], f['rule']) for f in findings], [(want_text, 'KW')] * want_count, findings)
                self.assertTrue(result.stdout.endswith((f'{want_count} findings in {source.as_posix()}\n' + json.dumps({'host_summary':{'input':source.as_posix()}},ensure_ascii=False,separators=(',',':'))+'\nLMCODE_RETURN_BEGIN\n{"ok":true,"result":null}\nLMCODE_RETURN_END\n').encode()), result.stdout)

    def test_multi_file_groups_follow_argument_order(self):
        # Group order is argument order (never sorted, never walked order) and
        # each count line names its file; a host that sorts would swap groups.
        binary = self.compile('@number fn num(n: Node) -> void implements Rule { ts.report(n, n.text(), "N", "minor"); }')
        names = ['z-last.json', 'a-first.json', 'm-mid.json']
        for index, name in enumerate(names):
            (self.work / name).write_bytes(b'[' + b','.join([str(index + 1).encode()] * (index + 1)) + b']\n')
        paths = [self.work / name for name in names]
        result = self.run_host('check', '--lang', 'json', binary, *paths)
        self.assertEqual((result.returncode, result.stderr), (0, b''), result)
        lines = result.stdout.split(b'\n')
        self.assertEqual(lines.pop(), b'')
        counts = [line.decode() for line in lines if b' findings in ' in line]
        self.assertEqual(counts, [f'{i + 1} findings in {paths[i].as_posix()}' for i in range(3)])
        groups = [json.loads(line) for line in lines if line.startswith(b'[')]
        self.assertEqual([[int(f['message']) for f in group] for group in groups], [[1], [2, 2], [3, 3, 3]])

class LauncherOS(unittest.TestCase):
    # Not part of --prove-controls: this asserts process-launch mechanics, not
    # host output. The failing control is Windows-only by construction (the
    # extension-less shim fails CreateProcess with WinError 193; lmok 9dca21c).
    def test_compiler_launcher_is_platform_correct(self):
        argv = compiler_argv()
        if os.name == 'nt':
            self.assertTrue(argv[0].lower().endswith(('.cmd', '.exe', '.bat')), argv)
        r = subprocess.run([*argv, '--version'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        # This oracle checks launcher mechanics; RulePacksOS above compiles and
        # executes actual v6 entries. Source package metadata can be historical.
        self.assertRegex(r.stdout, r'^lmscript \d+\.\d+\.\d+\b', r.stdout)

if __name__ == '__main__':
    names = unittest.defaultTestLoader.getTestCaseNames(RulePacksOS)
    result = unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite(
        [unittest.defaultTestLoader.loadTestsFromTestCase(RulePacksOS),
         unittest.defaultTestLoader.loadTestsFromTestCase(LauncherOS)]))
    if not result.wasSuccessful(): sys.exit(1)
    if PROVE:
        MUTATE = True
        for name in names:
            control = unittest.TestResult()
            RulePacksOS(name).run(control)
            if not control.failures or control.errors:
                raise AssertionError('mutation did not produce an assertion failure: ' + name + str(control.errors))
            print('ok - caught rule-output mutant: ' + name)
    print('ok - rule packs, line endings, unicode, anonymous kinds and ordering (' + str(len(names)) + ' oracles + launcher)')
