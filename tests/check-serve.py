#!/usr/bin/env python3
"""REST contract of `lmok serve` and `lmok remote` against a live server.

usage: check-serve.py HOST [DSLB]
Every check talks HTTP to a real server process started from the repository
root; DSLB (optional) is a compiled lmcode program used by /v1/lmcode.
"""
from pathlib import Path
import base64
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
HOST = str(Path(sys.argv[1]).resolve())
DSLB = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
checks = 0


def ok(label):
    global checks
    checks += 1
    print(f'ok - serve: {label}')


def start(*extra, cwd=ROOT):
    tmp = tempfile.mkdtemp(prefix='lmok-serve-test-')
    port_file = Path(tmp) / 'port'
    proc = subprocess.Popen([HOST, 'serve', '--port', '0', '--port-file', str(port_file), *extra],
                            cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    deadline = time.time() + 20
    while time.time() < deadline:
        if port_file.exists() and port_file.read_text().strip():
            return proc, f'http://127.0.0.1:{port_file.read_text().strip()}', Path(tmp)
        if proc.poll() is not None:
            raise SystemExit(f'server exited early: {proc.communicate()}')
        time.sleep(0.05)
    proc.kill()
    raise SystemExit('server did not publish its port')


def call(base, method, path, body=None, raw=None, expect=200):
    data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
    req = urllib.request.Request(base + path, data=data, method=method)
    if data is not None:
        req.add_header('Content-Type', 'application/json')
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            status, payload, headers = resp.status, resp.read(), resp.headers
    except urllib.error.HTTPError as e:
        status, payload, headers = e.code, e.read(), e.headers
    assert status == expect, (method, path, status, payload[:500])
    if headers.get('Content-Type', '').startswith('application/json'):
        return json.loads(payload)
    return payload


def remote(base, *args, stdin=None, env_extra=None, cwd=ROOT):
    env = dict(os.environ, LMOK_SERVER=base)
    env.update(env_extra or {})
    return subprocess.run([HOST, 'remote', *args], cwd=cwd, env=env, input=stdin, capture_output=True, timeout=120)


def local(*args, stdin=None, cwd=ROOT):
    return subprocess.run([HOST, *args], cwd=cwd, input=stdin, capture_output=True, timeout=120)


proc, base, tmp = start('--idle-timeout', '120')
try:
    # --- discovery: every button and selection is reachable from GET / ---
    index = call(base, 'GET', '/')
    assert index['name'] == 'lmok' and index['api'] == 'v1'
    paths = {(b['method'], b['path']) for b in index['buttons']}
    for needed in [('POST', '/v1/run'), ('POST', '/v1/shutdown'), ('PATCH', '/v1/settings'), ('POST', '/v1/presets/{name}'),
                   ('GET', '/v1/results/{id}'), ('DELETE', '/v1/cache'), ('POST', '/v1/lmcode')]:
        assert needed in paths, needed
    for sel in index['selections'].values():
        call(base, 'GET', sel)
    ok('GET / lists buttons and every selection list answers')

    health = call(base, 'GET', '/v1/health')
    assert health['ok'] and health['preset'] == 'default' and health['idle_timeout_s'] == 120 and 0 < health['idle_remaining_ms'] <= 120000
    langs = call(base, 'GET', '/v1/languages')['languages']
    assert 'json' in langs and 'tsx' in langs and len(langs) == 11
    packs = call(base, 'GET', '/v1/packs')['packs']
    assert 'rules/json-tier1.json' in packs and 'rules/c-tier1.json' in packs
    assert set(call(base, 'GET', '/v1/commands')) >= {'run', 'parse', 'lint', 'lmcode'}
    ok('health, languages, packs and command catalog')

    # --- presets and settings ---
    presets = call(base, 'GET', '/v1/presets')
    names = [p['name'] for p in presets['presets']]
    assert presets['current'] == 'default' and {'default', 'interactive', 'ci', 'fresh', 'memo', 'json', 'c'} <= set(names)
    view = call(base, 'POST', '/v1/presets/json')
    assert view['preset'] == 'json' and view['settings']['lang'] == 'json' and view['settings']['pack'] == 'rules/json-tier1.json'
    assert call(base, 'GET', '/v1/presets/c')['settings']['metadata'] is True
    call(base, 'POST', '/v1/presets/nope', expect=404)
    ok('presets list, show and apply; unknown preset is 404')

    view = call(base, 'PATCH', '/v1/settings', {'results_max': 5, 'lang': 'c'})
    assert view['settings']['results_max'] == 5 and view['settings']['lang'] == 'c' and view['settings']['pack'] == 'rules/json-tier1.json'
    for bad in [{'bogus': 1}, {'cache': 'yes'}, {'results_max': -1}, {'cwd': str(tmp / 'missing')}, {'idle_timeout_s': 1.5}]:
        assert call(base, 'PATCH', '/v1/settings', bad, expect=422)['error']['code'] in ('unknown_setting', 'invalid_setting')
    assert call(base, 'GET', '/v1/settings')['settings']['results_max'] == 5, 'rejected patch must not apply partially'
    view = call(base, 'PUT', '/v1/settings', {'cache': False})
    assert view['settings']['lang'] == '' and view['settings']['cache'] is False and view['preset'] == ''
    view = call(base, 'POST', '/v1/settings/reset')
    assert view['preset'] == 'default' and view['settings']['idle_timeout_s'] == 120 and view['settings']['cache'] is True
    schema = call(base, 'GET', '/v1/settings/schema')['settings']
    assert {s['name'] for s in schema} == set(view['settings'])
    ok('settings PATCH/PUT/reset with typed validation; startup idle override survives reset')

    # --- command buttons give the same bytes as the CLI ---
    sample = 'examples/data/sample.json'
    r = call(base, 'POST', '/v1/parse', {'file': sample})
    assert r['ok'] and r['stdout'].encode() == local('parse', sample).stdout and r['cached'] is False
    r2 = call(base, 'POST', '/v1/parse', {'file': sample})
    assert r2['cached'] is True and r2['key'] == r['key'] and r2['stdout'] == r['stdout'] and r2['id'] != r['id']
    q = call(base, 'POST', '/v1/query', {'pattern': '(pair key: (string (string_content) @key))', 'file': sample})
    assert q['stdout'].encode() == local('query', '(pair key: (string (string_content) @key))', sample).stdout
    t = call(base, 'POST', '/v1/parse', {'text': '{"a": [1, 2]}', 'lang': 'json'})
    assert t['ok'] and t['stdout'].startswith('(document (object')
    lint = call(base, 'POST', '/v1/lint', {'file': 'tests/fixtures/go-tier1-violations.go', 'pack': 'rules/go-tier1.json'})
    assert lint['stdout'].encode() == local('lint', 'rules/go-tier1.json', 'tests/fixtures/go-tier1-violations.go').stdout
    assert lint['stdout'].count('"rule":') == 7
    for verb, args in [('facts', ['facts', 'examples/data/sample.c']), ('metrics', ['metrics', 'examples/data/sample.c'])]:
        r = call(base, 'POST', f'/v1/{verb}', {'file': 'examples/data/sample.c'})
        assert r['stdout'].encode() == local(*args).stdout, verb
    d = call(base, 'POST', '/v1/dup', {'file': 'examples/data/sample.c', 'tokens': 20})
    assert d['stdout'].encode() == local('dup', '--tokens', '20', 'examples/data/sample.c').stdout
    o = call(base, 'POST', '/v1/outline', {'files': ['examples/data/sample.c'], 'budget': 400})
    assert o['stdout'].encode() == local('outline', 'examples/data/sample.c', '--budget', '400').stdout
    rules = call(base, 'POST', '/v1/rules', {'packs': ['rules/json-tier1.json']})
    assert rules['ok'] and rules['stdout'].encode() == local('rules', 'rules/json-tier1.json').stdout
    ok('parse/query/text/lint/facts/metrics/dup/outline/rules match the CLI byte for byte')

    # --- preset defaults feed the buttons ---
    call(base, 'POST', '/v1/presets/json')
    r = call(base, 'POST', '/v1/lint', {'file': 'examples/data/bad-config.json'})
    assert r['command'][:2] == ['lint', 'rules/json-tier1.json'], r['command']
    r = call(base, 'POST', '/v1/parse', {'text': '[true]'})
    assert r['command'][1:3] == ['--lang', 'json'] and r['ok']
    call(base, 'POST', '/v1/presets/default')
    assert call(base, 'POST', '/v1/lint', {'file': sample}, expect=400)['error']['code'] == 'bad_request'
    assert call(base, 'POST', '/v1/parse', {'text': '[]'}, expect=400)['error']['message'].startswith('"text" needs')
    assert call(base, 'POST', '/v1/parse', {'file': sample, 'lang': 'rust'}, expect=422)['error']['code'] == 'unknown_language'
    ok('preset lang/pack defaults drive buttons; missing selections are explained')

    # --- the cache follows input bytes ---
    work = tmp / 'work'
    work.mkdir()
    (work / 'doc.json').write_text('{"a": 1}')
    first = call(base, 'POST', '/v1/run', {'args': ['parse', 'doc.json'], 'cwd': str(work)})
    again = call(base, 'POST', '/v1/run', {'args': ['parse', 'doc.json'], 'cwd': str(work)})
    assert not first['cached'] and again['cached']
    (work / 'doc.json').write_text('{"a": [1]}')
    changed = call(base, 'POST', '/v1/run', {'args': ['parse', 'doc.json'], 'cwd': str(work)})
    assert not changed['cached'] and changed['key'] != first['key'] and 'array' in changed['stdout']
    nocache = call(base, 'POST', '/v1/run', {'args': ['parse', 'doc.json'], 'cwd': str(work), 'cache': False})
    assert not nocache['cached']
    dirrun = call(base, 'POST', '/v1/run', {'args': ['metrics', '--lang', 'json', '.'], 'cwd': str(work)})
    assert call(base, 'POST', '/v1/run', {'args': ['metrics', '--lang', 'json', '.'], 'cwd': str(work)})['cached']
    (work / 'more.json').write_text('[]')
    assert not call(base, 'POST', '/v1/run', {'args': ['metrics', '--lang', 'json', '.'], 'cwd': str(work)})['cached'], dirrun
    side = call(base, 'POST', '/v1/run', {'args': ['index', '--db', str(work / 'x.db'), str(work)]})
    assert side['cacheable'] is False and side['ok'], side
    cache = call(base, 'GET', '/v1/cache')
    assert cache['hits'] >= 3 and cache['entries'] == len(cache['keys']) > 0
    call(base, 'DELETE', f"/v1/cache/{first['key']}")
    call(base, 'DELETE', '/v1/cache/' + '0' * 64, expect=404)
    assert call(base, 'DELETE', '/v1/cache')['entries'] == 0
    call(base, 'PATCH', '/v1/settings', {'cache': False})
    assert not call(base, 'POST', '/v1/parse', {'file': sample})['cached']
    assert not call(base, 'POST', '/v1/parse', {'file': sample})['cached']
    call(base, 'PATCH', '/v1/settings', {'cache': True, 'cache_max_entries': 1})
    call(base, 'POST', '/v1/parse', {'file': sample})
    call(base, 'POST', '/v1/parse', {'file': 'examples/data/sample.c'})
    assert call(base, 'GET', '/v1/cache')['entries'] == 1
    call(base, 'POST', '/v1/settings/reset')
    ok('cache hits, input-byte and directory invalidation, side-effect commands bypass, LRU bound, clear')

    # --- results are kept for later and can feed the next command ---
    first = call(base, 'POST', '/v1/run', {'args': ['--lmcode-schema', '--json']})
    got = call(base, 'GET', f"/v1/results/{first['id']}")
    assert got['stdout'] == first['stdout']
    raw = call(base, 'GET', f"/v1/results/{first['id']}/stdout")
    assert raw == first['stdout'].encode()
    chained = call(base, 'POST', '/v1/run', {'args': ['parse', '--lang', 'json', '-'], 'stdin_from': first['id']})
    assert chained['ok'] and chained['stdout'].startswith('(document (object'), chained
    listing = call(base, 'GET', '/v1/results')['results']
    assert listing[0]['id'] == chained['id'] and 'stdout' not in listing[0]
    call(base, 'PATCH', '/v1/settings', {'results_max': 2})
    assert [r['id'] for r in call(base, 'GET', '/v1/results')['results']] == [chained['id'], first['id']]
    call(base, 'POST', '/v1/run', {'args': ['--version']})
    call(base, 'GET', f"/v1/results/{first['id']}", expect=404)
    call(base, 'DELETE', f"/v1/results/{chained['id']}")
    call(base, 'GET', f"/v1/results/{chained['id']}", expect=404)
    call(base, 'DELETE', '/v1/results')
    assert call(base, 'GET', '/v1/results')['results'] == []
    call(base, 'POST', '/v1/settings/reset')
    ok('results history, raw stdout, stdin_from chaining, results_max trimming and deletion')

    # --- uploads and placeholders ---
    up = call(base, 'POST', '/v1/run', {'args': ['parse', '--lang', 'python', '@{x.py}'], 'files': {'x.py': {'text': 'def f():\n    return 1\n'}}})
    assert up['ok'] and up['stdout'].startswith('(module (function_definition'), up
    b64 = call(base, 'POST', '/v1/run', {'args': ['parse', '@{y.json}'], 'files': {'y.json': {'base64': base64.b64encode(b'[1]').decode()}}})
    assert b64['ok'] and 'array' in b64['stdout']
    call(base, 'POST', '/v1/run', {'args': ['parse', '@{a}'], 'files': {'../evil': 'x'}}, expect=400)
    ok('inline uploads by text or base64 with @{name} placeholders; path escapes refused')

    # --- failures keep the CLI exit codes; protocol errors are JSON ---
    bad = call(base, 'POST', '/v1/run', {'args': ['parse', 'no/such/file.json']})
    want = local('parse', 'no/such/file.json')
    assert bad['exit_code'] == want.returncode != 0 and bad['stderr'].encode() == want.stderr and not bad['ok']
    usage = call(base, 'POST', '/v1/run', {'args': ['frobnicate']})
    assert usage['exit_code'] == 2 and 'usage: lmok' in usage['stderr']
    assert call(base, 'POST', '/v1/run', {'args': ['serve']}, expect=422)['error']['code'] == 'not_allowed'
    assert call(base, 'POST', '/v1/run', {'args': []}, expect=400)['error']['code'] == 'bad_request'
    assert call(base, 'POST', '/v1/run', raw=b'{not json', expect=400)['error']['code'] == 'invalid_json'
    assert call(base, 'GET', '/v1/nowhere', expect=404)['error']['code'] == 'not_found'
    assert call(base, 'GET', '/v2', expect=404)['error']['code'] == 'not_found'
    assert call(base, 'GET', '/v1/run', expect=405)['error']['message'] == 'allowed: POST'
    call(base, 'PATCH', '/v1/settings', {'max_body_bytes': 2048})
    call(base, 'POST', '/v1/run', raw=b'{"args":["--version"],"pad":"' + b'x' * 4096 + b'"}', expect=413)
    call(base, 'POST', '/v1/settings/reset')
    ok('command failures keep exit codes and stderr; 400/404/405/413/422 are JSON errors')

    # --- lmok remote replays a command exactly ---
    for args in (['parse', sample], ['query', '(pair (string))', sample], ['lint', 'rules/go-tier1.json', 'tests/fixtures/go-tier1-violations.go'],
                 ['parse', 'no/such.json'], ['frobnicate'], ['--version']):
        a, b = remote(base, *args), local(*args)
        assert (a.returncode, a.stdout, a.stderr) == (b.returncode, b.stdout, b.stderr), (args, a, b)
    piped = remote(base, 'parse', '--lang', 'json', '-', stdin=b'{"k": null}')
    assert piped.returncode == 0 and piped.stdout == local('parse', '--lang', 'json', '-', stdin=b'{"k": null}').stdout
    elsewhere = remote(base, 'parse', 'doc.json', cwd=work)
    assert elsewhere.returncode == 0 and b'array' in elsewhere.stdout, elsewhere
    js = remote(base, '--json', '--no-cache', '--version')
    assert json.loads(js.stdout)['cached'] is False
    api = remote(base, '--api', 'GET', '/v1/health')
    assert api.returncode == 0 and json.loads(api.stdout)['ok']
    assert remote(base, '--api', 'GET', '/v1/missing').returncode == 1
    down = subprocess.run([HOST, 'remote', '--server', 'http://127.0.0.1:9', '--version'], capture_output=True, timeout=30)
    assert down.returncode == 2 and b'cannot connect' in down.stderr
    ok('lmok remote replays stdout/stderr/exit, forwards stdin and cwd, --json, --api, unreachable server')

    # --- compiled lmcode programs ---
    if DSLB:
        want = local('--lmcode', str(DSLB))
        prog = base64.b64encode(DSLB.read_bytes()).decode()
        r = call(base, 'POST', '/v1/lmcode', {'program_base64': prog})
        assert (r['exit_code'], r['stdout'].encode()) == (want.returncode, want.stdout), (r, want)
        assert not r['cacheable']
        two = call(base, 'POST', '/v1/lmcode', {'programs': [{'base64': prog}, {'base64': prog, 'order': 'first'}]})
        assert two['command'][0] == '--lmcode' and two['command'][2] == '--first' and two['ok'], two
        call(base, 'POST', '/v1/presets/memo')
        assert not call(base, 'POST', '/v1/lmcode', {'program_base64': prog})['cached']
        assert call(base, 'POST', '/v1/lmcode', {'program_base64': prog})['cached']
        call(base, 'POST', '/v1/settings/reset')
        rem = remote(base, '--lmcode', str(DSLB))
        assert (rem.returncode, rem.stdout) == (want.returncode, want.stdout)
        call(base, 'POST', '/v1/lmcode', {'program_base64': 'not base64!'}, expect=400)
        ok('lmcode bytecode runs by upload, multi-part order, memo preset caching, remote --lmcode')

    # --- shutdown ---
    assert call(base, 'POST', '/v1/shutdown')['stopping']
    out, err = proc.communicate(timeout=20)
    assert proc.returncode == 0 and b'stopped (shutdown requested)' in out, (out, err)
    ok('POST /v1/shutdown stops the server with exit 0')
finally:
    if proc.poll() is None:
        proc.kill()

# --- idle timeout and startup options ---
proc, base, _ = start('--idle-timeout', '1', '--preset', 'ci')
t0 = time.time()
assert call(base, 'GET', '/v1/health')['preset'] == 'ci'
out, err = proc.communicate(timeout=30)
assert proc.returncode == 0 and b'stopped (idle timeout)' in out and 0.8 < time.time() - t0 < 10, (out, err)
ok('idle timeout stops a quiet server')

presets_file = Path(tempfile.mkdtemp(prefix='lmok-presets-')) / 'presets.json'
presets_file.write_text(json.dumps({'presets': [{'name': 'team', 'description': 'ours', 'settings': {'lang': 'go', 'idle_timeout_s': 30}}]}))
proc, base, _ = start('--presets', str(presets_file), '--preset', 'team')
try:
    view = call(base, 'GET', '/v1/settings')
    assert view['preset'] == 'team' and view['settings']['lang'] == 'go' and view['settings']['idle_timeout_s'] == 30
    assert 'team' in [p['name'] for p in call(base, 'GET', '/v1/presets')['presets']]
    call(base, 'POST', '/v1/shutdown')
    proc.communicate(timeout=20)
finally:
    if proc.poll() is None:
        proc.kill()
listing = local('serve', '--presets', str(presets_file), '--list-presets')
assert listing.returncode == 0 and b'team\tours' in listing.stdout and b'interactive\t' in listing.stdout
bad_file = presets_file.with_name('bad.json')
bad_file.write_text(json.dumps({'presets': [{'name': 'x', 'settings': {'lang': 3}}]}))
assert local('serve', '--presets', str(bad_file)).returncode == 2
assert local('serve', '--preset', 'nope', '--port', '0').returncode == 2
assert local('serve', '--port', 'abc').returncode == 2
ok('--presets file, --list-presets and startup option validation')

print(f'PASS: {checks} serve checks')
