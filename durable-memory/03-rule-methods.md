# Rule methods (R47/R48)

Historical v5 adoption notes below. Current v6 execution/dispatch and compiler
requirements are in [08-v6-entry-dispatch.md](08-v6-entry-dispatch.md).

lmok exposes the tree-sitter CST to lmscript so analysis rules are
user-redefinable scripts, not compiled per-rule classes (the SonarQube
contrast is studied in docs/rule-snippets.md). Shipped in v0.2.0; needs
`lmscript` >= 0.2.442 (first npm release containing the R47/R48 compiler
feature — the exact number is a placeholder pending the staging publish, see
the TODO in scripts/select-compiler.sh).

## Mechanism

- Schema v2 carries `"interfaces"`: `{"Rule": {"params": [{"name": "n",
  "type": "Node"}], "returns": "void"}}` — a SAM interface (one signature,
  named parameters). Interfaces are compiler-facing metadata: no canonical
  signature, no import, no bytecode section; they only join the schema SHA-256
  that binds bytecode to its schema, so adding one invalidates previously
  compiled bytecode.
- A top-level `fn` may carry `@NAME` (the tree-sitter CST node kind the rule
  subscribes to; compile-time metadata only — nothing dispatches on it yet)
  and `implements Rule` (signature checked exactly against the interface at
  compile time; a mismatch is a located error, e.g.
  `function 'wrong_sig' does not implement interface Rule (Node) -> void; found (Node, int) -> void`).
- Host-side CST navigation: `Tree.root() -> Node`; `Node.kind()`, `.text()`
  (source slice; the Tree keeps a source copy so slices stay valid),
  `.is_named()`, `.is_missing()`, `.start_row()/.start_col()/.end_row()/.end_col()` (0-based,
  tree-sitter convention), `.child_count()` / `.child(i)` (NAMED children
  only; `child` is bounds-checked in the registry and throws the usual
  "out of range" run-time error, exit 3). Host boxes TSNode values into arena
  Node handles (`alignas` byte storage + static_assert fit in host.cpp).
- Demo: `examples/cst-rules.lms` — two rules (`@PAIR no_password_key`,
  `@STRING no_placeholder`) run over a nested JSON fixture; wired into
  `./test.sh` like the other examples.

## Known limitations / follow-ups

- **No dispatch on annotations** (by design in this slice): rules self-filter
  with `n.kind()` and the walk calls every rule on every node — O(rules ×
  nodes). The stated future direction is an lmcode bytecode (DSLB v6) rule entry table
  (annotation + interface per rule; the host walks once and dispatches by
  node kind). The host-side walk exists since v0.2.8 as the `lint`
  dispatcher with rule packs (conditions as data) — see
  [04-dispatcher.md](04-dispatcher.md). PARTIALLY LANDED in v0.2.3: `ts.query(root, pattern)` runs a
  tree-sitter S-expression query (the esquery analog from
  docs/rule-snippets.md) and returns every captured node as a `NodeList`, so
  the pattern — not a per-node loop — does the structural selection
  (`examples/query-json.lms`). Captures come back in cursor order (matches in
  byte order, captures in pattern order within a match); duplicates across
  captures/matches are kept; a zero-capture pattern is an empty list, not an
  error; invalid patterns throw a runtime error naming the byte offset and a
  reason word (syntax/node-type/field/capture/structure/language). Query
  predicates were NOT evaluated in v0.2.3; since v0.3.3 (E1) the host
  evaluates them per match with upstream semantics, including the `any-*`
  family and `#any-of?` — see 05-rule-tiers.md "E1 landed". Query NodeLists share the arena and the
  65536-handle cap with Tree.nodes() lists; Node handles have their own
  1<<20 cap and `NodeList.at(i)` hands out a pointer INTO the list (no
  handle allocated per visit, so walking a 180k-node tree is fine); all
  arenas reset before each `--lmcode` program, so caps are per run. Nodes
  borrow the owning Tree (arena lifetime).
- **Beyond JSON** (LANDED in v0.2.4): tree-sitter-c (master commit `b780e47`,
  no external scanner — parser.c only, ABI 15) is vendored and compiled like
  the JSON grammar. The lmcode surface is explicit per grammar on purpose:
  `ts.parse_c(text)` / `ts.query_c(root, pattern)` next to the JSON pair, and
  `--lang json|c` on the CLI (`json` default; unknown lang exits 2 with the
  known-languages list). `examples/lint-c.lms` runs real security rules
  (unsafe `gets`/`strcpy`/`sprintf` calls, FIXME comments) over C. EXTENDED
  after v0.4.0: tree-sitter-python (v0.23.6, ABI 14) and tree-sitter-bash
  (v0.25.1, ABI 15) vendored the same way — the FIRST grammars with an
  external scanner, so `src/scanner.c` joined the CMake TS_SOURCES list.
  The lmcode surface stayed explicit per grammar on purpose:
  adding a FIFTH grammar should generalize this to a language parameter
  instead of another parse_x/query_x pair.
