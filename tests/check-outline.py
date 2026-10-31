#!/usr/bin/env python3
"""Byte-exact outline contracts, isolated from the developer's DB and ignores."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

host = str(Path(sys.argv[1]).resolve())
mode = sys.argv[2]
fixture = Path(__file__).resolve().parent / 'fixtures' / 'outline'
env = dict(os.environ)
env.pop('LMOK_DB', None)
with tempfile.TemporaryDirectory(prefix='lmok-outline-') as directory:
    root = Path(directory)
    (root / '.git').mkdir()
    shutil.copytree(fixture, root / 'src')

    def run(*args, code=0):
        p = subprocess.run([host, *args], cwd=root, env=env, capture_output=True)
        assert p.returncode == code, (args, p.returncode, p.stdout, p.stderr)
        if code == 0:
            assert p.stderr == b'', p.stderr
        return p.stdout

    a = 'src/a.c\n  alpha  function  0:0  signature: int alpha(void) {\n'
    b = 'src/b.c\n  beta  function  0:0  signature: int beta(void) {\n'
    c = ('src/c.py\n  Box  class  0:0  signature: class Box:\n'
         '  get  method  1:4  scope: Box  signature: def get(self):\n'
         '  self  parameter  1:12  scope: Box.get  signature: self\n')
    plain = (a + b + c).encode()
    ranked = (b.replace('src/b.c\n', 'src/b.c (refs: 2)\n') +
              a.replace('src/a.c\n', 'src/a.c (refs: 0)\n') +
              c.replace('src/c.py\n', 'src/c.py (refs: 0)\n')).encode()
    if mode == 'live':
        assert run('outline', 'src') == plain
        assert run('outline', 'src/c.py', './src/b.c', 'src/a.c', 'src/b.c') == plain
        (root / 'src/empty.c').write_text(' \n')
        assert run('outline', 'src') == plain
        assert not (root / '.lmok').exists()
    elif mode == 'db':
        run('index', 'src')
        assert run('outline', 'src') == ranked
        assert run('outline', '--db', '.lmok/lmok.db', 'src') == ranked
        assert run('outline', 'src/c.py', 'src/b.c', 'src/a.c') == ranked
        # DB ranking must never substitute stale definitions/signatures.
        (root / 'src/b.c').write_text('int fresh(void) {}\n')
        changed = run('outline', 'src')
        assert b'fresh  function  0:0  signature: int fresh(void) {}' in changed
        assert b'beta  function' not in changed
    elif mode == 'budget':
        assert run('outline', 'src', '--budget', '8') == plain
        assert run('outline', 'src', '--budget', '7') == (a+b+'... (1 more files)\n').encode()
        assert run('outline', '--budget', '3', 'src') == (a+'... (2 more files)\n').encode()
        assert run('outline', 'src', '--budget', '1') == b'... (3 more files)\n'
        run('index', 'src')
        assert run('outline', 'src', '--budget', '3') == (b.replace('src/b.c\n', 'src/b.c (refs: 2)\n')+'... (2 more files)\n').encode()
    elif mode == 'errors':
        for value in ('0', '-1', '1x', '', '18446744073709551616'):
            run('outline', 'src', '--budget', value, code=2)
        run('outline', 'src', '--db', 'absent.db', code=2)
        run('outline', 'src', '--budget', code=2)
        run('outline', 'src', '--unknown', code=2)
        run('outline', code=2)
        run('outline', 'absent.c', code=3)
        (root / 'bad.db').write_bytes(b'not sqlite')
        p = subprocess.run([host, 'outline', 'src', '--db', 'bad.db'], cwd=root, env=env, capture_output=True)
        assert p.returncode in (2, 3) and not p.stdout, (p.returncode, p.stdout, p.stderr)
    else:
        raise AssertionError(mode)
