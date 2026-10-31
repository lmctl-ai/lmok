#!/usr/bin/env python3
"""Project/ignore/suppression oracles; byte output and native Windows compiler shim."""
import json
from contextlib import closing
import os
from pathlib import Path
import shutil
import sqlite3
from contextlib import closing
import subprocess
import sys
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
HOST = Path(sys.argv.pop(1)).resolve()
COMPILER = os.environ.get('LMSCRIPT_BIN', 'lmscript')
def compiler():
    if os.name == 'nt' and not COMPILER.lower().endswith(('.exe','.cmd','.bat')):
        for p in (COMPILER+'.cmd', shutil.which('lmscript.cmd')):
            if p and Path(p).exists(): return p
    return COMPILER
class ProjectTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='project 雪 ',dir=ROOT/'build');self.root=Path(self.tmp.name)
        self.pack=self.root/'pack.json';self.pack.write_text(json.dumps({'lang':'c','rules':[{'id':'A','on':'identifier','message':'id','severity':'major'},{'id':'B','on':'identifier','message':'low','severity':'info'}]}),encoding='utf8')
    def tearDown(self): self.tmp.cleanup()
    def run_host(self,*args): return subprocess.run([str(HOST),*map(str,args)],cwd=self.root,capture_output=True,timeout=20)
    def source(self,name,text):
        p=self.root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(text.encode());return p
    def findings(self,r):
        self.assertEqual(r.returncode,0,r.stderr);return [json.loads(s) for s in r.stdout.splitlines() if s.startswith(b'{"row"')]
    def config(self,**kw):
        obj={'version':1,'packs':{'c':['pack.json']},'severity_floor':'major','fail_on':'findings'};obj.update(kw)
        self.source('.lmok/config.json',json.dumps(obj));return obj
    def compile(self):
        p=self.source('entry.lms','p=_lmcode_ schema='+json.dumps(str(ROOT/'schema/lmok.dsl.json'))+';\n@identifier fn ids(n: Node) -> void implements Rule { ts.report(n,"id","A","major"); }\nend _lmcode_\n');out=self.root/'rule.dslb'
        r=subprocess.run(["node",str(ROOT/"scripts/compile-fixture.mjs"),str(p),str(out)],capture_output=True,timeout=30);self.assertEqual(r.returncode,0,r.stderr);return out
    def test_inline_selective_previous_line_crlf_and_override(self):
        p=self.source('x.c','int a; // lmok-disable: A\r\nint b;\r\n// lmok-disable\r\nint c;\r\nint d;\r\n')
        r=self.run_host('lint',self.pack,p);found=self.findings(r)
        self.assertEqual([(f['row'],f['rule']) for f in found],[(0,'B'),(1,'B'),(4,'A'),(4,'B')]);self.assertIn(b'(4 suppressed)',r.stdout)
        all_=self.findings(self.run_host('lint',self.pack,p,'--no-suppress'));self.assertEqual(len(all_),8)
    def test_utf8_namespaced_ids_and_comment_boundaries(self):
        self.pack.write_text(json.dumps({'lang':'c','rules':[{'id':'régle:team/R','on':'identifier','message':'id','severity':'major'}]}),encoding='utf8')
        p=self.source('x.c','int x; /*lmok-disable: régle:team/R*/\n')
        r=self.run_host('lint',self.pack,p);self.assertEqual(self.findings(r),[]);self.assertEqual(r.stderr,b'')
        self.assertIn(b'(1 suppressed)',r.stdout)
        self.assertEqual(len(self.findings(self.run_host('lint',self.pack,p,'--no-suppress'))),1)
    def test_token_and_typo_warning(self):
        p=self.source('x.c','int a; // notlmok-disable\nint b; // lmok-disable: TYPO\n')
        r=self.run_host('lint',self.pack,p);self.assertEqual(len(self.findings(r)),4);self.assertIn(b"unknown rule id 'TYPO'",r.stderr)
    def test_ignore_negation_shared_walk_and_explicit_file(self):
        self.source('.lmokignore','# comment\r\npack.json\r\ngenerated/\r\n!generated/keep.c\r\n**/skip?.c\r\n')
        self.source('generated/drop.c','int drop;');self.source('generated/keep.c','int keep;');self.source('src/skip1.c','int skip;');self.source('src/live.c','int live;')
        found=self.findings(self.run_host('lint',self.pack,'.'));self.assertEqual(len(found),4)
        explicit=self.findings(self.run_host('lint',self.pack,'generated/drop.c'));self.assertEqual(len(explicit),2)
        r=self.run_host('facts','.');self.assertEqual(r.returncode,0,r.stderr);self.assertNotIn(b'generated/drop.c',r.stdout);self.assertNotIn(b'skip1.c',r.stdout);self.assertIn(b'generated/keep.c',r.stdout)
        r=self.run_host('index','--db',self.root/'db.sqlite','.');self.assertEqual(r.returncode,0,r.stderr)
        # SQLite's transaction context does not close its handle; Windows refuses fixture removal.
        with closing(sqlite3.connect(self.root/'db.sqlite')) as db: paths=[r[0] for r in db.execute('select path from files')]
        self.assertEqual(len(paths),2,paths)
    def test_check_sink_and_directory_ignores(self):
        rule=self.compile();self.source('.lmokignore','drop.c\n');self.source('drop.c','int drop;');self.source('keep.c','// lmok-disable: A\nint keep;\nint live;')
        r=self.run_host('check','--lang','c',rule,'.');self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        arrays=[json.loads(s) for s in r.stdout.splitlines() if s.startswith(b'[')];self.assertEqual(len(arrays),1);self.assertEqual(len(arrays[0]),1);self.assertIn(b'(1 suppressed)',r.stdout)
        r=self.run_host('check','--no-suppress','--lang','c',rule,'keep.c');self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(len(json.loads(r.stdout.splitlines()[0])),2)
    def test_ci_floor_fail_on_ignores_and_json_lines(self):
        self.config(ignore=['ignored.c']);self.source('ignored.c','int ignored;');self.source('live.c','int live; // lmok-disable: A\n')
        r=self.run_host('ci');self.assertEqual(r.returncode,0,r.stderr);records=[json.loads(s) for s in r.stdout.splitlines()];self.assertEqual([v['rule'] for v in records if 'rule' in v],['B']);self.assertEqual(records[-1]['summary']['suppressed'],1)
        r=self.run_host('ci','--no-suppress');self.assertEqual(r.returncode,1,r.stderr)
        self.config(fail_on='none');self.assertEqual(self.run_host('ci','--no-suppress').returncode,0)
        self.config(severity_floor='critical');self.assertEqual(self.run_host('ci','--no-suppress').returncode,0)
    def test_ci_rejects_bad_configuration_and_baseline_is_not_ignored(self):
        self.source('live.c','int live;')
        for kw in [{'severity_floor':'typo'},{'fail_on':'typo'},{'baseline':'old.jsonl'},{'unexpected':1},{'packs':{'bad':['pack.json']}}]:
            self.config(**kw);r=self.run_host('ci');self.assertEqual(r.returncode,2,r.stderr);self.assertIn(b'project config',r.stderr)
    def test_all_ignored_is_success_and_escape_pattern_refused(self):
        self.source('only.c','int only;');self.source('.lmokignore','*.c\n');r=self.run_host('lint',self.pack,'.');self.assertEqual((r.returncode,r.stdout),(0,b''),r.stderr)
        self.source('.lmokignore','../outside\n');self.assertEqual(self.run_host('lint',self.pack,'.').returncode,2)
    def test_root_anchors_directory_rules_and_globs(self):
        self.source('root/a.c','int root;');self.source('nest/root/a.c','int nested;')
        self.source('file.c','int file;');self.source('deep/x1.c','int x;')
        self.source('.lmokignore','/root/\nfile.c/\n**/x[0-9].c\n')
        r=self.run_host('lint',self.pack,'.');self.assertEqual(len(self.findings(r)),4)
        self.assertIn(b'nest/root/a.c',r.stdout);self.assertIn(b'./file.c',r.stdout)
        self.source('.lmokignore','root/a.c\n')
        r=self.run_host('lint',self.pack,'.');self.assertEqual(len(self.findings(r)),6)
        self.assertIn(b'nest/root/a.c',r.stdout)
    def test_ci_json_replaces_invalid_utf8_in_findings(self):
        self.pack.write_text(json.dumps({'lang':'c','rules':[{'id':'BAD','on':'comment','message':'{text}','severity':'major'}]}))
        self.config();(self.root/'invalid.c').write_bytes(b'// bad \xff\n')
        r=self.run_host('ci');self.assertEqual(r.returncode,1,r.stderr)
        rows=[json.loads(line) for line in r.stdout.splitlines()]
        self.assertEqual(rows[0]['message'],'// bad \ufffd')
    def test_slash_pattern_also_excludes_directory_descendants(self):
        self.source('a/b/child.c','int hidden;');self.source('nested/a/b/child.c','int kept;')
        self.source('.lmokignore','a/b\n')
        r=self.run_host('lint',self.pack,'.');self.assertEqual(len(self.findings(r)),2)
        self.assertIn(b'nested/a/b/child.c',r.stdout)
    def test_ci_uses_host_language_registry(self):
        self.source('java.json',json.dumps({'lang':'java','rules':[{'id':'J','on':'identifier','message':'java','severity':'major'}]}))
        self.config(packs={'java':['java.json']});self.source('Example.java','class Example {}\n')
        r=self.run_host('ci');self.assertEqual(r.returncode,1,r.stderr)
        rows=[json.loads(line) for line in r.stdout.splitlines()]
        self.assertEqual([v['rule'] for v in rows if 'rule' in v],['J'])
    def test_multiple_packs_languages_and_config_path(self):
        self.source('second.json',json.dumps({'lang':'python','rules':[{'id':'P','on':'identifier','message':'python','severity':'critical'}]}))
        self.config(packs={'c':['pack.json'],'python':['second.json']})
        self.source('live.c','int live;');self.source('live.py','live=1\n')
        r=self.run_host('ci','--config','.lmok/config.json',self.root)
        self.assertEqual(r.returncode,1,r.stderr)
        rows=[json.loads(s) for s in r.stdout.splitlines()];self.assertEqual(sorted(v['rule'] for v in rows if 'rule' in v),['A','B','P'])
        self.config(packs={'c':['pack.json','pack.json']})
        r=self.run_host('ci');self.assertEqual(r.returncode,2);self.assertIn(b'duplicate rule id',r.stderr)
    def test_check_typo_without_emitted_findings(self):
        rule=self.compile();p=self.source('empty.c','// lmok-disable: TYPO\n')
        r=self.run_host('check','--lang','c',rule,p)
        self.assertEqual(r.returncode,0,r.stderr);self.assertIn(b"unknown rule id 'TYPO'",r.stderr)
if __name__=='__main__': unittest.main()
