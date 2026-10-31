#!/usr/bin/env python3
"""Search oracles use only temporary files/DBs; native output is compared as bytes."""
from pathlib import Path
import os
import sqlite3
import subprocess
import sys
import tempfile

host = str(Path(sys.argv[1]).resolve())
mode = sys.argv[2]
with tempfile.TemporaryDirectory(prefix='lmok-find-') as temp:
    root = Path(temp)
    (root / 'src').mkdir()
    (root / 'src/a.c').write_bytes('// foo( foo( food\r\n// café foo( "quoted" aaaa\n// foo bar\n'.encode())
    (root / 'src/b.py').write_bytes(b'# foo( end\n# FOOD foo\n')
    (root / 'src/c.c').write_bytes(b'// no match\n')
    env = os.environ.copy()
    env.pop('LMOK_DB', None)

    def run(*args, code=0):
        p = subprocess.run([host, *args], cwd=root, env=env, capture_output=True, timeout=30)
        assert p.returncode == code, (args, p.returncode, p.stdout, p.stderr)
        return p

    def find(pattern, *args, code=0):
        return run('find', pattern, '--db', 'index.db', *args, code=code)

    run('index', 'src', '--db', 'index.db')
    expected = ('src/a.c:0:3: // foo( foo( food\n'
                'src/a.c:0:8: // foo( foo( food\n'
                'src/a.c:1:9: // café foo( "quoted" aaaa\n'
                'src/b.py:0:2: # foo( end\n').encode()
    if mode == 'literal':
        result = find('foo(')
        assert result.stdout == expected and not result.stderr, result
        assert find('foo(', '--lang', 'python').stdout == b'src/b.py:0:2: # foo( end\n'
        assert find('"quoted"').stdout == 'src/a.c:1:14: // café foo( "quoted" aaaa\n'.encode()
        assert find('foo bar').stdout == b'src/a.c:2:3: // foo bar\n'
        assert find('food\r\n// café').stdout == b'src/a.c:0:13: // foo( foo( food\n'
        assert find('FOO(').stdout == b''  # literal exact verification rejects folded candidate
        assert find('absent').stdout == b''
        # Adjacent/overlapping hits are separate occurrences, not highlight() unions.
        assert find('aaa').stdout == ('src/a.c:1:23: // café foo( "quoted" aaaa\n'
                                       'src/a.c:1:24: // café foo( "quoted" aaaa\n').encode()
        (root / 'src/a.c').write_bytes(b'// changed on disk\n')
        assert find('foo(').stdout == expected  # explicit stored-snapshot contract
        run('index', 'src', '--db', 'index.db')
        assert find('foo(').stdout == b'src/b.py:0:2: # foo( end\n'
        (root / 'src/b.py').unlink()
        run('index', 'src', '--db', 'index.db')
        assert find('foo(').stdout == b''
    elif mode == 'fts':
        assert find('"foo bar"', '--fts').stdout == b'src/a.c:2:3: // foo bar\n'
        assert find('foo NOT bar', '--fts').stdout == (b'src/b.py:0:2: # foo( end\n'
                                                      b'src/b.py:1:2: # FOOD foo\n'
                                                      b'src/b.py:1:7: # FOOD foo\n')
        assert find('foo OR foo', '--fts', '--lang', 'python').stdout == find('foo', '--fts', '--lang', 'python').stdout
        assert find('aaa', '--fts').stdout == find('aaa').stdout
        assert find('"quoted"', '--fts').stdout == 'src/a.c:1:15: // café foo( "quoted" aaaa\n'.encode()
        assert find('quot*', '--fts').stdout == find('quoted', '--fts').stdout
        assert find('NEAR(quoted aaaa, 10)', '--fts').stdout == ('src/a.c:1:15: // café foo( \"quoted\" aaaa\n'
                                                                 'src/a.c:1:23: // café foo( \"quoted\" aaaa\n').encode()
        assert find('café', '--fts').stdout == 'src/a.c:1:3: // café foo( \"quoted\" aaaa\n'.encode()
        assert find('fo', '--fts').stdout == b''  # native trigram short-term behavior
        for bad in ['foo(', '"unclosed', 'foo OR', 'bogus:foo']:
            p = find(bad, '--fts', code=2)
            assert b'invalid FTS5 query' in p.stderr and not p.stdout, p
    elif mode == 'limit':
        first = b'src/a.c:0:3: // foo( foo( food\n'
        p = find('foo(', '--limit', '1')
        assert p.stdout == first and p.stderr == b'find: truncated after 1 hits (use --limit N)\n', p
        assert find('foo(', '--limit', '4').stdout == expected
        assert not find('foo(', '--limit', '4').stderr
        assert find('foo', '--fts', '--limit', '1').stdout == first
        (root / 'src/many.c').write_bytes(b'// ' + b'needle ' * 1001 + b'\n')
        run('index', 'src', '--db', 'index.db')
        p = find('needle')
        assert len(p.stdout.splitlines()) == 1000 and b'truncated after 1000 hits' in p.stderr
        assert b':0:6996:' in p.stdout.splitlines()[-1]
        if os.name != 'nt' and Path('/dev/full').exists():
            with open('/dev/full', 'wb') as out:
                p = subprocess.run([host, 'find', 'foo', '--db', 'index.db', '--limit', '1'], cwd=root, env=env, stdout=out, stderr=subprocess.PIPE)
            assert p.returncode == 4, p
    elif mode == 'fallback':
        p = find('é')
        assert p.stdout == 'src/a.c:1:6: // café foo( "quoted" aaaa\n'.encode() and b'scanning stored text' in p.stderr
        p = find('fo', '--lang', 'python')
        assert p.stdout == b'src/b.py:0:2: # foo( end\nsrc/b.py:1:7: # FOOD foo\n'
        # Legacy schema-2 databases stay readable, then index upgrades only FTS.
        with sqlite3.connect(root / 'index.db') as db:
            rows = db.execute('SELECT rowid,path,lang,text FROM file_text').fetchall()
            symbols = db.execute('SELECT * FROM symbols').fetchall()
            db.execute('DROP TABLE file_text')
            db.execute('CREATE VIRTUAL TABLE file_text USING fts5(path UNINDEXED,lang UNINDEXED,text)')
            db.executemany('INSERT INTO file_text(rowid,path,lang,text) VALUES (?,?,?,?)', rows)
        p = find('foo(')
        assert p.stdout == expected and b'legacy text index' in p.stderr
        assert find('foo', '--fts', '--lang', 'python').stdout == b'src/b.py:0:2: # foo( end\nsrc/b.py:1:7: # FOOD foo\n'
        with sqlite3.connect(root / 'index.db') as db:
            assert 'trigram' not in db.execute("SELECT sql FROM sqlite_master WHERE name='file_text'").fetchone()[0]
        assert run('index', 'src', '--db', 'index.db').stdout == b'indexed 0 files (3 unchanged, 0 removed)\n'
        with sqlite3.connect(root / 'index.db') as db:
            assert "tokenize='trigram'" in db.execute("SELECT sql FROM sqlite_master WHERE name='file_text'").fetchone()[0]
            assert db.execute('SELECT * FROM symbols').fetchall() == symbols
            assert db.execute('SELECT rowid,path,lang,text FROM file_text').fetchall() == rows
        assert not find('foo(').stderr
        (root / 'src/bytes.c').write_bytes(b'// \xff foo(\x00 foo(\n')
        run('index', 'src', '--db', 'index.db')
        assert find('foo(').stdout.endswith(b'src/bytes.c:0:5: // \xff foo(\x00 foo(\nsrc/bytes.c:0:11: // \xff foo(\x00 foo(\n')
        # A unique needle after NUL must survive FTS tokenizer truncation.
        (root / 'src/nul.c').write_bytes(b'// prefix\x00 only_after_nul\n')
        run('index', 'src', '--db', 'index.db')
        p = find('only_after_nul')
        assert p.stdout == b'src/nul.c:0:11: // prefix\x00 only_after_nul\n' and b'scanning stored byte-text' in p.stderr
        # NULL safety metadata (e.g. imported rows) is conservatively scanned.
        with sqlite3.connect(root / 'index.db') as db:
            db.execute("UPDATE file_text SET byte_scan=NULL WHERE path='src/nul.c'")
        assert find('only_after_nul').stdout == p.stdout
        if os.name != 'nt':
            p = find(os.fsdecode(b'\xff f'))
            assert b'src/bytes.c:0:3:' in p.stdout and b'scanning stored text' in p.stderr
    elif mode == 'errors':
        p = run('find', 'foo', '--db', 'missing/new.db', code=2)
        assert b'run `lmok index` first' in p.stderr and not (root / 'missing').exists()
        (root / 'bad.db').write_bytes(b'not a database')
        run('find', 'foo', '--db', 'bad.db', code=3)
        for args in [(), ('',), ('foo', '--lang', 'rust'), ('foo', '--limit', '0'),
                     ('foo', '--limit', '-1'), ('foo', '--limit', '1x'),
                     ('foo', '--limit', '18446744073709551616'), ('foo', '--db'),
                     ('foo', '--fts', '--fts'), ('foo', 'bar'), ('--wat',)]:
            run('find', *args, code=2)
        assert run('find', '--db', 'index.db', '--', '--literal').stdout == b''
        env['LMOK_DB'] = str(root / 'index.db')
        assert run('find', 'foo(').stdout == expected
    else:
        raise AssertionError(mode)
print('find:', mode, 'passed')
