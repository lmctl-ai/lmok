# Build, run, and test

- Scripting/the full gate currently need Git access to private `lmctlhq/lmctl-mono`;
  standalone CMake host builds do not. First bootstrap once per session: `export LMSCRIPT_BIN="$(scripts/bootstrap-compiler.sh)"`,
  then `export PATH="$(dirname "$LMSCRIPT_BIN"):$PWD/build:$PATH"`. The npm-only
  package cannot compile repository fixtures. See [prerequisites](../README.md#build-and-run).
- Sync sources: `scripts/sync-deps.sh` (tree-sitter, all 11 grammars and SQLite into
  `third_party/`, sha256-verified, idempotent; `--verify` checks the pins).
- Vendor SDK: `scripts/sync-sdk.sh` needs an lmcode-cpp checkout
  (`LMCODE_CPP=/path/to/lmcode-cpp`); `sdk.lock` then pins every vendored
  file. `scripts/sync-sdk.sh --verify` checks the pins.
- Build: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build`.
  CMake merges `schema/lmok.host.json` with the pinned canonical std into
  git-ignored `schema/lmok.dsl.json` and embeds it plus `THIRD_PARTY_NOTICES.md`.
- Test: `./test.sh` runs the native host checks (pin verifies — including a digest of
  the extracted third_party/ trees, build, `--version`, `--notices`
  byte-compare, embedded-schema byte-compare, full and `--without-std`
  signature parity with the `compiler.lock` source compiler, CLI parse/query/lint cases,
  rule packs against violations/clean fixtures, lmcode bytecode runs, examples,
  rules/lmcode, the reparse contract, the v0.4.0 review regressions — regex
  bound, arena caps/reset, directory-walk errors, predicate semantics —
  and the static-link check). The aggregate count is printed as `PASS: N checks`. Put `build/` on
  `PATH` before compiling examples by hand, otherwise the compiler tries to
  resolve `install="github:lmctl-ai/lmok@latest"` over the network.
- Full static is the default on GNU/Linux (`LMOK_FULL_STATIC=ON`);
  macOS links only system libraries (no `-static` exists there).

## Adding an example

1. Create `examples/<name>.lms` using `_lmcode_ cli="lmok" install="github:lmctl-ai/lmok@latest"` and `run_lmcode`.
2. Run with `"$LMSCRIPT_BIN" examples/<name>.lms`; ordinary scripts do not save bytecode.
3. Add expected output assertions (`#| <expected>` header lines) for `./test.sh`.
4. For maintainer entry fixtures, use `node scripts/compile-fixture.mjs INPUT OUTPUT.dslb`, then `build/lmok check --lang LANG OUTPUT.dslb INPUT_SOURCE`.

## Adding a host function/method

1. Declare in `schema/lmok.host.json` (CMake merges host-only declarations
   with the pinned canonical std into `schema/lmok.dsl.json`).
2. Implement in `cpp/host.cpp` (tree-sitter headers stay in that one TU) and
   register in `cpp/main.cpp` via `dslvm::Registry`.
3. Verify `./test.sh` passes and schema signatures match the compiler.

## Releases

The repository publishes five platform binaries through GitHub Releases and a
rolling `dist` branch. See [verified install commands and the release/main flag
boundary](../README.md#published-host-and-raw-urls). A release or dist binary may
lag main's compiler contract; use source setup for the full gate.

## Native host flag contract

The host accepts only the lmcode option family, with no retired aliases. The full gate checks exact schema/signature output, new loader routing and rejection before file effects in scratch HOME. SDK20325f7 is synchronized through the owner script for actual source-compiler v6 capabilities; package metadata alone is insufficient. Internal DSLB and .dslb names remain.
