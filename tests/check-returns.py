#!/usr/bin/env python3
"""Host adoption checks for the lmcode ResultSink terminal-frame contract.

Dynamic tests use ordinary imports with the pinned compiler. Checked-in v6
fixtures additionally exercise real `$return` body and entry lowering from the
prepared slice-3 compiler, without making native gates wait for its release.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HOST = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else ROOT / "build/lmok"
COMPILER = os.environ.get("LMSCRIPT_BIN", "lmscript")


def compiler_argv():
    if os.name == "nt" and not COMPILER.lower().endswith((".cmd", ".exe", ".bat")):
        for candidate in (COMPILER + ".cmd", shutil.which("lmscript.cmd")):
            if candidate and Path(candidate).exists():
                return [candidate]
    return [COMPILER]


class ReturnChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        fixtures = ROOT / "tests/fixtures/returns"
        for line in (fixtures / "fixtures.lock").read_text().splitlines():
            digest, name = line.split("  ")
            assert hashlib.sha256((fixtures / name).read_bytes()).hexdigest() == digest, name
        schema_hash = hashlib.sha256((ROOT / "schema/lmok.dsl.json").read_bytes()).digest()
        for path in fixtures.glob("*.dslb"):
            assert path.read_bytes()[:38] == b"DSLB\x00\x06" + schema_hash, path

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="lmok-return-", dir=ROOT / "build")
        self.work = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def compile(self, body, name="program"):
        source = self.work / (name + ".lms")
        bytecode = self.work / (name + ".dslb")
        source.write_text(
            'p = _lmcode_ schema=' + json.dumps(str(ROOT / "schema/lmok.dsl.json")) + ";\n"
            + body + "\nend _lmcode_\n"
            + "savedsl(p, ARGV[0]);\n",
            encoding="utf-8", newline="\n",
        )
        result = subprocess.run(["node", str(ROOT/"scripts/compile-fixture.mjs"), str(source), str(bytecode)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return bytecode

    def host_run(self, *args):
        result = subprocess.run([str(HOST), *map(str, args)], capture_output=True)
        result.stdout = result.stdout.decode("utf8", errors="strict")
        result.stderr = result.stderr.decode("utf8", errors="strict")
        return result

    def frame(self, output):
        self.assertTrue(output.endswith("LMCODE_RETURN_END\n"), repr(output))
        self.assertNotIn("\r", output)
        self.assertEqual(output.count("LMCODE_RETURN_BEGIN\n"), 1)
        self.assertEqual(output.count("LMCODE_RETURN_END\n"), 1)
        lines = output.splitlines()
        self.assertGreaterEqual(len(lines), 3, repr(output))
        self.assertEqual(lines[-3], "LMCODE_RETURN_BEGIN", repr(output))
        self.assertEqual(lines[-1], "LMCODE_RETURN_END", repr(output))
        envelope=json.loads(lines[-2]);self.assertTrue(envelope["ok"]);return envelope["result"]

    def test_body_float_uses_shortest_round_trip_frame(self):
        binary = self.compile("lmcode_return_set(0.1 + 0.2);", "float")
        result = self.host_run("--lmcode", binary)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertTrue(result.stdout.endswith("LMCODE_RETURN_END\n"), repr(result.stdout))
        self.assertEqual(self.frame(result.stdout), 0.30000000000000004)
        self.assertTrue(result.stdout.endswith("LMCODE_RETURN_BEGIN\n{\"ok\":true,\"result\":0.30000000000000004}\nLMCODE_RETURN_END\n"))

    def test_unset_result_is_null_frame(self):
        binary = self.compile("let untouched = 1;", "unset")
        result = self.host_run("--lmcode", binary)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertIsNone(self.frame(result.stdout))

    def test_body_and_entry_share_one_sink(self):
        binary = self.compile(
            'lmcode_return_put("body", "yes");\n'
            '@pair fn check(n: Node) -> void implements Rule {\n'
            '  lmcode_return_put("entry", "yes");\n'
            '  ts.report(n, "finding", "R_RETURN", "warning");\n'
            '}\n',
            "entry",
        )
        target = self.work / "input.json"
        target.write_text('{"value":1}', encoding="utf-8")
        result = self.host_run("check", "--lang", "json", binary, target)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertEqual(self.frame(result.stdout), {"body": "yes", "entry": "yes"})
        self.assertEqual(result.stdout.count('"rule":"R_RETURN"'), 1)

    def test_entry_error_discards_partial_sink_and_emits_no_frame(self):
        binary = self.compile(
            'lmcode_return_put("before", "discard");\n'
            '@pair fn fail(n: Node) -> void implements Rule {\n'
            '  lmcode_return_put("partial", "discard");\n'
            '  n.child(999);\n'
            '}\n',
            "entry-error",
        )
        target = self.work / "input.json"
        target.write_text('{"value":1}', encoding="utf-8")
        result = self.host_run("check", "--lang", "json", binary, target)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(json.loads(result.stdout.splitlines()[-2])["ok"]);self.assertNotIn("result",json.loads(result.stdout.splitlines()[-2]))

    def test_scalar_field_failure_has_no_frame(self):
        binary = self.compile('lmcode_return_set(7); lmcode_return_put("field", 1);', "scalar")
        result = self.host_run("--lmcode", binary)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(json.loads(result.stdout.splitlines()[-2])["ok"]);self.assertNotIn("result",json.loads(result.stdout.splitlines()[-2]))

    def test_lmcode_does_not_dispatch_rule_entries(self):
        binary = self.compile(
            'lmcode_return_put("body", "yes");\n'
            '@pair fn entry(n: Node) -> void implements Rule {\n'
            '  lmcode_return_put("entry", "must-not-run");\n'
            '}\n',
            "body-only",
        )
        result = self.host_run("--lmcode", binary)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertEqual(self.frame(result.stdout), {"body": "yes"})

    def test_multiple_programs_get_independent_frames(self):
        first = self.compile('lmcode_return_set("first");', "first")
        second = self.compile('lmcode_return_set("second");', "second")
        unset = self.compile("let untouched = 1;", "between")
        result = self.host_run("--lmcode", first,"--lmcode", unset,"--lmcode", second)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertEqual(result.stdout.count("LMCODE_RETURN_BEGIN\n"), 1)
        self.assertEqual(result.stdout.count("LMCODE_RETURN_END\n"), 1)
        self.assertEqual(self.frame(result.stdout), ["first",None,"second"])

    def test_owned_collection_survives_entry_and_multiple_nodes(self):
        binary = self.compile(
            'let values = str_list(); values.add("captured");\n'
            'lmcode_return_put("captured", values); values.add("mutated");\n'
            '@pair fn entry(n: Node) -> void implements Rule {\n'
            '  lmcode_return_put("entry", "seen");\n'
            '}\n',
            "owned-list",
        )
        target = self.work / "items.json"
        target.write_text('[{"value":1},{"value":2}]', encoding="utf-8")
        result = self.host_run("check", "--lang", "json", binary, target)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertEqual(self.frame(result.stdout), {"captured": ["captured"], "entry": "seen"})
        self.assertNotIn("mutated", result.stdout)

    def test_sarif_entry_failure_has_no_terminal_frame(self):
        binary = self.compile(
            '@pair fn fail(n: Node) -> void implements Rule { n.child(999); }\n',
            "sarif-error",
        )
        target = self.work / "input.json"
        target.write_text('{"value":1}', encoding="utf-8")
        sarif = self.work / "result.sarif"
        result = self.host_run("check", "--lang", "json", binary, target, "--sarif", sarif)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(json.loads(result.stdout.splitlines()[-2])["ok"]);self.assertNotIn("result",json.loads(result.stdout.splitlines()[-2]))

    def test_real_return_syntax_body_fixture(self):
        result = self.host_run("--lmcode", ROOT / "tests/fixtures/returns/body.dslb")
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertEqual(self.frame(result.stdout),0.30000000000000004);self.assertIn("host_summary",result.stdout)

    def test_real_return_entries_across_files_and_sarif_order(self):
        first, second = self.work / "first.json", self.work / "second.json"
        first.write_bytes(b'{"a":1,"b":2}')
        second.write_bytes(b'{"c":3}')
        sarif = self.work / "ok.sarif"
        result = self.host_run("check", ROOT / "tests/fixtures/returns/entry.dslb", first, second, "--sarif", sarif)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertEqual(self.frame(result.stdout), {"body":"kept", "captured":["owned"], '\"a\":1':"visited", "last":'\"c\":3', '\"b\":2':"visited", '\"c\":3':"visited"})
        groups = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("[")]
        self.assertEqual([[f["rule"] for f in group] for group in groups], [["R_FIRST", "R_SECOND", "R_FIRST", "R_SECOND"], ["R_FIRST", "R_SECOND"]])
        log = json.loads(sarif.read_bytes())["runs"][0]
        self.assertEqual([f["ruleId"] for f in log["results"]], ["R_FIRST", "R_SECOND"] * 3)
        self.assertEqual([r["id"] for r in log["tool"]["driver"]["rules"]], ["R_FIRST", "R_SECOND"])

    def test_real_return_entry_error_and_sarif_write_failure(self):
        target = self.work / "input.json"
        target.write_bytes(b'{"x":1}')
        result = self.host_run("check", ROOT / "tests/fixtures/returns/entry-error.dslb", target)
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertFalse(json.loads(result.stdout.splitlines()[-2])["ok"]);self.assertNotIn("result",json.loads(result.stdout.splitlines()[-2]))
        self.assertIn("RUN_HOST", result.stdout)
        # A directory is an invalid SARIF destination on native OSes. This is an
        # application failure after successful body/entries, so it cannot frame.
        result = self.host_run("check", ROOT / "tests/fixtures/returns/entry.dslb", target, "--sarif", self.work)
        self.assertEqual(result.returncode, 4, result.stdout + result.stderr)
        self.assertFalse(json.loads(result.stdout.splitlines()[-2])["ok"]);self.assertNotIn("result",json.loads(result.stdout.splitlines()[-2]))

if __name__ == "__main__":
    unittest.main(verbosity=2)
