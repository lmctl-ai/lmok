#!/usr/bin/env bash
# From repository root after build and bootstrap-compiler.sh setup (LMSCRIPT_BIN).
set -eu
export PATH="$PWD/build:$PATH"
build/lmok metrics examples/data/sample.c
build/lmok dup --tokens 20 examples/data/sample.c
mkdir -p build/rules-lmcode
node scripts/compile-fixture.mjs rules/lmcode/c-function-cognitive-complexity.lms build/rules-lmcode/c-function-cognitive-complexity.dslb
build/lmok check --lang c build/rules-lmcode/c-function-cognitive-complexity.dslb examples/data/sample.c
