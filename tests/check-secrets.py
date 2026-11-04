#!/usr/bin/env python3
"""Secrets are generated at runtime so the repository itself stays clean."""
import json
import hashlib
import random
import sqlite3
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOST = Path(sys.argv[1]).resolve()

def run(*args):
    return subprocess.run([str(HOST), *map(str, args)], capture_output=True, timeout=60)

def findings(pack, source, *args):
    r = run('lint', pack, source, *args)
    assert r.returncode == 0, (r.stdout, r.stderr)
    return [json.loads(line) for line in r.stdout.splitlines() if line.startswith(b'{')], r

def reject(actual, expected):
    try:
        assert actual == expected
    except AssertionError:
        return
    raise AssertionError('failing control did not fail')

rng = random.Random(191011)
# Assemble provider prefixes: no real-shaped credentials are committed.
samples = [
    ('AWS', 'AK' + 'IA' + 'A1B2C3D4E5F6G7H8'),
    ('GITHUB', 'gh' + 'p_' + 'aB9' * 12),
    ('GITHUB', 'github' + '_pat_' + 'aB1_' * 20 + 'Ab'),
    ('SLACK', 'xo' + 'xb-' + '12345678901-12345678901-' + 'Ab9' * 10),
    ('PRIVATE_KEY', '-----BEGIN ' + 'RSA PRIVATE KEY-----'),
    ('JWT', 'ey' + 'JhbGciOiJIUzI1NiJ9.eyJzdWIiOiJmYWtlIn0.' + 'abCD09_-' * 4),
    ('ENTROPY', ''.join(rng.sample('abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789', 40))),
]
# Controls precede the real oracles: corrupted expected counts and leaked bytes are rejected.


