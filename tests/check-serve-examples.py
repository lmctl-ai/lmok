#!/usr/bin/env python3
"""Every lmscript example, run locally and again through `lmok serve`.

usage: check-serve-examples.py HOST [LMSCRIPT]   (LMSCRIPT defaults to the one on PATH)

For each examples/*.lms the script is run as written (the host runs as a
child process), then a copy is run whose host command is `lmok remote`:
`run_lmcode(p)` gains {command:[HOST, "remote", "--lmcode", "$DSLFILE"]} and
`system(["lmok", ...])` becomes `system([HOST, "remote", ...])`. Both runs must
exit alike, print identical stdout, and print every `#|` expect line. The
examples/serve/*.lms scripts already target the server; they are run once and
checked against their expect lines. A second pass over the same server must
be served from its result cache where the program allows it.
"""
from pathlib import Path
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
HOST = str(Path(sys.argv[1]).resolve())
LMSCRIPT = sys.argv[2] if len(sys.argv) > 2 else (shutil.which('lmscript') or sys.exit('lmscript not found on PATH'))
OUT = ROOT / 'build' / 'serve-examples'
shutil.rmtree(OUT, ignore_errors=True)
OUT.mkdir(parents=True)


def lms_string(text):
    return json.dumps(text.replace('\\', '/'))


def expectations(source):
    return [line[2:].rstrip('\r') for line in source.splitlines() if line.startswith('#|')]


def run_script(path, env):
    cmd = [LMSCRIPT, str(path)] if not LMSCRIPT.endswith('.js') else ['node', LMSCRIPT, str(path)]
    return subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, timeout=300)


def http(base, method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(base + path, data=data, method=method, headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=60) as resp:
        return json.loads(resp.read())


host_dir = str(Path(HOST).parent)
env = dict(os.environ, PATH=host_dir + os.pathsep + os.environ.get('PATH', ''))
port_file = Path(tempfile.mkdtemp(prefix='lmok-serve-examples-')) / 'port'
server = subprocess.Popen([HOST, 'serve', '--port', '0', '--port-file', str(port_file), '--preset', 'interactive'],
                          cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
try:
    deadline = time.time() + 20
    while not (port_file.exists() and port_file.read_text(encoding='utf-8').strip()):
        if time.time() > deadline or server.poll() is not None:
            raise SystemExit('server did not start')
        time.sleep(0.05)
    base = f'http://127.0.0.1:{port_file.read_text(encoding='utf-8').strip()}'
    env['LMOK_SERVER'] = base
    remote_call = f'run_lmcode(p, {{command:[{lms_string(HOST)}, "remote", "--lmcode", "$DSLFILE"]}})'

    compared = 0
    for example in sorted((ROOT / 'examples').glob('*.lms')):
        source = example.read_text(encoding='utf-8')
        rewritten = source.replace('run_lmcode(p)', remote_call)
        rewritten = re.sub(r'system\(\["lmok",', f'system([{lms_string(HOST)}, "remote",', rewritten)
        if rewritten == source:
            continue
        copy = OUT / example.name
        copy.write_text(rewritten, encoding='utf-8')
        here = run_script(example, env)
        there = run_script(copy, env)
        assert here.returncode == there.returncode, (example.name, here, there)
        assert here.stdout == there.stdout, (example.name, here.stdout[-2000:], there.stdout[-2000:], there.stderr[-2000:])
        lines = there.stdout.decode('utf-8').splitlines()
        for want in expectations(source):
            assert want in lines, (example.name, want, lines[-20:])
        compared += 1
        print(f'ok - {example.name}: local and server runs are identical')
    assert compared >= 15, compared

    served = 0
    for example in sorted((ROOT / 'examples' / 'serve').glob('*.lms')):
        result = run_script(example, env)
        assert result.returncode == 0, (example.name, result)
        lines = result.stdout.decode('utf-8').splitlines()
        for want in expectations(example.read_text(encoding='utf-8')):
            assert want in lines, (example.name, want, lines)
        served += 1
        print(f'ok - serve/{example.name}: expected output from the server')
    assert served >= 1

    # Re-running the system()-based examples hits the server cache.
    before = http(base, 'GET', '/v1/cache')['hits']
    for name in ('lint-go.lms', 'lint-repo.lms'):
        result = run_script(OUT / name, env)
        assert result.returncode == 0, result
    after = http(base, 'GET', '/v1/cache')['hits']
    assert after > before, (before, after)
    print(f'ok - repeated example commands were served from the cache ({after - before} hits)')

    http(base, 'POST', '/v1/shutdown')
    out, _ = server.communicate(timeout=30)
    assert server.returncode == 0 and b'stopped (shutdown requested)' in out
finally:
    if server.poll() is None:
        server.kill()
print(f'PASS: {compared} examples matched through lmok serve, {served} server examples')
