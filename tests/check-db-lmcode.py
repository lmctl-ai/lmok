#!/usr/bin/env python3
"""Exercise the compiled Ts DB surface and LBC111 against real indexed fixtures."""
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile

host = Path(sys.argv[1]).resolve()
work = Path(sys.argv[2]).resolve()
mode = sys.argv[3]
root = Path(__file__).resolve().parent.parent
scratch = Path(tempfile.mkdtemp(prefix='db-lmcode-', dir=work))
env = dict(os.environ, LMOK_DB=str(scratch / 'wrong.db'))

def run(args, *, cwd=root, expected=0, environ=env):
    p = subprocess.run([str(host), *map(str, args)], cwd=cwd, env=environ,
                       capture_output=True, text=True, encoding="utf-8", timeout=30)
    assert p.returncode == expected, (args, p.returncode, p.stdout, p.stderr)
    assert not p.stderr, p.stderr
    return p.stdout

def compile_source(body, name='probe'):
    source = scratch / (name + '.lms')
    source.write_text('p = _lmcode_ cli="lmok";\n' + body + '\nend _lmcode_\n', encoding='utf-8')
    return compile_file(source, name)

def compile_file(source, name):
    chunk = scratch / (name + '.dslb')
    p = subprocess.run(['node', str(root / 'scripts/compile-fixture.mjs'), str(source), str(chunk)],
                       cwd=root, capture_output=True, text=True, encoding="utf-8")
    assert p.returncode == 0 and chunk.exists(), (p.stdout, p.stderr)
    return chunk

def domain(output):
    return output.split('{"host_summary":')[0]

def frame(output):
    return json.loads(output.split('LMCODE_RETURN_BEGIN\n')[1].split('\nLMCODE_RETURN_END')[0])

db = scratch / 'index.db'
run(['index', '--db', db, 'tests/fixtures/db-rule'])
if mode == 'surface':
    cases = [('db_files', ''), ('db_files', 'c'), ('db_files', 'absent'),
             ('db_symbols', '%'), ('db_symbols', 'shared_%'), ('db_symbols', "' OR 1=1 --"),
             ('db_refs', 'ghost_fn'), ('db_refs', '%'), ('db_refs', ''), ('db_refs', "' OR 1=1 --")]
    body = '\n'.join('println(ts.' + method + '(' + json.dumps(arg) + '));' for method, arg in cases)
    chunk = compile_source(body)
    # Add strings requiring escaping to the DB, including a scoped symbol. No
    # host-produced JSON is used to construct the independent expected bytes.
    con = sqlite3.connect(db)
    con.execute('UPDATE symbols SET scope=? WHERE name=?', ('scope"\\\n雪', 'shared_helper'))
    con.execute('UPDATE files SET path=? WHERE lang=?', ('odd"\\\n雪.py', 'python'))
    con.commit()
    files_sql = "SELECT path,lang,sha256 FROM files WHERE ?='' OR lang=? ORDER BY path"
    symbols_sql = ('SELECT s.name,s.kind,f.path,s.start_row,s.start_col,s.end_row,s.end_col,s.scope '
                   'FROM symbols s JOIN files f ON f.id=s.file_id WHERE s.name LIKE ? '
                   'ORDER BY f.path,s.start_row,s.start_col,s.name,s.kind,s.id')
    refs_sql = ('SELECT r.name,r.kind,f.path,r.start_row,r.start_col FROM refs r JOIN files f ON f.id=r.file_id '
                'WHERE r.name=? ORDER BY f.path,r.start_row,r.start_col,r.kind,r.id')
    expected = []
    for method, arg in cases:
        query = {'db_files': files_sql, 'db_symbols': symbols_sql, 'db_refs': refs_sql}[method]
        cursor = con.execute(query, (arg, arg) if method == "db_files" else (arg,))
        fields = [column[0] for column in cursor.description]
        rows = [dict(zip(fields, row)) for row in cursor]
        expected.append(json.dumps(rows, ensure_ascii=False, separators=(',', ':')))
    con.close()
    output = run(['--lmcode', chunk, '--db', db])
    assert domain(output) == '\n'.join(expected) + '\n', output
    assert frame(output)['ok']
    # Explicit path beats a bad environment value; env and project default
    # yield the same bytes, and a shared invocation may query from both parts.
    assert domain(run(['--lmcode', chunk], environ=dict(env, LMOK_DB=str(db)))) == domain(output)
    project = scratch / 'project'
    (project / '.lmok').mkdir(parents=True)
    shutil.copyfile(db, project / '.lmok/lmok.db')
    assert domain(run(['--lmcode', chunk], cwd=project, environ=dict(env, LMOK_DB=''))) == domain(output)
    both = run(['--first', chunk, '--db', db, '--last', chunk])
    assert both.count(expected[0] + '\n') == 2, both
    assert frame(both)['ok']
elif mode == 'errors':
    for method in ('db_files', 'db_symbols', 'db_refs'):
        chunk = compile_source('println(ts.' + method + '(""));', method)
        missing = scratch / (method + '-missing.db')
        result = frame(run(['--lmcode', chunk, '--db', missing], expected=3))
        assert result['phase'] == 'run' and result['line'] == 2 and result['column'] > 0, result
        assert 'run `lmok index` first' in result['message'], result
        assert not missing.exists()
    for args in (['--db'], ['--db', ''], ['--db', db, '--db', db]):
        result = frame(run(['--lmcode', chunk, *args], expected=2))
        assert result['phase'] == 'cli', result
    corrupt = scratch / 'corrupt.db'
    corrupt.write_text('not SQLite')
    result = frame(run(['--lmcode', chunk, '--db', corrupt], expected=3))
    assert result['phase'] == 'run' and result['line'] == 2, result
    # DB access is lazy: a non-DB script must work without any index.
    plain = compile_source('println("no DB needed");', 'plain')
    assert domain(run(['--lmcode', plain])) == 'no DB needed\n'
elif mode == 'rule':
    source = root / 'rules/lmcode/c-repowide-undefined-call.lms'
    chunk = compile_file(source, 'rule')
    expected = ''.join(line[2:] + '\n' for line in source.read_text().splitlines() if line.startswith('#|'))
    assert expected, 'pin actual run output in rule header'
    output = run(['--lmcode', chunk, '--db', db])
    assert domain(output) == expected, output
    # Python's ghost_fn must not satisfy the C call; another C file's helper
    # does, and printf is whitelisted. Remove C definitions to prove the join.
    con = sqlite3.connect(db)
    con.execute("DELETE FROM symbols WHERE name='shared_helper'")
    con.commit()
    output = domain(run(['--lmcode', chunk, '--db', db]))
    assert '"name":"shared_helper"' in output and output.endswith('2 findings\n'), output
    # Insert a C function definition: the remaining ghost call now resolves
    # without changing/reparsing any definition source in the rule.
    con.execute("INSERT INTO symbols(file_id,name,kind,start_row,start_col,end_row,end_col) "
                "SELECT id,'ghost_fn','function',0,0,0,1 FROM files WHERE path LIKE '%/b.c'")
    con.execute("INSERT INTO symbols(file_id,name,kind,start_row,start_col,end_row,end_col) "
                "SELECT id,'shared_helper','function',0,0,0,1 FROM files WHERE path LIKE '%/b.c'")
    con.commit()
    assert domain(run(['--lmcode', chunk, '--db', db])) == '0 findings\n'
    con.close()
else:
    raise AssertionError(mode)
print('PASS DB lmcode ' + mode)
