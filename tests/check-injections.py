#!/usr/bin/env python3
"""Native byte-oracle tests. Coordinates are zero-based UTF-8 bytes, not characters."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HOST = Path(sys.argv.pop(1)).resolve() if len(sys.argv)>1 and not sys.argv[1].startswith('--') else ROOT/'build'/('lmok.exe' if os.name=='nt' else 'lmok')
PROVE = '--prove-controls' in sys.argv
if PROVE: sys.argv.remove('--prove-controls')
CORRUPT = False
# Hand-counted: first header ends at byte 35; fn inner spans [35,73).
# Second header ends at 107; "雪"; + two spaces = 8 bytes; fn second = 32 bytes.
SOURCE = ('# 雪\r\nif (true) {\r\np = _lmcode_;\r\n'
          'fn inner() -> int { return helper(); }\r\nend _lmcode_\r\n}\r\n'
          'p = _lmcode_;\r\n"雪";  fn second() -> int { return 2; }\r\nend _lmcode_\r\n').encode()

class Injections(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='injection 雪 ',dir=ROOT/'build')
        self.path=Path(self.tmp.name)/'entrée 雪.lms'
        self.path.write_bytes(SOURCE)
    def tearDown(self): self.tmp.cleanup()
    def run_host(self,*args):
        r=subprocess.run([str(HOST),*map(str,args)],capture_output=True)
        if CORRUPT: r.stdout += b'\r'
        self.assertEqual(r.returncode,0,r.stderr)
        self.assertEqual(r.stderr,b'')
        self.assertTrue(not r.stdout or r.stdout.endswith(b'\n'),r.stdout)
        return r.stdout
    def facts(self): return [json.loads(x) for x in self.run_host('facts',self.path).splitlines() if x]
    def test_absolute_facts_and_multiple_nested_blocks(self):
        facts=[x for x in self.facts() if x['kind']=='def.function']
        self.assertEqual([(x['name'],x['lang'],x['row'],x['col'],x['end_row'],x['end_col'],x['start_byte'],x['end_byte'],x['name_row'],x['name_col']) for x in facts],
                         [('inner','lmcode',3,0,3,38,35,73,3,3),('second','lmcode',7,8,7,40,115,147,7,11)])
        calls=[x for x in self.facts() if x['kind']=='ref.call']
        self.assertEqual([(x['name'],x['scope'],x['row'],x['col'],x['start_byte'],x['end_byte']) for x in calls],[('helper','inner',3,27,62,70)])
    def test_query_selects_inner_grammar(self):
        self.assertEqual(self.run_host('query','--lang','lmcode','(function_declaration (identifier) @name)',self.path),
                         b'identifier 3:3-3:8\nidentifier 7:11-7:17\n')
    def test_line_shift_moves_findings_by_one(self):
        def locations():
            return [(r['name'],r['row'],r['col'],r['start_byte']) for r in self.facts() if r['kind']=='def.function']
        before=locations(); self.assertEqual(before,[('inner',3,0,35),('second',7,8,115)])
        self.path.write_bytes(b'\r\n'+SOURCE)
        self.assertEqual(locations(),[(name,row+1,col,byte+2) for name,row,col,byte in before])
    def test_lint_query_and_kind_use_absolute_bytes(self):
        pack=Path(self.tmp.name)/'rules.json'
        pack.write_text(json.dumps({'lang':'lmcode','rules':[
            {'id':'INNER','on':'identifier','text_matches':'^inner$','message':'{text}','fix':'renamed'},
            {'id':'CALL','query':'(call_expression (identifier) @name)','message':'{text}'}]}))
        out=self.run_host('lint',pack,self.path)
        rows=[json.loads(x) for x in out.splitlines() if x.startswith(b'{')]
        self.assertEqual([(r['rule'],r['row'],r['col'],r['end_col'],r['message']) for r in rows],[('INNER',3,3,8,'inner'),('CALL',3,27,33,'helper')])
        self.assertEqual(rows[0]['fix'],{'start_byte':38,'end_byte':43,'text':'renamed'})
        self.assertTrue(out.endswith(b'2 findings in '+self.path.as_posix().encode()+b'\n'))
    def test_splices_and_empty_or_non_code_bodies(self):
        # A quoted marker in a splice is data; no evaluation or substitution.
        text=('p = _lmcode_;\nlet v = ${pick("雪", {a: 1})}; fn spliced() -> int { return 1; }\nend _lmcode_\n'
              'p = _lmcode_;\nend _lmcode_\np = _json_;\n{"text":"fn fake() -> int {}"}\nend _json_\n')
        self.path.write_bytes(text.encode())
        self.assertEqual(self.run_host('query','--lang','lmcode','(function_declaration (identifier) @name)',self.path),b'identifier 1:35-1:42\n')
        self.assertEqual(self.run_host('query','--lang','lmcode','(splice_call (identifier) @name)',self.path),b'identifier 1:10-1:14\n')
        defs=[r for r in self.facts() if r['kind']=='def.function']
        self.assertEqual([(r['name'],r['row'],r['col'],r['start_byte'],r['end_byte']) for r in defs],[('spliced',1,32,46,79)])
    def test_compiled_check_and_directory_selection(self):
        rule=Path(self.tmp.name)/'rule.lms';binary=Path(self.tmp.name)/'rule.dslb'
        rule.write_text('p = _lmcode_ schema='+json.dumps(str(ROOT/'schema/lmok.dsl.json'))+';\n'
                        '@function_declaration fn check(n: Node) -> void implements Rule { ts.report(n,"function","INJECT","minor"); }\n'
                        'end _lmcode_\n',encoding='utf-8')
        compiled=subprocess.run(['node',str(ROOT/'scripts/compile-fixture.mjs'),str(rule),str(binary)],cwd=ROOT,capture_output=True)
        self.assertEqual(compiled.returncode,0,compiled.stderr)
        out=self.run_host('check','--lang','lmcode',binary,self.path)
        rows=json.loads(out.splitlines()[0])
        self.assertEqual([(r['row'],r['col'],r['end_row'],r['end_col'],r['rule']) for r in rows],[(3,0,3,38,'INJECT'),(7,8,7,40,'INJECT')])
        self.assertTrue(out.endswith(b'LMCODE_RETURN_END\n'))
        self.path.write_bytes(SOURCE+b'# lmok-disable: UNKNOWN\n')
        warned=subprocess.run([str(HOST),'check','--lang','lmcode',str(binary),str(self.path)],capture_output=True)
        self.assertEqual(warned.returncode,0,warned.stderr)
        self.assertEqual(warned.stderr.count(b"unknown rule id 'UNKNOWN'"),1,warned.stderr)
        self.path.write_bytes(SOURCE)
        # Only the target file is in this subdirectory; rule source must not be linted.
        folder=Path(self.tmp.name)/'inputs';folder.mkdir();self.path.rename(folder/self.path.name)
        out=self.run_host('check','--lang','lmcode',binary,folder)
        self.assertEqual([(r['row'],r['col']) for r in json.loads(out.splitlines()[0])],[(3,0),(7,8)])

    def test_project_ci_and_pack_author_dispatch(self):
        pack=Path(self.tmp.name)/'rules.json'
        pack.write_text(json.dumps({'lang':'lmcode','rules':[{'id':'INNER','on':'function_declaration','message':'inner'}]}))
        config=Path(self.tmp.name)/'config.json'
        config.write_text(json.dumps({'packs':{'lmcode':['rules.json']}}))
        out=self.run_host('ci','--config',config,Path(self.tmp.name))
        rows=[json.loads(x) for x in out.splitlines() if x.startswith(b'{')]
        self.assertEqual([(r['row'],r['col'],r['rule']) for r in rows if 'row' in r],[(3,0,'INNER'),(7,8,'INNER')])
        self.path.write_bytes(b'p = _lmcode_;\nfn only() -> int { return 1; } # lmok-expect: INNER\nend _lmcode_\n')
        result=json.loads(self.run_host('test',pack,self.path))
        self.assertEqual(result,{'ok':True,'expected':1,'actual':1,'diff':[]})

    def test_standalone_and_outer_queries_are_independent(self):
        self.path.write_bytes(b'fn outer() {\np = _lmcode_;\nfn inner() -> int { return 1; }\nend _lmcode_\n}\n')
        self.assertEqual(self.run_host('query','--lang','lmscript','(function_declaration (identifier) @name)',self.path),b'identifier 0:3-0:8\n')
        bare=Path(self.tmp.name)/'bare.lmcode';bare.write_bytes(b'fn bare() -> int { return 1; }\n')
        self.assertEqual(self.run_host('query','--lang','lmcode','(function_declaration (identifier) @name)',bare),b'identifier 0:3-0:7\n')

if __name__=='__main__':
    if PROVE:
        CORRUPT=True
        for name in unittest.defaultTestLoader.getTestCaseNames(Injections):
            result=unittest.TestResult();Injections(name).run(result)
            if result.errors or not result.failures: raise SystemExit('control survived: '+name+str(result.errors))
        print('CAUGHT output corruption in injection oracles');CORRUPT=False
    unittest.main()
