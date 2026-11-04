#!/usr/bin/env python3
from contextlib import closing
import json
import os
import pathlib
import sqlite3
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
if len(sys.argv) > 1:
    HOST = pathlib.Path(sys.argv[1]).resolve()
else:
    HOST = pathlib.Path(os.environ.get('LMOK_HOST', ROOT / 'build' / 'lmok')).resolve()

def run(cwd, *args):
    return subprocess.run([str(HOST), *map(str, args)], cwd=cwd,
                          capture_output=True, text=True, encoding="utf-8", timeout=30)

with tempfile.TemporaryDirectory(prefix='review-baseline-', dir=ROOT / 'build') as td:
    work = pathlib.Path(td)
    pack = work / 'one.json'
    pack.write_text(json.dumps({'lang': 'c', 'rules': [
        {'id': 'N', 'on': 'number_literal', 'message': 'number', 'severity': 'major'}]}), encoding='utf-8')
    src = work / 'supp.c'
    src.write_text('int a=1; // lmok-disable:N\n\nint b=2;\n', encoding='utf-8')
    base = run(work, 'lint', pack, src)
    assert base.returncode == 0, base
    (work / 'base.txt').write_text(base.stdout, encoding='utf-8')
    repeat = run(work, 'lint', pack, src, '--baseline', work / 'base.txt', '--fail-on-new')
    assert repeat.returncode == 0, (repeat.returncode, repeat.stdout, repeat.stderr)
    assert '0 new, 1 baseline' in repeat.stdout, repeat.stdout

    # New output carries an unambiguous JSON filename when a human footer
    # could be mistaken for suppression metadata or split across lines.
    for name in ['source (2 suppressed)'] + ([] if os.name == 'nt' else ['line\nbreak.c']):
        odd = work / name
        odd.write_text('int a=1;\n', encoding='utf-8')
        saved = run(work, 'lint', pack, odd)
        baseline = work / 'odd.txt'
        baseline.write_text(saved.stdout, encoding='utf-8')
        again = run(work, 'lint', pack, odd, '--baseline', baseline, '--fail-on-new')
        assert again.returncode == 0, (again.returncode, again.stdout, again.stderr)
        assert json.loads(saved.stdout.splitlines()[0])['file'] == odd.as_posix()

    dup = work / 'dup.c'
    dup.write_text('int a=1;\n', encoding='utf-8')
    db = work / 'history.db'
    for _ in range(2):
        recorded = run(work, 'lint', pack, dup, '--db', db)
        assert recorded.returncode == 0, recorded
    dup.write_text('int a=1; int b=1;\n', encoding='utf-8')
    fresh = run(work, 'lint', pack, dup, '--db', db, '--new-since', 'last', '--fail-on-new')
    assert fresh.returncode == 4, (fresh.returncode, fresh.stdout, fresh.stderr)
    assert '1 new, 1 baseline' in fresh.stdout, fresh.stdout

    # A clean scan is still an observation: after recording no findings, a
    # reintroduced violation must be new rather than inheriting an older row.
    clean = work / 'clean.c'
    clean.write_text('int a=1;\n', encoding='utf-8')
    clean_db = work / 'clean.db'
    assert run(work, 'lint', pack, clean, '--db', clean_db).returncode == 0
    clean.write_text('int a;\n', encoding='utf-8')
    assert run(work, 'lint', pack, clean, '--db', clean_db).returncode == 0
    clean.write_text('int a=1;\n', encoding='utf-8')
    reintroduced = run(work, 'lint', pack, clean, '--db', clean_db,
                       '--new-since', 'last', '--fail-on-new')
    assert reintroduced.returncode == 4, (reintroduced.returncode, reintroduced.stdout)
    assert '1 new, 0 baseline' in reintroduced.stdout, reintroduced.stdout

    # Rule-pack scope is part of the baseline identity. A different pack with
    # the same rule id/message must not consume the first pack's observation.
    pack_b = work / 'other.json'
    pack_b.write_text(json.dumps({'lang': 'c', 'rules': [
        {'id': 'N', 'on': 'number_literal', 'message': 'number', 'severity': 'major'}]}), encoding='utf-8')
    scoped = run(work, 'lint', pack, clean, '--db', clean_db)
    assert scoped.returncode == 0, scoped
    other = run(work, 'lint', pack_b, clean, '--db', clean_db,
                '--new-since', 'last', '--fail-on-new')
    assert other.returncode == 4, (other.returncode, other.stdout)
    assert '1 new, 0 baseline' in other.stdout, other.stdout

    # Partial scans leave unvisited files' observations available.
    partial_db = work / 'partial.db'
    left, right = work / 'left.c', work / 'right.c'
    left.write_text('int a=1;\n', encoding='utf-8')
    right.write_text('int b=1;\n', encoding='utf-8')
    assert run(work, 'lint', pack, left, right, '--db', partial_db).returncode == 0
    left.write_text('int a;\n', encoding='utf-8')
    assert run(work, 'lint', pack, left, '--db', partial_db).returncode == 0
    partial = run(work, 'lint', pack, left, right, '--db', partial_db,
                  '--new-since', 'last', '--fail-on-new')
    assert partial.returncode == 0, (partial.stdout, partial.stderr)
    assert '0 new, 1 baseline' in partial.stdout, partial.stdout
    historical = run(work, 'lint', pack, right, '--db', partial_db,
                     '--new-since', '1', '--fail-on-new')
    assert historical.returncode == 0, historical.stdout
    # Legacy DBs have no clean-scan/scope history. Never guess allowances.
    with closing(sqlite3.connect(partial_db)) as con, con:
        con.execute('drop table run_files')
    legacy = run(work, 'lint', pack, right, '--db', partial_db,
                 '--new-since', 'last', '--fail-on-new')
    assert legacy.returncode == 4, (legacy.stdout, legacy.stderr)

    secret = work / 'secret.c'
    secret.write_text('const char *key="' + 'AK' + 'IA' + 'A1B2C3D4E5F6G7H8' + '";\n', encoding='utf-8')
    secret_base = run(work, 'lint', ROOT / 'rules' / 'secrets-c.json', secret)
    assert secret_base.returncode == 0, secret_base
    # Simulate a legacy JSON-lines baseline that predates safe snippet identity.
    legacy_lines = []
    for line in secret_base.stdout.splitlines():
        if line.startswith('{'):
            item = json.loads(line)
            item.pop('snippet', None)
            line = json.dumps(item, separators=(',', ':'))
        legacy_lines.append(line)
    (work / 'secret-base.txt').write_text('\n'.join(legacy_lines) + '\n', encoding='utf-8')
    secret_again = run(work, 'lint', ROOT / 'rules' / 'secrets-c.json', secret,
                       '--baseline', work / 'secret-base.txt', '--fail-on-new')
    assert secret_again.returncode == 4, (secret_again.returncode, secret_again.stdout)

    repo = work / 'repo'
    repo.mkdir()
    subprocess.run(['git', 'init', '-q', str(repo)], check=True)
    subprocess.run(['git', '-C', str(repo), 'config', 'user.email', 'review@example.test'], check=True)
    subprocess.run(['git', '-C', str(repo), 'config', 'user.name', 'Review'], check=True)
    # Git's non-NUL quoting is ambiguous for tabs/newlines on POSIX. Windows
    # cannot create those names portably, so use a Unicode name there.
    quoted = repo / ('a\tb.c' if os.name != 'nt' else 'a雪b.c')
    quoted.write_text('int a=1;\n', encoding='utf-8')
    subprocess.run(['git', '-C', str(repo), 'add', '.'], check=True)
    subprocess.run(['git', '-C', str(repo), 'commit', '-qm', 'fixture'], check=True)
    quoted.write_text('int a=2;\n', encoding='utf-8')
    plain = run(repo, 'lint', pack, '.', '--changed')
    assert plain.returncode == 0, (plain.returncode, plain.stdout, plain.stderr)
    assert 'findings in' in plain.stdout, plain.stdout

print('review baseline regressions: pass')
