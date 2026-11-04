#!/usr/bin/env python3
"""No implicit service bounds; real HTTP barriers, no scheduler waits."""
from pathlib import Path
import json, re, subprocess, sys, tempfile, unittest, urllib.request, urllib.error
ROOT = Path(__file__).resolve().parents[1]
HOST = str(Path(sys.argv.pop(1)).resolve())
class UnboundedServe(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT / 'build', prefix='serve-unbounded-')
        self.addCleanup(self.temp.cleanup)
        port = Path(self.temp.name) / 'port'
        self.proc = subprocess.Popen([HOST, 'serve', '--port', '0', '--port-file', str(port)], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.stop)
        # The listening line is emitted after binding and publishing the port.
        ready = self.proc.stdout.readline()
        self.assertIn(b'listening on', ready)
        self.base = 'http://127.0.0.1:' + port.read_text(encoding='utf-8').strip()
    def request(self, method, path, value=None):
        data = None if value is None else json.dumps(value).encode()
        req = urllib.request.Request(self.base + path, data=data, method=method, headers={'Content-Type':'application/json'})
        try:
            with urllib.request.urlopen(req) as r: return r.status, json.load(r)
        except urllib.error.HTTPError as e: return e.code, e.read()
    def stop(self):
        if self.proc.poll() is None:
            try: self.request('POST', '/v1/shutdown')
            except OSError: self.proc.terminate()  # Only the child created by this test.
        self.proc.communicate()
    def test_default_idle_and_body_are_unbounded(self):
        status, body = self.request('GET', '/v1/settings')
        self.assertEqual(status, 200)
        self.assertEqual(body['settings']['idle_timeout_s'], 0)
        self.assertEqual(body['settings']['max_body_bytes'], 0)
    def test_body_beyond_old_ceiling_and_explicit_override(self):
        status, body = self.request('POST', '/v1/parse', {'text':'{}' + ' ' * (17 * 1024 * 1024), 'lang':'json'})
        self.assertEqual(status, 200)
        self.assertEqual(body['exit_code'], 0)
        self.assertEqual(self.request('PATCH', '/v1/settings', {'max_body_bytes':1024})[0], 200)
        self.assertEqual(self.request('POST', '/v1/parse', {'text':' ' * 2048, 'lang':'json'})[0], 413)
        self.assertEqual(self.request('PATCH', '/v1/settings', {'max_body_bytes':0, 'idle_timeout_s':0})[0], 200)
    def test_no_invisible_socket_deadline_policy(self):
        # Neither public surface declares a caller socket deadline. An OS socket
        # timer would bypass that contract even with idle_timeout_s set to zero.
        source = (ROOT / 'cpp/serve.cpp').read_text(encoding='utf-8')
        self.assertIsNone(re.search(r'\bSO_(?:RCV|SND)TIMEO\b', source), 'undeclared OS socket deadline')
if __name__ == '__main__': unittest.main()
