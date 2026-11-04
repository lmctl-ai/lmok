# 17 — SDK sync to lmcode-cpp origin/main and the retired-flag sweep (2026-11-04 port)

The envelope itself predates this note (see [14-invocation-return.md](14-invocation-return.md)).
This port round-trips the vendored SDK to lmcode-cpp origin/main (`93bddf1`) so the vendored
`invocation.hpp`/`result.hpp`/`collections.hpp`/`string.hpp`/`std.hpp` are the current contract;
`sdk.lock` matches. main.cpp needed no changes (run_cli already speaks `InvocationResults`).

Sweeps and policy:

- Retired flags: README/examples/README/docs/user-journey-review.md now describe the compatibility
  boundary without teaching the retired `--dsl*` spellings. `rules/lmscript/modernization.json` and
  `tests/fixtures/language-packs/**` plus `tests/check-language-rule-packs.py` KEEP the retired
  spellings on purpose: they are the linter's own rules/fixtures that flag retired ids in user code.
- Windows text encoding (family lesson): every tests/*.py and scripts/*.py read_text/write_text/
  open (text mode) and every subprocess.run/check_output with text=True now passes
  `encoding='utf-8'` (and `errors='replace'` for captures). Two transforms, then a call-level audit:
  0 remaining. Binary-mode opens and git byte streams are untouched by design.
- Compiler probe: scripts/select-compiler.sh is now the shared capability probe (as in the lmdraw
  family, host name lmok) — no local stub probe any more.
- Version 0.6.0 -> 0.6.1. test.sh passes locally (144 checks) after the sweep.
