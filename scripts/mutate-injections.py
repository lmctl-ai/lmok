#!/usr/bin/env python3
"""Opt-in production-copy S5 controls; never edits the working host sources."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
CASES=[
 ('missing injection','host.cpp','if(node_kind(node)=="lmcode_body")','if(false)'),
 ('relative origin','host.cpp','const TSRange range{ts_node_start_point(node),','const TSRange range{TSPoint{0,0},'),
 ('first body only','host.cpp','if(node_kind(node)=="lmcode_body")trees.push_back(parse_range(Grammar::Lmcode,node));','if(node_kind(node)=="lmcode_body"){trees.push_back(parse_range(Grammar::Lmcode,node));break;}'),
 ('wrong fact language','facts.cpp','fact.language="lmcode";','fact.language="lmscript";'),
 ('duplicate suppression state','main.cpp','const auto *owner=alias==host.suppression_owners.end()?n->owner:alias->second;','const auto *owner=n->owner;'),
 ('compiled check skips injection','main.cpp','const auto trees=lmok::analysis_trees(grammar,raw,std::filesystem::u8path(target.path).extension()==".lms");','const auto trees=lmok::analysis_trees(grammar,raw,false);'),
]
with tempfile.TemporaryDirectory(prefix='injection-mutants-',dir=ROOT/'build') as tmp:
    clone=Path(tmp)/'source'
    shutil.copytree(ROOT,clone,ignore=shutil.ignore_patterns('.git','build','__pycache__'))
    build=Path(tmp)/'build'
    (clone/'build').mkdir()
    def command(args):
        r=subprocess.run(args,cwd=clone,capture_output=True,text=True)
        assert r.returncode==0,r.stdout+r.stderr
    command(['cmake','-S',str(clone),'-B',str(build),'-G','Ninja','-DCMAKE_BUILD_TYPE=Release'])
    originals={name:(clone/'cpp'/name).read_text() for name in {c[1] for c in CASES}}
    def run():
        command(['cmake','--build',str(build),'-j2'])
        return subprocess.run(['python3',str(clone/'tests/check-injections.py'),str(build/('lmok.exe' if os.name=='nt' else 'lmok'))],cwd=clone,capture_output=True,text=True)
    r=run();assert r.returncode==0,r.stdout+r.stderr;print('BASELINE PASS',flush=True)
    for name,file,before,after in CASES:
        for f,src in originals.items():(clone/'cpp'/f).write_text(src)
        assert originals[file].count(before)==1,('anchor',name)
        (clone/'cpp'/file).write_text(originals[file].replace(before,after))
        r=run();assert r.returncode!=0,('SURVIVED',name)
        assert 'AssertionError' in r.stderr and 'FAILED (failures=' in r.stderr,r.stdout+r.stderr
        assert 'ERROR:' not in r.stderr,r.stderr
        print('CAUGHT '+name,flush=True)
    print('PASS 6 production-copy mutants',flush=True)