- **Void rules can't count** (R35 forbids caller-local captures, and
  `implements Rule` fixes the signature to `(Node) -> void`), so
  `examples/lint-json.lms` needs int predicate helpers evaluated twice.
  LANDED in v0.2.2: the host findings sink — `ts.report(node, message)` /
  `ts.report_count()` / `ts.report_json()` (per-`--lmcode`-file state in the
  Host arena, reset before each program run like the SDK's per-run collection
  reset; capped at 65536 findings). Rules are now just annotated void fns
  calling `ts.report`; rendering stays script-side. See
  `examples/report-json.lms`. EXTENDED in v0.2.5: a 4-arg overload
  `ts.report(n, message, rule, severity)` attaches SonarQube-style metadata;
  both strings are free-form (no host-side severity enum — policy belongs to
  the script) and `report_json()` emits `rule`/`severity` only when present
  (key order row, col, end_row, end_col, rule, severity, message), so 2-arg
  output bytes are unchanged. `examples/rule-catalog.lms` shows the user-side
  half: `json_parse(ts.report_json())`, per-severity counts, and a quality
  gate (`critical > 0` → FAIL) as a plain script-side `if`.
- **No recursion in lmcode**: `examples/cst-rules.lms` walks the CST with
  fixed-depth nested loops. LANDED in v0.2.1: `Tree.nodes() -> NodeList`
  (flat pre-order list of NAMED nodes, root first; `len()` + bounds-checked
  `at(i)`, which is exactly the shape R36 foreach needs) replaces the manual
  walk for whole-tree scans — see `examples/lint-json.lms`. It is flattened
  into the host arena on every call (O(tree) time and memory per call; fine
  at this scale, cache it in a local if looping). Remaining reason for the v6
  entry table is dispatch cost/ergonomics, not reach.
- **Suppression is host-side policy**: place `lmok-disable` or
  `lmok-disable: RULE1,RULE2` on a source line (or the line immediately
  before it). The token is matched anywhere on the line, so comments and
  strings both count. Lint, check, and ci remove matching findings before
  output, report the suppressed count, and accept `--no-suppress` to show all.
  Lint can diagnose ids absent from its pack; dynamic check rules can warn only
  for ids that were not observed in that run. `examples/suppress-findings.lms`
  demonstrates the marker without a script-side filtering map.
- **SARIF 2.1.0 interop is script-side too** (v0.2.7, no host change):
  `examples/sarif-report.lms` turns the findings sink into a minimal valid
  SARIF document (`build/sarif-results.sarif`, validated structurally in
  test.sh) — the format GitHub code scanning and VS Code consume. All policy
  lives in lmcode: the severity→level mapping (critical→error, minor→note),
  the 0-based→1-based region conversion (SARIF startLine/startColumn are
  1-based; its endColumn is 1-based EXCLUSIVE, matching our 0-based exclusive
  end_col after a uniform +1), the artifact uri label, and the
  `tool.driver.rules` catalog — which is derived from the findings
  themselves (distinct ruleId, first-seen order) rather than duplicated,
  because lmcode fns cannot read chunk-level constants (R35). Std has no
  JSON *builder* (only `json_parse`/`Json.to_json`), so emission is string
  concatenation with a two-line `jesc` helper; a std `json_build` (or
  StrMap/StrList `to_json`) would shrink this further.
- **Compiler/SDK coupling**: the vendored lmcode-cpp pin (`sdk.lock`) must
  exactly match the implicit std set of the compiler in use, or test.sh's
  registry/compiler signature parity fails. Current pin `d70a12c` (181 std
  signatures) matches npm `@lmctl-ai/lmctl` >= 0.2.442; the older `ee1f084`
  (72 signatures) matched <= 0.2.440.
- **Compiler version numbering**: the lmctl-mono build reports its own version
  (e.g. 0.2.38) unrelated to npm release numbering; the numeric floor in
  select-compiler.sh targets npm numbering, with the functional probe (DSLB
  v5, schema-hash binding, implicit std) as the real gate.
- **The fixture compiler does not create parent directories** — compiling into a fresh
  subdirectory needs a `mkdir -p` first.

## Verification

```sh
./test.sh   # needs lmscript >= 0.2.442 on PATH; released 0.2.420 fails at
            # check 8: schema error "unknown top-level key interfaces"
lmscript examples/cst-rules.lms build/cst-rules.dslb
build/lmok --lmcode build/cst-rules.dslb
# finding: password key "password" at row 4 col 4
# finding: placeholder string "TODO" at row 6 col 9
# {"ok": true, "result": null}
```
