#!/usr/bin/env python3
"""Native v6 rule dispatch: deterministic fixtures, no network or installed host."""
import json
import os
import shutil
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HOST = str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv) > 1 else str(ROOT / 'build/lmok')
# Printed file paths use forward slashes on every OS (canonical_path in cpp/main.cpp, same as lint), so expect .as_posix().
COMPILER = os.environ.get('LMSCRIPT_BIN', 'lmscript')

def compiler_argv():
    # On Windows npm installs `lmscript` as an extension-less sh script plus a
    # `lmscript.cmd` shim; CreateProcess can run only the shim (WinError 193 otherwise).
    if os.name == 'nt' and not COMPILER.lower().endswith(('.cmd', '.exe', '.bat')):
        for candidate in (COMPILER + '.cmd', shutil.which('lmscript.cmd')):
            if candidate and Path(candidate).exists():
                return [candidate]
    return [COMPILER]

class EntryChecks(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix='entry-test-', dir=ROOT / 'build')
        self.work = Path(self.scratch.name)
        self.source = self.work / 'input.json'
        self.source.write_text('{"a":1,"b":2}')
    def tearDown(self):
        self.scratch.cleanup()
    def compile(self, body):
        src = self.work / 'rules.lms'
        binary = self.work / 'rules.dslb'
        src.write_text('p = _lmcode_ schema=' + json.dumps(str(ROOT / 'schema/lmok.dsl.json')) + ';\n' + body + '\nend _lmcode_\n')
        r = subprocess.run(["node",str(ROOT/"scripts/compile-fixture.mjs"), str(src), str(binary)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return binary
    def run_host(self, binary, *files, lang='json'):
        return subprocess.run([HOST, 'check', '--lang', lang, str(binary), *(str(f) for f in (files or (self.source,)))], capture_output=True, text=True)
    def arrays(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stderr, '')
        return [json.loads(line) for line in result.stdout.splitlines() if line.startswith('[')]
    def test_duplicate_annotations_in_source_order_and_body_once(self):
        binary = self.compile('''println("configured");
@pair fn first(n: Node) -> void implements Rule { ts.report(n, "first", "ID_A", "minor"); }
@pair fn second(n: Node) -> void implements Rule { ts.report(n, "second", "ID_B", "major"); }
''')
        r = self.run_host(binary, self.source, self.source)
        arrays = self.arrays(r)
        self.assertEqual(len(arrays), 2)
        self.assertEqual(arrays[0], arrays[1], 'findings reset for each input')
        self.assertEqual([f['rule'] for f in arrays[0]], ['ID_A','ID_B','ID_A','ID_B'])
        self.assertEqual(r.stdout.splitlines().count('configured'), 1)
    def test_real_kind_absent_from_input_finds_nothing(self):
        # `null` is a JSON node kind that this input does not contain (an unknown kind is refused at load, see below).
        binary = self.compile('@null fn ignored(n: Node) -> void implements Rule { ts.report(n, "wrong"); }')
        self.assertEqual(self.arrays(self.run_host(binary)), [[]])
    def test_anonymous_keyword_kind(self):
        self.source.write_text('int f(void) { if (1) return 0; return 1; }')
        binary = self.compile('@if fn keyword(n: Node) -> void implements Rule { if (!n.is_named()) { ts.report(n, n.text(), "KW", "minor"); } }')
        findings = self.arrays(self.run_host(binary, lang='c'))[0]
        self.assertEqual([(f['rule'],f['message']) for f in findings], [('KW','if')])
    def test_entry_failure_is_located_run_error(self):
        binary = self.compile('@pair fn bad(n: Node) -> void implements Rule { n.child(999); }')
        r = self.run_host(binary)
        self.assertEqual(r.returncode,3,r.stdout+r.stderr)
        error = json.loads(r.stdout.splitlines()[-2])
        self.assertEqual(error['phase'],'run')
        self.assertIn('out of range',error['message'])
        self.assertGreater(error['line'],0)
        self.assertGreater(error['column'],0)
    def test_v5_refused_before_body(self):
        binary = self.compile('println("must not run");')
        b=bytearray(binary.read_bytes()); b[4:6]=b'\0\5'; binary.write_bytes(b)
        r=self.run_host(binary)
        self.assertEqual(r.returncode,3,r.stdout+r.stderr)
        error=json.loads(r.stdout.splitlines()[-2])
        self.assertEqual(error['code'],'LOAD_BAD_BYTECODE')
        self.assertIn('unsupported bytecode version 5',error['message'])
    def test_duplicate_entry_names_refused(self):
        binary=self.compile('@pair fn first(n: Node) -> void implements Rule {}\n@pair fn second(n: Node) -> void implements Rule {}')
        b=bytearray(binary.read_bytes()); p=38
        def u16():
            nonlocal p
            n=struct.unpack_from('>H',b,p)[0];p+=2;return n
        def u32():
            nonlocal p
            n=struct.unpack_from('>I',b,p)[0];p+=4;return n
        for _ in range(u32()):
            p+=1;n=u32();p+=n
        n=u16();p+=4*n;n=u16();p+=8*n
        def region():
            nonlocal p
            n=u16();p+=5*n+2;n=u32();p+=n;n=u32();p+=10*n
        region();self.assertEqual(u16(),2);first=p
        p+=14;region();b[p:p+4]=b[first:first+4];binary.write_bytes(b)
        r=self.run_host(binary)
        self.assertEqual(r.returncode,3,r.stdout+r.stderr)
        self.assertIn("duplicate entry name 'first'",json.loads(r.stdout.splitlines()[-2])['message'])
    def test_parent_chains_do_not_exhaust_node_handles(self):
        # WP14 F4: a rule walking n.parent() for every array used to allocate depth^2 handles; depth 5000 hit the 1<<20 cap.
        self.source.write_text('[' * 5000 + ']' * 5000)
        binary = self.compile('@array fn depth(n: Node) -> void implements Rule { let d = 0; let c = n; while (c.has_parent()) { c = c.parent(); d = d + 1; } if (d == 4999) { ts.report(n, "deepest", "DEEP", "minor"); } }')
        findings = self.arrays(self.run_host(binary))[0]
        self.assertEqual([f['rule'] for f in findings], ['DEEP'])
    def test_missing_nodes_are_detectable(self):
        # WP14 F8: a MISSING placeholder dispatches as its expected kind with zero width; Node.is_missing tells it apart.
        self.source.write_text('{"a": 1, "b": }')
        binary = self.compile('@number fn num(n: Node) -> void implements Rule { ts.report(n, "missing=" + str(n.is_missing()), "NUM", "minor"); }')
        findings = self.arrays(self.run_host(binary))[0]
        self.assertEqual([f['message'] for f in findings], ['missing=false', 'missing=true'])
    def test_unknown_annotation_kinds_are_refused_at_load(self):
        # WP14 F3: a misspelled or abstract kind used to be silent (the rule simply never fired).
        binary = self.compile('@typo_kind_that_does_not_exist fn t(n: Node) -> void implements Rule { ts.report(n, "typo"); }')
        r = self.run_host(binary)
        self.assertEqual(r.returncode, 3, r.stdout + r.stderr)
        error = json.loads(r.stdout.splitlines()[-2])
        self.assertEqual((error['phase'], error['code']), ('load', 'LOAD_UNBOUND'))
        self.assertIn("annotation '@typo_kind_that_does_not_exist' is not a node kind of the json grammar", error['message'])
        self.source.write_text('int f(void) { return 1; }')
        binary = self.compile('@expression fn abstract(n: Node) -> void implements Rule { ts.report(n, "never"); }')
        r = self.run_host(binary, lang='c')
        self.assertEqual(r.returncode, 3, r.stdout + r.stderr)
        self.assertIn("'@expression' is a supertype of the c grammar", json.loads(r.stdout.splitlines()[-2])['message'])
        # Concrete kinds keep working: the ERROR kind, an anonymous token and a named node.
        self.source.write_text('{"a" 1}')
        binary = self.compile('@ERROR fn e(n: Node) -> void implements Rule { ts.report(n, "error", "E", "major"); }\n@document fn d(n: Node) -> void implements Rule { ts.report(n, "doc", "D", "info"); }')
        findings = self.arrays(self.run_host(binary))[0]
        self.assertEqual(sorted(f['rule'] for f in findings), ['D', 'E'])
    def test_each_group_names_its_file(self):
        # WP14 F6: consumers must not have to correlate groups with the argument list by position.
        other = self.work / 'other.json'; other.write_text('{"c":3}')
        binary = self.compile('@pair fn p(n: Node) -> void implements Rule { ts.report(n, "pair", "P", "minor"); }')
        r = self.run_host(binary, self.source, other)
        self.arrays(r)
        counts = [line for line in r.stdout.splitlines() if ' findings in ' in line]
        self.assertEqual(counts, ['2 findings in ' + self.source.as_posix(), '1 findings in ' + other.as_posix()])
    def test_body_findings_are_their_own_group(self):
        # WP14 F2: findings reported by the body were silently discarded by the per-file reset.
        binary = self.compile("""ts.report(ts.parse("{}").root(), "from the body", "BODY", "major");
@pair fn p(n: Node) -> void implements Rule { ts.report(n, "pair", "P", "minor"); }""")
        r = self.run_host(binary)
        arrays = self.arrays(r)
        self.assertEqual([[f['rule'] for f in a] for a in arrays], [['BODY'], ['P', 'P']])
        self.assertEqual([line for line in r.stdout.splitlines() if ' findings in ' in line], ['1 findings in body', '2 findings in ' + self.source.as_posix()])
    def test_entry_failure_prints_the_partial_group_first(self):
        # WP14 F5: the findings collected before a failing entry are printed, then the located error, exit 3.
        binary = self.compile("""@pair fn first(n: Node) -> void implements Rule { ts.report(n, "before", "F1", "minor"); }
@number fn boom(n: Node) -> void implements Rule { if (n.text() == "2") { n.child(999); } }""")
        r = self.run_host(binary, self.source, self.source)
        self.assertEqual(r.returncode, 3, r.stdout + r.stderr)
        lines = r.stdout.splitlines()
        self.assertEqual([f['rule'] for f in json.loads(lines[0])], ['F1', 'F1'])
        self.assertEqual(lines[1], '2 findings in ' + self.source.as_posix())
        error = json.loads(lines[3]); self.assertEqual(error['phase'], 'run'); self.assertIn('out of range', error['message'])
        self.assertEqual(len(lines), 5, 'the second input is not processed after an error')
    def test_missing_input_and_bad_usage(self):
        binary=self.compile('@pair fn first(n: Node) -> void implements Rule {}')
        r=self.run_host(binary,self.work/'missing.json')
        self.assertEqual(r.returncode,3,r.stdout+r.stderr)
        self.assertIn('cannot read',r.stdout+r.stderr)
        failure=json.loads(r.stdout.splitlines()[-2]);self.assertFalse(failure['ok']);self.assertEqual(failure['phase'],'run');self.assertNotIn('result',failure)
        r=subprocess.run([HOST,'check'],capture_output=True)
        self.assertEqual(r.returncode,2)
        value=json.loads(r.stdout.splitlines()[-2]);self.assertEqual(value['phase'],'cli');self.assertNotIn('result',value)

if __name__ == '__main__':
    unittest.main()
