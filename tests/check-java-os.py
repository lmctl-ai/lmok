#!/usr/bin/env python3
"""Java grammar, rules and facts oracles, executed on every native CI platform."""
import collections
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
HOST=Path(sys.argv.pop(1)).resolve() if len(sys.argv)>1 and not sys.argv[1].startswith('--') else ROOT/'build'/('lmok.exe' if os.name=='nt' else 'lmok')
PROVE='--prove-controls' in sys.argv
if PROVE:sys.argv.remove('--prove-controls')
CORRUPT=False
class JavaOS(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory(prefix='java space ',dir=ROOT/'build');self.work=Path(self.tmp.name)
 def tearDown(self):self.tmp.cleanup()
 def run_host(self,*args):
  r=subprocess.run([str(HOST),*map(str,args)],capture_output=True,cwd=ROOT,timeout=30)
  if CORRUPT:r.stdout+=b'\r'
  self.assertEqual(r.returncode,0,r.stderr.decode('utf-8','replace'));self.assertEqual(r.stderr,b'');self.assertNotIn(b'\r',r.stdout)
  return r.stdout
 def test_parse_query_utf8_crlf(self):
  for newline in [b'\n',b'\r\n']:
   source=self.work/'Café 雪.java';source.write_bytes(b'class Demo {'+newline+b'  int value;'+newline+b'}'+newline)
   tree=self.run_host('parse','--lang','java',source);self.assertTrue(tree.startswith(b'(program '),tree);self.assertNotIn(b'ERROR',tree)
   self.assertEqual(self.run_host('query','--lang','java','(identifier) @name',source),b'identifier 0:6-0:10\nidentifier 1:6-1:11\n')
 def test_facts_and_extension_walk(self):
  source=self.work/'Symbols.java';source.write_text('import java.util.List;\ninterface Face { void work(); }\nclass Demo { int field; void run() { work(); } }\n',encoding='utf-8')
  rows=[json.loads(line) for line in self.run_host('facts',self.work).splitlines()]
  actual={(r['kind'],r['name'],r['scope']) for r in rows}
  expected={('def.interface','Face',''),('def.method','work','Face'),('def.class','Demo',''),('def.field','field','Demo'),('def.method','run','Demo'),('ref.call','work','Demo.run'),('ref.import','java.util.List','')}
  self.assertTrue(expected<=actual,actual)
  self.assertTrue(all(r['lang']=='java' for r in rows));self.assertTrue(all(r['confidence']=='candidate' for r in rows if r['kind']=='ref.call'))
 def test_tier1_positive_and_clean(self):
  pack=ROOT/'rules/java-tier1.json'
  output=self.run_host('lint',pack,ROOT/'tests/fixtures/java-tier1-violations.java').splitlines()
  rows=[json.loads(line) for line in output[:-1]]
  counts=collections.Counter(r['rule'] for r in rows)
  self.assertEqual(counts,{**{'LJV%03d'%i:1 for i in range(1,10)},'LJV008':2})
  self.assertEqual(self.run_host('lint',pack,ROOT/'tests/fixtures/java-tier1-clean.java'),b'0 findings in '+(ROOT/'tests/fixtures/java-tier1-clean.java').as_posix().encode()+b'\n')
 def test_typed_java_host_methods(self):
  compiler=os.environ.get('LMSCRIPT_BIN','lmscript')
  if os.name=='nt' and not compiler.lower().endswith(('.cmd','.exe','.bat')):compiler=shutil.which(compiler+'.cmd') or compiler+'.cmd'
  source=self.work/'api.lms';binary=self.work/'api.dslb'
  body='let t = ts.parse_java("class A {}"); println(t.root().kind()); for (n in ts.query_java(t.root(), "(identifier) @id")) { println(n.text()); } let next = ts.reparse_java(t, "class A {}"); println(next.root().kind());'
  source.write_text('p = _lmcode_ schema='+json.dumps(str(ROOT/'schema/lmok.dsl.json'))+';\n'+body+'\nend _lmcode_\n',encoding='utf-8')
  r=subprocess.run(["node",str(ROOT/"scripts/compile-fixture.mjs"),str(source),str(binary)],capture_output=True,cwd=ROOT,timeout=30);self.assertEqual(r.returncode,0,r.stderr)
  self.assertEqual(self.run_host('--lmcode',binary),b'program\nA\nprogram\n{"host_summary":{"part":0}}\nLMCODE_RETURN_BEGIN\n{"ok":true,"result":null}\nLMCODE_RETURN_END\n')
 def test_rule_omission_controls(self):
  pack=json.loads((ROOT/'rules/java-tier1.json').read_text(encoding='utf-8'))
  expected=collections.Counter({**{'LJV%03d'%i:1 for i in range(1,10)},'LJV008':2})
  for rule in pack['rules']:
   mutant=self.work/(rule['id']+'.json');mutant.write_text(json.dumps({**pack,'rules':[r for r in pack['rules'] if r['id']!=rule['id']]}),encoding='utf-8')
   lines=self.run_host('lint',mutant,ROOT/'tests/fixtures/java-tier1-violations.java').splitlines()
   actual=collections.Counter(json.loads(line)['rule'] for line in lines[:-1])
   with self.assertRaises(AssertionError):self.assertEqual(actual,expected)
   self.assertEqual(actual[rule['id']],0)
   print('CAUGHT Java rule omission '+rule['id'])
 def test_check_java_annotation(self):
  compiler=os.environ.get('LMSCRIPT_BIN','lmscript')
  if os.name=='nt' and not compiler.lower().endswith(('.cmd','.exe','.bat')):compiler=shutil.which(compiler+'.cmd') or compiler+'.cmd'
  source=self.work/'rule.lms';binary=self.work/'rule.dslb'
  source.write_text('p = _lmcode_ schema='+json.dumps(str(ROOT/'schema/lmok.dsl.json'))+';\n@method_declaration fn methods(n: Node) -> void implements Rule { ts.report(n, "method", "JAVA", "minor"); }\nend _lmcode_\n',encoding='utf-8')
  result=subprocess.run(["node",str(ROOT/"scripts/compile-fixture.mjs"),str(source),str(binary)],capture_output=True,cwd=ROOT,timeout=30);self.assertEqual(result.returncode,0,result.stderr)
  code=self.work/'Input.java';code.write_text('class Demo { void run() {} }',encoding='utf-8')
  output=self.run_host('check','--lang','java',binary,code).splitlines();self.assertEqual(json.loads(output[0])[0]['rule'],'JAVA');self.assertTrue(output[1].startswith(b'1 findings in '))
if __name__=='__main__':
 if PROVE:
  suite=unittest.defaultTestLoader.loadTestsFromTestCase(JavaOS)
  CORRUPT=True
  result=unittest.TestResult();suite.run(result)
  if len(result.failures)+len(result.errors)!=result.testsRun:raise SystemExit('output-corruption control survived')
  print('CAUGHT Java output corruption in '+str(result.testsRun)+' native oracles');CORRUPT=False
 unittest.main()
