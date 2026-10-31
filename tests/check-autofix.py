#!/usr/bin/env python3
"""Portable byte-level checks for token-safe lint fixes.

The test deliberately builds its packs and source files in a temporary native
directory. It therefore exercises the same CLI on Linux, macOS and MinGW,
including UTF-8 paths, byte offsets and CRLF files.
"""
import json
import hashlib
import sqlite3
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HOST = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else ROOT / "build" / ("lmok.exe" if os.name == "nt" else "lmok")
LMSCRIPT = os.environ.get("LMSCRIPT_BIN", "lmscript")
PROVE = "--prove-controls" in sys.argv
if PROVE: sys.argv.remove("--prove-controls")
MUTATE = False


class Autofix(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="lmok autofix ", dir=ROOT / "build")
        self.work = Path(self.temp.name)
        self.cpack = self.work / "fixes-c.json"
        self.bpack = self.work / "fixes-bash.json"
        self.cpack.write_text(json.dumps({
            "lang": "c",
            "rules": [
                {"id": "FIX_C_GETS", "on": "identifier", "text_matches": "^gets$",
                 "message": "replace gets", "severity": "major", "fix": "safe_gets"},
                {"id": "FIX_C_COPY", "on": "identifier", "text_matches": "^strcpy$",
                 "message": "replace strcpy", "severity": "major", "fix": "safe_copy"},
            ],
        }), encoding="utf-8")
        self.bpack.write_text(json.dumps({
            "lang": "bash",
            "rules": [
                {"id": "FIX_B_SUBST", "on": "command_substitution", "text_matches": "^`",
                 "message": "use dollar substitution", "severity": "minor", "fix": "$(date)"},
                {"id": "FIX_B_WORD", "on": "word", "text_matches": "^foo$",
                 "message": "rename word", "severity": "minor", "fix": "bar"},
            ],
        }), encoding="utf-8")

    def tearDown(self):
        self.temp.cleanup()

    def run_host(self, *args):
        result = subprocess.run([str(HOST), *map(str, args)], capture_output=True)
        if MUTATE:
            # A false success/refusal must fail the corresponding positive or
            # negative oracle, rather than making an unsafe fix look green.
            result.returncode = 97 if result.returncode == 0 else 0
        return result

    def lint(self, pack, source, *flags):
        return self.run_host("lint", pack, *flags, source)

    @staticmethod
    def findings(result):
        if not result.stdout:
            return []
        return [json.loads(line) for line in result.stdout.splitlines() if line.startswith(b'{"row":')]

    def test_finding_contains_byte_fix_and_two_language_rules_are_pinned(self):
        c = self.work / "input.c"
        before = b"int f() { gets(x); strcpy(a, b); }\n"
        c.write_bytes(before)
        result = self.lint(self.cpack, c)
        self.assertEqual((result.returncode, result.stderr), (0, b""), result.stdout)
        findings = self.findings(result)
        self.assertEqual([f["rule"] for f in findings], ["FIX_C_GETS", "FIX_C_COPY"])
        for finding, token in zip(findings, (b"gets", b"strcpy")):
            self.assertIn("fix", finding)
            start = before.index(token)
            self.assertEqual(finding["fix"], {"start_byte": start, "end_byte": start + len(token),
                                               "text": b"safe_gets".decode() if token == b"gets" else "safe_copy"})
        bash = self.work / "input.sh"
        bash.write_bytes(b"value=`date` foo\n")
        result = self.lint(self.bpack, bash)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual([f["rule"] for f in self.findings(result)], ["FIX_B_SUBST", "FIX_B_WORD"])

    def test_fix_writes_bottom_up_preserves_crlf_and_is_idempotent(self):
        source = self.work / "crlf.c"
        before = b"// caf\xc3\xa9\r\nint f() { gets(x); strcpy(a, b); }\r\n"
        source.write_bytes(before)
        result = self.lint(self.cpack, source, "--fix")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        after = b"// caf\xc3\xa9\r\nint f() { safe_gets(x); safe_copy(a, b); }\r\n"
        self.assertEqual(source.read_bytes(), after)
        self.assertIn(b"2 fixed", result.stdout)
        self.assertNotIn(b"\n", source.read_bytes().replace(b"\r\n", b""))
        again = self.lint(self.cpack, source, "--fix")
        self.assertEqual(again.returncode, 0, again.stdout + again.stderr)
        self.assertEqual(source.read_bytes(), after)
        self.assertIn(b"0 fixed", again.stdout)

    def test_diff_prints_text_free_preview_without_writing(self):
        source = self.work / "diff.c"
        before = b"int f() { gets(x); }\n"
        source.write_bytes(before)
        result = self.lint(self.cpack, source, "--diff")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(source.read_bytes(), before)
        self.assertIn(b"--- ", result.stdout)
        self.assertIn(b"+++ ", result.stdout)
        self.assertIn(b"-[source omitted]", result.stdout)
        self.assertIn(b"+[source omitted]", result.stdout)
        preview = self.lint(self.cpack, source, "--fix", "--diff")
        self.assertEqual(preview.returncode, 0, preview.stdout + preview.stderr)
        self.assertEqual(source.read_bytes(), before)
        self.assertIn(b"1 fixed", preview.stderr)

    def test_suppressed_finding_is_never_fixed(self):
        source = self.work / "suppressed.c"
        before = b"// lmok-disable: FIX_C_GETS\nint f() { gets(x); strcpy(a, b); }\n"
        source.write_bytes(before)
        result = self.lint(self.cpack, source, "--fix")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(source.read_bytes(), before.replace(b"strcpy", b"safe_copy"))
        self.assertIn(b"1 fixed", result.stdout)

    def test_overlaps_are_refused_and_file_is_unchanged(self):
        pack = self.work / "overlap.json"
        pack.write_text(json.dumps({"lang": "c", "rules": [
            {"id": "A", "on": "identifier", "text_matches": "^gets$", "message": "a", "fix": "one"},
            {"id": "B", "on": "identifier", "text_matches": "^gets$", "message": "b", "fix": "two"},
        ]}), encoding="utf-8")
        source = self.work / "overlap.c"
        before = b"int f() { gets(x); }\n"
        source.write_bytes(before)
        result = self.lint(pack, source, "--fix")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"overlap", result.stderr.lower())
        self.assertEqual(source.read_bytes(), before)

    def test_tree_safety_refuses_parse_errors_and_sibling_injection(self):
        source = self.work / "bad.c"
        before = b"int f() { gets(\n"
        source.write_bytes(before)
        result = self.lint(self.cpack, source, "--fix")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(source.read_bytes(), before)
        malicious = self.work / "malicious.c"
        malicious_pack = self.work / "malicious-pack.json"
        malicious_pack.write_text(json.dumps({"lang": "c", "rules": [
            {"id": "BAD", "on": "identifier", "text_matches": "^gets$", "message": "bad",
             "fix": "gets(x); int injected;"},
        ]}), encoding="utf-8")
        malicious.write_bytes(b"int f() { gets; }\n")
        result = self.lint(malicious_pack, malicious, "--fix")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(malicious.read_bytes(), b"int f() { gets; }\n")

    def test_valid_syntax_cannot_change_ancestors_outside_the_reported_node(self):
        source = self.work / "ancestor.c"
        before = b"int f() { return foo(x); }\n"
        source.write_bytes(before)
        pack = self.work / "ancestor.json"
        pack.write_text(json.dumps({"lang": "c", "rules": [
            {"id": "BAD", "on": "identifier", "text_matches": "^foo$", "message": "bad", "fix": "sizeof"},
        ]}), encoding="utf-8")
        result = self.lint(pack, source, "--fix")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(source.read_bytes(), before)
        self.assertIn(b"tree shape", result.stderr)

    def test_new_error_nodes_are_refused_before_writing(self):
        source = self.work / "new-error.c"
        before = b"int foo;\n"
        source.write_bytes(before)
        pack = self.work / "new-error.json"
        pack.write_text(json.dumps({"lang": "c", "rules": [
            {"id": "BAD", "on": "identifier", "text_matches": "^foo$", "message": "bad", "fix": "("},
        ]}), encoding="utf-8")
        result = self.lint(pack, source, "--fix")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(source.read_bytes(), before)
        self.assertIn(b"ERROR", result.stderr)

    def test_normalized_crlf_noop_is_not_counted_or_rewritten(self):
        source = self.work / "noop.c"
        before = b"/*a\r\nb*/\r\nint x;\r\n"
        source.write_bytes(before)
        pack = self.work / "noop.json"
        pack.write_text(json.dumps({"lang": "c", "rules": [
            {"id": "NOOP", "on": "comment", "message": "style", "fix": "/*a\nb*/"},
        ]}), encoding="utf-8")
        result = self.lint(pack, source, "--fix")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(source.read_bytes(), before)
        self.assertIn(b"0 fixed", result.stdout)

    def test_equal_range_wrapper_nodes_cannot_hide_an_ancestor_change(self):
        source = self.work / "wrapper.sh"
        before = b"foo\necho done\n"
        source.write_bytes(before)
        pack = self.work / "wrapper.json"
        pack.write_text(json.dumps({"lang": "bash", "rules": [
            {"id": "BAD", "on": "word", "text_matches": "^foo$", "message": "bad", "fix": "if true; then :; fi"},
        ]}), encoding="utf-8")
        result = self.lint(pack, source, "--fix")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(source.read_bytes(), before)

    def test_invalid_fix_type_is_a_pack_error_and_metavariables_are_plain_text(self):
        bad = self.work / "bad-fix.json"
        bad.write_text(json.dumps({"lang": "c", "rules": [
            {"id": "BAD", "on": "identifier", "message": "bad", "fix": {"text": "x"}},
        ]}), encoding="utf-8")
        source = self.work / "bad.c"
        source.write_bytes(b"int foo;\n")
        result = self.lint(bad, source)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"fix", result.stderr.lower())
        plain = self.work / "plain.json"
        plain.write_text(json.dumps({"lang": "c", "rules": [
            {"id": "PLAIN", "on": "identifier", "text_matches": "^foo$", "message": "plain", "fix": "$X"},
        ]}), encoding="utf-8")
        result = self.lint(plain, source)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.findings(result)[0]["fix"]["text"], "$X")
        result = self.lint(plain, source, "--fix")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(source.read_bytes(), b"int $X;\n")

    def test_shipped_packs_match_pinned_before_after_bytes(self):
        for lang, ext in (("c", "c"), ("bash", "sh")):
            with self.subTest(lang=lang):
                source = self.work / ("shipped." + ext)
                source.write_bytes((ROOT / ("tests/fixtures/autofix-" + lang + "-before." + ext)).read_bytes())
                expected = (ROOT / ("tests/fixtures/autofix-" + lang + "-after." + ext)).read_bytes()
                result = self.lint(ROOT / ("rules/" + lang + "-autofix.json"), source, "--fix")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(source.read_bytes(), expected)
                self.assertIn(b"2 fixed", result.stdout)
                again = self.lint(ROOT / ("rules/" + lang + "-autofix.json"), source, "--fix")
                self.assertEqual(again.returncode, 0, again.stdout + again.stderr)
                self.assertIn(b"0 fixed", again.stdout)

    def test_replacement_newlines_follow_crlf_and_remaining_findings_are_relinted(self):
        pack = self.work / "relint.json"
        pack.write_text(json.dumps({"lang": "c", "rules": [
            {"id": "OLD", "on": "identifier", "text_matches": "^gets$", "message": "old", "fix": "(\nsafe_gets\n)"},
            {"id": "NEW", "on": "identifier", "text_matches": "^safe_gets$", "message": "new", "severity": "major"},
        ]}), encoding="utf-8")
        source = self.work / "insert-lines.c"
        source.write_bytes(b"int f() { gets(x); }\r\n")
        result = self.lint(pack, source, "--fix")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(source.read_bytes(), b"int f() { (\r\nsafe_gets\r\n)(x); }\r\n")
        self.assertEqual([finding["rule"] for finding in self.findings(result)], ["NEW"])

    def test_diff_keeps_no_final_newline_marker_and_utf8_path(self):
        source = self.work / "caf\u00e9 space.c"
        source.write_bytes(b"int f() { gets(x); }")
        result = self.lint(self.cpack, source, "--diff")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(source.read_bytes(), b"int f() { gets(x); }")
        self.assertEqual(result.stdout.count(b"\\ No newline at end of file"), 2)
        self.assertIn(b"@@ -1,1 +1,1 @@\n", result.stdout)
        self.assertIn(b"caf\xc3\xa9 space.c", result.stdout)

    def test_diff_is_read_only_for_database_and_apply_records_final_bytes(self):
        source = self.work / "record.c"
        before = b"int f() { gets(x); }\n"
        after = b"int f() { safe_gets(x); }\n"
        source.write_bytes(before)
        db = self.work / "fix.db"
        preview = self.lint(self.cpack, source, "--diff", "--db", db)
        self.assertEqual(preview.returncode, 0, preview.stdout + preview.stderr)
        self.assertFalse(db.exists())
        self.assertEqual(source.read_bytes(), before)
        result = self.lint(self.cpack, source, "--fix", "--db", db)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(source.read_bytes(), after)
        connection = sqlite3.connect(db)
        try:
            self.assertEqual(connection.execute("select sha256,size from files").fetchall(),
                             [(hashlib.sha256(after).hexdigest(), len(after))])
            self.assertEqual(connection.execute("select count(*) from findings").fetchone()[0], 0)
        finally:
            connection.close()  # Windows requires this before TemporaryDirectory cleanup.

    def test_fix_and_opt_in_rule_metadata_coexist(self):
        source = self.work / "metadata.c"
        source.write_bytes(b"int foo;\n")
        pack = self.work / "metadata.json"
        pack.write_text(json.dumps({"lang": "c", "rules": [
            {"id": "META", "on": "identifier", "message": "style", "fix": "bar", "tags": ["autofix"]},
        ]}), encoding="utf-8")
        result = self.lint(pack, source, "--metadata")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        finding = self.findings(result)[0]
        self.assertEqual(finding["fix"], {"start_byte": 4, "end_byte": 7, "text": "bar"})
        self.assertEqual(finding["metadata"]["tags"], ["autofix"])

    def test_entry_report_fix_emits_the_same_fix_shape(self):
        compiler = os.environ.get("LMSCRIPT_BIN", LMSCRIPT)
        if os.name == "nt" and not compiler.lower().endswith((".cmd", ".exe", ".bat")):
            compiler = next((p for p in (compiler + ".cmd", shutil.which("lmscript.cmd")) if p and Path(p).exists()), compiler)
        src = self.work / "entry.lms"
        bytecode = self.work / "entry.dslb"
        schema = ROOT / "schema" / "lmok.dsl.json"
        src.write_text(
            "p = _lmcode_ schema=" + json.dumps(str(schema)) + ";\n"
            "@identifier fn f(n: Node) -> void implements Rule { ts.report_fix(n, \"fix\", \"ENTRY_FIX\", \"minor\", \"renamed\"); }\n"
            "end _lmcode_\n", encoding="utf-8")
        compiled = subprocess.run(["node",str(ROOT/"scripts/compile-fixture.mjs"), str(src), str(bytecode)], capture_output=True, text=True)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        source = self.work / "entry.c"
        source.write_bytes(b"int old;\n")
        result = self.run_host("check", "--lang", "c", bytecode, source)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        finding = json.loads(next(line for line in result.stdout.splitlines() if line.startswith(b"[")))[0]
        self.assertEqual(finding["fix"], {"start_byte": 4, "end_byte": 7, "text": "renamed"})
        self.assertEqual(source.read_bytes(), b"int old;\n")


if __name__ == "__main__":
    names = unittest.defaultTestLoader.getTestCaseNames(Autofix)
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Autofix))
    if not result.wasSuccessful(): sys.exit(1)
    if PROVE:
        MUTATE = True
        for name in names:
            control = unittest.TestResult()
            Autofix(name).run(control)
            if not control.failures or control.errors:
                raise AssertionError("autofix false-result control did not fail an assertion: " + name + str(control.errors))
            print("ok - caught autofix false-success/refusal control: " + name)
    print("ok - token-safe fixes and native byte contracts (" + str(len(names)) + " groups)")
