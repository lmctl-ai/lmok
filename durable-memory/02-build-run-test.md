# Build, run, and test

- Sync sources: `scripts/sync-deps.sh` (tree-sitter + tree-sitter-json into
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
  signature parity with `lmscript >= 0.2.442`, CLI parse/query/lint cases,
  rule packs against violations/clean fixtures, lmcode bytecode runs, examples,
  rules/lmcode, the reparse contract, the v0.4.0 review regressions — regex
  bound, arena caps/reset, directory-walk errors, predicate semantics —
  and the static-link check). The aggregate count is printed as `PASS: N checks`. Put `build/` on
  `PATH` before compiling examples by hand, otherwise the compiler tries to
  resolve `install="github:lmctl-ai/lmok@latest"` over the network.
- Full static is the default on GNU/Linux (`LMOK_FULL_STATIC=ON`);
  macOS links only system libraries (no `-static` exists there).

## Adding an example

1. Create `examples/<name>.lms` using `_dsl_ cli="lmok" install="github:lmctl-ai/lmok@latest"`.
2. Compile and run: `PATH=$PWD/build:$PATH lmscript examples/<name>.lms build/<name>.dslb && ./build/lmok --lmcode build/<name>.dslb`.
3. Add expected output assertions (`#| <expected>` header lines) for `./test.sh`.

## Adding a host function/method

1. Declare in `schema/lmok.host.json` (CMake merges host-only declarations
   with the pinned canonical std into `schema/lmok.dsl.json`).
2. Implement in `cpp/host.cpp` (tree-sitter headers stay in that one TU) and
   register in `cpp/main.cpp` via `dslvm::Registry`.
3. Verify `./test.sh` passes and schema signatures match the compiler.

## Releases

Binaries are built by the `ci` workflow on every push to `main` (plus manual
`workflow_dispatch`, where the `only` input selects one matrix leg; pushes
always run all five legs: linux-x64, linux-arm64, macos-arm64, macos-x64,
windows-x64) and published by force-updating the rolling `dist` orphan branch
on main; there is no release job. The Windows leg builds the full host with
MinGW (`choco install mingw ninja`); tree-sitter is portable C and the SDK is
standard-library-only with `_WIN32` paths, so no stub is needed — unlike
lmcurl, whose stub exists only because native TLS/curl packaging is deferred
there.

## Native host flag contract

The host accepts only the lmcode option family, with no retired aliases. The full gate checks exact schema/signature output, new loader routing and rejection before file effects in scratch HOME. SDK20325f7 is synchronized through the owner script for actual source-compiler v6 capabilities; package metadata alone is insufficient. Internal DSLB and .dslb names remain.
