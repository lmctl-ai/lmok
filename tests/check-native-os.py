#!/usr/bin/env python3
"""Offline byte-level host checks, executed on each native Actions runner."""
import json
import os
from pathlib import Path
import platform
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HOST = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else ROOT / 'build' / ('lmok.exe' if os.name == 'nt' else 'lmok')
PROVE = '--prove-controls' in sys.argv
if PROVE: sys.argv.remove('--prove-controls')
MUTATE = False


def architecture(data):
    if data[:4] == b'\x7fELF':
        return {62: 'x64', 183: 'arm64'}.get(struct.unpack_from('<H' if data[5] == 1 else '>H', data, 18)[0])
    if data[:2] == b'MZ':
        pe = struct.unpack_from('<I', data, 60)[0]
        assert data[pe:pe+4] == b'PE\0\0', 'missing PE signature'
        return {0x8664: 'x64', 0xaa64: 'arm64'}.get(struct.unpack_from('<H', data, pe+4)[0])
    if data[:4] == b'\xcf\xfa\xed\xfe':
        return {0x1000007: 'x64', 0x100000c: 'arm64'}.get(struct.unpack_from('<I', data, 4)[0])
    raise AssertionError('unknown native executable format')


class NativeOS(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='native os ', dir=ROOT / 'build')
        self.work = Path(self.temp.name)
    def tearDown(self):
        self.temp.cleanup()

    def test_script_grammar_queries(self):
        for lang, suffix, text in (
                ('lmscript', '.lms', 'fn f() { return 1; }\n'),
                ('lmcode', '.lmcode', 'fn f() -> int { return 1; }\n')):
            source = self.work / ('entrée 雪' + suffix)
            source.write_bytes(text.encode('utf-8'))
            self.exact(self.run_host('query', '--lang', lang,
                '(function_declaration (identifier) @name)', source),
                b'identifier 0:3-0:4\n')
    def run_host(self, *args, data=None, cwd=None, binary=HOST):
        result = subprocess.run([str(binary), *map(str, args)], input=data, cwd=cwd, capture_output=True, timeout=15)
        if MUTATE: result.stdout += b'\r'  # Deliberate output corruption: every byte oracle must reject it.
        return result
    def exact(self, result, output, code=0, error=b''):
        self.assertEqual((result.returncode, result.stdout, result.stderr), (code, output, error))
    def test_go_cli_and_extension_dispatch(self):
        source = self.work / 'go input.go'
        # Pin both newline forms; native text writes would translate LF to CRLF.
        text = 'package main\nfunc café_水() {}\n'
        source.write_bytes(text.encode('utf-8'))
        parsed = self.run_host('parse', '--lang', 'go', source)
        self.assertEqual(parsed.returncode, 0, parsed.stderr)
        self.assertTrue(parsed.stdout.startswith(b'(source_file (package_clause'), parsed.stdout)
        self.exact(self.run_host('query', '--lang', 'go', '(function_declaration name: (identifier) @name)', source),
                   b'identifier 1:5-1:14\n')
        facts = self.run_host('facts', source)  # .go extension selects Go without --lang.
        self.assertEqual((facts.returncode, facts.stderr), (0, b''), facts)
        rows = [json.loads(line) for line in facts.stdout.splitlines()]
        self.assertIn({'file': source.as_posix(), 'lang': 'go', 'kind': 'def.function', 'name': 'café_水',
                       'scope': '', 'confidence': 'exact', 'row': 1, 'col': 0, 'end_row': 1,
                       'end_col': 19, 'start_byte': 13, 'end_byte': 32, 'name_row': 1, 'name_col': 5}, rows)
        source.write_bytes(text.replace('\n', '\r\n').encode('utf-8'))
        crlf = self.run_host('facts', source)
        self.assertEqual((crlf.returncode, crlf.stderr), (0, b''), crlf)
        function = next(r for r in map(json.loads, crlf.stdout.splitlines()) if r['kind'] == 'def.function')
        self.assertEqual((function['start_byte'], function['end_byte']), (14, 33))
        pack = self.work / 'go.json'
        pack.write_text(json.dumps({'lang': 'go', 'rules': [{'id': 'GO', 'on': 'function_declaration', 'message': 'go'}]}), encoding='utf-8')
        lint = self.run_host('lint', pack, source)  # .go extension selects the pack language.
        self.assertEqual((lint.returncode, lint.stderr), (0, b''), lint)
        found = json.loads(lint.stdout.splitlines()[0])
        self.assertEqual((found['rule'], found['message']), ('GO', 'go'))
    def test_javascript_cli_and_extension_dispatch(self):
        source = self.work / 'javascript input.js'
        source.write_bytes(b'function caf\xc3\xa9(value) { return Math.abs(value); }\n')
        parsed = self.run_host('parse', '--lang', 'javascript', source)
        self.assertEqual(parsed.returncode, 0, parsed.stderr)
        self.assertTrue(parsed.stdout.startswith(b'(program (function_declaration'), parsed.stdout)
        self.exact(self.run_host('query', '--lang', 'javascript', '(function_declaration name: (identifier) @name)', source),
                   b'identifier 0:9-0:14\n')
        facts = self.run_host('facts', source)
        self.assertEqual((facts.returncode, facts.stderr), (0, b''), facts)
        rows = [json.loads(line) for line in facts.stdout.splitlines()]
        self.assertIn('def.function', {row['kind'] for row in rows})
        self.assertIn('ref.call', {row['kind'] for row in rows})
        pack = self.work / 'javascript.json'
        pack.write_text(json.dumps({'lang': 'javascript', 'rules': [{'id': 'JS', 'on': 'call_expression', 'message': 'call'}]}), encoding='utf-8')
        lint = self.run_host('lint', pack, source)
        self.assertEqual((lint.returncode, lint.stderr), (0, b''), lint)
        self.assertEqual(json.loads(lint.stdout.splitlines()[0])['rule'], 'JS')
    def test_typescript_cli_and_extension_dispatch(self):
        source = self.work / 'typescript input.ts'
        source.write_bytes(b'function greet(name: string): string { return name; }\n')
        parsed = self.run_host('parse', '--lang', 'typescript', source)
        self.assertEqual(parsed.returncode, 0, parsed.stderr)
        self.assertTrue(parsed.stdout.startswith(b'(program (function_declaration'), parsed.stdout)
        self.exact(self.run_host('query', '--lang', 'typescript', '(function_declaration name: (identifier) @name)', source),
                   b'identifier 0:9-0:14\n')
        facts = self.run_host('facts', source)
        self.assertEqual((facts.returncode, facts.stderr), (0, b''), facts)
        self.assertIn(b'"kind":"def.function","name":"greet"', facts.stdout)
    def test_tsx_cli_and_extension_dispatch(self):
        source = self.work / 'tsx input.tsx'
        source.write_bytes(b'export function Greeting(name: string) { return <div>{name}</div>; }\n')
        parsed = self.run_host('parse', '--lang', 'tsx', source)
        self.assertEqual(parsed.returncode, 0, parsed.stderr)
        self.assertTrue(parsed.stdout.startswith(b'(program (export_statement declaration: (function_declaration'), parsed.stdout)
        self.exact(self.run_host('query', '--lang', 'tsx', '(function_declaration name: (identifier) @name)', source),
                   b'identifier 0:16-0:24\n')
        facts = self.run_host('facts', source)
        self.assertEqual((facts.returncode, facts.stderr), (0, b''), facts)
        self.assertIn(b'"kind":"def.function","name":"Greeting"', facts.stdout)
    def test_paths(self):
        folder = self.work / 'space 雪 café'
        folder.mkdir()
        source = folder / 'input 雪.json'; source.write_bytes(b'{"a":1}')
        expected = b'number 0:5-0:6\n'
        self.exact(self.run_host('query', '(number) @n', source), expected)
        self.exact(self.run_host('query', '(number) @n', source.name, cwd=folder), expected)
        if os.name == 'nt':
            self.assertTrue(source.drive, 'exercise a real Windows drive path')
            self.exact(self.run_host('query', '(number) @n', str(source).replace('/', '\\')), expected)
        else:
            backslash = folder / 'literal\\name.json'; backslash.write_bytes(source.read_bytes())
            self.exact(self.run_host('query', '(number) @n', backslash), expected)
        pack = self.work / 'pack 雪.json'
        pack.write_text(json.dumps({'lang':'json','rules':[{'id':'N','on':'number','message':'number'}]}), encoding='utf-8')
        for target in (source, folder):
            result = self.run_host('lint', pack, target)
            self.assertEqual((result.returncode, result.stderr), (0, b''))
            self.assertNotIn(b'\r', result.stdout)
            lines = result.stdout.decode('utf-8').splitlines()
            self.assertEqual(json.loads(lines[0])['rule'], 'N')
            self.assertEqual(lines[1], '1 findings in ' + source.as_posix())
        go = folder / 'input.go'
        go.write_text('package main\nfunc café_水() {}\n', encoding='utf-8')
        self.exact(self.run_host('parse', '--lang', 'go', go),
                   b'(source_file (package_clause (package_identifier)) (function_declaration name: (identifier) parameters: (parameter_list) body: (block)))\n')
        self.exact(self.run_host('query', '--lang', 'go', '(function_declaration name: (identifier) @name)', go),
                   b'identifier 1:5-1:14\n')
        facts = self.run_host('facts', go)
        self.assertEqual((facts.returncode, facts.stderr), (0, b''), facts)
        self.assertIn(b'"lang":"go"', facts.stdout)
        self.assertIn(b'"kind":"def.function","name":"caf\xc3\xa9_\xe6\xb0\xb4"', facts.stdout)
        bad = folder / 'bad 雪.dslb'; bad.write_bytes(b'not bytecode')
        result = self.run_host('--lmcode', bad)
        self.assertEqual(result.returncode, 3)
        self.assertEqual(result.stderr, b'')
        error = json.loads(result.stdout.splitlines()[-2])
        self.assertEqual(error['code'], 'LOAD_BAD_BYTECODE')
        self.assertIn('bad magic', error['message'])  # Proves the Unicode file was actually opened.
        link = folder / 'linked.json'
        try: link.symlink_to(source.name)
        except OSError as e:
            if os.name != 'nt' or getattr(e, 'winerror', None) not in (5, 1314): raise
            print('ok - symlink unavailable under Windows runner privilege; other path cases ran')
        else: self.exact(self.run_host('query', '(number) @n', link), expected)
    def test_stdin_bytes_and_line_endings(self):
        for payload, expected in [
            (b'[\n 1,\n 2\n]\n', b'number 1:1-1:2\nnumber 2:1-2:2\n'),
            (b'[\r\n 1,\r\n 2\r\n]\r\n', b'number 1:1-1:2\nnumber 2:1-2:2\n'),
            (b'\xef\xbb\xbf{"a":1}', b'number 0:8-0:9\n'),
            ('{"é":"雪"}'.encode(), b'string_content 0:2-0:4\nstring_content 0:7-0:10\n'),
        ]:
            source = self.work / 'input.json'; source.write_bytes(payload)
            query = '(string_content) @n' if b'\xc3' in payload else '(number) @n'
            self.exact(self.run_host('query', query, source), expected)
            self.exact(self.run_host('query', query, '-', data=payload), expected)
        # ^Z must not terminate Windows stdin, and CR bytes must not be translated.
        for byte in (b'\x1a', b'\0', b'\xff'):
            payload = b'int before;' + byte + b'int after;\r\n'
            source = self.work / 'binary.c'; source.write_bytes(payload)
            expected = b'identifier 0:4-0:10\nidentifier 0:16-0:21\n'
            self.exact(self.run_host('query', '--lang', 'c', '(identifier) @n', source), expected)
            self.exact(self.run_host('query', '--lang', 'c', '(identifier) @n', '-', data=payload), expected)
    def test_empty_and_large_inputs(self):
        for payload in (b'', b' \r\n\t'):
            self.exact(self.run_host('parse', '-', data=payload), b'(document)\n')
        # Cross many read-buffer boundaries without making the output huge.
        payload = b' ' * (2 * 1024 * 1024 + 17) + b'123'
        expected = b'number 0:2097169-0:2097172\n'
        source = self.work / 'large.json'; source.write_bytes(payload)
        self.exact(self.run_host('query', '(number) @n', source), expected)
        self.exact(self.run_host('query', '(number) @n', '-', data=payload), expected)
    def test_metadata_and_executable(self):
        version = re.search(r'^project\(lmok VERSION ([0-9.]+)', (ROOT / 'CMakeLists.txt').read_text(), re.M).group(1)
        expected = ('lmok ' + version + '\n').encode()
        self.exact(self.run_host('--version'), expected)
        copied = self.work / ('copied tool.exe' if os.name == 'nt' else 'copied tool')
        shutil.copy2(HOST, copied)
        self.exact(self.run_host('--version', binary=copied, cwd=self.work), expected)
        self.exact(self.run_host('--notices'), (ROOT / 'THIRD_PARTY_NOTICES.md').read_bytes())
        self.exact(self.run_host('--lmcode-schema', '--json'), (ROOT / 'schema/lmok.dsl.json').read_bytes())
        signatures = self.run_host('--lmcode-signatures')
        self.assertEqual((signatures.returncode, signatures.stderr), (0, b''))
        self.assertTrue(signatures.stdout.endswith(b'\n'))
        self.assertNotIn(b'\r', signatures.stdout)
        lines = signatures.stdout.splitlines()
        self.assertEqual(lines, sorted(set(lines)))
        self.assertIn(b'context ts: Ts', lines)
        self.exact(self.run_host('--lmcode-signatures', cwd=self.work), signatures.stdout)
    def test_errors_and_json_newlines(self):
        missing = self.work / 'missing.json'
        self.exact(self.run_host('parse', missing), b'', 3, ('cannot read ' + str(missing) + '\n').encode())
        self.exact(self.run_host('parse', self.work), b'', 3, ('cannot read ' + str(self.work) + ': is a directory\n').encode())
        for argv in ([], ['--version', 'extra'], ['parse'], ['query', '(number) @n']):
            result = self.run_host(*argv)
            self.assertEqual((result.returncode, result.stdout), (2, b''))
            self.assertTrue(result.stderr.startswith(b'usage: lmok '))
            self.assertNotIn(b'\r', result.stderr)
        bad = self.work / 'bad.dslb'; bad.write_bytes(b'not bytecode')
        result = self.run_host('--lmcode', bad)
        self.assertEqual((result.returncode, result.stderr), (3, b''))
        self.assertTrue(result.stdout.endswith(b'\n'))
        self.assertNotIn(b'\r', result.stdout)
        report = json.loads(result.stdout.splitlines()[-2])
        self.assertEqual((report['ok'], report['phase'], report['code']), (False, 'load', 'LOAD_BAD_BYTECODE'))
    def test_bounded_regex_and_query(self):
        source = self.work / 'input.json'; source.write_bytes(json.dumps('a'*30+'!').encode())
        self.exact(self.run_host('query', '((string_content) @s (#match? @s "(a+)+$"))', source), b'', 3, b'regex work limit exceeded (65536)\n')
        self.exact(self.run_host('query', '(array '*300+'(number) @n'+')'*300, source), b'', 3, b'query error at byte offset 1792: nesting limit exceeded (256)\n')
        source.write_bytes(json.dumps('a'*16385).encode())
        result = self.run_host('query', '((string_content) @s (#match? @s "a+"))', source)
        self.assertEqual((result.returncode, result.stdout), (0, b''))
        self.assertIn(b'regex not applied to a 16385-byte node text', result.stderr)
    def test_architecture_and_linkage(self):
        data = HOST.read_bytes()
        target = os.environ.get('LMOK_TEST_PLATFORM', '').rsplit('-', 1)[-1]
        if not target: target = {'amd64': 'x64', 'x86_64': 'x64', 'aarch64': 'arm64', 'arm64': 'arm64'}.get(platform.machine().lower())
        self.assertIn(target, ('x64', 'arm64'))
        if MUTATE:
            data = bytearray(data)
            other = 'arm64' if target == 'x64' else 'x64'
            if data[:4] == b'\x7fELF': struct.pack_into('<H' if data[5] == 1 else '>H', data, 18, 183 if other == 'arm64' else 62)
            elif data[:2] == b'MZ': struct.pack_into('<H', data, struct.unpack_from('<I', data, 60)[0]+4, 0xaa64 if other == 'arm64' else 0x8664)
            else: struct.pack_into('<I', data, 4, 0x100000c if other == 'arm64' else 0x1000007)
        self.assertEqual(architecture(data), target)
        if sys.platform.startswith('linux'):
            tool = subprocess.run(['ldd', str(HOST)], capture_output=True, env=dict(os.environ, LC_ALL='C'))
            text = tool.stdout + tool.stderr
            self.assertTrue(b'not a dynamic executable' in text or b'statically linked' in text, text)
        elif sys.platform == 'darwin':
            tool = subprocess.run(['otool', '-L', str(HOST)], capture_output=True, check=True)
            libraries = [line.strip().split(b' (', 1)[0] for line in tool.stdout.splitlines()[1:]]
            self.assertTrue(libraries)
            self.assertTrue(all(x.startswith((b'/usr/lib/', b'/System/Library/')) for x in libraries), libraries)
        elif os.name == 'nt':
            tool = subprocess.run(['objdump', '-p', str(HOST)], capture_output=True, check=True)
            libraries = re.findall(rb'DLL Name:\s*(\S+)', tool.stdout)
            self.assertTrue(libraries)
            allowed = {b'kernel32.dll', b'msvcrt.dll', b'ucrtbase.dll', b'advapi32.dll', b'shell32.dll', b'user32.dll', b'ws2_32.dll', b'bcrypt.dll', b'ntdll.dll'}
            self.assertTrue(all(x.lower() in allowed or x.lower().startswith((b'api-ms-win-', b'ext-ms-win-')) for x in libraries), libraries)
        else: self.fail('unsupported native OS')

