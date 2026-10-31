#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"
BUILD_DIR="${BUILD_DIR:-build}"
HOST="$BUILD_DIR/lmok"
PATH="$HERE/$BUILD_DIR:$PATH"
export PATH
UNAME_S="$(uname -s)"
case "$UNAME_S" in MINGW*|MSYS*|CYGWIN*) HOST="$HOST.exe";; esac
stat_size(){ stat -c %s "$1" 2>/dev/null || stat -f%z "$1"; }
# Scratch lives under the (git-ignored) build dir: /tmp is an MSYS virtual
# path in Git Bash, and the Node-based lmscript compiler resolves it against
# a Windows drive where it does not exist. On Windows the path is converted
# to a native forward-slash path so it can be embedded in .lms sources.
mkdir -p "$HERE/$BUILD_DIR"
WORK="$(mktemp -d "$HERE/$BUILD_DIR/test-tmp-XXXXXX")"
case "$UNAME_S" in MINGW*|MSYS*|CYGWIN*) WORK="$(cd "$WORK" && pwd -W | tr '\\' '/')";; esac
trap 'rm -rf "$WORK"' EXIT
pass=0
ok(){ pass=$((pass+1)); echo "ok $pass - $1"; }
fail(){ echo "FAIL: $*" >&2; exit 1; }

LMSCRIPT="$(scripts/select-compiler.sh)"
export LMSCRIPT_BIN="$LMSCRIPT"
compile_fixture(){ node scripts/compile-fixture.mjs "$@"; }
# Fixture compilation imports the compiler library (dist/dsl.js) next to the compiler
# binary; upstream removed the public bytecode export, and the npm CLI package does not
# ship the library. Require it and point at the bootstrap when only the CLI is installed.
_lms_real="$(node -p 'require("fs").realpathSync(process.argv[1])' "$LMSCRIPT")"
if [ -n "${LMCODE_COMPILER_MODULE:-}" ]; then _libcheck="$LMCODE_COMPILER_MODULE";
else _libcheck="$(dirname "$_lms_real")/../dist/dsl.js"; fi
[ -f "$_libcheck" ] || { echo "error: lmscript compiler library not found at $_libcheck" >&2;
  echo "run scripts/bootstrap-compiler.sh and set LMSCRIPT_BIN to the path it prints" >&2; exit 2; }
scripts/sync-sdk.sh --verify >/dev/null || fail "SDK header pin mismatch"
ok "SDK headers match sdk.lock"
scripts/sync-deps.sh --verify >/dev/null || fail "deps pin mismatch"
ok "third_party/ tree-sitter and JSON/C/Python/Bash/Java/Go/JavaScript/TypeScript/TSX/lmscript/lmcode grammars match deps.lock"

