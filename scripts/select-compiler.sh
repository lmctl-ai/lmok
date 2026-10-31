#!/usr/bin/env bash
set -euo pipefail
candidate="${LMSCRIPT_BIN:-$(command -v lmscript || true)}"
[ -n "$candidate" ] || { echo 'source compiler required: set LMSCRIPT_BIN' >&2; exit 2; }
# Require the renamed introspection surface. Public execution is exercised by
# tests/check-invocations.py; fixture bytecode uses compiler-library APIs only.
"$candidate" --lmcode-signatures schema/lmok.dsl.json | grep -Fq 'fn lmcode_return_null() -> void'
printf '%s\n' "$candidate"
