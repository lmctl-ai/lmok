#!/usr/bin/env python3
"""Portable IDE-refactoring checks and mutation controls for `lmok refactor`."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HOST = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else ROOT / "build" / ("lmok.exe" if os.name == "nt" else "lmok")
PROVE = "--prove-controls" in sys.argv
if PROVE:
    sys.argv.remove("--prove-controls")

CORRUPT_MODE = None


class RefactorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="lmok refactor ", dir=ROOT / "build")
        self.work = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def run_refactor(self, *args, cwd=None):
        return subprocess.run(
            [str(HOST), "refactor", *map(str, args)],
            cwd=cwd or self.work,
            capture_output=True,
            timeout=30,
        )

    def test_dry_run_is_default_zero_write_and_matches_git_apply_including_utf8_and_crlf(self):
        repo = self.work / "repo"
        repo.mkdir()
        subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
        subprocess.run(["git", "config", "user.name", "Test"], cwd=repo, check=True)
        subprocess.run(["git", "config", "user.email", "test@example.com"], cwd=repo, check=True)

        f1 = repo / "Café 雪.cpp"
        f2 = repo / "win_crlf.cpp"
        orig1 = "int compute(int x, int y) {\n  return legacy_fn(x + 1, y * 2);\n}\n"
        orig2 = "int win(int a, int b) {\r\n  return legacy_fn(a,\r\n                   b);\r\n}\r\n"
        f1.write_text(orig1, encoding="utf-8")
        f2.write_bytes(orig2.encode("utf-8"))
        subprocess.run(["git", "add", "."], cwd=repo, check=True)
        subprocess.run(["git", "commit", "-qm", "baseline"], cwd=repo, check=True)

        # Dry-run must NOT touch files and must exit 0.
        dry = self.run_refactor("-p", "legacy_fn($A, $B)", "--to", "modern_fn($B, $A)", str(repo), cwd=repo)
        if CORRUPT_MODE == "DRYRUN_WRITES":
            f1.write_text("CORRUPTED", encoding="utf-8")
        self.assertEqual(dry.returncode, 0, dry.stderr.decode("utf-8", "replace"))
        self.assertEqual(f1.read_text(encoding="utf-8"), orig1)
        self.assertEqual(f2.read_bytes(), orig2.encode("utf-8"))
        self.assertIn(b"dry-run: 2 replacement(s) in 2 file(s)", dry.stderr)

        # --check must exit 1 when changes are pending, 0 when no matches.
        chk_pending = self.run_refactor("-p", "legacy_fn($A, $B)", "--to", "modern_fn($B, $A)", "--check", str(repo), cwd=repo)
        self.assertEqual(chk_pending.returncode, 1)
        chk_none = self.run_refactor("-p", "no_such_call($A)", "--to", "other($A)", "--check", str(repo), cwd=repo)
        self.assertEqual(chk_none.returncode, 0)

        # Apply the dry-run unified diff with `git apply` in a clean copy and compare byte-for-byte with `--apply`.
        rel_dry = self.run_refactor("-p", "legacy_fn($A, $B)", "--to", "modern_fn($B, $A)", ".", cwd=repo)
        self.assertEqual(rel_dry.returncode, 0, rel_dry.stderr.decode("utf-8", "replace"))
        diff_bytes = b"BAD_DIFF\n" if CORRUPT_MODE == "BROKEN_DIFF_HEADER" else rel_dry.stdout
        git_ap = subprocess.run(["git", "apply", "--whitespace=nowarn"], cwd=repo, input=diff_bytes, capture_output=True)
        self.assertEqual(git_ap.returncode, 0, git_ap.stderr.decode("utf-8", "replace"))
        after_git1 = f1.read_bytes()
        after_git2 = f2.read_bytes()

        # Reset and run `lmok refactor --apply`.
        subprocess.run(["git", "checkout", "--", "."], cwd=repo, check=True)
        ap = self.run_refactor("-p", "legacy_fn($A, $B)", "--to", "modern_fn($B, $A)", "--apply", ".", cwd=repo)
        self.assertEqual(ap.returncode, 0, ap.stderr.decode("utf-8", "replace"))
        self.assertEqual(f1.read_bytes(), after_git1)
        self.assertEqual(f2.read_bytes(), after_git2)
        self.assertIn(b"\r\n", after_git2)
        self.assertNotIn(b"a/./", rel_dry.stdout)

    def test_variadic_args_comma_elision_and_nested_composition(self):
        src = self.work / "api.cpp"
        src.write_text(
            "void demo(int a, int b) {\n"
            "  log_msg(\"plain\");\n"
            "  log_msg(\"one\", a);\n"
            "  log_msg(\"two\", a, b + 1);\n"
            "  int r = swap(swap(1, 2), 3);\n"
            "}\n",
            encoding="utf-8",
        )
        r1 = self.run_refactor("-p", "log_msg($FMT, $...ARGS)", "--to", "emit(LOG_INFO, $FMT, $...ARGS)", "--apply", src)
        self.assertEqual(r1.returncode, 0, r1.stderr.decode("utf-8", "replace"))
        r2 = self.run_refactor("-p", "swap($A, $B)", "--to", "pair($B, $A)", "--apply", src)
        self.assertEqual(r2.returncode, 0, r2.stderr.decode("utf-8", "replace"))
        got = src.read_text(encoding="utf-8")
        self.assertIn('emit(LOG_INFO, "plain");', got)
        self.assertIn('emit(LOG_INFO, "one", a);', got)
        self.assertIn('emit(LOG_INFO, "two", a, b + 1);', got)
        self.assertIn("int r = pair(3, pair(2, 1));", got)

    def test_conditional_auto_parenthesization_and_multi_language_rewrites(self):
        cpp = self.work / "prec.cpp"
        cpp.write_text(
            "int calc(int a, int b) {\n"
            "  int x = 1 / sq(a + b);\n"
            "  int y = 1 / sq(a);\n"
            "  int z = sq(a + b);\n"
            "  return x + y + z;\n"
            "}\n",
            encoding="utf-8",
        )
        r_cpp = self.run_refactor("-p", "sq($X)", "--to", "$X * $X", "--apply", cpp)
        self.assertEqual(r_cpp.returncode, 0, r_cpp.stderr.decode("utf-8", "replace"))
        got_cpp = cpp.read_text(encoding="utf-8")
        expected_prec = "1 / ((a + b) * (a + b))"
        if CORRUPT_MODE == "NO_PARENS_PREC":
            got_cpp = got_cpp.replace(expected_prec, "1 / a + b * a + b")
        self.assertIn(expected_prec, got_cpp)
        self.assertIn("1 / (a * a)", got_cpp)
        self.assertIn("int z = (a + b) * (a + b);", got_cpp)

        # Python, Java, Go, TypeScript expression / statement rewrites
        py = self.work / "mod.py"
        py.write_text("x = old_call(a, b)\ny = old_call(c, d + 1)\n", encoding="utf-8")
        self.assertEqual(self.run_refactor("-p", "old_call($A, $B)", "--to", "new_call($B, $A)", "--apply", py).returncode, 0)
        self.assertEqual(py.read_text(encoding="utf-8"), "x = new_call(b, a)\ny = new_call(d + 1, c)\n")

        java = self.work / "App.java"
        java.write_text("class App { void f() { Assert.assertEquals(a, b); } }\n", encoding="utf-8")
        self.assertEqual(self.run_refactor("-p", "Assert.assertEquals($E, $A)", "--to", "Assertions.assertEquals($E, $A)", "--apply", java).returncode, 0)
        self.assertIn("Assertions.assertEquals(a, b);", java.read_text(encoding="utf-8"))

        go = self.work / "main.go"
        go.write_text("package p\nfunc f() { oldFn(x, y) }\n", encoding="utf-8")
        self.assertEqual(self.run_refactor("-p", "oldFn($A, $B)", "--to", "newFn($B, $A)", "--apply", go).returncode, 0)
        self.assertIn("newFn(y, x)", go.read_text(encoding="utf-8"))

        ts = self.work / "app.ts"
        ts.write_text("export const v = legacyFn(1, 2);\n", encoding="utf-8")
        self.assertEqual(self.run_refactor("-p", "legacyFn($A, $B)", "--to", "modernFn($B, $A)", "--apply", ts).returncode, 0)
        self.assertIn("modernFn(2, 1)", ts.read_text(encoding="utf-8"))

    def test_statement_deletion_and_unbraced_control_flow_guard(self):
        src = self.work / "del.cpp"
        orig = (
            "void f(int c) {\n"
            "  DBG(\"first\");\n"
            "  int keep = 1;\n"
            "  if (c)\n"
            "    DBG(\"sole\");\n"
            "}\n"
        )
        src.write_text(orig, encoding="utf-8")
        refused = self.run_refactor("-p", "DBG($...A);", "--to", "", src)
        if CORRUPT_MODE == "UNBRACED_DELETE":
            refused = subprocess.CompletedProcess(refused.args, 0, b"", b"")
        self.assertEqual(refused.returncode, 3)
        self.assertEqual(src.read_text(encoding="utf-8"), orig)

        skipped = self.run_refactor("-p", "DBG($...A);", "--to", "", "--skip-unsafe", "--apply", src)
        self.assertEqual(skipped.returncode, 0, skipped.stderr.decode("utf-8", "replace"))
        self.assertEqual(
            src.read_text(encoding="utf-8"),
            "void f(int c) {\n  int keep = 1;\n  if (c)\n    DBG(\"sole\");\n}\n",
        )

    def test_rename_local_cursor_shadow_exclusion_and_collision_refusal(self):
        src = self.work / "scope.cpp"
        src.write_text(
            "struct S { int x; };\n"
            "int other(int x) { return x; }\n"
            "int target(int x, S s) {\n"
            "  int total = x + s.x;\n"
            "  {\n"
            "    int x = 99;\n"
            "    total += x;\n"
            "  }\n"
            "  return total + x;\n"
            "}\n",
            encoding="utf-8",
        )
        # Cursor at `int target(int x, S s)` line 3 col 16
        r = self.run_refactor("--rename", "x", "--at", f"{src}:3:16", "--to", "val", "--apply", src)
        self.assertEqual(r.returncode, 0, r.stderr.decode("utf-8", "replace"))
        got = src.read_text(encoding="utf-8")
        if CORRUPT_MODE == "SHADOW_LEAK":
            got = got.replace("int x = 99;", "int val = 99;")
        self.assertIn("int other(int x) { return x; }", got)
        self.assertIn("int target(int val, S s) {", got)
        self.assertIn("int total = val + s.x;", got)
        self.assertIn("int x = 99;\n    total += x;", got)
        self.assertIn("return total + val;", got)

        # Colliding with existing `total` in target scope must refuse (exit 3) and leave file unchanged.
        before_coll = src.read_text(encoding="utf-8")
        coll = self.run_refactor("--rename", "val", "--at", f"{src}:3:16", "--to", "total", "--apply", src)
        self.assertEqual(coll.returncode, 3)
        self.assertEqual(src.read_text(encoding="utf-8"), before_coll)

    def test_cpp_class_ctor_dtor_header_and_gated_method_rename(self):
        hdr = self.work / "widget.h"
        cpp = self.work / "widget.cpp"
        hdr.write_text(
            "class Widget {\n"
            "public:\n"
            "  Widget();\n"
            "  ~Widget();\n"
            "  void draw();\n"
            "};\n",
            encoding="utf-8",
        )
        cpp.write_text(
            "#include \"widget.h\"\n"
            "Widget::Widget() {}\n"
            "Widget::~Widget() {}\n"
            "void Widget::draw() {}\n"
            "void run(Widget *w, int *other) {\n"
            "  w->draw();\n"
            "  other->draw();\n"
            "}\n",
            encoding="utf-8",
        )
        # Rename Widget -> Panel across .h and .cpp using --header-lang cpp
        r_cls = self.run_refactor("--rename", "Widget", "--to", "Panel", "--kind", "class", "--header-lang", "cpp", "--apply", hdr, cpp)
        self.assertEqual(r_cls.returncode, 0, r_cls.stderr.decode("utf-8", "replace"))
        self.assertIn("class Panel {", hdr.read_text(encoding="utf-8"))
        self.assertIn("Panel();", hdr.read_text(encoding="utf-8"))
        self.assertIn("~Panel();", hdr.read_text(encoding="utf-8"))
        self.assertIn("Panel::Panel() {}", cpp.read_text(encoding="utf-8"))
        self.assertIn("Panel::~Panel() {}", cpp.read_text(encoding="utf-8"))
        self.assertIn("void Panel::draw() {}", cpp.read_text(encoding="utf-8"))
        self.assertIn("void run(Panel *w, int *other) {", cpp.read_text(encoding="utf-8"))

        # Method rename `--kind method --scope Panel` flags candidate sites (`w->draw()`, `other->draw()`)
        # and refuses `--apply` without `--allow-candidates` or `--exact-only`.
        snap_hdr = hdr.read_text(encoding="utf-8")
        snap_cpp = cpp.read_text(encoding="utf-8")
        gated = self.run_refactor("--rename", "draw", "--to", "render", "--kind", "method", "--scope", "Panel", "--header-lang", "cpp", "--apply", hdr, cpp)
        if CORRUPT_MODE == "CANDIDATE_AUTOAPPLY":
            gated = subprocess.CompletedProcess(gated.args, 0, b"", b"")
        self.assertEqual(gated.returncode, 3)
        self.assertEqual(hdr.read_text(encoding="utf-8"), snap_hdr)
        self.assertEqual(cpp.read_text(encoding="utf-8"), snap_cpp)

        # --exact-only renames only the exact Panel declaration & Panel::render definition.
        exact = self.run_refactor("--rename", "draw", "--to", "render", "--kind", "method", "--scope", "Panel", "--header-lang", "cpp", "--exact-only", "--apply", hdr, cpp)
        self.assertEqual(exact.returncode, 0, exact.stderr.decode("utf-8", "replace"))
        self.assertIn("void render();", hdr.read_text(encoding="utf-8"))
        self.assertIn("void Panel::render() {}", cpp.read_text(encoding="utf-8"))
        self.assertIn("w->draw();", cpp.read_text(encoding="utf-8"))

    def test_import_rewrite_and_sequential_rules_and_filters(self):
        c_file = self.work / "imp.cpp"
        c_file.write_text(
            "// \"legacy/old.h\" comment untouched\n"
            "#include \"legacy/old.h\"\n"
            "const char *s = \"legacy/old.h\";\n"
            "int h1(int tmp_a) { return old_call(tmp_a); }\n"
            "int h2(int keep_a) { return old_call(keep_a); }\n",
            encoding="utf-8",
        )
        r_imp = self.run_refactor("--import", "legacy/old.h", "--to", "core/new.h", "--apply", c_file)
        self.assertEqual(r_imp.returncode, 0, r_imp.stderr.decode("utf-8", "replace"))
        got = c_file.read_text(encoding="utf-8")
        self.assertIn('// "legacy/old.h" comment untouched', got)
        self.assertIn('#include "core/new.h"', got)
        self.assertIn('const char *s = "legacy/old.h";', got)

        # --where filter selects only calls where $X matches ^tmp_
        r_wh = self.run_refactor("-p", "old_call($X)", "--to", "new_call($X)", "--where", "X~^tmp_", "--apply", c_file)
        self.assertEqual(r_wh.returncode, 0, r_wh.stderr.decode("utf-8", "replace"))
        got2 = c_file.read_text(encoding="utf-8")
        self.assertIn("return new_call(tmp_a);", got2)
        self.assertIn("return old_call(keep_a);", got2)

        # --rules sequential pack + --expect digest + --format json
        pack = self.work / "codemod.json"
        pack.write_text(json.dumps({
            "lang": "cpp",
            "rules": [
                {"id": "STEP1", "pattern": "step1($X)", "to": "step2($X, 0)"},
                {"id": "STEP2", "pattern": "step2($X, $Y)", "to": "final_fn($X, $Y)"},
            ],
        }), encoding="utf-8")
        seq = self.work / "seq.cpp"
        seq.write_text("int k(int a) { return step1(a); }\n", encoding="utf-8")
        j = self.run_refactor("--rules", pack, "--format", "json", seq)
        self.assertEqual(j.returncode, 0, j.stderr.decode("utf-8", "replace"))
        payload = json.loads(j.stdout.decode("utf-8"))
        digest = payload["digest"]
        self.assertEqual(len(digest), 16)
        self.assertEqual(payload["sites_count"], 2)

        bad_dig = self.run_refactor("--rules", pack, "--expect", "0000000000000000", "--apply", seq)
        self.assertEqual(bad_dig.returncode, 3)
        self.assertEqual(seq.read_text(encoding="utf-8"), "int k(int a) { return step1(a); }\n")

        good_dig = self.run_refactor("--rules", pack, "--expect", digest, "--apply", seq)
        self.assertEqual(good_dig.returncode, 0, good_dig.stderr.decode("utf-8", "replace"))
        self.assertEqual(seq.read_text(encoding="utf-8"), "int k(int a) { return final_fn(a, 0); }\n")

    def test_comment_loss_refusal_syntax_errors_and_multi_file_preflight_atomicity(self):
        # 1. Comment loss refused unless --drop-comments
        cm = self.work / "comments.cpp"
        cm_orig = "int g() { return legacy(1 /* keep */, 2); }\n"
        cm.write_text(cm_orig, encoding="utf-8")
        ref = self.run_refactor("-p", "legacy($A, $B)", "--to", "modern($B)", "--apply", cm)
        self.assertEqual(ref.returncode, 3)
        self.assertEqual(cm.read_text(encoding="utf-8"), cm_orig)
        ok_drop = self.run_refactor("-p", "legacy($A, $B)", "--to", "modern($B)", "--drop-comments", "--apply", cm)
        self.assertEqual(ok_drop.returncode, 0, ok_drop.stderr.decode("utf-8", "replace"))
        self.assertEqual(cm.read_text(encoding="utf-8"), "int g() { return modern(2); }\n")

        # 2. Pre-existing unrelated parse ERROR tolerated by default, blocked by --strict-parse
        broken_elsewhere = self.work / "err_tolerated.cpp"
        broken_elsewhere.write_text("void ok() { old_call(1); }\nvoid bad( { ;;; \n", encoding="utf-8")
        tol = self.run_refactor("-p", "old_call($X)", "--to", "new_call($X)", broken_elsewhere)
        self.assertEqual(tol.returncode, 0, tol.stderr.decode("utf-8", "replace"))
        strict = self.run_refactor("-p", "old_call($X)", "--to", "new_call($X)", "--strict-parse", broken_elsewhere)
        self.assertEqual(strict.returncode, 3)

        # 3. Multi-file preflight atomicity: file1 must NOT be written when file2 fails verification
        ok_file = self.work / "z1_good.cpp"
        bad_file = self.work / "z2_bad.cpp"
        ok_orig = "void a() { foo(1); }\n"
        bad_orig = "void b() { int x = foo(2); }\n"
        ok_file.write_text(ok_orig, encoding="utf-8")
        bad_file.write_text(bad_orig, encoding="utf-8")
        # Deleting foo(...) when the second site is inside an expression (`int x = foo(2);`) breaks file2's syntax!
        atomic_fail = self.run_refactor("-p", "foo($X)", "--to", "()", "--apply", ok_file, bad_file)
        if CORRUPT_MODE == "PARTIAL_PREFLIGHT_WRITE":
            ok_file.write_text("CORRUPT", encoding="utf-8")
        self.assertNotEqual(atomic_fail.returncode, 0)
        self.assertEqual(ok_file.read_text(encoding="utf-8"), ok_orig)
        self.assertEqual(bad_file.read_text(encoding="utf-8"), bad_orig)

    def test_inside_not_inside_and_polyglot_imports(self):
        # Scoped pattern rewrite with --inside and --not-inside + expression wrap
        cpp = self.work / "scoped_pat.cpp"
        cpp.write_text(
            "int target_fn(int v) {\n"
            "  int a = raw_get(v);\n"
            "  for (int i = 0; i < 1; ++i) {\n"
            "    a += raw_get(i);\n"
            "  }\n"
            "  return a;\n"
            "}\n",
            encoding="utf-8",
        )
        r = self.run_refactor(
            "-p", "raw_get($X)", "--to", "safe_get(sanitize($X))",
            "--inside", "int target_fn($T $P) { $...B; }",
            "--not-inside", "for (int $I = $INIT; $COND; $INC) { $...D; }",
            "--apply", cpp,
        )
        self.assertEqual(r.returncode, 0, r.stderr.decode("utf-8", "replace"))
        got = cpp.read_text(encoding="utf-8")
        self.assertIn("int a = safe_get(sanitize(v));", got)
        self.assertIn("a += raw_get(i);", got)

        # Polyglot --import across Python, Go, and TypeScript
        py = self.work / "imp.py"
        py.write_text("from old_pkg.util import f\nx = 'old_pkg.util'\n", encoding="utf-8")
        self.assertEqual(self.run_refactor("--import", "old_pkg.util", "--to", "new_pkg.core", "--apply", py).returncode, 0)
        self.assertEqual(py.read_text(encoding="utf-8"), "from new_pkg.core import f\nx = 'old_pkg.util'\n")

        go = self.work / "imp.go"
        go.write_text("package p\nimport \"old/mod\"\nconst s = \"old/mod\"\n", encoding="utf-8")
        self.assertEqual(self.run_refactor("--import", "old/mod", "--to", "new/mod", "--apply", go).returncode, 0)
        self.assertEqual(go.read_text(encoding="utf-8"), "package p\nimport \"new/mod\"\nconst s = \"old/mod\"\n")

        ts = self.work / "imp.ts"
        ts.write_text("import { x } from \"old/mod\";\nconst s = \"old/mod\";\n", encoding="utf-8")
        self.assertEqual(self.run_refactor("--import", "old/mod", "--to", "new/mod", "--apply", ts).returncode, 0)
        self.assertEqual(ts.read_text(encoding="utf-8"), "import { x } from \"new/mod\";\nconst s = \"old/mod\";\n")

    def test_changed_and_git_clean_guards(self):
        repo = self.work / "gitrepo"
        repo.mkdir()
        subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
        subprocess.run(["git", "config", "user.name", "Test"], cwd=repo, check=True)
        subprocess.run(["git", "config", "user.email", "test@example.com"], cwd=repo, check=True)
        clean_f = repo / "clean.cpp"
        dirty_f = repo / "dirty.cpp"
        clean_f.write_text("void f() { old_fn(1); }\n", encoding="utf-8")
        dirty_f.write_text("void g() { old_fn(2); }\n", encoding="utf-8")
        subprocess.run(["git", "add", "."], cwd=repo, check=True)
        subprocess.run(["git", "commit", "-qm", "init"], cwd=repo, check=True)

        # Modify dirty_f relative to HEAD
        dirty_f.write_text("void g() { old_fn(2); }\n// local edit\n", encoding="utf-8")

        # --changed restricts to dirty_f only
        chg = self.run_refactor("-p", "old_fn($X)", "--to", "new_fn($X)", "--changed", ".", cwd=repo)
        self.assertEqual(chg.returncode, 0, chg.stderr.decode("utf-8", "replace"))
        self.assertIn(b"dirty.cpp", chg.stdout)
        self.assertNotIn(b"clean.cpp", chg.stdout)

        # --apply --git-clean refuses when modified target has uncommitted git changes
        gc = self.run_refactor("-p", "old_fn($X)", "--to", "new_fn($X)", "--git-clean", "--apply", ".", cwd=repo)
        self.assertEqual(gc.returncode, 3)
        self.assertIn(b"uncommitted git changes", gc.stderr)
        self.assertIn("old_fn(1);", clean_f.read_text(encoding="utf-8"))


def main():
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(RefactorTests)
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)
    if not result.wasSuccessful():
        sys.exit(1)

    if PROVE:
        global CORRUPT_MODE
        mutations = [
            "DRYRUN_WRITES",
            "BROKEN_DIFF_HEADER",
            "NO_PARENS_PREC",
            "UNBRACED_DELETE",
            "SHADOW_LEAK",
            "CANDIDATE_AUTOAPPLY",
            "PARTIAL_PREFLIGHT_WRITE",
        ]
        for mode in mutations:
            CORRUPT_MODE = mode
            res = unittest.TextTestRunner(stream=open(os.devnull, "w")).run(
                unittest.defaultTestLoader.loadTestsFromTestCase(RefactorTests)
            )
            CORRUPT_MODE = None
            if res.wasSuccessful():
                print(f"CONTROL FAILURE: mutation {mode} went undetected!", file=sys.stderr)
                sys.exit(1)
        print(f"All {len(mutations)} refactor mutation controls detected cleanly.")


if __name__ == "__main__":
    main()
