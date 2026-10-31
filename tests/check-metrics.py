#!/usr/bin/env python3
"""Hand-computed ten-function C/Python score and nesting table, native OS."""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
HOST = Path(sys.argv[1]).resolve()
COMPILER = os.environ.get('LMSCRIPT_BIN', 'lmscript')
if os.name == 'nt' and not COMPILER.lower().endswith(('.cmd','.exe','.bat')):
    COMPILER = COMPILER+'.cmd' if Path(COMPILER+'.cmd').exists() else shutil.which('lmscript.cmd')
# Each row is a distinct function, with handwritten cognitive and max-nesting values.
TABLE = [
    ('flat',0,0,'return a + b;', 'return a + b'),
    ('branch',1,1,'if(a){return 1;} return 0;', 'if a:\n    return 1\nreturn 0'),
    ('nested',3,2,'if(a){if(b){return 1;}} return 0;', 'if a:\n    if b:\n        return 1\nreturn 0'),
    ('loop',6,3,'for(;;){if(a){while(b){return 1;}}} return 0;', 'for x in xs:\n    if a:\n        while b:\n            return 1\nreturn 0'),
    ('alternative',2,1,'if(a){return 1;}else{return 0;}', 'if a:\n    return 1\nelse:\n    return 0'),
    ('chain',3,1,'if(a){return 1;}else if(b){return 2;}else{return 0;}', 'if a:\n    return 1\nelif b:\n    return 2\nelse:\n    return 0'),
    ('same_boolean',2,1,'if(a&&b&&c){return 1;} return 0;', 'if a and b and c:\n    return 1\nreturn 0'),
    ('mixed_boolean',3,1,'if(a&&b||c){return 1;} return 0;', 'if a and b or c:\n    return 1\nreturn 0'),
    ('siblings',2,1,'if(a){return 1;} if(b){return 2;} return 0;', 'if a:\n    return 1\nif b:\n    return 2\nreturn 0'),
    ('deep',21,6,'if(a){if(b){if(c){if(d){if(e){if(f){return 1;}}}}}} return 0;', 'if a:\n    if b:\n        if c:\n            if d:\n                if e:\n                    if f:\n                        return 1\nreturn 0'),
]

def run(*args):
    result = subprocess.run([str(HOST), *map(str,args)],capture_output=True,timeout=60)
    assert result.returncode == 0, (args,result.stdout,result.stderr)
    return result.stdout

def rejected(actual, expected):
    try: assert actual == expected
    except AssertionError: return
    raise AssertionError('oracle accepted a mutant')

with tempfile.TemporaryDirectory(prefix='metrics os ',dir=ROOT/'build') as tmp:
    tmp = Path(tmp)
    for lang, ext, col in [('c','c',3),('python','py',4)]:
        pack = (ROOT/'rules/lmcode'/f'{lang}-function-cognitive-complexity.lms').read_text()
        def compile_pack(threshold, zero=False):
            source = pack.replace('cli="lmok"','schema='+json.dumps(str(ROOT/'schema/lmok.dsl.json')))
            source = source.replace('score > 15', threshold)
            if zero: source = source.replace('let score = cognitive_score(n);','let score = 0;')
            path = tmp/'rule.lms'; binary=tmp/'rule.dslb'; path.write_text(source)
            result = subprocess.run(["node",str(ROOT/"scripts/compile-fixture.mjs"),str(path),str(binary)],capture_output=True,timeout=60)
            assert result.returncode == 0,(result.stdout,result.stderr)
            return binary
        def function(name, body):
            if lang=='c': return f'int {name}(void){{'+body+'}\n'
            return 'def '+name+'():\n'+'\n'.join('    '+line for line in body.splitlines())+'\n'
        whole = tmp/('table 雪.'+ext)
        whole.write_bytes(''.join(function(row[0],row[col]) for row in TABLE).encode())
        expected = [row[1] for row in TABLE]
        binary = compile_pack('score > -1', zero=True)
        mutant = json.loads(run('check','--lang',lang,binary,whole).splitlines()[0])
        rejected([int(f['message'].split('(')[1][:-1]) for f in mutant],expected)
        print('ok -',lang,'zero-score mutant rejected before table oracle')
        binary = compile_pack('score > -1')
        actual = json.loads(run('check','--lang',lang,binary,whole).splitlines()[0])
        assert [int(f['message'].split('(')[1][:-1]) for f in actual] == expected,(lang,actual,expected)
        metrics = json.loads(run('metrics','--lang',lang,whole))
        assert metrics['functions'] == 10 and metrics['max_nesting']==6 and metrics['lines'] == (10 if lang=='c' else 52), metrics
        hand_lines = [2,4,5,6,5,7,4,4,6,9]
        for index,row in enumerate(TABLE):
            single=tmp/(row[0]+'.'+ext); single.write_bytes(function(row[0],row[col]).encode())
            m=json.loads(run('metrics',single))
            assert m['functions']==1 and m['max_nesting']==row[2] and m['lines']==(1 if lang=='c' else hand_lines[index]),(row[0],m)
        binary=compile_pack('score > 15')
        actual=json.loads(run('check','--lang',lang,binary,whole).splitlines()[0])
        assert len(actual)==1 and actual[0]['message']=='cognitive complexity (21)',actual
        # Sum 1+2+3+4+5=15 is the strict threshold boundary.
        boundary=tmp/('boundary.'+ext)
        body=TABLE[-1][col]
        if lang=='c': body=body.replace('if(f){return 1;}','return 1;')
        else: body=body.replace('                    if f:\n                        return 1','                    return 1')
        boundary.write_text(function('boundary',body))
        assert json.loads(run('check','--lang',lang,binary,boundary).splitlines()[0])==[]
        # Arithmetic has zero complexity; strings/comments containing keywords do too.
        for data,lines in [(b'',0),(b'\n',1),(b'\n\n',2),(b'x=1',1),(b'x=1\r\n',1)]:
            boundary.write_bytes(data)
            assert json.loads(run('metrics','--lang',lang,boundary))['lines']==lines
        print('ok -',lang,'ten cognitive/nesting hand values, strict threshold and physical-line boundaries')
    go=tmp/'go-count.go'
    go.write_bytes(b'package main\nfunc one() {}\nfunc (r R) two() { if true {} }\n')
    m=json.loads(run('metrics',go))
    assert (m['lines'],m['functions'],m['max_nesting'])==(3,2,1),m
print('ok - metrics and cognitive complexity, including newly registered Go definitions')
