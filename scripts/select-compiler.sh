#!/usr/bin/env bash
set -euo pipefail
# R49 requires entry-capable v6 bytecode, not only annotation syntax.
NEED_VERSION="0.2.444"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/lmcode-compiler-XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) WORK="$(cd "$WORK" && pwd -W | tr '\\' '/')" ;;
esac
candidate="${LMSCRIPT_BIN:-}"
if [[ -z "$candidate" ]]; then candidate="$(command -v lmscript || true)"; fi
if [[ -z "$candidate" ]]; then echo "error: no lmscript compiler found; install @lmctl-ai/lmctl >= $NEED_VERSION or set LMSCRIPT_BIN" >&2; exit 2; fi
# Test source compiler capabilities; its package version may be historical.
SCHEMA="$WORK/implicit-std.schema.json"
PROBE="$WORK/probe.lms"
cat > "$SCHEMA" <<'JSON'
{"dsl.schema":2,"name":"implicit_std_probe","types":{},"functions":{},"context":{}}
JSON
python3 - "$SCHEMA" "$PROBE" "$WORK" <<'PY'
import json, pathlib, sys
schema, probe, work = sys.argv[1:]
def source(marker):
    return (f'p = {marker} schema={json.dumps(schema)};\n'
            'let value = <<~STD_PROBE;\n'
            '  raw ${not_an_outer_field} "quotes"\n'
            '  STD_PROBE\n'
            'let n = str_len(value);\n'
            f'end {marker}\n'
            f'savedsl(p, {json.dumps(str(pathlib.Path(work) / (marker[1:-1] + ".dslb")))});\n')
pathlib.Path(probe).write_text(source('_dsl_'))
pathlib.Path(work, 'probe-lmcode.lms').write_text(source('_lmcode_'))
PY
for source in "$PROBE" "$WORK/probe-lmcode.lms"; do
  out="$WORK/$(basename "$source" .lms | sed 's/^probe$/dsl/; s/^probe-lmcode$/lmcode/')".dslb
  "$candidate" "$source" "$out" >"$WORK/compiler.out" 2>"$WORK/compiler.err" || { cat "$WORK/compiler.err" >&2; exit 2; }
  python3 - "$out" "$SCHEMA" <<'PY'
import hashlib, pathlib, sys
bytecode = pathlib.Path(sys.argv[1]).read_bytes()
schema = pathlib.Path(sys.argv[2]).read_bytes()
if bytecode[:6] != b'DSLB\x00\x06': raise SystemExit('error: compiler did not emit DSL bytecode v6')
if bytecode[6:38] != hashlib.sha256(schema).digest(): raise SystemExit('error: bytecode schema hash does not match raw host schema bytes')
PY
done
"$candidate" --lmcode-signatures "$SCHEMA" | grep -Fq 'fn str_len(string) -> int' || { echo "error: compiler inspection surface lacks implicit std fn str_len(string) -> int" >&2; exit 2; }
printf '%s\n' "$candidate"