if __name__ == '__main__':
    names = unittest.defaultTestLoader.getTestCaseNames(NativeOS)
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(NativeOS))
    if not result.wasSuccessful(): sys.exit(1)
    if PROVE:
        MUTATE = True
        for name in names:
            control = unittest.TestResult()
            NativeOS(name).run(control)
            if not control.failures or control.errors:
                raise AssertionError('mutation did not produce an assertion failure: ' + name + str(control.errors))
            print('ok - caught native-output/architecture mutant: ' + name)
        if sys.platform.startswith('linux'):
            # A real dynamically linked ELF must fail the static-binary promise.
            MUTATE = False
            with tempfile.TemporaryDirectory(prefix='dynamic-link-control-', dir=ROOT / 'build') as directory:
                source = Path(directory) / 'control.c'
                dynamic = Path(directory) / 'control'
                source.write_text('int main(void) { return 0; }\n')
                subprocess.run(['cc', str(source), '-o', str(dynamic)], check=True, capture_output=True)
                HOST = dynamic
                control = unittest.TestResult()
                NativeOS('test_architecture_and_linkage').run(control)
                if not control.failures or control.errors:
                    raise AssertionError('dynamically linked negative control did not fail the linkage assertion')
                print('ok - caught real dynamically linked ELF negative control')

    print('ok - native OS byte/path/linkage contracts (' + str(len(names)) + ' groups)')
