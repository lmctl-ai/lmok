#!/usr/bin/env python3
"""Native-OS metadata and annotation harness controls (no shell or live tools)."""
import json, os, shutil, subprocess, sys, tempfile, unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
HOST = Path(sys.argv.pop(1)).resolve()
class Contracts(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='rule contracts ', dir=ROOT/'build')
        self.work = Path(self.tmp.name)
        self.pack = self.work/'pack.json'
        self.rule = dict(id='R',on='call_expression',message='call',severity='major',tags=['z','a','a'],cwe=['CWE-78'],owasp='A03:2021',url='https://example.org/rule',confidence='high')
        self.write_pack()
    def tearDown(self): self.tmp.cleanup()
    def write_pack(self): self.pack.write_text(json.dumps(dict(lang='c',rules=[self.rule])),encoding='utf8')
    def run_host(self,*args): return subprocess.run([str(HOST),*map(str,args)],capture_output=True,text=True,encoding="utf8",timeout=30)
    def file(self,text,name='fixture é.c'):
        p=self.work/name;p.write_bytes(text.encode());return p
    def test_metadata_opt_in_and_catalog(self):
        src=self.file('int f(){ return foo(); }\n')
        plain=self.run_host('lint',self.pack,src);self.assertEqual(plain.returncode,0,plain.stderr)
        base=json.loads(plain.stdout.splitlines()[0]);self.assertNotIn('metadata',base)
        result=self.run_host('lint',self.pack,src,'--metadata');self.assertEqual(result.returncode,0,result.stderr)
        meta=json.loads(result.stdout.splitlines()[0]).pop('metadata');self.assertEqual(meta['tags'],['a','z']);self.assertEqual(meta['owasp'],['A03:2021']);self.assertEqual(meta['confidence'],'high')
        listing=self.run_host('rules',self.pack);self.assertEqual(listing.returncode,0,listing.stderr)
        rows=[json.loads(line) for line in listing.stdout.splitlines()];self.assertEqual(rows[0]['id'],'R');self.assertEqual(rows[0]['severity'],'major');self.assertEqual(rows[0]['metadata'],meta)
        self.assertEqual(listing.stdout,self.run_host('rules',self.pack).stdout)
    def test_ci_metadata_uses_the_same_opt_in_contract(self):
        self.file('int f(){foo();}\n')
        config=self.work/'config.json';config.write_text(json.dumps(dict(version=1,packs={'c':['pack.json']},fail_on='none')))
        plain=self.run_host('ci','--config',config,self.work);self.assertEqual(plain.returncode,0,plain.stderr)
        finding=next(json.loads(line) for line in plain.stdout.splitlines() if '"rule"' in line);self.assertNotIn('metadata',finding)
        rich=self.run_host('ci','--config',config,self.work,'--metadata');self.assertEqual(rich.returncode,0,rich.stderr)
        finding=next(json.loads(line) for line in rich.stdout.splitlines() if '"rule"' in line);self.assertEqual(finding['metadata']['tags'],['a','z'])
    def test_sarif_pack_and_ci_metadata_is_included_without_json_flag(self):
        src=self.file('int f(){foo();}\n');artifact=self.work/'lint.sarif'
        r=self.run_host('lint',self.pack,src,'--sarif',artifact);self.assertEqual(r.returncode,0,r.stderr)
        self.assertNotIn('metadata',json.loads(r.stdout.splitlines()[0]))
        run=json.loads(artifact.read_text(encoding='utf8'))['runs'][0];rule=run['tool']['driver']['rules'][0]
        self.assertEqual(rule['helpUri'],self.rule['url']);self.assertIn('properties',rule,'pack rule metadata must reach SARIF')
        self.assertEqual(rule['properties']['cwe'],['CWE-78']);self.assertEqual(rule['properties']['tags'],['a','z']);self.assertEqual(rule['properties']['confidence'],'high')
        self.assertEqual(run['results'][0]['properties'],rule['properties'])
        config=self.work/'config.json';config.write_text(json.dumps(dict(version=1,packs={'c':['pack.json']},fail_on='none')))
        artifact=self.work/'ci.sarif';r=self.run_host('ci','--config',config,self.work,'--sarif',artifact);self.assertEqual(r.returncode,0,r.stderr)
        run=json.loads(artifact.read_text(encoding='utf8'))['runs'][0]
        self.assertEqual(run['tool']['driver']['rules'][0]['properties']['tags'],['a','z'])
        self.assertEqual(run['results'][0]['properties']['owasp'],['A03:2021'])
    def test_catalog_sort_order_is_independent_of_pack_order(self):
        other=self.work/'second.json';other.write_text(json.dumps(dict(lang='c',rules=[dict(self.rule,id='A')])))
        first=self.run_host('rules',self.pack,other);second=self.run_host('rules',other,self.pack)
        self.assertEqual(first.returncode,0,first.stderr);self.assertEqual(first.stdout,second.stdout)
        self.assertEqual([json.loads(line)['id'] for line in first.stdout.splitlines()],['A','R'])
        duplicate=self.run_host('rules',self.pack,self.pack);self.assertEqual(duplicate.returncode,2);self.assertIn('duplicate catalog rule',duplicate.stderr)
    def test_buffer_copy_is_not_labeled_as_owasp_injection(self):
        # OWASP 2021 A03 maps CWE-78/95; CWE-120 is not in its published mapping.
        rules=json.loads((ROOT/'rules/c-tier1.json').read_text(encoding='utf8'))['rules']
        rule=next(rule for rule in rules if rule['id']=='LBC001')
        self.assertEqual(rule['cwe'],['CWE-120']);self.assertNotIn('A03:2021',rule.get('owasp',[]))
    def test_invalid_metadata_is_located(self):
        for key,value in [('tags',[42]),('cwe',False),('url',4),('confidence','certain')]:
            self.rule[key]=value;self.write_pack();r=self.run_host('rules',self.pack)
            self.assertEqual(r.returncode,2,r.stdout);self.assertIn('rule R',r.stderr);self.assertIn(key,r.stderr)
            self.rule.pop(key)
    def test_annotation_pass_fail_and_located_diff(self):
        src=self.file('int f(){\r\n foo(); /* lmok-expect: R */\r\n}\r\n')
        r=self.run_host('test',self.pack,src);self.assertEqual(r.returncode,0,r.stderr);self.assertTrue(json.loads(r.stdout)['ok'])
        src=self.file('int f(){\n foo(); // lmok-expect: WRONG\n}\n')
        r=self.run_host('test',self.pack,src);self.assertEqual(r.returncode,1,r.stderr)
        row=json.loads(r.stdout);self.assertFalse(row['ok']);self.assertEqual({d['kind'] for d in row['diff']},{'missing','unexpected'})
        self.assertIn(':2:',r.stderr)
        # The suite's own checker rejects a deliberately wrong expected result.
        with self.assertRaises(AssertionError): self.assertTrue(row['ok'])
    def test_comment_tokens_not_strings_or_next_line(self):
        src=self.file('char *s="lmok-expect: R";\nint f(){return foo();}\n')
        r=self.run_host('test',self.pack,src);self.assertEqual(r.returncode,1,r.stderr)
        self.assertEqual([d['kind'] for d in json.loads(r.stdout)['diff']],['unexpected'])
        src=self.file('// lmok-expect: R\nint f(){return foo();}\n')
        r=self.run_host('test',self.pack,src);self.assertEqual(r.returncode,1,r.stderr);self.assertEqual(len(json.loads(r.stdout)['diff']),2)
    def test_utf8_namespaced_rule_id_is_one_token(self):
        self.rule['id']='régle:R';self.write_pack()
        src=self.file('int f(){foo();} // lmok-expect: régle:R\n')
        r=self.run_host('test',self.pack,src);self.assertEqual(r.returncode,0,r.stderr)
        self.assertTrue(json.loads(r.stdout)['ok'])
        listing=self.run_host('rules',self.pack);self.assertEqual(json.loads(listing.stdout)['id'],'régle:R')
    def test_multiple_ids_and_clean_files(self):
        self.rule['id']='A';other=dict(self.rule,id='B');self.pack.write_text(json.dumps(dict(lang='c',rules=[self.rule,other])))
        src=self.file('int f(){foo();} // lmok-expect: A,B\n')
        clean=self.file('int f(){return 0;}\n','clean.c')
        r=self.run_host('test',self.pack,src,clean);self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(json.loads(r.stdout)['expected'],2)
    def test_macro_comment_is_not_lost_in_preprocessor_token(self):
        self.rule['on']='preproc_def';self.write_pack()
        src=self.file('#define VALUE 64 // lmok-expect: R\n')
        r=self.run_host('test',self.pack,src);self.assertEqual(r.returncode,0,r.stderr)
        self.assertEqual(json.loads(r.stdout)['expected'],1)
    def test_report_meta_entry(self):
        schema=json.dumps(str(ROOT/'schema/lmok.dsl.json'))
        meta=json.dumps(json.dumps({'tags':['entry'],'confidence':'medium','url':'https://example.org/entry'}))
        src=self.work/'entry.lms';binary=self.work/'entry.dslb'
        src.write_text('p = _lmcode_ schema='+schema+';\n@call_expression fn call(n: Node) -> void implements Rule { ts.report(n,"legacy","ENTRY","minor"); ts.report_meta(n,"entry","ENTRY","minor",'+meta+'); }\nend _lmcode_\n',encoding='utf8')
        compiler=os.environ.get('LMSCRIPT_BIN','lmscript')
        if os.name=='nt' and not compiler.endswith(('.cmd','.exe','.bat')): compiler=shutil.which('lmscript.cmd') or compiler+'.cmd'
        r=subprocess.run(["node",str(ROOT/"scripts/compile-fixture.mjs"),str(src),str(binary)],capture_output=True,text=True,encoding="utf8");self.assertEqual(r.returncode,0,r.stderr)
        target=self.file('int f(){foo();}\n')
        plain=self.run_host('check','--lang','c',binary,target);self.assertEqual(plain.returncode,0,plain.stderr);self.assertNotIn('metadata',json.loads(plain.stdout.splitlines()[0])[0])
        r=self.run_host('check','--lang','c',binary,target,'--metadata');self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(json.loads(r.stdout.splitlines()[0])[1]['metadata']['tags'],['entry'])
        artifact=self.work/'entry.sarif';r=self.run_host('check','--lang','c',binary,target,'--sarif',artifact);self.assertEqual(r.returncode,0,r.stderr)
        self.assertNotIn('metadata',json.loads(r.stdout.splitlines()[0])[0])
        run=json.loads(artifact.read_text(encoding='utf8'))['runs'][0]
        self.assertIn('properties',run['tool']['driver']['rules'][0],'entry metadata must reach SARIF catalog')
        self.assertEqual(run['tool']['driver']['rules'][0]['properties']['tags'],['entry'])
        self.assertEqual(run['tool']['driver']['rules'][0]['helpUri'],'https://example.org/entry')
        self.assertEqual(run['results'][1]['properties']['confidence'],'medium')
if __name__=='__main__': unittest.main()