with tempfile.TemporaryDirectory(prefix='secrets os ', dir=ROOT / 'build') as tmp:
    tmp = Path(tmp)
    for lang, ext in [('json','json'), ('python','py'), ('c','c'), ('bash','sh')]:
        pack = ROOT / 'rules' / ('secrets-' + lang + '.json')
        source = tmp / ('fake 雪.' + ext)
        def write(values):
            if lang == 'json': text = json.dumps(dict(values))
            elif lang == 'c': text = '\n'.join('const char *' + k + ' = ' + json.dumps(v) + ';' for k,v in values)
            elif lang == 'bash': text = '\n'.join(k + "='" + v + "'" for k,v in values)
            else: text = '\n'.join(k + ' = ' + json.dumps(v) for k,v in values)
            source.write_bytes((text + '\r\n').encode('utf-8'))
        write([('long_credential_assignment_password' if name == 'ENTROPY' else 'value'+str(i), value) for i,(name,value) in enumerate(samples)])
        if lang == 'json':
            # Execute broken packs before the positive oracle: omission and raw-text
            # regressions must be rejected by the same assertions used below.
            data = json.loads(pack.read_text(encoding='utf-8'))
            bad = tmp / 'mutant.json'
            data['rules'] = data['rules'][1:]
            bad.write_text(json.dumps(data), encoding='utf-8')
            reject(len(findings(bad, source)[0]), len(samples))
            data = json.loads(pack.read_text(encoding='utf-8'))
            for rule in data['rules']: rule['redact'] = False
            bad.write_text(json.dumps(data), encoding='utf-8')
            raw = findings(bad, source)[0][0]['message']
            reject(raw, 'AWS secret ' + samples[0][1][:4] + '[REDACTED]')
            print('ok - executed failing controls: omitted detector and disabled redaction')
        db = tmp / (lang + '.db')
        sarif = tmp/(lang+'.sarif')
        fs, result = findings(pack, source, '--db', db, '--sarif', sarif)
        assert len(fs) == len(samples), (lang, fs, result.stderr)
        for (name, secret), finding in zip(samples, fs):
            assert finding['rule'].startswith('SEC_' + name), (lang, finding)
            assert finding['message'].endswith(secret[:4] + '[REDACTED]'), finding
            assert secret.encode() not in result.stdout + result.stderr, (lang, finding)
        con = sqlite3.connect(db)
        try: stored = con.execute('select message, snippet from findings order by id').fetchall()
        finally: con.close()
        assert [x[0] for x in stored] == [f['message'] for f in fs]
        for (_,secret), (_,snippet) in zip(samples,stored):
            raw = "'"+secret+"'" if lang=='bash' else secret
            assert snippet == 'sha256:'+hashlib.sha256(raw.encode()).hexdigest(), (lang,'raw secret persisted in baseline snippet')
        for recorded in tmp.glob(lang+'.db*'):
            blob=recorded.read_bytes()
            for _,secret in samples: assert secret.encode() not in blob, 'plaintext in SQLite/WAL'
        sarif_bytes=sarif.read_bytes()
        for _,secret in samples: assert secret.encode() not in sarif_bytes
        assert [r['message']['text'] for r in json.loads(sarif_bytes)['runs'][0]['results']] == [f['message'] for f in fs]
        # The digest preserves baseline identity when a secret changes but its
        # visible four-character prefix remains the same.
        if lang=='json':
            baseline = run('lint',pack,source,'--db',db,'--new-since','last')
            assert baseline.returncode==0 and not any(line.startswith(b'{') for line in baseline.stdout.splitlines()), baseline
            rotated=samples[0][1][:-1]+'9'
            text=source.read_text(encoding='utf-8').replace(samples[0][1],rotated)
            source.write_bytes(text.encode())
            changed=run('lint',pack,source,'--db',db,'--new-since','last')
            fs_changed=[json.loads(line) for line in changed.stdout.splitlines() if line.startswith(b'{')]
            assert rotated.encode() not in changed.stdout+changed.stderr
            assert changed.returncode==0 and len(fs_changed)==1 and fs_changed[0]['rule']=='SEC_AWS',changed
            write([('long_credential_assignment_password' if name == 'ENTROPY' else 'value'+str(i), value) for i,(name,value) in enumerate(samples)])
        # Random ordinary strings are not credentials; entropy needs a sensitive assignment key.
        write([('value'+str(i), ''.join(rng.choices('abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789', k=48))) for i in range(1000)])
        fs, _ = findings(pack, source)
        assert fs == [], (lang, fs)
        # Low-entropy values under sensitive keys must not fire.
        write([('password', 'a' * 40), ('value1', 'github' + '_pat_x'), ('value2', 'gh' + 'p_' + 'a' * 35)])
        assert findings(pack, source)[0] == []
        if lang == 'c':
            source.write_text('void set(void){ const char *password; password = '+json.dumps(samples[-1][1])+'; }', encoding='utf-8')
            fs, result = findings(pack, source)
            assert len(fs)==1 and fs[0]['rule']=='SEC_ENTROPY',fs
            assert samples[-1][1].encode() not in result.stdout
        print('ok -', lang, 'provider shapes, redaction/DB, 1000 random strings, low entropy')
    # Predicate arithmetic, strict comparison, arity/domain and bound.
    source = tmp / 'entropy.json'
    source.write_bytes(b'["abab", "aaaaaaaa", ""]')
    pattern = '((string_content) @s (#entropy-gt? @s "0.9"))'
    good = run('query', pattern, source)
    assert good.returncode == 0 and len(good.stdout.splitlines()) == 1, good
    assert run('query', pattern.replace('0.9', '1'), source).stdout == b''
    for value in ['nan', '-1', '8.1', '1x']:
        assert run('query', pattern.replace('0.9', value), source).returncode == 3
    source.write_text(json.dumps(['ab' * 9000]), encoding='utf-8')
    assert run('query', pattern, source).stdout == b''
    assert run('query', '((string_content) @s (#entropy-gt? @s))', source).returncode == 3
    bad = tmp/'unknown-capture.json'
    bad.write_text(json.dumps(dict(lang='json',rules=[dict(id='BAD',query='(number) @value',capture='typo',message='bad')])), encoding='utf-8')
    assert run('lint',bad,source).returncode == 3  # no numbers match; typo still fails
    bad.write_text(json.dumps(dict(lang='json',rules=[dict(id='BAD',on='string_content',redact='true',message='bad')])), encoding='utf-8')
    assert run('lint',bad,source).returncode == 2
    print('ok - entropy strict threshold/domain/arity/bound, capture typo and redact type')
    # Scan only tracked sources (generated build/npm/vendor trees are not the repository).
    paths = subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT).split(b'\0')
    for lang, extensions in [('json',{'.json'}),('python',{'.py'}),('c',{'.c','.h'}),('bash',{'.sh','.bash'})]:
        files = [ROOT / p.decode() for p in paths if p and Path(p.decode()).suffix in extensions]
        fs, r = findings(ROOT / 'rules' / ('secrets-'+lang+'.json'), files[0])
        assert fs == []
        for file in files[1:]:
            assert findings(ROOT / 'rules' / ('secrets-'+lang+'.json'), file)[0] == [], file
    print('ok - zero findings in tracked lmok sources')
