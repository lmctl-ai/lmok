#!/usr/bin/env python3
"""DB CLI contracts, independent schema oracle, and read-only path resolution."""
from contextlib import contextmanager
import hashlib
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import sys
import tempfile

host = str(Path(sys.argv[1]).resolve())
repo = Path(__file__).resolve().parent.parent
schema = (repo / 'docs/tier3/db-design.md').read_text().split('```sql\n', 1)[1].split('```', 1)[0]
embedded = (repo / 'cpp/db.cpp').read_text().split('R"LMOKSQL(\n', 1)[1].split(')LMOKSQL"', 1)[0]
assert embedded == schema, 'embedded schema must remain verbatim, including FTS and indexes'

@contextmanager
def connect(path):
    con = sqlite3.connect(path)
    try:
        with con:
            yield con
    finally:
        con.close()


with tempfile.TemporaryDirectory(prefix='lmok-db-') as tmp:
    root = Path(tmp).resolve()
    base_env = dict(os.environ)
    base_env.pop('LMOK_DB', None)
    base_env['XDG_CACHE_HOME'] = str(root / 'cache-é-水')

    def run(*args, cwd=root, extra_env=None, code=0):
        env = dict(base_env)
        env.update(extra_env or {})
        result = subprocess.run([host, 'status', *map(str, args)], cwd=cwd,
                                env=env, capture_output=True, text=True, encoding="utf-8", timeout=15)
        assert result.returncode == code, (args, result)
        return result

    missing = root / 'missing.db'
    r = run('--db', missing, code=2)
    assert 'run `lmok index` first' in r.stderr and not missing.exists()
    assert not run('--db', root / 'no-parent' / 'x.db', code=2).stdout
    assert not (root / 'no-parent').exists()
    run('--db', root / 'no-parent' / 'x.db', '--init', code=3)
    assert not (root / 'no-parent').exists(), 'explicit paths must not create parent dirs'
    for args in [('--db',), ('--db', ''), ('--db', '--init'), ('--what',),
                 ('extra',), ('--init', '--init'), ('--db', 'a', '--db', 'b')]:
        run(*args, code=2)

    db = root / 'unicode-é-水.db'
    expected = f'db: {db.as_posix()}\nschema_version: 2\n' + ''.join(
        f'{name}: 0\n' for name in ('files', 'symbols', 'refs', 'edges', 'runs', 'findings')) + 'last_run: none\n'
    assert run('--db', db, '--init').stdout == expected
    assert run('--db', db).stdout == expected
    with connect(db) as con:
        assert con.execute('pragma journal_mode').fetchone()[0] == 'wal'
        assert dict(con.execute('select key,value from meta'))['schema_version'] == '2'
        indexes = {x[0] for x in con.execute("select name from sqlite_master where type='index'")}
        assert set(re.findall(r'CREATE INDEX (\w+)', schema)) <= indexes
        assert 'fts5' in con.execute("select sql from sqlite_master where name='file_text'").fetchone()[0]
        con.execute("insert into files values(1,'x.c','c','hash',0,10,1)")
        con.execute("insert into symbols values(1,1,'f','function',0,0,0,1,'','exact')")
        con.execute("insert into refs values(1,1,'f','call',0,0,0,1,'exact')")
        con.execute("insert into edges values(1,'x.h','include',NULL,'exact')")
        con.execute("insert into runs values(10,1,'old')")
        con.execute("insert into runs values(2,20,'new')")
        con.execute("insert into findings values(1,2,1,'X','minor','message',0,0,0,1,0,NULL)")
        con.execute("insert into file_text(path,lang,text) values('x.c','c','needle')")
        assert con.execute("select path from file_text where file_text match 'needle'").fetchall() == [('x.c',)]
    output = run('--db', db).stdout
    for name in ('files', 'symbols', 'refs', 'edges', 'findings'):
        assert f'{name}: 1\n' in output
    assert 'runs: 2\n' in output
    assert 'last_run: id=2 started_at=20 command="new"\n' in output
    assert run('--init', '--db', db).stdout == output, 'init must preserve an existing DB'
    # A concurrent writer must not block status or expose uncommitted facts.
    with connect(db) as con:
        con.execute('BEGIN IMMEDIATE')
        con.execute("insert into runs values(3,30,'uncommitted')")
        assert run('--db', db).stdout == output
        con.rollback()
    env_db = root / 'env.db'
    assert run('--db', db, extra_env={'LMOK_DB': str(env_db)}).stdout == output
    assert run(extra_env={'LMOK_DB': str(db)}).stdout == output
    assert not env_db.exists()

    with connect(db) as con:
        con.execute("update meta set value='999' where key='schema_version'")
    for args in [(), ('--init',)]:
        r = run('--db', db, *args, code=2)
        assert 'delete' in r.stderr and 're-index' in r.stderr
    with connect(db) as con:
        assert con.execute("select value from meta where key='schema_version'").fetchone()[0] == '999'
        assert con.execute('select count(*) from files').fetchone()[0] == 1
    unknown = root / 'unknown.db'
    with connect(unknown) as con:
        con.execute('create table precious(value)')
    run('--db', unknown, '--init', code=2)
    with connect(unknown) as con:
        assert not con.execute("select name from sqlite_master where name='meta'").fetchall()
    corrupt = root / 'corrupt.db'
    corrupt.write_bytes(b'not a database')
    run('--db', corrupt, code=3)

    # Nearest ancestor wins; both a Git dir and a worktree .git file qualify.
    project = root / 'project'
    child = project / 'a' / 'b'
    child.mkdir(parents=True)
    (project / '.git').mkdir()
    run(cwd=child, code=2)
    assert not (project / '.lmok').exists()
    r = run('--init', cwd=child)
    assert f'db: {(project / ".lmok/lmok.db").as_posix()}\n' in r.stdout
    (project / '.git').rmdir()
    (project / '.git').write_text('gitdir: elsewhere\n')
    assert run(cwd=child).stdout == r.stdout
    local = project / 'a' / '.lmok'
    local.mkdir()
    r = run(cwd=child, code=2)
    assert str((local / 'lmok.db').as_posix()) in r.stderr
    assert not (local / 'lmok.db').exists()
    # Outside a project, no cache directories on reads; stable SHA-256 on cwd.
    outside = root / 'outside'
    outside.mkdir()
    expected_db = root / 'cache-é-水' / 'lmok' / (hashlib.sha256(outside.as_posix().encode()).hexdigest() + '.db')
    r = run(cwd=outside, code=2)
    assert expected_db.as_posix() in r.stderr
    assert not (root / 'cache-é-水').exists()
    assert f'db: {expected_db.as_posix()}\n' in run('--init', cwd=outside).stdout
    # Empty XDG uses HOME/.cache; no mutation of the process environment.
    default_cache = root / 'home-é-水' / '.cache' / 'lmok' / expected_db.name
    r = run(cwd=outside, extra_env={'XDG_CACHE_HOME': '', 'HOME': str(root / 'home-é-水')}, code=2)
    assert default_cache.as_posix() in r.stderr and not (root / 'home-é-水').exists()
    if os.name != 'nt' and Path('/dev/full').exists():
        with open('/dev/full', 'w') as full:
            r = subprocess.run([host, 'status', '--db', str(expected_db)], stdout=full, stderr=subprocess.PIPE)
            assert r.returncode == 4
print('DB contracts: exact schema/FTS5, counts, last run, mismatch, precedence, no read creation, stdout errors')
