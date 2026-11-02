#!/usr/bin/env python3
"""An interactive triage session against one `lmok serve` process.

usage: check-serve-triage.py HOST

A client starts small and widens step by step, each request building on the
previous answers held by the server:

  1. scan one file           lint names.c                  -> LBC001 strcpy, row 5
  2. locate the warning      facts names.c                 -> inside copy_name()
  3. deep search the folder  facts .  (every related file)  -> callers in main.c, report.c;
                                                              prototype in names.h; unrelated.c no
  4. refine on the callers   query #eq? copy_name, per file -> main.c passes argv[1] (confirmed),
                                                              report.c a literal (benign)
  5. widen the lint          lint the callers               -> no new warnings
  6. revisit                 repeat step 1 and 3            -> served from the cache
  7. fix and re-verify       edit names.c, re-run 1 and 3   -> warning gone, cache invalidated

The fixture is copied to a scratch directory so step 7 can edit it; the
session points the server at that directory with PATCH /v1/settings.
"""
from pathlib import Path
import json
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
HOST = str(Path(sys.argv[1]).resolve())
FIXTURE = ROOT / 'tests/fixtures/serve-triage'
checks = 0


def ok(label):
    global checks
    checks += 1
    print(f'ok - triage: {label}')


def call(base, method, path, body=None, expect=200):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(base + path, data=data, method=method, headers={'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            status, payload = resp.status, resp.read()
    except urllib.error.HTTPError as e:
        status, payload = e.code, e.read()
    assert status == expect, (method, path, status, payload[:500])
    return json.loads(payload)


def records(result):
    """JSON-lines stdout of a command result (summary lines are skipped)."""
    assert result['ok'], result
    return [json.loads(line) for line in result['stdout'].splitlines() if line.startswith('{')]


def findings(result):
    return [r for r in records(result) if 'rule' in r]


work = Path(tempfile.mkdtemp(prefix='lmok-triage-'))
project = work / 'project'
shutil.copytree(FIXTURE, project)
port_file = work / 'port'
server = subprocess.Popen([HOST, 'serve', '--port', '0', '--port-file', str(port_file), '--preset', 'c'],
                          cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
try:
    deadline = time.time() + 20
    while not (port_file.exists() and port_file.read_text().strip()):
        if time.time() > deadline or server.poll() is not None:
            raise SystemExit(f'server did not start: {server.poll()}')
        time.sleep(0.05)
    base = f'http://127.0.0.1:{port_file.read_text().strip()}'

    # Session setup: the `c` preset selects --lang c, the C tier-1 pack and rule
    # metadata; the client then points the server at its project directory.
    start = call(base, 'GET', '/v1/settings')
    assert start['preset'] == 'c' and start['settings']['lang'] == 'c' and start['settings']['pack'] == 'rules/c-tier1.json'
    view = call(base, 'PATCH', '/v1/settings', {'cwd': str(project), 'pack': str(ROOT / 'rules/c-tier1.json')})
    assert Path(view['effective_cwd']) == project.resolve() or Path(view['effective_cwd']) == project, view
    ok('session starts from the c preset and is pointed at the project')

    # 1. Scan one local file.
    scan = call(base, 'POST', '/v1/lint', {'file': 'names.c'})
    hits = findings(scan)
    assert len(hits) == 1, hits
    warning = hits[0]
    assert warning['rule'] == 'LBC001' and warning['severity'] == 'critical' and 'strcpy' in warning['message'], warning
    assert warning['row'] == 5 and scan['command'][-1] == '--metadata', (warning, scan['command'])
    ok(f"step 1: one warning in names.c: {warning['rule']} {warning['message']} at row {warning['row']}")

    # 2. Where is it? The enclosing function comes from the same file's facts.
    local = records(call(base, 'POST', '/v1/facts', {'file': 'names.c'}))
    enclosing = [f for f in local if f['kind'] == 'def.function' and f['row'] <= warning['row'] <= f['end_row']]
    assert [f['name'] for f in enclosing] == ['copy_name'], enclosing
    sink = enclosing[0]['name']
    params = [f['name'] for f in local if f['kind'] == 'def.parameter' and f['scope'] == sink]
    assert params == ['dst', 'src'], params
    safe = [f for f in local if f['kind'] == 'def.function' and f['name'] != sink]
    assert [f['name'] for f in safe] == ['copy_title']
    ok(f'step 2: the warning is inside {sink}({", ".join(params)}); copy_title in the same file is clean')

    # 3. Deep search: every related file in the folder that declares or calls it.
    folder = records(call(base, 'POST', '/v1/facts', {'files': ['.']}))
    callers = sorted({(f['file'], f['scope'], f['row']) for f in folder if f['kind'] == 'ref.call' and f['name'] == sink})
    declared = sorted({f['file'] for f in folder if f['kind'] == 'def.prototype' and f['name'] == sink})
    norm = lambda p: Path(p).name
    assert [(norm(f), s, r) for f, s, r in callers] == [('main.c', 'main', 6), ('report.c', 'report_header', 3)], callers
    assert [norm(f) for f in declared] == ['names.h'], declared
    assert not any(norm(f['file']) == 'unrelated.c' and f['name'] == sink for f in folder)
    ok('step 3: folder search finds callers in main.c and report.c, the prototype in names.h, nothing in unrelated.c')

    # 4. Refine: only the copy_name call sites, and what flows into `src`.
    pattern = f'((call_expression function: (identifier) @fn arguments: (argument_list (_) (_) @src)) (#eq? @fn "{sink}"))'
    verdicts = {}
    for file, scope, row in callers:
        q = call(base, 'POST', '/v1/query', {'pattern': pattern, 'file': file})
        lines = q['stdout'].splitlines()
        assert len(lines) == 2, (file, lines)
        kind, span = lines[1].split(' ')
        (r0, c0), (r1, c1) = [tuple(int(x) for x in p.split(':')) for p in span.split('-')]
        assert r0 == r1 == row
        text = (project / norm(file)).read_text().splitlines()[r0][c0:c1]
        verdicts[norm(file)] = (scope, kind, text)
    assert verdicts == {'main.c': ('main', 'subscript_expression', 'argv[1]'),
                        'report.c': ('report_header', 'string_literal', '"report"')}, verdicts
    # argv flows in only where the caller's own scope has an argv parameter.
    main_params = [f['name'] for f in folder if f['kind'] == 'def.parameter' and f['scope'] == 'main' and norm(f['file']) == 'main.c']
    assert main_params == ['argc', 'argv'], main_params
    confirmed = sorted(f for f, (_, kind, text) in verdicts.items() if kind != 'string_literal' and text.split('[')[0] in main_params)
    assert confirmed == ['main.c']
    ok('step 4: refined query confirms main.c passes argv[1] (command-line input) into strcpy; report.c passes a literal')

    # 5. Widen the lint to the callers: the warning is not repeated elsewhere.
    wider = call(base, 'POST', '/v1/lint', {'files': [norm(f) for f, _, _ in callers]})
    assert findings(wider) == [] and wider['stdout'].count('0 findings in') == 2, wider['stdout']
    ok('step 5: linting the callers adds no new warnings')

    # 6. Revisit earlier steps: answers come from the server's cache.
    again = call(base, 'POST', '/v1/lint', {'file': 'names.c'})
    again_folder = call(base, 'POST', '/v1/facts', {'files': ['.']})
    assert again['cached'] and again_folder['cached'] and again['stdout'] == scan['stdout']
    history = call(base, 'GET', '/v1/results')['results']
    assert [r['command'][0] for r in reversed(history)][:5] == ['lint', 'facts', 'facts', 'query', 'query'], history
    ok('step 6: repeated steps are cache hits; the session history lists every step in order')

    # 7. Fix the sink and verify: the edit invalidates both the file and folder entries.
    source = project / 'names.c'
    source.write_text(source.read_text().replace('strcpy(dst, src);', 'snprintf(dst, 16, "%s", src);'))
    fixed = call(base, 'POST', '/v1/lint', {'file': 'names.c'})
    assert not fixed['cached'] and findings(fixed) == [], fixed['stdout']
    refreshed = call(base, 'POST', '/v1/facts', {'files': ['.']})
    assert not refreshed['cached']
    assert not any(f['kind'] == 'ref.call' and f['name'] == 'strcpy' for f in records(refreshed))
    assert call(base, 'POST', '/v1/facts', {'files': ['.']})['cached']
    ok('step 7: after the fix the warning is gone and stale cache entries are not reused')

    call(base, 'POST', '/v1/shutdown')
    out, _ = server.communicate(timeout=30)
    assert server.returncode == 0 and b'stopped (shutdown requested)' in out
finally:
    if server.poll() is None:
        server.kill()
    shutil.rmtree(work, ignore_errors=True)
print(f'PASS: {checks} triage checks')