# release versioning (CI stamps this into the build; see scripts/next-version.sh)
nv() { LMOK_TAGS="$1" GITHUB_REF_TYPE= GITHUB_REF_NAME= scripts/next-version.sh; }
[ "$(nv 'v7.0.0')" = 7.0.1 ] || fail "next-version bumps the patch of the latest tag"
[ "$(nv 'v7.0.9
v7.0.10')" = 7.0.11 ] || fail "next-version sorts versions numerically"
[ "$(nv 'v0.0.1')" = "$(sed -n 's/^project(lmok VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)" ] || fail "next-version uses CMakeLists' VERSION when it is ahead of every tag"
[ "$(GITHUB_REF_TYPE=tag GITHUB_REF_NAME=v1.2.3 scripts/next-version.sh)" = 1.2.3 ] || fail "next-version on a tag is the tag"
ok "release version selection (scripts/next-version.sh)"

cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release >"$WORK/cmake.log" 2>&1 ||
  { tail -30 "$WORK/cmake.log" >&2; fail "cmake configure ($BUILD_DIR)"; }
cmake --build "$BUILD_DIR" -j2 >"$WORK/build.log" 2>&1 ||
  { grep -n -B 3 -A 4 "error:" "$WORK/build.log" >&2 || true; tail -30 "$WORK/build.log" >&2; fail "cmake build ($BUILD_DIR)"; }
ok "host build ($BUILD_DIR)"
python3 tests/check-host-flags.py schema/lmok.dsl.json "$HOST" || fail "hard host flag cutover"

want_version="lmok $(sed -n 's/^project(lmok VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
[ "$("$HOST" --version)" = "$want_version" ] || fail "--version prints $want_version"
set +e; "$HOST" --version extra >/dev/null 2>&1; code=$?; set -e
[ "$code" = 2 ] || fail "--version with an extra argument exits 2 (got $code)"
ok "--version prints exactly '$want_version'"

"$HOST" --notices >"$WORK/notices.txt" || fail "--notices print"
cmp THIRD_PARTY_NOTICES.md "$WORK/notices.txt" || fail "--notices differs from THIRD_PARTY_NOTICES.md"
ok "--notices prints byte-identical THIRD_PARTY_NOTICES.md"

"$HOST" --lmcode-schema --json >"$WORK/embedded-schema.json" || fail "embedded schema print"
cmp schema/lmok.dsl.json "$WORK/embedded-schema.json" || fail "embedded schema differs from source bytes"
ok "--lmcode-schema --json prints byte-identical embedded schema bytes"
"$HOST" --lmcode-schema >"$WORK/schema-summary.txt" || fail "schema summary print"
python3 - "$WORK/schema-summary.txt" <<'PY'
import re,subprocess,sys
summary=open(sys.argv[1]).read()
sigs=subprocess.run(['build/lmok','--lmcode-signatures'],capture_output=True,text=True).stdout.splitlines()
missing=[]
for line in sigs:
  if line.startswith('method '): needle=line[7:]
  elif line.startswith('fn '): needle=line[3:]
  elif line.startswith('context '): needle=line[8:]
  else: needle=line
  if needle not in summary: missing.append(needle)
assert not missing, missing
print('all', len(sigs), 'registry members listed in --lmcode-schema summary')
PY
ok "--lmcode-schema summary lists every registry member"
diff -u <("$LMSCRIPT" --lmcode-signatures schema/lmok.dsl.json) <("$HOST" --lmcode-signatures) || fail "host/compiler signatures differ"
python3 scripts/merge-std.py schema/lmok.host.json "$WORK/merged-schema.json"
cmp schema/lmok.dsl.json "$WORK/merged-schema.json" || fail "generated schema differs from pinned host/std merge"
python3 scripts/merge-std.py --host-only schema/lmok.dsl.json "$WORK/host-only-schema.json"
cmp schema/lmok.host.json "$WORK/host-only-schema.json" || fail "host-only input contains standard members"
"$HOST" --lmcode-signatures >"$WORK/registry-signatures.txt"
"$LMSCRIPT" --lmcode-signatures cpp/std/lmcode-std.schema.json >"$WORK/standard-signatures.txt"
python3 - "$WORK" <<'PYHOST'
from pathlib import Path
import sys
work=Path(sys.argv[1])
registry=set((work/'registry-signatures.txt').read_text().splitlines())
standard=set((work/'standard-signatures.txt').read_text().splitlines())
assert len(standard)==206, len(standard)  # canonical v3 catalog including all 24 return overloads
(work/'host-signatures.txt').write_bytes(''.join(line+'\n' for line in sorted(registry-standard)).encode('utf-8'))
PYHOST
diff -u <("$LMSCRIPT" --lmcode-signatures schema/lmok.host.json --without-std) "$WORK/host-signatures.txt" || fail "host-only registry/compiler signatures differ"
ok "typed registry signatures match explicit canonical merge and host-only schema"
python3 tests/check-returns.py "$HOST" || fail "invocation return protocol"
ok "body/entry result ownership, terminal frames and failure silence"

python3 tests/release-hygiene.py || fail "release input policy"
ok "release input policy: tests/docs do not publish; binary inputs and tags do"

# --- parse ---
cat > "$WORK/sample.json" <<'EOF'
{"tool": "lmok", "ok": true, "items": [1, null]}
EOF
out="$("$HOST" parse "$WORK/sample.json")"
[ "$out" = '(document (object (pair key: (string (string_content)) value: (string (string_content))) (pair key: (string (string_content)) value: (true)) (pair key: (string (string_content)) value: (array (number) (null)))))' ] \
  || fail "parse sexp: $out"
ok "parse: object/array/true/null sexp"

out="$(printf '{"a": [1, 2]}' | "$HOST" parse -)"
[ "$out" = '(document (object (pair key: (string (string_content)) value: (array (number) (number)))))' ] \
  || fail "parse stdin sexp: $out"
ok "parse: reads stdin with '-'"

printf '{"a": 1,,}' > "$WORK/bad.json"
out="$("$HOST" parse "$WORK/bad.json")"
[ "$out" = '(document (object (pair key: (string (string_content)) value: (number)) (ERROR)))' ] \
  || fail "parse malformed sexp: $out"
ok "parse: malformed input still prints a tree with an ERROR node (exit 0)"

set +e; out="$("$HOST" parse "$WORK/no-such-file" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'cannot read'* ]] || fail "missing input file (exit $code): $out"
ok "parse missing file refused (exit 3)"

set +e; "$HOST" parse >/dev/null 2>&1; code=$?; set -e
[ "$code" = 2 ] || fail "parse without arguments exits 2 (got $code)"
ok "parse without arguments refused (exit 2)"

# --- query ---
out="$("$HOST" query '(pair key: (string (string_content) @key))' examples/data/sample.json)"
[ "$out" = 'string_content 1:3-1:7
string_content 2:3-2:10
string_content 3:3-3:5
string_content 4:3-4:12' ] || fail "query captures: $out"
ok "query: captured nodes printed as kind start-end (0-based)"

set +e; out="$("$HOST" query '(pair' examples/data/sample.json 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'query error at byte offset'*'syntax'* ]] || fail "bad query pattern (exit $code): $out"
ok "query: invalid pattern refused (exit 3, offset + reason)"

out="$("$HOST" query '(pair (string))' examples/data/sample.json)"
[ -z "$out" ] || fail "zero-capture query must print nothing: $out"
ok "query: zero-capture pattern returns an empty list (exit 0)"

set +e; "$HOST" query '(x)' >/dev/null 2>&1; code=$?; set -e
[ "$code" = 2 ] || fail "query without a file argument exits 2 (got $code)"
ok "query without a file argument refused (exit 2)"

# --- --lang c ---
out="$("$HOST" parse --lang c examples/data/sample.c)"
echo "$out" | grep -qF '(translation_unit (preproc_include path: (system_lib_string)) (function_definition type: (primitive_type)' \
  || fail "parse --lang c sexp head: $out"
echo "$out" | grep -qF '(call_expression function: (identifier) arguments: (argument_list (identifier)))' \
  || fail "parse --lang c gets() call: $out"
ok "parse --lang c: translation_unit with preproc_include and calls"

out="$("$HOST" query --lang c '(call_expression function: (identifier) @fn)' examples/data/sample.c)"
[ "$out" = 'identifier 4:2-4:6
identifier 5:2-5:8' ] || fail "query --lang c captures: $out"
ok "query --lang c: called-function identifiers (gets, printf)"

set +e; out="$("$HOST" parse --lang rust examples/data/sample.c 2>&1)"; code=$?; set -e
[ "$code" = 2 ] && [[ "$out" == *"unknown language 'rust' (known: json, c, python, bash, java, go, javascript, typescript, tsx, lmscript, lmcode)"* ]] || fail "unknown --lang (exit $code): $out"
ok "unknown --lang refused with the known-languages list (exit 2)"

# --- Go grammar (tree-sitter-go) ---
out="$("$HOST" parse --lang go examples/data/sample.go)"
echo "$out" | grep -qF '(source_file (package_clause (package_identifier)) (import_declaration (import_spec path: (interpreted_string_literal (interpreted_string_literal_content)))) (function_declaration name: (identifier)' \
  || fail "parse --lang go sexp head: $out"
out="$("$HOST" query --lang go '(call_expression function: (selector_expression field: (field_identifier) @fn))' examples/data/sample.go)"
[ "$out" = 'field_identifier 5:5-5:12' ] || fail "query --lang go captures: $out"
ok "parse/query --lang go: package, imports, functions and selector calls"

# --- JavaScript grammar (tree-sitter-javascript) ---
out="$("$HOST" parse --lang javascript examples/data/sample.js)"
echo "$out" | grep -qF '(program (import_statement (import_clause' || fail "parse --lang javascript: $out"
out="$("$HOST" query --lang javascript '(function_declaration name: (identifier) @name)' examples/data/sample.js)"
[ "$out" = 'identifier 2:9-2:16' ] || fail "query --lang javascript captures: $out"
ok "parse/query --lang javascript: imports, functions and calls"

# --- TypeScript grammar (tree-sitter-typescript) ---
out="$("$HOST" parse --lang typescript examples/data/sample.ts)"
echo "$out" | grep -qF '(program (import_statement' || fail "parse --lang typescript: $out"
out="$("$HOST" query --lang typescript '(function_declaration name: (identifier) @name)' examples/data/sample.ts)"
[ "$out" = 'identifier 11:9-11:16' ] || fail "query --lang typescript captures: $out"
ok "parse/query --lang typescript: interfaces, functions and type annotations"

# --- TSX grammar (tree-sitter-typescript, separate parser) ---
out="$("$HOST" parse --lang tsx examples/data/sample.tsx)"
echo "$out" | grep -qF '(program (interface_declaration name: (type_identifier) body:' || fail "parse --lang tsx: $out"
out="$("$HOST" query --lang tsx '(function_declaration name: (identifier) @name)' examples/data/sample.tsx)"
[ "$out" = 'identifier 2:16-2:24' ] || fail "query --lang tsx captures: $out"
ok "parse/query --lang tsx: JSX and function declarations"

# --- query predicates (E1) ---
printf '{"pass": 1, "word": 2}' > "$WORK/mini.json"
out="$("$HOST" query '(pair key: (string (string_content) @k) (#eq? @k "pass"))' "$WORK/mini.json")"
[ "$out" = 'string_content 0:2-0:6' ] || fail "predicate eq exact: $out"
ok "query predicate: #eq? exact text match fires"

out="$("$HOST" query '(pair key: (string (string_content) @k) (#eq? @k "pas"))' "$WORK/mini.json")"
[ -z "$out" ] || fail "predicate eq substring must not fire: $out"
ok "query predicate: #eq? substring does not fire"

out="$("$HOST" query '(pair key: (string (string_content) @k) (#not-eq? @k "pass"))' "$WORK/mini.json")"
[ "$out" = 'string_content 0:13-0:17' ] || fail "predicate not-eq: $out"
ok "query predicate: #not-eq? excludes the equal capture"

# upstream tree-sitter evaluates #match? UNANCHORED (regex is_match); authors
# anchor with ^...$ themselves — pin both halves of that contract.
out="$("$HOST" query '(pair key: (string (string_content) @k) (#match? @k "pas"))' "$WORK/mini.json")"
[ "$out" = 'string_content 0:2-0:6' ] || fail "predicate match unanchored: $out"
out="$("$HOST" query '(pair key: (string (string_content) @k) (#match? @k "^pas$"))' "$WORK/mini.json")"
[ -z "$out" ] || fail "predicate match anchored must not fire on 'pass': $out"
ok "query predicate: #match? is unanchored (substring fires, ^...\$ does not)"

out="$("$HOST" query '(pair key: (string (string_content) @k) (#not-match? @k "^pas"))' "$WORK/mini.json")"
[ "$out" = 'string_content 0:13-0:17' ] || fail "predicate not-match: $out"
ok "query predicate: #not-match? excludes regex hits"

out="$("$HOST" query '((string_content) @c (#set! "role" "x"))' "$WORK/mini.json")"
[ "$(grep -c string_content <<<"$out")" = 2 ] || fail "unknown directive must be ignored: $out"
ok "query predicate: unknown directives (#set!) are ignored, match kept"

# --- lint (rule-pack dispatcher) ---
out="$("$HOST" lint examples/rules/c-security.json examples/data/sample.c)"
[ "$out" = '{"row":4,"col":2,"end_row":4,"end_col":6,"rule":"LBC001","severity":"critical","message":"unsafe call '"'"'gets'"'"'"}
1 findings in examples/data/sample.c' ] || fail "lint c-security: $out"
ok "lint: c-security pack over sample.c (query + text_matches)"

out="$("$HOST" lint examples/rules/json-config.json examples/data/bad-config.json)"
[ "$out" = '{"row":2,"col":3,"end_row":2,"end_col":11,"rule":"LBJ001","severity":"critical","message":"password key \"pass[REDACTED]\"","snippet":"sha256:5e884898da28047151d0e56f8dc6292773603d0d6aabbdd62a11ef721d1542d8"}
{"row":3,"col":13,"end_row":3,"end_col":17,"rule":"LBJ002","severity":"minor","message":"placeholder string TODO"}
2 findings in examples/data/bad-config.json' ] || fail "lint json-config: $out"
ok "lint: json-config pack over bad-config.json (sorted by row, col)"

printf 'int main(void) { return 0; }\n' > "$WORK/clean.c"
out="$("$HOST" lint examples/rules/c-security.json "$WORK/clean.c")"
[ "$out" = "0 findings in $WORK/clean.c" ] || fail "lint clean file: $out"
ok "lint: zero findings still prints the summary line (exit 0)"

printf '{"lang":"c","rules":[{"id":"X","query":"(pair","message":"m"}]}' > "$WORK/badquery.json"
set +e; out="$("$HOST" lint "$WORK/badquery.json" examples/data/sample.c 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'rule X: query error at byte offset'* ]] || fail "lint bad query (exit $code): $out"
ok "lint: bad query in pack refused (exit 3, rule id + offset)"

printf '{"lang":"c","rules":[{"id":"Y","on":"comment","query":"(comment)","message":"m"}]}' > "$WORK/both.json"
set +e; out="$("$HOST" lint "$WORK/both.json" examples/data/sample.c 2>&1)"; code=$?; set -e
[ "$code" = 2 ] && [[ "$out" == *"rule Y: 'on' and 'query' are mutually exclusive"* ]] || fail "lint on+query (exit $code): $out"
ok "lint: on+query together refused (exit 2)"

printf 'not json' > "$WORK/broken.json"
set +e; out="$("$HOST" lint "$WORK/broken.json" examples/data/sample.c 2>&1)"; code=$?; set -e
[ "$code" = 2 ] && [[ "$out" == *'invalid JSON'* ]] || fail "lint malformed pack (exit $code): $out"
ok "lint: malformed pack refused (exit 2)"

set +e; out="$("$HOST" lint examples/rules/c-security.json "$WORK/no-such.c" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'cannot read'* ]] || fail "lint missing file (exit $code): $out"
ok "lint: unreadable file aborts fast (exit 3)"

# --- lint: directory expansion (E4) ---
"$HOST" lint examples/rules/c-security.json tests/fixtures/lintdir >"$WORK/lintdir.out" || fail "lint dir run"
cat >"$WORK/lintdir.expected" <<'EXPECTED_DIR'
{"row":2,"col":2,"end_row":2,"end_col":6,"rule":"LBC001","severity":"critical","message":"unsafe call 'gets'"}
{"row":3,"col":2,"end_row":3,"end_col":12,"rule":"LBC003","severity":"minor","message":"goto used"}
2 findings in tests/fixtures/lintdir/a.c
{"row":0,"col":0,"end_row":0,"end_col":24,"rule":"LBC002","severity":"minor","message":"unresolved FIXME comment"}
1 findings in tests/fixtures/lintdir/b.c
EXPECTED_DIR
cmp "$WORK/lintdir.expected" "$WORK/lintdir.out" || { diff "$WORK/lintdir.expected" "$WORK/lintdir.out" >&2; fail "lint dir output"; }
ok "lint: directory expands recursively (sorted, hidden/.txt skipped, .json skipped for c pack)"

"$HOST" lint examples/rules/c-security.json examples/data/sample.c tests/fixtures/lintdir >"$WORK/lintmixed.out" || fail "lint mixed run"
cat >"$WORK/lintmixed.expected" <<'EXPECTED_MIX'
{"row":4,"col":2,"end_row":4,"end_col":6,"rule":"LBC001","severity":"critical","message":"unsafe call 'gets'"}
1 findings in examples/data/sample.c
{"row":2,"col":2,"end_row":2,"end_col":6,"rule":"LBC001","severity":"critical","message":"unsafe call 'gets'"}
{"row":3,"col":2,"end_row":3,"end_col":12,"rule":"LBC003","severity":"minor","message":"goto used"}
2 findings in tests/fixtures/lintdir/a.c
{"row":0,"col":0,"end_row":0,"end_col":24,"rule":"LBC002","severity":"minor","message":"unresolved FIXME comment"}
1 findings in tests/fixtures/lintdir/b.c
EXPECTED_MIX
cmp "$WORK/lintmixed.expected" "$WORK/lintmixed.out" || fail "lint mixed file+dir output"
ok "lint: mixed file + directory args in order"

mkdir -p "$WORK/emptydir"
set +e; out="$("$HOST" lint examples/rules/c-security.json "$WORK/emptydir" 2>&1)"; code=$?; set -e
[ "$code" = 2 ] && [[ "$out" == *'no lintable files in'* ]] || fail "lint empty dir (exit $code): $out"
ok "lint: directory with no lintable files refused (exit 2)"

# --- tier-1 rule packs (canonical library in rules/) ---
"$HOST" lint rules/c-tier1.json tests/fixtures/c-tier1-violations.c >"$WORK/c-tier1.out" || fail "lint c-tier1 violations run"
cat >"$WORK/c-tier1.expected" <<'EXPECTED_C'
{"row":3,"col":0,"end_row":4,"end_col":0,"rule":"LBC007","severity":"minor","message":"object-like macro (prefer const or enum)"}
{"row":5,"col":4,"end_row":5,"end_col":11,"rule":"LBC009","severity":"minor","message":"function name not snake_case: BadName"}
{"row":6,"col":2,"end_row":6,"end_col":10,"rule":"LBC008","severity":"minor","message":"register storage class"}
{"row":8,"col":2,"end_row":8,"end_col":6,"rule":"LBC001","severity":"critical","message":"unsafe call 'gets'"}
{"row":9,"col":2,"end_row":9,"end_col":8,"rule":"LBC001","severity":"critical","message":"unsafe call 'strcat'"}
{"row":10,"col":2,"end_row":10,"end_col":8,"rule":"LBC004","severity":"critical","message":"command exec call 'system'"}
{"row":11,"col":2,"end_row":11,"end_col":7,"rule":"LBC004","severity":"critical","message":"command exec call 'popen'"}
{"row":12,"col":6,"end_row":12,"end_col":11,"rule":"LBC005","severity":"major","message":"assignment in condition (meant ==?)"}
{"row":13,"col":9,"end_row":13,"end_col":14,"rule":"LBC005","severity":"major","message":"assignment in condition (meant ==?)"}
{"row":14,"col":2,"end_row":14,"end_col":12,"rule":"LBC003","severity":"minor","message":"goto used"}
{"row":15,"col":2,"end_row":15,"end_col":19,"rule":"LBC010","severity":"minor","message":"commented-out code"}
{"row":16,"col":2,"end_row":16,"end_col":29,"rule":"LBC002","severity":"minor","message":"unresolved FIXME/TODO comment"}
{"row":20,"col":0,"end_row":22,"end_col":13,"rule":"LBC006","severity":"major","message":"K&R function definition (use ANSI prototypes)"}
{"row":27,"col":20,"end_row":27,"end_col":26,"rule":"LBC011","severity":"minor","message":"cast of 'malloc' result (redundant in C, can hide a missing prototype)"}
14 findings in tests/fixtures/c-tier1-violations.c
EXPECTED_C
cmp "$WORK/c-tier1.expected" "$WORK/c-tier1.out" || { diff "$WORK/c-tier1.expected" "$WORK/c-tier1.out" >&2; fail "c-tier1 violations output"; }
out="$("$HOST" lint rules/c-tier1.json tests/fixtures/c-tier1-clean.c)"
[ "$out" = "0 findings in tests/fixtures/c-tier1-clean.c" ] || fail "c-tier1 clean: $out"
ok "lint: c-tier1 pack, all 11 rules fire on violations, clean fixture silent"

"$HOST" lint rules/json-tier1.json tests/fixtures/json-tier1-violations.json >"$WORK/json-tier1.out" || fail "lint json-tier1 violations run"
cat >"$WORK/json-tier1.expected" <<'EXPECTED_J'
{"row":1,"col":3,"end_row":1,"end_col":14,"rule":"LBJ004","severity":"minor","message":"key not snake_case: ServiceName"}
{"row":3,"col":5,"end_row":3,"end_col":13,"rule":"LBJ001","severity":"critical","message":"credential key \"pass[REDACTED]\"","snippet":"sha256:5e884898da28047151d0e56f8dc6292773603d0d6aabbdd62a11ef721d1542d8"}
{"row":3,"col":5,"end_row":3,"end_col":13,"rule":"LBJ104","severity":"critical","message":"exact password key","snippet":"sha256:5e884898da28047151d0e56f8dc6292773603d0d6aabbdd62a11ef721d1542d8"}
{"row":5,"col":3,"end_row":5,"end_col":16,"rule":"LBJ001","severity":"critical","message":"credential key \"pass[REDACTED]\"","snippet":"sha256:8225f9a40596dbdf5e29ec9f0aecf137b72b57f48afa01d3c2296f1896972b40"}
{"row":6,"col":3,"end_row":6,"end_col":13,"rule":"LBJ001","severity":"critical","message":"credential key \"api_[REDACTED]\"","snippet":"sha256:87fd0842be8454da3063361d336189da097ae5830cded00c2c42a6fc39e9f188"}
{"row":7,"col":13,"end_row":7,"end_col":17,"rule":"LBJ002","severity":"minor","message":"placeholder string TODO"}
{"row":8,"col":11,"end_row":8,"end_col":15,"rule":"LBJ003","severity":"minor","message":"null value present"}
{"row":9,"col":11,"end_row":9,"end_col":15,"rule":"LBJ005","severity":"minor","message":"number encoded as string: 8080"}
{"row":10,"col":2,"end_row":10,"end_col":14,"rule":"LBJ006","severity":"critical","message":"sensitive credential key aws_[REDACTED]","snippet":"sha256:d96aaff5bffd28ff4cdb78f4a58ebea96ac69c476b4a1910a4736bb77d3c0e74"}
{"row":10,"col":3,"end_row":10,"end_col":13,"rule":"LBJ001","severity":"critical","message":"credential key \"aws_[REDACTED]\"","snippet":"sha256:4f85c7065125d71388d2c77489418da49369e932afcf70bd4662752be2a5ac6e"}
{"row":11,"col":2,"end_row":11,"end_col":15,"rule":"LBJ006","severity":"critical","message":"sensitive credential key priv[REDACTED]","snippet":"sha256:cb75d3f6e6085a723a4e08a49f2350189566164f2413fe54563d52be07b7c55f"}
{"row":12,"col":2,"end_row":12,"end_col":11,"rule":"LBJ006","severity":"critical","message":"sensitive credential key api_[REDACTED]","snippet":"sha256:de9685549b5f39b6143a1c1504d0b3940ec3b87ef03d8572df6333412fbd1fdb"}
{"row":13,"col":14,"end_row":13,"end_col":35,"rule":"LBJ007","severity":"major","message":"insecure HTTP URL"}
{"row":15,"col":4,"end_row":15,"end_col":24,"rule":"LBJ007","severity":"major","message":"insecure HTTP URL"}
{"row":17,"col":13,"end_row":17,"end_col":16,"rule":"LBJ008","severity":"minor","message":"unpinned version \"*\""}
{"row":18,"col":16,"end_row":18,"end_col":24,"rule":"LBJ008","severity":"minor","message":"unpinned version \"latest\""}
{"row":19,"col":19,"end_row":19,"end_col":21,"rule":"LBJ009","severity":"minor","message":"empty container placeholder"}
{"row":20,"col":20,"end_row":20,"end_col":22,"rule":"LBJ009","severity":"minor","message":"empty container placeholder"}
{"row":21,"col":15,"end_row":21,"end_col":218,"rule":"LBJ010","severity":"minor","message":"string exceeds 200 source bytes"}
{"row":22,"col":23,"end_row":22,"end_col":227,"rule":"LBJ010","severity":"minor","message":"string exceeds 200 source bytes"}
20 findings in tests/fixtures/json-tier1-violations.json
EXPECTED_J
cmp "$WORK/json-tier1.expected" "$WORK/json-tier1.out" || { diff "$WORK/json-tier1.expected" "$WORK/json-tier1.out" >&2; fail "json-tier1 violations output"; }
out="$("$HOST" lint rules/json-tier1.json tests/fixtures/json-tier1-clean.json)"
[ "$out" = "0 findings in tests/fixtures/json-tier1-clean.json" ] || fail "json-tier1 clean: $out"
ok "lint: json-tier1 pack, all 11 rules fire on violations, clean fixture silent"

# --- DSL bytecode ---
cat > "$WORK/smoke.lms" <<EOF
p = _lmcode_ cli="lmok" install="github:lmctl-ai/lmok@latest";
let tree = ts.parse("{\"name\": \"lmok\", \"tags\": [\"tree-sitter\", \"json\"]}");
println(tree.sexp());
println("has_error -> " + str(tree.has_error()));
println("nodes -> " + str(tree.node_count()));
end _lmcode_
EOF
compile_fixture "$WORK/smoke.lms" "$WORK/smoke.dslb" >/dev/null || fail "smoke compile"
[ "$(od -An -t u1 -j 5 -N 1 "$WORK/smoke.dslb" | tr -d ' ')" = 6 ] || fail "smoke was not compiled as bytecode v6"
out="$("$HOST" --lmcode "$WORK/smoke.dslb")"
[ "$(tail -1 <<<"$out")" = 'LMCODE_RETURN_END' ] || fail "smoke run: $out"
grep -qxF '{"ok":true,"result":null}' <<<"$out" || fail "smoke run protocol: $out"
grep -qxF '(document (object (pair key: (string (string_content)) value: (string (string_content))) (pair key: (string (string_content)) value: (array (string (string_content)) (string (string_content))))))' <<<"$out" || fail "smoke sexp line: $out"
grep -q '^has_error -> false$' <<<"$out" || fail "smoke has_error line: $out"
grep -q '^nodes -> 34$' <<<"$out" || fail "smoke node_count line: $out"
ok "bytecode: ts.parse/sexp/has_error/node_count through the DSL"

cat > "$WORK/error-tree.lms" <<EOF
p = _lmcode_ cli="lmok" install="github:lmctl-ai/lmok@latest";
let tree = ts.parse("{\"a\": 1,,}");
if (!tree.has_error()) { println("bad"); }
println("error flagged");
end _lmcode_
EOF
compile_fixture "$WORK/error-tree.lms" "$WORK/error-tree.dslb" >/dev/null || fail "error-tree compile"
out="$("$HOST" --lmcode "$WORK/error-tree.dslb")"
grep -q '^error flagged$' <<<"$out" || fail "malformed JSON must set has_error: $out"
ok "bytecode: malformed JSON flags Tree.has_error"

cat > "$WORK/wrong-arg.lms" <<EOF
p = _lmcode_ cli="lmok" install="github:lmctl-ai/lmok@latest";
ts.parse(42);
end _lmcode_
EOF
set +e; compile_fixture "$WORK/wrong-arg.lms" "$WORK/wrong-arg.dslb" >"$WORK/type-error.txt" 2>&1; code=$?; set -e
[ "$code" = 2 ] || fail "wrong-argument call must be a compile error (exit $code)"
ok "type mismatch refused at compile (exit 2)"

cat > "$WORK/oob.lms" <<EOF
p = _lmcode_ cli="lmok" install="github:lmctl-ai/lmok@latest";
let names = str_list().add("a");
names.at(5);
end _lmcode_
EOF
compile_fixture "$WORK/oob.lms" "$WORK/oob.dslb" >/dev/null || fail "compile"
set +e; out="$("$HOST" --lmcode "$WORK/oob.dslb" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'out of range'* ]] || fail "StrList.at bounds (exit $code): $out"
ok "StrList.at bounds refused at runtime (exit 3)"

cat > "$WORK/bad-pattern.lms" <<EOF
p = _lmcode_ cli="lmok" install="github:lmctl-ai/lmok@latest";
let c = ts.parse_c("int x;\\n");
ts.match_c(c.root(), "strcpy(\$A");
end _lmcode_
EOF
compile_fixture "$WORK/bad-pattern.lms" "$WORK/bad-pattern.dslb" >/dev/null || fail "bad-pattern compile"
set +e; out="$("$HOST" --lmcode "$WORK/bad-pattern.dslb" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *"pattern 'strcpy(\$A' does not parse as c"* ]] || fail "ts.match_c bad pattern (exit $code): $out"
ok "bytecode: ts.match_c with an unparseable pattern is a runtime error naming the fragment (exit 3)"

cat > "$WORK/root-parent.lms" <<EOF
p = _lmcode_ cli="lmok" install="github:lmctl-ai/lmok@latest";
let r = ts.parse("{}").root();
if (r.has_parent()) { println("bad"); }
r.parent();
end _lmcode_
EOF
compile_fixture "$WORK/root-parent.lms" "$WORK/root-parent.dslb" >/dev/null || fail "root-parent compile"
set +e; out="$("$HOST" --lmcode "$WORK/root-parent.dslb" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'root node has no parent'* ]] || fail "Node.parent on root (exit $code): $out"
ok "Node.parent: has_parent() false at root, parent() on root refused (exit 3)"

set +e; out="$("$HOST" 2>&1)"; code=$?; set -e
[ "$code" = 2 ] && [[ "$out" == *'usage: lmok'* ]] || fail "bad args refusal (exit $code): $out"
ok "bad arguments refused (exit 2)"
set +e; out="$("$HOST" --lmcode "$WORK/no-such.dslb" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'"phase":"load"'* ]] || fail "missing bytecode refusal (exit $code): $out"
ok "missing bytecode file refused at load (exit 3)"

"$HOST" --lmcode "$WORK/smoke.dslb" --lmcode "$WORK/error-tree.dslb" >"$WORK/multi.jsonl" || fail "multi-bytecode run"
grep -qxF '{"ok":true,"result":[null,null],"parts":2}' "$WORK/multi.jsonl" || fail "multiple bytecode files did not run in order"
ok "multiple bytecode files run sequentially"

# --- examples compile and run ---
mkdir -p build
for f in parse-string parse-file detect-error cst-rules lint-json report-json query-json lint-c rule-catalog suppress-findings sarif-report incremental-reparse pattern-rules autofix-report; do
  compile_fixture "examples/$f.lms" "build/$f.dslb" >"$WORK/$f.compile.log" 2>&1 || fail "$f example compile"
  "$HOST" --lmcode "build/$f.dslb" >"$WORK/$f.out" || fail "$f example run"
  # every '# expect:' line must appear in the program output
  while IFS= read -r line; do
    grep -qxF "${line:2}" "$WORK/$f.out" || fail "$f example output missing: ${line:2} (got $(cat "$WORK/$f.out"))"
  done < <(grep '^#|' "examples/$f.lms")
done
ok "examples compile and their expected output matches"

# --- SARIF artifact (written by examples/sarif-report.lms above) ---
python3 - <<'PY'
import json
doc = json.load(open("build/sarif-results.sarif"))
assert doc["version"] == "2.1.0", doc["version"]
run = doc["runs"][0]
assert run["tool"]["driver"]["name"] == "lmok"
assert [r["id"] for r in run["tool"]["driver"]["rules"]] == ["LBC001", "LBC002"]
results = run["results"]
assert len(results) == 4, len(results)
region = results[0]["locations"][0]["physicalLocation"]["region"]
# first finding is the gets() call at 0-based row 5 col 2 -> 1-based 6:3
assert (region["startLine"], region["startColumn"]) == (6, 3), region
assert results[0]["level"] == "error" and results[-1]["level"] == "note"
assert results[-1]["locations"][0]["physicalLocation"]["artifactLocation"]["uri"] == "src/sample.c"
print("SARIF 2.1.0: 4 results, regions and levels correct")
PY
ok "SARIF artifact is valid JSON with the expected structure"

# --- lint-repo.lms (outer lmscript driving the CLI dispatcher) ---
"$LMSCRIPT" examples/lint-repo.lms >"$WORK/lint-repo.out" 2>"$WORK/lint-repo.err" ||
  { cat "$WORK/lint-repo.err" >&2; fail "lint-repo example run"; }
while IFS= read -r line; do
  grep -qxF "${line:2}" "$WORK/lint-repo.out" || fail "lint-repo output missing: ${line:2} (got $(cat "$WORK/lint-repo.out"))"
done < <(grep '^#|' examples/lint-repo.lms)
ok "lint-repo: lmscript drives lmok lint and counts findings"

"$LMSCRIPT" examples/lint-go.lms >"$WORK/lint-go.out" 2>"$WORK/lint-go.err" ||
  { cat "$WORK/lint-go.err" >&2; fail "lint-go example run"; }
while IFS= read -r line; do
  grep -qxF "${line:2}" "$WORK/lint-go.out" || fail "lint-go output missing: ${line:2} (got $(cat "$WORK/lint-go.out"))"
done < <(grep '^#|' examples/lint-go.lms)
ok "lint-go: lmscript drives the Go tier-1 pack"

"$LMSCRIPT" examples/lint-javascript.lms >"$WORK/lint-javascript.out" 2>"$WORK/lint-javascript.err" ||
  { cat "$WORK/lint-javascript.err" >&2; fail "lint-javascript example run"; }
while IFS= read -r line; do
  grep -qxF "${line:2}" "$WORK/lint-javascript.out" || fail "lint-javascript output missing: ${line:2} (got $(cat "$WORK/lint-javascript.out"))"
done < <(grep '^#|' examples/lint-javascript.lms)
ok "lint-javascript: lmscript drives the JavaScript tier-1 pack"

"$LMSCRIPT" examples/lint-typescript.lms >"$WORK/lint-typescript.out" 2>"$WORK/lint-typescript.err" ||
  { cat "$WORK/lint-typescript.err" >&2; fail "lint-typescript example run"; }
while IFS= read -r line; do
  grep -qxF "${line:2}" "$WORK/lint-typescript.out" || fail "lint-typescript output missing: ${line:2} (got $(cat "$WORK/lint-typescript.out"))"
done < <(grep '^#|' examples/lint-typescript.lms)
ok "lint-typescript: lmscript drives the TypeScript tier-1 pack"

"$LMSCRIPT" examples/lint-tsx.lms >"$WORK/lint-tsx.out" 2>"$WORK/lint-tsx.err" ||
  { cat "$WORK/lint-tsx.err" >&2; fail "lint-tsx example run"; }
while IFS= read -r line; do
  grep -qxF "${line:2}" "$WORK/lint-tsx.out" || fail "lint-tsx output missing: ${line:2} (got $(cat "$WORK/lint-tsx.out"))"
done < <(grep '^#|' examples/lint-tsx.lms)
ok "lint-tsx: lmscript drives the TSX tier-1 pack"

# Entry rules declare their language/input in a non-executable test header.
# Aggregate scripts without that header retain the body-only --lmcode path.
run_rule() {
  local rule_source="$1" rule_bytecode="$2" rule_spec rule_lang rule_input
  rule_spec="$(sed -n 's/^# check: //p' "$rule_source")"
  if [ -n "$rule_spec" ]; then
    read -r rule_lang rule_input <<< "$rule_spec"
    "$HOST" check --lang "$rule_lang" "$rule_bytecode" "$rule_input"
  else
    "$HOST" --lmcode "$rule_bytecode"
  fi
}

# --- rules/lmcode (lmcode-native tier-1 rules) ---
mkdir -p build/rules-lmcode
for f in c-function-metrics c-switch-style json-duplicate-values json-duplicate-keys json-size-limits c-nesting-depth json-nesting-depth c-scanf-width c-symbol-rules c-cross-file; do
  compile_fixture "rules/lmcode/$f.lms" "build/rules-lmcode/$f.dslb" >"$WORK/$f.compile.log" 2>&1 || fail "$f rule compile"
  run_rule "rules/lmcode/$f.lms" "build/rules-lmcode/$f.dslb" >"$WORK/$f.out" || fail "$f rule run"
  while IFS= read -r line; do
    grep -qxF "${line:2}" "$WORK/$f.out" || fail "$f output missing: ${line:2} (got $(cat "$WORK/$f.out"))"
  done < <(grep '^#|' "rules/lmcode/$f.lms")
  # clean-fixture variant: substitute the fixture path AND the fixture output target
  # into a scratch copy, expect an empty findings array and a zero count
  sed -e 's/-violations\./-clean./' -e 's|cross/a\.c|cross/clean-a.c|g' -e 's|cross/b\.c|cross/clean-b.c|g' \
      -e "s|build/rules-lmcode/$f.dslb|$WORK/$f-clean.dslb|" \
    "rules/lmcode/$f.lms" > "$WORK/$f-clean.lms"
  compile_fixture "$WORK/$f-clean.lms" "$WORK/$f-clean.dslb" >/dev/null 2>&1 || fail "$f clean compile"
  out="$(run_rule "$WORK/$f-clean.lms" "$WORK/$f-clean.dslb")"
  grep -qxF '[]' <<<"$out" || fail "$f clean fixture findings: $out"
  grep -qx '0 findings\( in .*\)\?' <<<"$out" || fail "$f clean fixture count: $out"
done
ok "rules/lmcode: ten rule files fire on violations fixtures, silent on clean"

# --- incremental reparse contract (E5) ---
cat > "$WORK/reparse-contract.lms" <<EOF2
p = _lmcode_ cli="lmok" install="github:lmctl-ai/lmok@latest";
let old = ts.parse("{}");
old.edit(1, 1, 2);
let new = ts.reparse(old, "{ }");
println("ranges " + ts.changed_ranges(old, new));
println("old root kind " + old.root().kind());
end _lmcode_
EOF2
compile_fixture "$WORK/reparse-contract.lms" "$WORK/reparse-contract.dslb" >/dev/null || fail "reparse-contract compile"
out="$("$HOST" --lmcode "$WORK/reparse-contract.dslb")"
grep -q '^ranges \[\[1,2\]\]$' <<<"$out" || fail "changed_ranges: $out"
grep -q '^old root kind document$' <<<"$out" || fail "old tree alive after reparse: $out"
ok "reparse: old tree stays alive (changed_ranges + root kind readable)"

cat > "$WORK/bad-edit.lms" <<EOF2
p = _lmcode_ cli="lmok" install="github:lmctl-ai/lmok@latest";
let t = ts.parse("{}");
t.edit(5, 1, 5);
end _lmcode_
EOF2
compile_fixture "$WORK/bad-edit.lms" "$WORK/bad-edit.dslb" >/dev/null || fail "bad-edit compile"
set +e; out="$("$HOST" --lmcode "$WORK/bad-edit.dslb" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'bad byte offsets'* ]] || fail "edit out of range (exit $code): $out"
ok "Tree.edit: out-of-range offsets refused (exit 3)"

# --- review fixes: regex crash, reparse contract, arena caps, directory walk, predicates ---
# C1: std::regex backtracks recursively; a 100 KB single-line comment with the
# shipped LBC010 rule used to segfault (exit 139). Now bounded (16 KiB cap,
# one stderr note), both for pack text_matches and query #match?.
# (binary write: Python's text mode would emit CRLF on Windows and shift the byte count)
python3 -c "import sys; sys.stdout.buffer.write(b'// x = ' + b'a'*100000 + b';\n')" > "$WORK/longcomment.c"
set +e; out="$("$HOST" lint rules/c-tier1.json "$WORK/longcomment.c" 2>"$WORK/longcomment.err")"; code=$?; set -e
[ "$code" = 0 ] && [ "$out" = "0 findings in $WORK/longcomment.c" ] || fail "long comment lint (exit $code): $out"
grep -qE 'regex not applied to a [0-9]+-byte node text' "$WORK/longcomment.err" || fail "long comment lint: missing stderr note: $(cat "$WORK/longcomment.err")"
set +e; out="$("$HOST" query --lang c '((comment) @c (#match? @c "(.*)*b"))' "$WORK/longcomment.c" 2>/dev/null)"; code=$?; set -e
[ "$code" = 0 ] && [ -z "$out" ] || fail "long comment #match? (exit $code): $out"
ok "regex over node text is bounded: 100 KB comment lints and queries without crashing (one stderr note)"

# M1: reparse without an edit must not hand back the old tree for new text.
cat > "$WORK/reparse-noedit.lms" <<EOF2
p = _lmcode_ cli="lmok" install="github:lmctl-ai/lmok@latest";
let same = ts.parse("{}");
println("same " + ts.reparse(same, "{}").sexp());
let old = ts.parse("{}");
let new = ts.reparse(old, "[1, 2, 3]");
println("unreachable " + new.sexp());
end _lmcode_
EOF2
compile_fixture "$WORK/reparse-noedit.lms" "$WORK/reparse-noedit.dslb" >/dev/null || fail "reparse-noedit compile"
set +e; out="$("$HOST" --lmcode "$WORK/reparse-noedit.dslb" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'no pending edit for changed text'* ]] && grep -q '^same (document (object))$' <<<"$out" \
  || fail "reparse without edit (exit $code): $out"
cat > "$WORK/reparse-badlen.lms" <<EOF2
p = _lmcode_ cli="lmok" install="github:lmctl-ai/lmok@latest";
let t = ts.parse("{\"a\": 1}");
t.edit(1, 1, 50);
let n = ts.reparse(t, "{\"a\": 1}");
end _lmcode_
EOF2
compile_fixture "$WORK/reparse-badlen.lms" "$WORK/reparse-badlen.dslb" >/dev/null || fail "reparse-badlen compile"
set +e; out="$("$HOST" --lmcode "$WORK/reparse-badlen.dslb" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'new text is 8 bytes but the pending edit implies 57'* ]] || fail "reparse inconsistent edit (exit $code): $out"
ok "reparse contract: identical text ok, changed text without edit refused, edit/text length mismatch refused (exit 3)"

# M2: iterating a NodeList allocates no handles, so a 180k-node tree walks in one run.
python3 - "$WORK/many.c" <<'PY'
import sys
with open(sys.argv[1], 'w') as f:
    f.write('#include <stdio.h>\n')
    for i in range(10000):
        f.write(f'int f{i}(int a, int b) {{ return a + b * {i}; }}\n')
PY
cat > "$WORK/bigwalk.lms" <<EOF2
p = _lmcode_ cli="lmok" install="github:lmctl-ai/lmok@latest";
let t = ts.parse_c(file_read("$WORK/many.c"));
let ns = t.nodes();
let c = 0;
for (n in ns) { if (n.kind() == "identifier") { c = c + 1; } }
println("named " + str(ns.len()) + " identifiers " + str(c));
end _lmcode_
EOF2
compile_fixture "$WORK/bigwalk.lms" "$WORK/bigwalk.dslb" >/dev/null || fail "bigwalk compile"
out="$("$HOST" --lmcode "$WORK/bigwalk.dslb")" || fail "bigwalk run: $out"
grep -q '^named 180003 identifiers 50000$' <<<"$out" || fail "bigwalk output: $out"
ok "NodeList iteration over a 180k-node C tree runs in one program (no per-visit Node handles)"

# M3: arenas reset per --lmcode file: 3 runs x 400k root() handles would exceed
# the 1<<20 Node cap if the arena carried over; findings reset too.
cat > "$WORK/handles.lms" <<EOF2
p = _lmcode_ cli="lmok" install="github:lmctl-ai/lmok@latest";
let t = ts.parse("{}");
let r = t.root();
for (i in range(0, 400000)) { r = t.root(); }
ts.report(r, "one per run");
println("findings " + str(ts.report_count()));
end _lmcode_
EOF2
compile_fixture "$WORK/handles.lms" "$WORK/handles.dslb" >/dev/null || fail "handles compile"
out="$("$HOST" --lmcode "$WORK/handles.dslb" --lmcode "$WORK/handles.dslb" --lmcode "$WORK/handles.dslb")" || fail "handles x3 run: $out"
[ "$(grep -c '^findings [123]$' <<<"$out")" = 3 ] && [ "$(grep -c '^LMCODE_RETURN_END$' <<<"$out")" = 1 ] || fail "handles x3 output: $out"
ok "--lmcode shared invocation context: 3 x 400k node visits retain findings in one envelope"

# M4: an iterator error during directory expansion fails fast (exit 3), never a silent partial list.
if [ "$(id -u 2>/dev/null || echo 0)" != 0 ] && [[ "$UNAME_S" != MINGW* && "$UNAME_S" != MSYS* && "$UNAME_S" != CYGWIN* ]]; then
  mkdir -p "$WORK/walkerr/ok" "$WORK/walkerr/locked"
  printf 'int main(void) { return 0; }\n' > "$WORK/walkerr/ok/z.c"
  chmod 000 "$WORK/walkerr/locked"
  set +e; out="$("$HOST" lint examples/rules/c-security.json "$WORK/walkerr" 2>&1)"; code=$?; set -e
  chmod 755 "$WORK/walkerr/locked"
  [ "$code" = 3 ] && [[ "$out" == *'cannot read '*'walkerr/locked'* ]] || fail "lint unreadable subdir (exit $code): $out"
  ok "lint: unreadable subdirectory aborts the run (exit 3) instead of silently dropping files"
else
  ok "lint: unreadable-subdirectory check skipped (root or Windows cannot make a directory unreadable)"
fi

# M5: hidden-path filtering applies below the argument root only.
mkdir -p "$WORK/.dotproj/src" "$WORK/.dotproj/.git"
cp tests/fixtures/lintdir/a.c "$WORK/.dotproj/src/a.c"
cp tests/fixtures/lintdir/a.c "$WORK/.dotproj/.git/skip.c"
out="$("$HOST" lint examples/rules/c-security.json "$WORK/.dotproj")" || fail "lint dot-root dir run"
[ "$(tail -1 <<<"$out")" = "2 findings in $WORK/.dotproj/src/a.c" ] && ! grep -q 'skip.c' <<<"$out" || fail "lint dot-root dir: $out"
ok "lint: a directory argument under a dot-path is lintable; hidden entries below it are still skipped"

# M6: predicate semantics follow upstream; malformed or unknown predicates are refused.
printf '{"a": "a", "b": "c"}' > "$WORK/pred.json"
out="$("$HOST" query '((pair key: (string (string_content) @k) value: (string (string_content) @v)) (#eq? @k @v))' "$WORK/pred.json")"
[ "$out" = 'string_content 0:2-0:3
string_content 0:7-0:8' ] || fail "eq capture-vs-capture: $out"
out="$("$HOST" query '((pair key: (string (string_content) @k) value: (string (string_content) @v)) (#not-eq? @k @v))' "$WORK/pred.json")"
[ "$out" = 'string_content 0:12-0:13
string_content 0:17-0:18' ] || fail "not-eq capture-vs-capture: $out"
out="$("$HOST" query '((string_content) @s (#any-of? @s "a" "zzz"))' "$WORK/pred.json")"
[ "$(grep -c string_content <<<"$out")" = 2 ] || fail "any-of: $out"
out="$("$HOST" query '((string_content) @s (#not-any-of? @s "a" "b"))' "$WORK/pred.json")"
[ "$out" = 'string_content 0:17-0:18' ] || fail "not-any-of: $out"
printf '[[1, 2], [3, 4]]' > "$WORK/quant.json"
out="$("$HOST" query '((array (number) @n) (#eq? @n "1"))' "$WORK/quant.json")"
[ "$out" = 'number 0:2-0:3' ] || fail "eq? literal: $out"
# any-* must REJECT when no node satisfies (web-binding `some`; the Rust
# binding's loop would pass vacuously) — including an absent optional capture.
out="$("$HOST" query '((array (number) @n) (#any-eq? @n "9"))' "$WORK/quant.json")"
[ -z "$out" ] || fail "any-eq? with no satisfying node must reject: $out"
out="$("$HOST" query '((array (number)? @n) (#any-eq? @n "1"))' "$WORK/quant.json")"
[ "$out" = 'number 0:2-0:3' ] || fail "any-eq? over an absent optional capture must reject: $out"
out="$("$HOST" query '((array (number) @n) (#any-match? @n "^4$"))' "$WORK/quant.json")"
[ "$out" = 'number 0:13-0:14' ] || fail "any-match?: $out"
out="$("$HOST" query '((array (number) @n) (#match? @n "^[34]$"))' "$WORK/quant.json")"
[ "$out" = 'number 0:10-0:11
number 0:13-0:14' ] || fail "match? literal regex: $out"
out="$("$HOST" query '((string_content) @s (#is? local) (#set! "role" "x"))' "$WORK/pred.json")"
[ "$(grep -c string_content <<<"$out")" = 4 ] || fail "is?/set! must be accepted and ignored: $out"
set +e; out="$("$HOST" query '((string_content) @s (#match? @s @s))' "$WORK/pred.json" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'#match? second argument must be a regex literal'* ]] || fail "match? with capture regex (exit $code): $out"
set +e; out="$("$HOST" query '((string_content) @s (#eq? @s))' "$WORK/pred.json" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'#eq? takes 2 arguments, got 1'* ]] || fail "eq? arity (exit $code): $out"
set +e; out="$("$HOST" query '((string_content) @s (#lua-match? @s "x"))' "$WORK/pred.json" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'#lua-match? is not a supported predicate'* ]] || fail "unknown predicate (exit $code): $out"
set +e; out="$("$HOST" query '((string_content) @s (#match? @s "("))' "$WORK/pred.json" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *"#match? has an invalid regex '('"* ]] || fail "invalid regex (exit $code): $out"
ok "query predicates: eq?/not-eq? zip captures, any-of?/any-eq?/any-match? evaluated, is?/set! ignored, malformed/unknown refused (exit 3)"

# M7: rule ids are unique within a pack; the shipped C pack has exactly 10 distinct rules.
printf '{"lang":"c","rules":[{"id":"DUP","on":"comment","message":"a"},{"id":"DUP","on":"goto_statement","message":"b"}]}' > "$WORK/dup.json"
set +e; out="$("$HOST" lint "$WORK/dup.json" examples/data/sample.c 2>&1)"; code=$?; set -e
[ "$code" = 2 ] && [[ "$out" == *'rule DUP: duplicate rule id'* ]] || fail "duplicate rule id (exit $code): $out"
python3 - <<'PY'
import json
for path, want in (("rules/c-tier1.json", 11), ("rules/json-tier1.json", 11), ("rules/go-tier1.json", 6), ("rules/javascript-tier1.json", 4), ("rules/typescript-tier1.json", 3), ("rules/tsx-tier1.json", 3)):
    ids = [r["id"] for r in json.load(open(path))["rules"]]
    assert len(ids) == want and len(set(ids)) == want, (path, ids)
PY
ok "rule packs: duplicate ids refused (exit 2); shipped packs carry unique ids (c 11, json 11, Go 6, JavaScript 4, TypeScript 3, TSX 3)"

# Minor CLI contracts from the same review.
set +e; out="$("$HOST" parse "$WORK" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'is a directory'* ]] || fail "parse on a directory (exit $code): $out"
set +e; out="$("$HOST" parse --lang 2>&1)"; code=$?; set -e
[ "$code" = 2 ] && [[ "$out" == *'--lang needs a value'* ]] || fail "--lang without value (exit $code): $out"
ok "CLI: a directory where a file is expected is refused (exit 3); --lang without a value is a usage error (exit 2)"

if [ "$UNAME_S" = "Darwin" ]; then
  BAD_LIBS="$(otool -L "$HOST" | tail -n +2 | awk '{print $1}' | grep -v '^/usr/lib/\|^/System/Library/' || true)"
  [ -z "$BAD_LIBS" ] || fail "host links non-system libraries: $BAD_LIBS"
  ok "host links only system libraries ($(stat_size "$HOST") bytes, otool -L clean)"
elif [[ "$UNAME_S" == MINGW* || "$UNAME_S" == MSYS* || "$UNAME_S" == CYGWIN* ]]; then
  # Windows binaries always reference system DLLs; the MinGW link is otherwise
  # fully static (-static -static-libstdc++ -static-libgcc, see CMakeLists).
  [ -f "$HOST" ] || fail "host executable missing: $HOST"
  ok "host runs on Windows ($(stat_size "$HOST") bytes, static C++ runtime)"
else
  LDD_OUT="$(ldd "$HOST" 2>&1 || true)"
  if grep -qx 'LMOK_FULL_STATIC:BOOL=OFF' "$BUILD_DIR/CMakeCache.txt"; then
    [[ "$LDD_OUT" != *'not a dynamic executable'* ]] || fail "dynamic build unexpectedly static"
    ok "host uses requested dynamic linkage (sanitizer-compatible)"
  else
  [[ "$LDD_OUT" == *'not a dynamic executable'* ]] || fail "host is not fully static: $LDD_OUT"
  ok "host is fully static ($(stat_size "$HOST") bytes, ldd clean)"
  fi
fi

# --- --lang python / --lang bash ---
cat > "$WORK/sample.py" <<'EOF_PY'
import os

def greet(name):
    return os.path.join("hi", name)
EOF_PY
out="$("$HOST" parse --lang python "$WORK/sample.py")"
echo "$out" | grep -qF '(module (import_statement name: (dotted_name (identifier))) (function_definition name: (identifier)' \
  || fail "parse --lang python sexp head: $out"
echo "$out" | grep -qF '(call function: (attribute object: (attribute object: (identifier) attribute: (identifier)) attribute: (identifier))' \
  || fail "parse --lang python os.path.join call: $out"
ok "parse --lang python: module with import and dotted attribute call"

out="$("$HOST" query --lang python '(call function: (attribute attribute: (identifier) @a))' "$WORK/sample.py")"
[ "$out" = 'identifier 3:19-3:23' ] || fail "query --lang python captures: $out"
ok "query --lang python: called attribute identifier (join)"

cat > "$WORK/sample.sh" <<'EOF_SH'
#!/bin/sh
TARGET=$1
echo "hi $TARGET"
EOF_SH
out="$("$HOST" parse --lang bash "$WORK/sample.sh")"
echo "$out" | grep -qF '(program (comment) (variable_assignment name: (variable_name) value: (simple_expansion (variable_name)))' \
  || fail "parse --lang bash sexp head: $out"
echo "$out" | grep -qF '(string (string_content) (simple_expansion (variable_name)))' \
  || fail "parse --lang bash quoted expansion: $out"
ok "parse --lang bash: program with assignment and quoted expansion"

out="$("$HOST" query --lang bash '(command name: (command_name (word) @w))' "$WORK/sample.sh")"
[ "$out" = 'word 2:0-2:4' ] || fail "query --lang bash captures: $out"
ok "query --lang bash: command name (echo)"

# --- tier-1 rule packs: python and bash ---
"$HOST" lint rules/python-tier1.json tests/fixtures/python-tier1-violations.py >"$WORK/python-tier1.out" || fail "lint python-tier1 violations run"
cat >"$WORK/python-tier1.expected" <<'EXPECTED_PY'
{"row":2,"col":15,"end_row":2,"end_col":16,"rule":"LBP006","severity":"major","message":"wildcard import (from x import *)"}
{"row":4,"col":0,"end_row":4,"end_col":28,"rule":"LBP005","severity":"minor","message":"unresolved FIXME/TODO comment"}
{"row":6,"col":19,"end_row":6,"end_col":21,"rule":"LBP009","severity":"major","message":"mutable default argument (use None and build inside)"}
{"row":7,"col":13,"end_row":7,"end_col":17,"rule":"LBP003","severity":"critical","message":"dynamic eval/exec call 'eval'"}
{"row":8,"col":4,"end_row":8,"end_col":9,"rule":"LBP008","severity":"minor","message":"print() call left in code"}
{"row":9,"col":7,"end_row":9,"end_col":13,"rule":"LBP001","severity":"critical","message":"os shell-out call 'system'"}
{"row":10,"col":29,"end_row":10,"end_col":34,"rule":"LBP002","severity":"critical","message":"shell=True subprocess call"}
{"row":14,"col":4,"end_row":14,"end_col":16,"rule":"LBP004","severity":"minor","message":"assert used (removed under python -O)"}
{"row":17,"col":4,"end_row":18,"end_col":12,"rule":"LBP007","severity":"major","message":"bare except clause"}
9 findings in tests/fixtures/python-tier1-violations.py
EXPECTED_PY
cmp "$WORK/python-tier1.expected" "$WORK/python-tier1.out" || { diff "$WORK/python-tier1.expected" "$WORK/python-tier1.out" >&2; fail "python-tier1 violations output"; }
out="$("$HOST" lint rules/python-tier1.json tests/fixtures/python-tier1-clean.py)"
[ "$out" = "0 findings in tests/fixtures/python-tier1-clean.py" ] || fail "python-tier1 clean: $out"
ok "lint: python-tier1 pack, all 9 rules fire on violations, clean fixture silent"

"$HOST" lint rules/bash-tier1.json tests/fixtures/bash-tier1-violations.sh >"$WORK/bash-tier1.out" || fail "lint bash-tier1 violations run"
cat >"$WORK/bash-tier1.expected" <<'EXPECTED_SH'
{"row":1,"col":0,"end_row":1,"end_col":26,"rule":"LBS002","severity":"minor","message":"unresolved FIXME/TODO comment"}
{"row":3,"col":0,"end_row":3,"end_col":2,"rule":"LBS004","severity":"major","message":"cd without guard (use 'cd ... || exit')"}
{"row":4,"col":0,"end_row":4,"end_col":14,"rule":"LBS001","severity":"critical","message":"rm with recursive force flags"}
{"row":5,"col":0,"end_row":5,"end_col":4,"rule":"LBS006","severity":"critical","message":"eval used"}
{"row":6,"col":6,"end_row":6,"end_col":15,"rule":"LBS003","severity":"minor","message":"backtick command substitution (use $(...))"}
{"row":7,"col":0,"end_row":7,"end_col":23,"rule":"LBS005","severity":"major","message":"world-writable chmod 777"}
6 findings in tests/fixtures/bash-tier1-violations.sh
EXPECTED_SH
cmp "$WORK/bash-tier1.expected" "$WORK/bash-tier1.out" || { diff "$WORK/bash-tier1.expected" "$WORK/bash-tier1.out" >&2; fail "bash-tier1 violations output"; }
out="$("$HOST" lint rules/bash-tier1.json tests/fixtures/bash-tier1-clean.sh)"
[ "$out" = "0 findings in tests/fixtures/bash-tier1-clean.sh" ] || fail "bash-tier1 clean: $out"
ok "lint: bash-tier1 pack, all 6 rules fire on violations, clean fixture silent"

# --- rules/lmcode: python and bash logic rules ---
mkdir -p build/rules-lmcode
for f in python-function-metrics bash-unquoted-vars; do
  compile_fixture "rules/lmcode/$f.lms" "build/rules-lmcode/$f.dslb" >"$WORK/$f.compile.log" 2>&1 || fail "$f rule compile"
  run_rule "rules/lmcode/$f.lms" "build/rules-lmcode/$f.dslb" >"$WORK/$f.out" || fail "$f rule run"
  while IFS= read -r line; do
    grep -qxF "${line:2}" "$WORK/$f.out" || fail "$f output missing: ${line:2} (got $(cat "$WORK/$f.out"))"
  done < <(grep '^#|' "rules/lmcode/$f.lms")
  sed -e 's/-violations\./-clean./' -e "s|build/rules-lmcode/$f.dslb|$WORK/$f-clean.dslb|" \
    "rules/lmcode/$f.lms" > "$WORK/$f-clean.lms"
  compile_fixture "$WORK/$f-clean.lms" "$WORK/$f-clean.dslb" >/dev/null 2>&1 || fail "$f clean compile"
  out="$(run_rule "$WORK/$f-clean.lms" "$WORK/$f-clean.dslb")"
  grep -qxF '[]' <<<"$out" || fail "$f clean fixture findings: $out"
  grep -qx '0 findings\( in .*\)\?' <<<"$out" || fail "$f clean fixture count: $out"
done
ok "rules/lmcode: python and bash rule files fire on violations fixtures, silent on clean"

# --- adversarial robustness (host limits and filesystem failures) ---
python3 - "$HOST" "$WORK" <<'PY'
import pathlib, subprocess, sys
host, work = sys.argv[1:]; w = pathlib.Path(work)
for name, data in [('zero', b''), ('whitespace', b' \t\r\n')]:
    p = w / name; p.write_bytes(data)
    for lang, tree in [('json', '(document)\n'), ('c', '(translation_unit)\n')]:
        r = subprocess.run([host, 'parse', '--lang', lang, str(p)], capture_output=True, timeout=5)
        assert (r.returncode, r.stdout, r.stderr) == (0, tree.encode(), b''), r
r = subprocess.run([host, 'parse', work], capture_output=True, timeout=5)
assert (r.returncode, r.stdout, r.stderr) == (3, b'', f'cannot read {work}: is a directory\n'.encode()), r
PY
ok "robustness: empty/whitespace trees and directory input refusal (exact bytes)"

python3 - "$HOST" "$WORK" <<'PY'
import json, pathlib, subprocess, sys
host, work = sys.argv[1:]; w = pathlib.Path(work)
p = w / 'robust-pack.json'; source = w / 'robust-input.json'
for regex, text, code, message in [
    ('(a+)+$', 'a'*30+'!', 3, 'rule X: regex work limit exceeded (65536)\n'),
    ('a+$', 'a'*100000, 0, None),  # over the 16 KiB text cap: skipped with a stderr note
    ('a'*50000, 'a', 2, f'rules pack {p}: rule X: regex pattern limit exceeded (1024 bytes)\n'),
    ('a{30000}', 'a', 2, f'rules pack {p}: rule X: regex repetition limit exceeded (1024)\n'),
    ('(a{32}){33}', 'a', 2, f'rules pack {p}: rule X: regex repetition limit exceeded (1024)\n'),
]:
    p.write_text(json.dumps({'lang':'json','rules':[{'id':'X','on':'string_content','message':'m','text_matches':regex}]}))
    source.write_text(json.dumps(text))
    r = subprocess.run([host,'lint',str(p),str(source)], capture_output=True, timeout=5)
    if message is None:
        assert r.returncode == 0 and r.stdout == f'0 findings in {pathlib.Path(source).as_posix()}\n'.encode(), r
        assert b'regex not applied to a 100000-byte node text' in r.stderr, r
    else:
        assert (r.returncode,r.stdout,r.stderr) == (code,b'',message.encode()), r
PY
ok "robustness: regex backtracking, long match, compilation and repeat limits (exact bytes)"

python3 - "$HOST" "$WORK" <<'PY'
import json, pathlib, subprocess, sys
host, work = sys.argv[1:]; w = pathlib.Path(work); p = w / 'query-input.json'
p.write_text(json.dumps('a'*30+'!'))
for pattern, message in [
    ('((string_content) @s (#match? @s "(a+)+$"))', 'regex work limit exceeded (65536)\n'),
    ('((string_content) @s (#match? @s "["))', "query predicate error in pattern 0: #match? has an invalid regex '['\n"),
    ('(array '*300+'(number) @n'+')'*300, 'query error at byte offset 1792: nesting limit exceeded (256)\n'),
]:
    r = subprocess.run([host,'query',pattern,str(p)], capture_output=True, timeout=5)
    assert (r.returncode,r.stdout,r.stderr) == (3,b'',message.encode()), r
r = subprocess.run([host,'query','',str(p)],capture_output=True,timeout=5)
assert (r.returncode,r.stdout,r.stderr) == (0,b'',b''), r
# Delimiters in literals/comments do not count toward structural nesting.
p.write_text(json.dumps('('*300))
pattern = '((string_content) @s (#eq? @s "'+'('*300+'")) ;'+'('*300
r = subprocess.run([host,'query',pattern,str(p)],capture_output=True,timeout=5)
assert (r.returncode,r.stdout,r.stderr) == (0,b'string_content 0:1-0:301\n',b''), r
PY
ok "robustness: query nesting, regex predicate errors and empty queries (exact bytes)"

python3 - "$HOST" "$WORK" <<'PY'
import os, pathlib, subprocess, sys
host, work = sys.argv[1:]; d = pathlib.Path(work) / 'special'; d.mkdir()
p = d / 'a.c'; p.write_text('int a;')
if hasattr(os, 'mkfifo'):
    fifo = d / 'pipe.c'; os.mkfifo(fifo)
    r = subprocess.run([host,'lint','rules/c-tier1.json',str(d)],capture_output=True,timeout=5)
    assert (r.returncode,r.stdout,r.stderr) == (0,f'0 findings in {p.as_posix()}\n'.encode(),b''), r
    r = subprocess.run([host,'lint','rules/c-tier1.json',str(fifo)],capture_output=True,timeout=5)
    assert (r.returncode,r.stdout,r.stderr) == (3,b'',f'cannot read {fifo}\n'.encode()), r
if os.name == 'posix' and os.geteuid() != 0:
    p.chmod(0)
    try:
        r = subprocess.run([host,'lint','rules/c-tier1.json',str(d)],capture_output=True,timeout=5)
        assert (r.returncode,r.stdout,r.stderr) == (3,b'',f'cannot read {p}\n'.encode()), r
    finally: p.chmod(0o644)
    sub = d / 'blocked'; sub.mkdir(); sub.chmod(0)
    try:
        r = subprocess.run([host,'lint','rules/c-tier1.json',str(d)],capture_output=True,timeout=5)
        assert r.returncode == 3 and r.stdout == b'' and r.stderr.startswith(f'cannot read {sub}'.encode()), r
    finally: sub.chmod(0o755)
    sub.rename(d / '.hidden'); sub = d / '.hidden'; sub.chmod(0)
    try:
        r = subprocess.run([host,'lint','rules/c-tier1.json',str(d)],capture_output=True,timeout=5)
        assert (r.returncode,r.stdout,r.stderr) == (0,f'0 findings in {p.as_posix()}\n'.encode(),b''), r
    finally: sub.chmod(0o755)
PY
ok "robustness: FIFO and permission handling; hidden directories pruned (where supported)"

for index in -1 4294967296 9223372036854775807; do
expression="$index"
if [ "$index" = 9223372036854775807 ]; then expression="9007199254740991 * 1024 + 1023"; fi
cat > "$WORK/child-index.lms" <<EOF_CHILD
p = _lmcode_ cli="lmok" install="github:lmctl-ai/lmok@latest";
let r = ts.parse("{}").root();
r.child($expression);
end _lmcode_
EOF_CHILD
compile_fixture "$WORK/child-index.lms" "$WORK/child-index.dslb" >/dev/null || fail "child-index compile"
set +e; out="$("$HOST" --lmcode "$WORK/child-index.dslb" 2>&1)"; code=$?; set -e
expected="$(printf '%s\n%s\n%s' LMCODE_RETURN_BEGIN '{"ok":false,"phase":"run","code":"RUN_HOST","message":"dsl line 3:3: Node.child index '"$index"' out of range (named child count 1)","line":3,"column":3}' LMCODE_RETURN_END)"
[ "$code" = 3 ] && [ "$out" = "$expected" ] || fail "child-index (exit $code): $out"
done
ok "robustness: negative, uint32 overflow and int64-max Node.child indices (exact bytes)"

python3 - "$HOST" "$WORK" <<'PY'
import pathlib, subprocess, sys
host, work = sys.argv[1:]; p = pathlib.Path(work) / 'corrupt.dslb'
for data, message in [(b'', "not a DSL bytecode file: bad magic (expected 'DSLB')"),
                      (b'XSLB\x00\x05', "not a DSL bytecode file: bad magic (expected 'DSLB')"),
                      (b'DSLB', 'bytecode is truncated: wanted 2 byte(s) at offset 4, file has 4')]:
    p.write_bytes(data)
    r = subprocess.run([host,'--lmcode',str(p)],capture_output=True,timeout=5)
    expected = 'LMCODE_RETURN_BEGIN\n{"ok":false,"phase":"load","code":"LOAD_BAD_BYTECODE","message":"'+message+'"}\nLMCODE_RETURN_END\n'
    assert (r.returncode,r.stdout,r.stderr) == (3,expected.encode(),b''), r
PY
ok "robustness: empty, corrupted and truncated bytecode (exact bytes)"

python3 - "$HOST" "$WORK" <<'PY_UTF8'
import json, pathlib, subprocess, sys
host, work = sys.argv[1:]; w = pathlib.Path(work)
p = w / 'utf8-pack.json'; source = w / 'utf8-source.json'
p.write_text(json.dumps({'lang':'json','rules':[{'id':'X','on':'string_content','message':'{text}'}]}))
source.write_bytes(b'"\xff\xfe"')
r = subprocess.run([host,'lint',str(p),str(source)],capture_output=True,timeout=5)
expected = b'{"row":0,"col":1,"end_row":0,"end_col":3,"rule":"X","severity":"minor","message":"\\ufffd\\ufffd"}\n'+f'1 findings in {source.as_posix()}\n'.encode()
assert (r.returncode,r.stdout,r.stderr) == (0,expected,b''), r
source.write_text('"é水😀"', encoding='utf-8')
r = subprocess.run([host,'lint',str(p),str(source)],capture_output=True,timeout=5)
expected = '{"row":0,"col":1,"end_row":0,"end_col":10,"rule":"X","severity":"minor","message":"é水😀"}\n'+f'1 findings in {source.as_posix()}\n'
assert (r.returncode,r.stdout,r.stderr) == (0,expected.encode(),b''), r
PY_UTF8
ok "robustness: invalid UTF-8 replaced in JSON findings; valid UTF-8 preserved (exact bytes)"


python3 tests/check-entries.py "$HOST" || fail "v6 entry dispatch controls"
ok "v6 entries: kind dispatch, source order, anonymous nodes, per-file state and refusals"
python3 tests/check-script-scanners.py || fail "script grammar scanner link controls"
python3 tests/check-script-languages.py "$HOST" --prove-controls || fail "lmscript/lmcode grammar and facts oracles"
ok "lmscript/lmcode parse, query, lint, facts and native output controls"
python3 tests/check-java-os.py "$HOST" --prove-controls || fail "Java native OS oracles"
ok "Java grammar, facts, tier-1 rules and annotated checks on native OS"

python3 tests/check-native-os.py "$HOST" --prove-controls || fail "native OS contracts"
python3 tests/check-patterns.py "$HOST" || fail "pattern compiler cases"
ok "patterns: code-shaped patterns compile to pinned queries; root matches and exit-2 diagnostics byte-exact"
ok "native OS byte/path/linkage contracts and negative controls"
python3 tests/check-rules-os.py "$HOST" --prove-controls || fail "rule pack OS oracles"
ok "rule packs per OS: all fifteen lmcode programs, CRLF/no-newline/unicode fixtures, anonymous kinds, argument order"
# --- SQLite DB layer (keep additive checks at the end) ---
cmake --build "$BUILD_DIR" --target lmok_db_test -j2 >"$WORK/db-build.log" 2>&1 ||
  { cat "$WORK/db-build.log" >&2; fail "DB wrapper test build"; }
"$BUILD_DIR/lmok_db_test" "$WORK/wrapper.db" || fail "DB wrapper transactions and statements"
ok "DB wrapper: rollback/commit, prepared bindings, foreign keys"
python3 tests/check-db.py "$HOST" || fail "DB status and resolution contracts"
ok "DB status: verbatim schema, FTS5, counts, last run, version guard and path precedence"

# --- Go tier-1 pack: gosec-flavored checks with clean/violations controls ---
"$HOST" lint rules/go-tier1.json tests/fixtures/go-tier1-violations.go >"$WORK/go-tier1.out" || fail "go-tier1 violations run"
cat >"$WORK/go-tier1.expected" <<'EXPECTED_GO'
{"row":3,"col":1,"end_row":3,"end_col":13,"rule":"GOS003","severity":"major","message":"weak cryptographic package imported"}
{"row":4,"col":1,"end_row":4,"end_col":14,"rule":"GOS003","severity":"major","message":"weak cryptographic package imported"}
{"row":8,"col":1,"end_row":8,"end_col":9,"rule":"GOS002","severity":"critical","message":"unsafe package imported"}
{"row":14,"col":1,"end_row":14,"end_col":13,"rule":"GOS001","severity":"major","message":"exec.Command uses a variable argument; review the command and arguments"}
{"row":15,"col":1,"end_row":15,"end_col":27,"rule":"GOS005","severity":"minor","message":"blank identifier discards an assignment result; review returned errors"}
{"row":19,"col":19,"end_row":19,"end_col":43,"rule":"GOS006","severity":"critical","message":"TLS certificate verification is disabled"}
{"row":20,"col":19,"end_row":20,"end_col":23,"rule":"GOS004","severity":"major","message":"world-writable file mode 0777"}
7 findings in tests/fixtures/go-tier1-violations.go
EXPECTED_GO
cmp "$WORK/go-tier1.expected" "$WORK/go-tier1.out" || { diff "$WORK/go-tier1.expected" "$WORK/go-tier1.out" >&2; fail "go-tier1 violations output"; }
out="$("$HOST" lint rules/go-tier1.json tests/fixtures/go-tier1-clean.go)"
[ "$out" = "0 findings in tests/fixtures/go-tier1-clean.go" ] || fail "go-tier1 clean: $out"
ok "lint: go-tier1 pack, all six rules fire on violations and clean fixture stays silent"

# --- JavaScript tier-1 pack: dynamic-code, process and DOM-sink controls ---
"$HOST" lint rules/javascript-tier1.json tests/fixtures/javascript-tier1-violations.js >"$WORK/javascript-tier1.out" || fail "javascript-tier1 violations run"
cat >"$WORK/javascript-tier1.expected" <<'EXPECTED_JS'
{"row":0,"col":21,"end_row":0,"end_col":36,"rule":"JS003","severity":"major","message":"child_process import exposes process execution APIs"}
{"row":2,"col":17,"end_row":2,"end_col":21,"rule":"JS001","severity":"critical","message":"eval executes a dynamically supplied string"}
{"row":3,"col":22,"end_row":3,"end_col":30,"rule":"JS002","severity":"critical","message":"Function constructor compiles a dynamically supplied string"}
{"row":5,"col":5,"end_row":5,"end_col":14,"rule":"JS004","severity":"major","message":"innerHTML assignment can create a DOM injection sink"}
4 findings in tests/fixtures/javascript-tier1-violations.js
EXPECTED_JS
cmp "$WORK/javascript-tier1.expected" "$WORK/javascript-tier1.out" || { diff "$WORK/javascript-tier1.expected" "$WORK/javascript-tier1.out" >&2; fail "javascript-tier1 violations output"; }
out="$("$HOST" lint rules/javascript-tier1.json tests/fixtures/javascript-tier1-clean.js)"
[ "$out" = "0 findings in tests/fixtures/javascript-tier1-clean.js" ] || fail "javascript-tier1 clean: $out"
ok "lint: JavaScript tier-1 pack, four rules fire on violations and clean fixture stays silent"

# --- TypeScript tier-1 pack: dynamic code, process import and DOM sink ---
"$HOST" lint rules/typescript-tier1.json tests/fixtures/typescript-tier1-violations.ts >"$WORK/typescript-tier1.out" || fail "typescript-tier1 violations run"
cat >"$WORK/typescript-tier1.expected" <<'EXPECTED_TS'
{"row":0,"col":21,"end_row":0,"end_col":36,"rule":"TS002","severity":"major","message":"child_process import exposes process execution APIs"}
{"row":2,"col":25,"end_row":2,"end_col":29,"rule":"TS001","severity":"critical","message":"eval executes a dynamically supplied string"}
{"row":4,"col":5,"end_row":4,"end_col":14,"rule":"TS003","severity":"major","message":"innerHTML assignment can create a DOM injection sink"}
3 findings in tests/fixtures/typescript-tier1-violations.ts
EXPECTED_TS
cmp "$WORK/typescript-tier1.expected" "$WORK/typescript-tier1.out" || { diff "$WORK/typescript-tier1.expected" "$WORK/typescript-tier1.out" >&2; fail "typescript-tier1 violations output"; }
out="$("$HOST" lint rules/typescript-tier1.json tests/fixtures/typescript-tier1-clean.ts)"
[ "$out" = "0 findings in tests/fixtures/typescript-tier1-clean.ts" ] || fail "typescript-tier1 clean: $out"
ok "lint: TypeScript tier-1 pack, all three rules fire on violations and clean fixture stays silent"

# --- TSX tier-1 pack is bound to the distinct JSX grammar ---
"$HOST" lint rules/tsx-tier1.json tests/fixtures/tsx-tier1-violations.tsx >"$WORK/tsx-tier1.out" || fail "tsx-tier1 violations run"
cat >"$WORK/tsx-tier1.expected" <<'EXPECTED_TSX'
{"row":0,"col":21,"end_row":0,"end_col":36,"rule":"TX002","severity":"major","message":"child_process import exposes process execution APIs"}
{"row":2,"col":25,"end_row":2,"end_col":29,"rule":"TX001","severity":"critical","message":"eval executes a dynamically supplied string"}
{"row":4,"col":5,"end_row":4,"end_col":14,"rule":"TX003","severity":"major","message":"innerHTML assignment can create a DOM injection sink"}
3 findings in tests/fixtures/tsx-tier1-violations.tsx
EXPECTED_TSX
cmp "$WORK/tsx-tier1.expected" "$WORK/tsx-tier1.out" || { diff "$WORK/tsx-tier1.expected" "$WORK/tsx-tier1.out" >&2; fail "tsx-tier1 violations output"; }
out="$("$HOST" lint rules/tsx-tier1.json tests/fixtures/tsx-tier1-clean.tsx)"
[ "$out" = "0 findings in tests/fixtures/tsx-tier1-clean.tsx" ] || fail "tsx-tier1 clean: $out"
ok "lint: TSX tier-1 pack, all three rules fire on violations and clean fixture stays silent"

# --- facts (E6: per-file knowledge-tree records as JSON Lines; cpp/facts.cpp, queries/<lang>/facts.scm) ---
for f in sample.c sample.go sample.js sample.json sample.py sample.sh sample.ts sample.tsx; do
  "$HOST" facts "tests/fixtures/facts/$f" >"$WORK/facts-$f.out" || fail "facts $f run"
  cmp "tests/fixtures/facts/expected/$f.jsonl" "$WORK/facts-$f.out" || { diff "tests/fixtures/facts/expected/$f.jsonl" "$WORK/facts-$f.out" >&2; fail "facts $f output"; }
done
ok "facts: byte-exact JSONL for C, JSON, Python, Bash, Go, JavaScript, TypeScript and TSX fixtures"

cat tests/fixtures/facts/expected/sample.c.jsonl tests/fixtures/facts/expected/sample.go.jsonl \
    tests/fixtures/facts/expected/sample.js.jsonl \
    tests/fixtures/facts/expected/sample.json.jsonl tests/fixtures/facts/expected/sample.py.jsonl \
    tests/fixtures/facts/expected/sample.sh.jsonl tests/fixtures/facts/expected/sample.ts.jsonl \
    tests/fixtures/facts/expected/sample.tsx.jsonl >"$WORK/facts-dir.expected"
"$HOST" facts tests/fixtures/facts >"$WORK/facts-dir.out" || fail "facts dir run"
cmp "$WORK/facts-dir.expected" "$WORK/facts-dir.out" || { diff "$WORK/facts-dir.expected" "$WORK/facts-dir.out" >&2; fail "facts dir output"; }
ok "facts: directory expansion (sorted, non-source files skipped) equals the per-file records"

python3 - <<'PY_FACTS'
import json
keys = ["file","lang","kind","name","scope","confidence","row","col","end_row","end_col","start_byte","end_byte","name_row","name_col"]
want = {
  "c": {"def.function","def.prototype","def.macro","def.struct","def.union","def.enum","def.enumerator","def.typedef",
        "def.variable","def.parameter","def.field","def.label","ref.call","ref.include"},
  "python": {"def.class","def.function","def.method","def.variable","def.parameter","def.import",
             "ref.call","ref.read","ref.import","ref.import_from","ref.import_wildcard"},
  "bash": {"def.function","def.variable","ref.call","ref.read","ref.source"},
  "go": {"def.function","def.method","def.type","def.variable","def.import","ref.call","ref.import"},
  "javascript": {"def.class","def.function","def.method","def.variable","def.import","ref.call","ref.import"},
  "typescript": {"def.class","def.function","def.method","def.type","def.variable","def.import","ref.call","ref.import"},
  "tsx": {"def.function","def.type"},
  "json": {"def.key"},
}
confidences = {"c": {"exact","candidate","external"}, "python": {"exact","candidate"},
               "bash": {"exact","candidate","unresolved"}, "go": {"exact","candidate"}, "javascript": {"exact","candidate"}, "typescript": {"exact","candidate"}, "tsx": {"exact"}, "json": {"exact"}}
scopes = {"c": {"", "point", "u", "color", "helper", "main"}, "python": {"", "af", "C", "C.m", "C.n"},
          "bash": {"", "local_fn", "f2"}, "go": {"", "Add", "Save"}, "javascript": {"", "create", "Widget", "Widget.render"}, "typescript": {"", "display", "Widget", "Widget.render"}, "tsx": {""}, "json": {"", "server", "server.tls", "server.hosts"}}
for lang, ext in (("c","c"),("python","py"),("bash","sh"),("go","go"),("javascript","js"),("typescript","ts"),("tsx","tsx"),("json","json")):
    seen_kinds, seen_conf, seen_scopes = set(), set(), set()
    for line in open(f"tests/fixtures/facts/expected/sample.{ext}.jsonl", encoding="utf-8"):
        rec = json.loads(line)
        assert list(rec) == keys, (lang, list(rec))
        assert rec["lang"] == lang and rec["file"] == f"tests/fixtures/facts/sample.{ext}", rec
        assert rec["start_byte"] < rec["end_byte"] and rec["row"] <= rec["name_row"] <= rec["end_row"], rec
        seen_kinds.add(rec["kind"]); seen_conf.add(rec["confidence"]); seen_scopes.add(rec["scope"])
    assert seen_kinds == want[lang], (lang, seen_kinds ^ want[lang])
    assert seen_conf == confidences[lang], (lang, seen_conf)
    assert seen_scopes == scopes[lang], (lang, seen_scopes)
PY_FACTS
ok "facts: valid JSON, fixed key order, every kind/confidence/scope chain per language exercised"

cp tests/fixtures/facts/sample.c "$WORK/noext.txt"
set +e; out="$("$HOST" facts "$WORK/noext.txt" 2>&1)"; code=$?; set -e
[ "$code" = 2 ] && [[ "$out" == *'cannot infer the language of '*'noext.txt from its extension (use --lang)'* ]] || fail "facts unknown extension (exit $code): $out"
"$HOST" facts --lang c "$WORK/noext.txt" | sed "s|$WORK/noext.txt|tests/fixtures/facts/sample.c|" >"$WORK/facts-forced.out" || fail "facts --lang run"
cmp tests/fixtures/facts/expected/sample.c.jsonl "$WORK/facts-forced.out" || fail "facts --lang c on .txt differs from sample.c"
"$HOST" facts --lang python tests/fixtures/facts >"$WORK/facts-py-only.out" || fail "facts --lang dir run"
cmp tests/fixtures/facts/expected/sample.py.jsonl "$WORK/facts-py-only.out" || fail "facts --lang python DIR must keep .py only"
set +e; "$HOST" facts >/dev/null 2>&1; code=$?; set -e; [ "$code" = 2 ] || fail "facts without arguments exits 2 (got $code)"
set +e; out="$("$HOST" facts --lang rust "$WORK/noext.txt" 2>&1)"; code=$?; set -e
[ "$code" = 2 ] && [[ "$out" == *"unknown language 'rust'"* ]] || fail "facts unknown --lang (exit $code): $out"
set +e; out="$("$HOST" facts "$WORK/no-such.c" 2>&1)"; code=$?; set -e
[ "$code" = 3 ] && [[ "$out" == *'cannot read'* ]] || fail "facts missing file (exit $code): $out"
ok "facts: --lang forces explicit files and filters directories; unknown extension/--lang exit 2; unreadable file exit 3"

# --- lint --db + findings (run recording and supersession; spike 2B) ---
mkdir -p "$WORK/dbproj/src"
cp tests/fixtures/db/a.c tests/fixtures/db/b.c "$WORK/dbproj/src/"
DBPATH="$WORK/dbproj/lmok.db"

# Recording must not change lint's stdout contract.
"$HOST" lint rules/c-tier1.json "$WORK/dbproj/src" >"$WORK/lint-nodb.out" || fail "lint dbproj run"
"$HOST" lint rules/c-tier1.json "$WORK/dbproj/src" --db "$DBPATH" >"$WORK/lint-db.out" || fail "lint --db run"
cmp "$WORK/lint-nodb.out" "$WORK/lint-db.out" || fail "lint --db changed stdout"
ok "lint --db: stdout byte-identical to plain lint; a missing DB is created via the resolution chain"

cat >"$WORK/findings.expected" <<EXPECTED_FINDINGS
{"row":4,"col":2,"end_row":4,"end_col":6,"rule":"LBC001","severity":"critical","message":"unsafe call 'gets'","path":"$WORK/dbproj/src/a.c"}
EXPECTED_FINDINGS
"$HOST" findings --db "$DBPATH" >"$WORK/findings.out" || fail "findings run"
cmp "$WORK/findings.expected" "$WORK/findings.out" || { diff "$WORK/findings.expected" "$WORK/findings.out" >&2; fail "findings output"; }
"$HOST" findings --rule LBC001 --db "$DBPATH" >"$WORK/findings-rule.out" || fail "findings --rule run"
cmp "$WORK/findings.expected" "$WORK/findings-rule.out" || fail "findings --rule LBC001"
"$HOST" findings --file "$WORK/dbproj/src/a.c" --db "$DBPATH" >"$WORK/findings-file.out" || fail "findings --file run"
cmp "$WORK/findings.expected" "$WORK/findings-file.out" || fail "findings --file"
out="$("$HOST" findings --rule LBC999 --db "$DBPATH")"
[ -z "$out" ] || fail "findings --rule LBC999 must be empty: $out"
ok "findings: stored findings print as lint JSON lines + path; --rule and --file filter"

# Fix the violation and re-lint: the old finding is superseded (gone from the
# default view) but kept as history; the clean file's row set is unaffected.
sed 's/gets(buf)/fgets(buf, sizeof buf, stdin)/' "$WORK/dbproj/src/a.c" >"$WORK/dbproj/src/a.fixed.c" &&
  mv "$WORK/dbproj/src/a.fixed.c" "$WORK/dbproj/src/a.c" || fail "edit fixture"
"$HOST" lint rules/c-tier1.json "$WORK/dbproj/src" --db "$DBPATH" >/dev/null || fail "re-lint after fix"
out="$("$HOST" findings --db "$DBPATH")"
[ -z "$out" ] || fail "the fixed finding must leave the default findings output: $out"
python3 - "$DBPATH" <<'PY_DB'
import sqlite3, sys
con = sqlite3.connect(sys.argv[1])
runs = con.execute('SELECT count(*) FROM runs').fetchone()[0]
total = con.execute('SELECT count(*) FROM findings').fetchone()[0]
live = con.execute('SELECT count(*) FROM findings WHERE superseded=0').fetchone()[0]
superseded = con.execute('SELECT count(*) FROM findings WHERE superseded=1').fetchone()[0]
files = con.execute('SELECT count(*) FROM files').fetchone()[0]
assert runs == 2 and total == 1 and live == 0 and superseded == 1 and files == 2, \
    (runs, total, live, superseded, files)
print('runs=2, files=2, findings: 1 superseded, 0 live')
PY_DB
ok "lint --db: re-lint supersedes the fixed finding (history kept, default view clean)"

set +e; out="$("$HOST" findings --db "$WORK/no-such.db" 2>&1)"; code=$?; set -e
[ "$code" = 2 ] && [[ "$out" == *'run `lmok index` first'* ]] || fail "findings missing DB (exit $code): $out"
set +e; out="$("$HOST" lint rules/c-tier1.json "$WORK/dbproj/src/b.c" --db "$WORK/no-parent-dir/x.db" 2>"$WORK/lint-db-fail.err")"; code=$?; set -e
[ "$code" = 0 ] && [ "$out" = "0 findings in $WORK/dbproj/src/b.c" ] || fail "lint --db unwritable DB (exit $code): $out"
grep -q 'run not recorded in DB' "$WORK/lint-db-fail.err" || fail "lint --db unwritable DB: missing stderr note"
set +e; "$HOST" lint rules/c-tier1.json --db >/dev/null 2>&1; code=$?; set -e
[ "$code" = 2 ] || fail "lint --db without a value exits 2 (got $code)"
ok "lint --db: recording failure never fails the lint (stderr note); findings on a missing DB exits 2"

# --- index/symbols/refs: facts -> DB pipeline (wave 2A) ---
HOST_ABS="$HERE/$HOST"
DBFIX="$WORK/dbcopy"
mkdir -p "$DBFIX" && cp tests/fixtures/db/index/* "$DBFIX/"
out="$(cd "$DBFIX" && "$HOST_ABS" index --db "$WORK/test.db" .)"
[ "$out" = "indexed 5 files (0 unchanged, 0 removed)" ] || fail "index fresh: $out"
(cd "$DBFIX" && "$HOST_ABS" symbols --db "$WORK/test.db") >"$WORK/db-symbols.out" || fail "symbols run"
cat >"$WORK/db-symbols.expected" <<'EXPECTED_DBSYM'
server key ./config.json 0:1-0:25
port key ./config.json 0:12-0:24
debug key ./config.json 0:27-0:41
helper function ./main.c 2:0-2:42
a parameter ./main.c 2:18-2:23
main function ./main.c 4:0-4:36
deploy function ./script.sh 1:0-3:1
os import ./tool.py 0:7-0:9
build function ./tool.py 3:0-4:34
path parameter ./tool.py 3:10-3:14
add prototype ./util.h 0:0-0:22
a parameter ./util.h 0:8-0:13
b parameter ./util.h 0:15-0:20
EXPECTED_DBSYM
cmp "$WORK/db-symbols.expected" "$WORK/db-symbols.out" || { diff "$WORK/db-symbols.expected" "$WORK/db-symbols.out" >&2; fail "symbols output"; }
out="$(cd "$DBFIX" && "$HOST_ABS" refs helper --db "$WORK/test.db")"
[ "$out" = 'helper call ./main.c 4:24-4:33' ] || fail "refs helper: $out"
out="$(cd "$DBFIX" && "$HOST_ABS" symbols --lang python --kind function --db "$WORK/test.db")"
[ "$out" = 'build function ./tool.py 3:0-4:34' ] || fail "symbols --lang/--kind filter: $out"
ok "index: fixture tree indexed; symbols/refs byte-exact, filters work"

touch "$DBFIX"/*
out="$(cd "$DBFIX" && "$HOST_ABS" index --db "$WORK/test.db" .)"
[ "$out" = "indexed 0 files (5 unchanged, 0 removed)" ] || fail "index touch-only: $out"
printf '\n\ndef ship(path):\n    return build(path)\n' >> "$DBFIX/tool.py"
out="$(cd "$DBFIX" && "$HOST_ABS" index --db "$WORK/test.db" .)"
[ "$out" = "indexed 1 files (4 unchanged, 0 removed)" ] || fail "index after edit: $out"
out="$(cd "$DBFIX" && "$HOST_ABS" symbols ship --db "$WORK/test.db")"
[ "$out" = 'ship function ./tool.py 10:0-11:22' ] || fail "edited file re-extracted: $out"
rm "$DBFIX/script.sh"
out="$(cd "$DBFIX" && "$HOST_ABS" index --db "$WORK/test.db" .)"
[ "$out" = "indexed 0 files (4 unchanged, 1 removed)" ] || fail "index after delete: $out"
out="$(cd "$DBFIX" && "$HOST_ABS" refs deploy --db "$WORK/test.db")"
[ -z "$out" ] || fail "removed file's refs must be gone: $out"
(cd "$DBFIX" && "$HOST_ABS" symbols --db "$WORK/test.db") | grep -q 'script.sh' && fail "removed file still in symbols"
out="$(cd "$DBFIX" && "$HOST_ABS" index --force --db "$WORK/test.db" .)"
[ "$out" = "indexed 4 files (0 unchanged, 0 removed)" ] || fail "index --force: $out"
ok "index: stat/hash invalidation, per-file re-extract, vanished removal, --force"

python3 tests/check-index-invalidation.py "$HOST" || fail "index subsecond invalidation"
ok "index: same-size subsecond edits refresh facts; touch/hash/legacy stat skips"

python3 - "$WORK/test.db" <<'PY'
import sqlite3, sys
con = sqlite3.connect(sys.argv[1])
rows = con.execute('select e.to_name, e.kind, e.confidence, f.path from edges e '
                   'left join files f on f.id = e.resolved_file order by e.to_name').fetchall()
assert rows == [('os', 'import', 'exact', None), ('util.h', 'include', 'exact', './util.h')], rows
conf = {r[0] for r in con.execute('select distinct confidence from symbols')}
assert conf <= {'exact', 'candidate', 'unresolved', 'external'}, conf
assert con.execute("select count(*) from file_text where file_text match 'helper'").fetchone()[0] == 1
print('edges resolve literal targets; confidence stored; FTS mirror populated')
PY
set +e; out="$("$HOST" symbols --db "$WORK/no-such.db" 2>&1)"; code=$?; set -e
[ "$code" = 2 ] && [[ "$out" == *'run `lmok index` first'* ]] || fail "symbols missing DB (exit $code): $out"
set +e; "$HOST" refs >/dev/null 2>&1; code=$?; set -e
[ "$code" = 2 ] || fail "refs without NAME exits 2 (got $code)"
set +e; out="$(cd "$DBFIX" && "$HOST_ABS" symbols --lang rust --db "$WORK/test.db" 2>&1)"; code=$?; set -e
[ "$code" = 2 ] && [[ "$out" == *"unknown language 'rust'"* ]] || fail "symbols unknown --lang (exit $code): $out"
ok "index: edges resolve (include hits ./util.h, import os external); missing DB refused (exit 2)"

python3 tests/check-project.py "$HOST" || fail "project CI and suppression"
ok "project CI, ignores and host suppression"

python3 tests/check-ci-planted.py "$HOST" || fail "planted CI finding"
ok "lmok ci planted-finding gate"

python3 tests/check-rule-contracts.py "$HOST" || fail "rule metadata and annotated pack harness"
ok "rule metadata and annotated pack harness"
"$HOST" test rules/c-tier1.json tests/fixtures/c-tier1-annotated.c || fail "annotated C pack"
"$HOST" test rules/python-tier1.json tests/fixtures/python-tier1-annotated.py || fail "annotated Python pack"
"$HOST" test examples/self-test/pack.json examples/self-test/fixture.c || fail "self-test example"
compile_fixture examples/report-metadata.lms build/report-metadata.dslb >"$WORK/report-metadata.compile" || fail "metadata example compile"
"$HOST" --lmcode build/report-metadata.dslb --metadata >"$WORK/report-metadata.json" || fail "metadata example run"
python3 - "$WORK/report-metadata.json" <<'PYMETA'
import json, sys
finding=json.loads(open(sys.argv[1],encoding='utf8').readline())[0]
assert finding['metadata']=={'tags':['example'],'confidence':'high'},finding
PYMETA
ok "annotated C/Python fixture pairs and self-test example"

python3 tests/check-secrets.py "$HOST" || fail "secrets packs and entropy"
ok "secrets packs, redaction, entropy and false positive controls"
python3 tests/check-autofix.py "$HOST" --prove-controls || fail "token-safe autofix native OS contracts"
ok "token-safe autofix, previews, suppression, rollback and pinned bytes"

python3 tests/check-metrics.py "$HOST" || fail "metrics and cognitive complexity"
ok "C/Python ten-function cognitive and nesting table"
python3 tests/check-dup.py "$HOST" || fail "normalized token duplication"
ok "token duplication ranges, normalization and controls"

# --- baseline / new-code-only + host SARIF (roadmap wave 1) ---
python3 tests/check-baseline-sarif.py "$HOST" || fail "baseline/new-code and SARIF contracts"
ok "baseline: --new-since/--baseline/--changed/--fail-on-new keying (moved finding stays baseline), controls proven; SARIF: lint/check/ci"

python3 tests/check-review-security.py "$HOST" || fail "review credential boundaries"
ok "review credential previews, Java storage, safe baseline rotation and diff flags"
python3 tests/check-review-baseline.py "$HOST" || fail "review baseline snapshots and Git filenames"
ok "review baseline snapshots and Git filenames"
python3 tests/check-review-sarif.py "$HOST" || fail "review SARIF Unicode and URIs"
ok "review SARIF Unicode and URIs"

# --- E9 live repo map and optional DB ranking ---
python3 tests/check-outline.py "$HOST" live || fail "outline live facts"
ok "outline live signatures, scopes, path order and duplicate inputs"
python3 tests/check-outline.py "$HOST" db || fail "outline DB ranking"
ok "outline explicit/default DB ranking and live definition authority"
python3 tests/check-outline.py "$HOST" budget || fail "outline budget"
ok "outline whole-file line budgets and exact truncation counts"
python3 tests/check-outline.py "$HOST" errors || fail "outline errors"
ok "outline invalid options, missing input and corrupt DB errors"

# --- review (main-line enhancement wave): index after lint --db, --changed revision hardening, import bindings ---
mkdir -p "$WORK/revidx/src"
printf 'int main(void) { gets(0); return 0; }\n' > "$WORK/revidx/src/a.c"
printf 'int helper(void) { return 1; }\n' > "$WORK/revidx/src/b.c"
REVHOST="$HERE/$HOST"; case "$HOST" in /*) REVHOST="$HOST";; esac
(cd "$WORK/revidx" && "$REVHOST" lint "$HERE/examples/rules/c-security.json" src --db t.db >/dev/null) || fail "revidx lint --db"
out="$(cd "$WORK/revidx" && "$REVHOST" index --db t.db src)" || fail "revidx index run: $out"
[ "$out" = "indexed 2 files (0 unchanged, 0 removed)" ] || fail "index after lint --db must extract the lint-created files rows: $out"
out="$(cd "$WORK/revidx" && "$REVHOST" symbols --db t.db helper)" || fail "revidx symbols"
[[ "$out" == *"helper function src/b.c"* ]] || fail "symbols after lint-then-index: $out"
out="$(cd "$WORK/revidx" && "$REVHOST" index --db t.db src)" || fail "revidx re-index"
[ "$out" = "indexed 0 files (2 unchanged, 0 removed)" ] || fail "second index must see extracted rows as unchanged: $out"
ok "index: files rows created by lint --db (no extraction) are indexed, not skipped as unchanged"

for rev in -O/etc/passwd --quiet -; do
  set +e; out="$("$HOST" lint examples/rules/c-security.json examples/data/sample.c "--changed=$rev" 2>&1)"; code=$?; set -e
  [ "$code" = 2 ] && [[ "$out" == *"bad --changed revision '$rev' (must not start with '-')"* ]] || fail "--changed=$rev must be refused (exit $code): $out"
done
ok "lint --changed: option-looking revisions are refused (exit 2) instead of being parsed by git"

printf 'import { a as b, c } from "m";\nimport def, * as ns from "n";\n' > "$WORK/imports.js"
cp "$WORK/imports.js" "$WORK/imports.ts"; cp "$WORK/imports.js" "$WORK/imports.tsx"
printf 'import java.util.*;\nimport java.util.List;\nimport static java.lang.Math.*;\nclass K {}\n' > "$WORK/imports.java"
python3 - "$HOST" "$WORK" <<'PY_IMPORTS'
import json, subprocess, sys
host, work = sys.argv[1:]
def defs(path):
    out = subprocess.run([host, 'facts', path], capture_output=True, check=True, text=True).stdout
    return sorted(json.loads(l)['name'] for l in out.splitlines() if json.loads(l)['kind'] == 'def.import')
for ext in ('js', 'ts', 'tsx'):
    assert defs(f'{work}/imports.{ext}') == ['b', 'c', 'def', 'ns'], (ext, defs(f'{work}/imports.{ext}'))
assert defs(f'{work}/imports.java') == ['List'], defs(f'{work}/imports.java')
PY_IMPORTS
ok "facts: import bindings — JS/TS/TSX aliased specifiers bind the alias only; Java wildcard imports bind no name"

# Deterministic plan check against the exact query used by index (no wall-time bound).
out="$("$BUILD_DIR/lmok_db_test" "$WORK/index-plan.db" --index-plan)" || fail "index extraction lookup plan"
[ "$out" = "index lookup: one uncorrelated FTS scan, placeholder/empty/duplicate controls" ] || fail "index plan output: $out"
ok "index extraction lookup materializes FTS paths once; old correlated plan rejected"

# --- indexed lmcode API and repo-wide C rule (schema additions recompile chunks) ---
python3 tests/check-db-lmcode.py "$HOST" "$WORK" surface || fail "DB lmcode surface"
ok "Ts DB JSON contracts, bound filters, explicit/env/project precedence and shared invocation"
python3 tests/check-db-lmcode.py "$HOST" "$WORK" errors || fail "DB lmcode errors"
ok "Ts DB missing/corrupt errors are located exit 3; script --db validation and lazy access"
python3 tests/check-db-lmcode.py "$HOST" "$WORK" rule || fail "DB lmcode rule"
ok "LBC111 byte-exact indexed C findings, cross-file resolution and language controls"

echo "PASS: $pass checks"
