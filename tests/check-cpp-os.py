#!/usr/bin/env python3
"""C++ grammar, header policy, rules, metrics and facts oracles across native CI platforms."""
import collections
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HOST = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 and not sys.argv[1].startswith('--') else ROOT / 'build' / ('lmok.exe' if os.name == 'nt' else 'lmok')
PROVE = '--prove-controls' in sys.argv
if PROVE:
    sys.argv.remove('--prove-controls')
CORRUPT = False


class CppOS(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='cpp space ', dir=ROOT / 'build')
        self.work = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def run_host(self, *args, check=True):
        r = subprocess.run([str(HOST), *map(str, args)], capture_output=True, cwd=ROOT, timeout=30)
        if CORRUPT and check:
            r.stdout += b'\r'
        if check:
            self.assertEqual(r.returncode, 0, r.stderr.decode('utf-8', 'replace'))
            self.assertEqual(r.stderr, b'')
            self.assertNotIn(b'\r', r.stdout)
        return r

    @staticmethod
    def _compiler_env():
        env = os.environ.copy()
        if 'LMCODE_COMPILER_MODULE' in env and env['LMCODE_COMPILER_MODULE']:
            return env
        bin_path = env.get('LMSCRIPT_BIN', '').strip()
        if not bin_path:
            sel = subprocess.run(
                ['bash', str(ROOT / 'scripts/select-compiler.sh')],
                capture_output=True, text=True, cwd=ROOT, timeout=15, check=True,
            )
            bin_path = sel.stdout.strip()
        env['LMSCRIPT_BIN'] = bin_path
        resolved = Path(bin_path).resolve().parent
        for cand in (
            resolved.parent / 'dist/dsl.js',
            resolved.parent / 'lib/node_modules/@lmctl-ai/lmctl/dist/dsl.js',
            ROOT / 'node_modules/@lmctl-ai/lmctl/dist/dsl.js',
        ):
            if cand.exists():
                env['LMCODE_COMPILER_MODULE'] = str(cand)
                break
        return env

    def test_parse_query_utf8_crlf_and_raw_strings(self):
        for newline in [b'\n', b'\r\n']:
            source = self.work / 'Café 雪.cpp'
            source.write_bytes(
                newline.join([
                    b'namespace outer::inner {',
                    b'template <typename T>',
                    b'class Demo {',
                    b'public:',
                    b'  T value;',
                    b'  auto fn() const { return [v = value]() { return v; }; }',
                    b'};',
                    b'}',
                    b'',
                ])
            )
            tree = self.run_host('parse', '--lang', 'cpp', source).stdout
            self.assertTrue(tree.startswith(b'(translation_unit '), tree)
            self.assertNotIn(b'ERROR', tree)
            self.assertNotIn(b'MISSING', tree)
            q = self.run_host('query', '--lang', 'cpp', '(class_specifier name: (type_identifier) @name)', source).stdout
            self.assertEqual(q, b'type_identifier 2:6-2:10\n')

        raw_ok = self.work / 'raw_ok.cpp'
        raw_ok.write_text(
            'const char *r0 = R"(a)b\n\U0001F680)";\n'
            'const char *r1 = R"d(a)b)"c)d";\n'
            'const char *r15 = R"0123456789abcde(clean)0123456789abcde";\n',
            encoding='utf-8',
        )
        tree_raw = self.run_host('parse', '--lang', 'cpp', raw_ok).stdout
        self.assertNotIn(b'ERROR', tree_raw)
        self.assertNotIn(b'MISSING', tree_raw)

        # Known v0.23.4 scanner limit: 16-char delimiter causes local ERROR without OOB crash.
        limits = self.run_host('parse', '--lang', 'cpp', ROOT / 'tests/fixtures/cpp-known-limits.cpp').stdout
        self.assertIn(b'ERROR', limits)

        # Strict "cpp" only: reject "c++"
        bad = self.run_host('parse', '--lang', 'c++', raw_ok, check=False)
        self.assertEqual(bad.returncode, 2)

    def test_facts_namespaces_methods_and_cast_filter(self):
        src = self.work / 'Symbols.cpp'
        src.write_text(
            '#include <vector>\n'
            '#include "local.hpp"\n'
            'using AliasPtr = int *;\n'
            'enum class Color { Red };\n'
            'namespace outer::inner {\n'
            'class Widget {\n'
            'public:\n'
            '  int count;\n'
            '  int in_cls() const { return count; }\n'
            '  friend void touch(Widget &w) {}\n'
            '  ~Widget();\n'
            '};\n'
            'template <class T>\n'
            'struct Box {\n'
            '  T val;\n'
            '  T size() const;\n'
            '};\n'
            '}\n'
            'outer::inner::Widget::~Widget() {}\n'
            'int outer::inner::Widget::in_out() const { return 1; }\n'
            'void MaybeNs::helper() {}\n'
            'template <class T>\n'
            'T outer::inner::Box<T>::size() const { return val; }\n'
            'void driver(void *p, const int *cp, double d, outer::inner::Widget &w) {\n'
            '  auto a = static_cast<int>(d);\n'
            '  auto b = dynamic_cast<outer::inner::Widget *>(&w);\n'
            '  auto c = const_cast<int *>(cp);\n'
            '  auto e = reinterpret_cast<long>(p);\n'
            '  w.in_cls();\n'
            '  helper_free(a, b, c, e);\n'
            '}\n',
            encoding='utf-8',
        )
        rows = [json.loads(line) for line in self.run_host('facts', src).stdout.splitlines()]
        self.assertTrue(all(r['lang'] == 'cpp' for r in rows))
        triples = {(r['kind'], r['name'], r['scope']) for r in rows}
        confs = {(r['kind'], r['name'], r['scope']): r['confidence'] for r in rows}
        self.assertIn(('def.include', 'vector', '') if False else ('ref.include', 'vector', ''), triples)
        self.assertEqual(confs[('ref.include', 'vector', '')], 'external')
        self.assertEqual(confs[('ref.include', 'local.hpp', '')], 'exact')
        self.assertIn(('def.namespace', 'outer::inner', ''), triples)
        self.assertIn(('def.class', 'Widget', 'outer.inner'), triples)
        self.assertIn(('def.struct', 'Box', 'outer.inner'), triples)
        self.assertIn(('def.type', 'AliasPtr', ''), triples)
        self.assertIn(('def.enum', 'Color', ''), triples)
        self.assertIn(('def.enumerator', 'Red', 'Color'), triples)
        self.assertIn(('def.method', 'in_cls', 'outer.inner.Widget'), triples)
        self.assertEqual(confs[('def.method', 'in_cls', 'outer.inner.Widget')], 'exact')
        self.assertIn(('def.function', 'touch', 'outer.inner.Widget'), triples)
        self.assertEqual(confs[('def.method', '~Widget', 'outer.inner.Widget')], 'exact')
        self.assertEqual(confs[('def.method', 'in_out', 'outer.inner.Widget')], 'exact')
        self.assertEqual(confs[('def.method', 'helper', 'MaybeNs')], 'candidate')
        self.assertEqual(confs[('def.method', 'size', 'outer.inner.Box')], 'exact')
        self.assertIn(('ref.call', 'in_cls', 'driver'), triples)
        self.assertIn(('ref.call', 'helper_free', 'driver'), triples)
        call_names = {r['name'] for r in rows if r['kind'] == 'ref.call'}
        for cast in ('static_cast', 'dynamic_cast', 'const_cast', 'reinterpret_cast'):
            self.assertNotIn(cast, call_names)

    def test_metrics_and_duplication(self):
        src = self.work / 'metrics.cpp'
        src.write_text(
            'void f(const int *xs, int n) {\n'
            '  for (int i = 0; i < n; ++i) {\n'
            '    for (int x : {1, 2, 3}) {\n'
            '      if (xs != nullptr) { (void)x; }\n'
            '    }\n'
            '  }\n'
            '}\n',
            encoding='utf-8',
        )
        m = json.loads(self.run_host('metrics', src).stdout)
        self.assertEqual(m['functions'], 1)
        self.assertEqual(m['max_nesting'], 3)

    def test_header_policy_lint_index_invalidation_and_ci(self):
        proj = self.work / 'proj'
        proj.mkdir()
        hdr = proj / 'hdr.h'
        hdr.write_text('namespace api {\ninline int *bad() { return NULL; }\n}\n', encoding='utf-8')
        (proj / 'impl.cpp').write_text('int *g() { return NULL; }\n', encoding='utf-8')

        # Default dir walk: .h -> c, .cpp -> cpp
        default_langs = {
            Path(r['file']).name: r['lang']
            for r in (json.loads(l) for l in self.run_host('facts', proj).stdout.splitlines())
        }
        self.assertEqual(default_langs.get('impl.cpp'), 'cpp')
        self.assertEqual(default_langs.get('hdr.h'), 'c')

        # --header-lang cpp overrides .h across facts and index, invalidating stale c rows.
        db = self.work / 'index.db'
        self.run_host('index', '--db', db, proj)
        with sqlite3.connect(db) as con:
            row = con.execute('SELECT lang FROM files WHERE path LIKE ?', ('%hdr.h',)).fetchone()
            self.assertEqual(row, ('c',))
        self.run_host('index', '--db', db, '--header-lang', 'cpp', proj)
        with sqlite3.connect(db) as con:
            row = con.execute('SELECT lang FROM files WHERE path LIKE ?', ('%hdr.h',)).fetchone()
            self.assertEqual(row, ('cpp',))
            ft = con.execute('SELECT lang FROM file_text WHERE path = ?', (hdr.resolve().as_posix(),)).fetchone()
            self.assertEqual(ft, ('cpp',))
            ns = con.execute("SELECT name, kind FROM symbols WHERE name = 'api'").fetchone()
            self.assertEqual(ns, ('api', 'namespace'))

        # Mixed pack: .h receives ONLY C rules by default and ONLY C++ rules with --header-lang cpp
        mixed = self.work / 'mixed.json'
        mixed.write_text(json.dumps({
            'rules': [
                {'id': 'C_ONLY', 'on': 'identifier', 'text_matches': '^api$', 'message': 'c saw api', 'lang': 'c'},
                {'id': 'CPP_NULL', 'on': 'null', 'text_matches': '^NULL$', 'message': 'cpp saw NULL', 'lang': 'cpp'},
            ]
        }), encoding='utf-8')
        out_c = self.run_host('lint', mixed, hdr).stdout.splitlines()
        rules_c = [json.loads(l)['rule'] for l in out_c[:-1]]
        self.assertNotIn('CPP_NULL', rules_c)
        out_cpp = self.run_host('lint', mixed, '--header-lang', 'cpp', proj).stdout.splitlines()
        rules_cpp = [json.loads(l)['rule'] for l in out_cpp if l.startswith(b'{')]
        self.assertTrue(all(r == 'CPP_NULL' for r in rules_cpp) and len(rules_cpp) == 2, rules_cpp)

        # Single-file explicit .h lint with C++-only pack uses Cpp without requiring --header-lang
        explicit = self.run_host('lint', ROOT / 'rules/cpp-tier1.json', hdr).stdout.splitlines()
        self.assertEqual([json.loads(l)['rule'] for l in explicit[:-1]], ['CPP007'])

        # Project config header_lang: cpp in ci
        lmok_dir = proj / '.lmok'
        lmok_dir.mkdir()
        (lmok_dir / 'config.json').write_text(json.dumps({
            'version': 1,
            'packs': {'cpp': [str((ROOT / 'rules/cpp-tier1.json').resolve())]},
            'severity_floor': 'critical',
            'fail_on': 'findings',
            'header_lang': 'cpp',
        }), encoding='utf-8')
        ci_lines = self.run_host('ci', proj).stdout.splitlines()
        ci_paths = {json.loads(l)['path'] for l in ci_lines[:-1]}
        self.assertTrue(any(p.endswith('hdr.h') for p in ci_paths), ci_lines)

    def test_tier1_positive_clean_and_rule_omission_controls(self):
        pack_path = ROOT / 'rules/cpp-tier1.json'
        viol = ROOT / 'tests/fixtures/cpp-tier1-violations.cpp'
        clean = ROOT / 'tests/fixtures/cpp-tier1-clean.cpp'

        # lmok test harness over annotated violations (including #define preproc_arg) and clean fixture
        test_out = json.loads(self.run_host('test', pack_path, viol, clean).stdout)
        self.assertTrue(test_out['ok'], test_out)

        output = self.run_host('lint', pack_path, viol).stdout.splitlines()
        rows = [json.loads(line) for line in output[:-1]]
        counts = collections.Counter(r['rule'] for r in rows)
        expected = collections.Counter({**{f'CPP{i:03d}': 1 for i in range(1, 10)}, 'CPP005': 2})
        self.assertEqual(counts, expected)

        clean_out = self.run_host('lint', pack_path, clean).stdout
        self.assertEqual(clean_out, b'0 findings in ' + clean.as_posix().encode() + b'\n')

        pack = json.loads(pack_path.read_text(encoding='utf-8'))
        for rule in pack['rules']:
            mutant = self.work / (rule['id'] + '.json')
            mutant.write_text(json.dumps({**pack, 'rules': [r for r in pack['rules'] if r['id'] != rule['id']]}), encoding='utf-8')
            lines = self.run_host('lint', mutant, viol).stdout.splitlines()
            actual = collections.Counter(json.loads(line)['rule'] for line in lines[:-1])
            with self.assertRaises(AssertionError):
                self.assertEqual(actual, expected)
            self.assertEqual(actual[rule['id']], 0)
            print('CAUGHT C++ rule omission ' + rule['id'])

    def test_typed_cpp_host_methods_and_raw_string_reparse(self):
        source = self.work / 'api.lms'
        binary = self.work / 'api.dslb'
        old_src = 'const char *s = R"d(hello)d"; int x = 1;'
        new_src = 'const char *s = R"d(world!)d"; int x = 1;'
        body = (
            f'let old_src = {json.dumps(old_src)};\n'
            f'let new_src = {json.dumps(new_src)};\n'
            'let t = ts.parse_cpp(old_src);\n'
            'println(t.root().kind());\n'
            'for (n in ts.query_cpp(t.root(), "(raw_string_content) @c")) { println(n.text()); }\n'
            't.edit(20, 25, 26);\n'
            'let next = ts.reparse_cpp(t, new_src);\n'
            'let fresh = ts.parse_cpp(new_src);\n'
            'println(next.root().kind());\n'
            'for (n in ts.query_cpp(next.root(), "(raw_string_content) @c")) { println(n.text()); }\n'
            'if (next.sexp() == fresh.sexp()) { println("MATCH"); }\n'
        )
        source.write_text('p = _lmcode_ schema=' + json.dumps(str(ROOT / 'schema/lmok.dsl.json')) + ';\n' + body + 'end _lmcode_\n', encoding='utf-8')
        r = subprocess.run(['node', str(ROOT / 'scripts/compile-fixture.mjs'), str(source), str(binary)], capture_output=True, cwd=ROOT, env=self._compiler_env(), timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        out = self.run_host('--lmcode', binary).stdout
        self.assertEqual(
            out,
            b'translation_unit\nhello\ntranslation_unit\nworld!\nMATCH\n{"host_summary":{"part":0}}\nLMCODE_RETURN_BEGIN\n{"ok":true,"result":null}\nLMCODE_RETURN_END\n',
        )

    def test_check_cpp_annotation(self):
        source = self.work / 'rule.lms'
        binary = self.work / 'rule.dslb'
        source.write_text(
            'p = _lmcode_ schema=' + json.dumps(str(ROOT / 'schema/lmok.dsl.json')) + ';\n'
            '@function_definition fn fns(n: Node) -> void implements Rule { ts.report(n, "fn", "CPP_ANNOT", "minor"); }\n'
            'end _lmcode_\n',
            encoding='utf-8',
        )
        result = subprocess.run(['node', str(ROOT / 'scripts/compile-fixture.mjs'), str(source), str(binary)], capture_output=True, cwd=ROOT, env=self._compiler_env(), timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        code = self.work / 'Input.cpp'
        code.write_text('namespace n { void run() {} }\n', encoding='utf-8')
        output = self.run_host('check', '--lang', 'cpp', binary, code).stdout.splitlines()
        self.assertEqual(json.loads(output[0])[0]['rule'], 'CPP_ANNOT')
        self.assertTrue(output[1].startswith(b'1 findings in '))


if __name__ == '__main__':
    if PROVE:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(CppOS)
        CORRUPT = True
        result = unittest.TestResult()
        suite.run(result)
        if len(result.failures) + len(result.errors) != result.testsRun:
            raise SystemExit('output-corruption control survived')
        print('CAUGHT C++ output corruption in ' + str(result.testsRun) + ' native oracles')
        CORRUPT = False
    unittest.main()
