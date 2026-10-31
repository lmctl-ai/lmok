# Independent review of the main-line enhancement wave (bef3003..065a833)

Reviewer: Fable, 2026-10-31. Scope: secret redaction surfaces (SARIF,
baselines, previews, DB), baseline/scan semantics against
docs/tier3/local-index-survey.md §4, byte/UTF-8 handling in SARIF and
suppressions, the seven new `facts.scm` files, and the test.sh pins added in
the range. Fixes landed with the review commit; L findings are recorded here
and deliberately NOT changed.

## Fixed (M)

- `index` after `lint --db` extracted nothing. `record_lint_run` creates
  `files` rows (real hash/stat) so findings have a file_id; `index` treated
  such a row as "unchanged" by stat and skipped it: `indexed 0 files (2
  unchanged)` with zero symbols and no `file_text` — a successful-looking
  empty index. `index` now loads an `extracted` flag (a `file_text` row
  exists) and only reuses rows that were actually extracted.
- `lint --changed=<rev>` accepted option-looking revisions: `--changed=--quiet`
  made git print nothing, so lint silently linted nothing and exited 0;
  `-O<file>` changed the diff semantics. Revisions starting with `-` (or
  empty) are refused with exit 2.
- `scripts/bootstrap-compiler.sh` publish step: with a stale or incomplete
  `$dest` (interrupted build, concurrent clone sharing the cache) `mv
  "$dest.partial" "$dest"` moved the build INSIDE the stale directory and the
  script failed with "Directory not empty", leaving an unusable cache and an
  empty path for callers. The stale directory is removed before publishing.
- Facts: `import { a as b }` (JS/TS/TSX) reported `a` as a `def.import`; `import
  java.util.*;` reported `util`. Both are "exact" facts that were false.
  Queries now use `!alias` / a trailing anchor.

## Verified, not changed

- Redaction boundary: display is the first four non-quote bytes +
  `[REDACTED]`; the finding snippet is `sha256:` of the normalized node text;
  previews omit all source; `--fix` is refused for redact rules; SARIF carries
  the redacted message and a location only; `findings` prints stored
  (redacted) messages and never the snippet; `--baseline FILE` matching
  refuses to let a snippet-less legacy entry absorb a `sha256:` snippet.
  Shipped redact rules capture token-shaped values (AWS/GitHub/Slack/JWT/
  private-key patterns, entropy-gated values) or key names; Java LJV005 hashes
  the whole field declaration.
- Scan semantics: `lint --db` records in ONE transaction only after every
  target linted (exit 3 paths return before recording); `--fail-on-new`
  returns 4 after recording (a gate failure is not a scan failure);
  `--changed`-filtered and pack-less files produce no `run_files` row, so the
  prior observation stays the baseline; `load_baseline_db` selects per-file
  latest run ≤ RUN within the pack scope.
- SARIF: code-point columns (hand-checked: `const char *s="雪"; int a=1;`
  → column 26), URI percent-encoding incl. `:`, Windows drive → `file:///`,
  CRLF rows consistent with tree-sitter's `\n`-only rows.
- Pins re-derived by hand: SARIF column 26; Go CRLF `def.function` bytes
  14..33 (`package main\r\n` = 14, `func café_水() {}` = 19 bytes); Go facts
  fixture `import text "strings"` at 3:7-3:21.

## L — recorded, not fixed

- Redacted snippet identity is an unsalted, fast SHA-256 of the normalized
  node text (`tests/check-secrets.py` pins the exact digest). For the shipped
  rules the preimage is a high-entropy token or a whole declaration, and the
  baseline typically lives next to the VCS history that holds the same bytes,
  so the added exposure is small; but a user-written redact rule capturing a
  bare low-entropy value (`password = "hunter2"`) exports a dictionary-
  attackable hash. If that matters, switch to an iterated or keyed digest
  (a format change; 0.x "delete + re-index" applies) and update the pinned
  test.
- The four-byte display prefix is right for prefixed tokens (`AKIA`, `ghp_`)
  and leaks half of an 8-character value otherwise; same mitigation space.
- `lmok-disable` is matched byte-wise anywhere on a line, including inside
  string literals (documented as "independent of comment grammar"): a string
  containing the marker suppresses findings on its own and the next line.
- `index` skips by mtime+size first (hash only when stat changed); the survey
  proposed hash-first as the correctness default. Documented trade-off in
  db-design §5; a same-size edit with a restored mtime is missed until
  `--force`.
- Facts quality edges in the new languages: Go methods carry no receiver
  type in `scope`; TypeScript `abstract class` is not a `def.class`; JS/TS
  arrow functions assigned to `const` are `def.variable`, not `def.function`;
  Java `import static a.B.*` binds nothing (correct) but `enum_constant` and
  local variables are not extracted; lmscript facts cover the outer language
  only (bodies are `ref.marker`, by design until S5).
- `--changed` whitelists `[A-Za-z0-9._/~^-]`; `@{...}` reflog syntax and
  `rev:path` forms are therefore unsupported (refused, not misparsed).
