#!/usr/bin/env python3
"""Native token normalization, clone thresholds, ranges and parser-token controls."""
import json
import subprocess
import sys
import tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
HOST = Path(sys.argv[1]).resolve()

def run(*args):
    return subprocess.run([str(HOST), *map(str,args)], capture_output=True, timeout=30)

def rejects(actual, expected):
    try: assert actual == expected
    except AssertionError: return
    raise AssertionError('control was not rejected')

with tempfile.TemporaryDirectory(prefix='dup os ',dir=ROOT/'build') as tmp:
    tmp = Path(tmp)
    for lang,ext,source,width,ends in [
        ('python','py','alpha = 1 + 2\nbeta = 3 + 4\n',5,[13,12]),
        ('c','c','int f(void){return 1;}\nint g(void){return 2;}\n',10,[22,22]),
    ]:
        p = tmp / ('clone 雪.'+ext)
        p.write_bytes(source.replace('\n','\r\n').encode())
        r = run('dup', '--lang',lang,'--tokens',width,p)
        assert r.returncode == 0, r.stderr
        fs = [json.loads(line) for line in r.stdout.splitlines()]
        expected = [dict(file=str(p).replace(chr(92),'/'),start_row=i,start_col=0,end_row=i,end_col=ends[i],tokens=width) for i in range(2)]
        # Broken count and off-by-one range controls run before the real oracle.
        rejects(fs, [])
        wrong = [dict(f,end_col=f['end_col']+1) for f in fs]
        rejects(wrong, expected)
        assert fs == expected, (lang,fs,expected)
        assert run('dup', '--tokens',width+1,'--lang',lang,p).stdout == b''
        assert run('dup','--tokens',width,p).stdout == r.stdout
        assert run('dup','--tokens',width,p).stdout == r.stdout  # deterministic
    p = tmp / 'keywords.c'
    p.write_text('if (one) work(); while (two) work();\n', encoding='utf-8')
    assert run('dup','--tokens','8',p).stdout == b'', 'keywords must not normalize to identifiers'
    p.write_text('a += 1; b -= 2;\n', encoding='utf-8')
    assert run('dup','--tokens','4',p).stdout == b'', 'operators must remain distinct'
    p.write_text('alpha = 1 + 2; /* comment words are not tokens */\nbeta = 3 + 4;\n', encoding='utf-8')
    assert len(run('dup','--tokens','6',p).stdout.splitlines()) == 2
    p.write_text('a + a + a;', encoding='utf-8')
    assert run('dup','--tokens','5',p).stdout == b'', 'overlapping repeats are not two clones'
    for n in ['0','-1','4x','99999999999999999999999']:
        assert run('dup','--tokens',n,p).returncode == 2
    assert run('metrics','--tokens','2',p).returncode == 2
    assert run('dup','--tokens','999999',p).stdout == b''
    assert run('dup', tmp/'missing.py').returncode == 3
print('ok - failing range/count controls; token normalization, keywords/operators, nonoverlap, bounds and CRLF/UTF-8 paths')
