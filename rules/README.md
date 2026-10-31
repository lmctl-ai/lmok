# rules/ — the lmok rule library

Three delivery forms, by how much logic a rule needs:

- **Rule packs** (`rules/*.json`) — conditions as data for the `lmok lint`
  dispatcher: a tree-sitter `query` pattern or an `on` node kind, an optional
  `text_matches` regex, message, severity. No code. See [rule tiers](../docs/rule-tiers.md)
  for the framework and [Rule packs](../README.md#rule-packs) for the format.
  Shipped: `c-tier1.json` (11 rules), `json-tier1.json` (11 rules:
  LBJ001–LBJ010 and LBJ104), `python-tier1.json` (9 rules),
  `bash-tier1.json` (6 rules), `go-tier1.json` (6 rules, GOS001–GOS006), and
  `javascript-tier1.json` (4 rules, JS001–JS004), and
  `typescript-tier1.json` (3 rules, TS001–TS003), and `tsx-tier1.json` (3 rules, TX001–TX003). The opt-in `bash-autofix.json` and
  `c-autofix.json` packs demonstrate four token-safe plain replacements. A
  pack `fix` is a literal replacement for the reported node; it does not
  perform metavariable substitution.
  JSON checks cover credential keys, placeholders, nulls, key naming, numeric
  strings, HTTP URLs, unpinned versions, empty containers, and long source
  strings. The length threshold is >200 source bytes excluding quotes, not
  decoded characters; see the inventory for each policy's scope.

  c-tier1 heuristic notes (from real-world validation on sds, kilo, cJSON —
  see [durable-memory/06-realworld-validation.md](../durable-memory/06-realworld-validation.md)):
  - LBC006 (K&R definitions) can also fire on parser error-recovery
    artifacts in macro-heavy code (statement-like macros used without a
    semicolon, e.g. unity's `EXPECT_ABORT_BEGIN`, produce a recovered
    `function_definition` whose shape mimics K&R). Genuine K&R is so rare
    in modern code that the rule is kept as-is.
  - LBC007 only flags `#define`s WITH a replacement value; value-less
    defines (include guards, `#define _CRT_SECURE_NO_DEPRECATE`) are
    skipped. Value-having feature-test macros (`_POSIX_C_SOURCE 200809L`)
    still fire — known noise, severity minor.
  - LBC009 checks function DEFINITIONS only (one finding per function;
    header prototypes are not separately flagged) and exempts ALL-CAPS
    names (macro-style convention).
  - LBC010 only considers a comment's first line, so doc comments with
    code examples deeper in the block no longer fire; the trade-off is
    that `/*`-blocks whose code starts on line 2+ are missed.

  python-tier1 heuristic notes:
  - LBP002 (`shell=True`) matches the keyword argument wherever it appears,
    not only subprocess calls — `shell=True` is subprocess vocabulary, so
    false positives elsewhere are unlikely.
  - LBP007 (bare except) anchors on the clause head: the `except_clause`
    node includes its block, so the regex is `^except\s*:` (no `$`).

  bash-tier1 heuristic notes:
  - LBS004 (cd) fires on guarded `cd x || exit` too — a pack cannot see the
    enclosing `list` node; refining that needs `Node.parent()` (lmcode).

  go-tier1 heuristic notes:
  - GOS001 reports `exec.Command` when an argument is an identifier; it does
    not determine whether the command is trusted or externally controlled.
  - GOS005 reports assignment results sent to `_` as an obvious ignored-result
    spot; without type information it cannot prove that the result is an error.
  - GOS006 catches the literal `InsecureSkipVerify: true`; computed values are
    outside a data-only rule pack's scope.

  javascript-tier1 heuristic notes:
  - JS001/JS002 flag `eval` and `new Function`; they do not prove input control.
  - JS003 recognizes direct child_process imports and literal `require()` calls.
  - JS004 flags direct `innerHTML` assignments; it does not track the assigned value.

- **lmcode rules** (`rules/lmcode/*.lms`) — rules that need LOGIC beyond
  selector+regex: arithmetic (`end_row - start_row`), counting children,
  per-node bookkeeping (StrMap for duplicate keys). Most use annotated
  `implements Rule` functions over the CST, reporting via `ts.report` with a
  rule key and severity; multi-node and set-based checks use ordinary
  functions or chunk-level logic. Shipped: `c-function-metrics.lms` (LBC101 function too
  long, LBC102 too many parameters), `c-switch-style.lms` (LBC103 switch
  without default, LBC104 if without braces), `json-duplicate-keys.lms`
  (LBJ101), `json-duplicate-values.lms` (LBJ105 repeated sibling string values,
  an opt-in copy/paste heuristic using exact source spellings),
  `json-size-limits.lms` (LBJ102), `c-nesting-depth.lms` (LBC105)
  and `json-nesting-depth.lms` (LBJ103) — the last two need `Node.parent()`
  (tier-2 extension E2, landed in v0.3.2) — plus `c-scanf-width.lms` (LBC106,
  needs E1 query predicates) and `c-symbol-rules.lms` (LBC107 unused static
  function, LBC108 undefined call, LBC109 direct recursion — set algebra in
  lmcode, no host index needed), and `c-cross-file.lms` (LBC110 calls to
  functions defined in none of the input files), plus
  `python-function-metrics.lms` (LBP101 function too long) and
  `bash-unquoted-vars.lms` (LBS101 unquoted variable — expansion node with
  no `string` ancestor, needs `Node.parent()`). In lmcode bytecode (DSLB v6), per-node files are
  entry-only programs: the host's `check` command indexes `@annotation` entries,
  walks the CST once, and calls matching rules in source order. Their `# check:`
  line identifies the grammar and fixture used by the test harness.
  `c-symbol-rules.lms` retains a translation-unit entry for set algebra plus its
  function-definition entry. `c-cross-file.lms` remains body-oriented because
  one entry cannot own facts from two trees. Finding sets are unchanged from the
  pre-v6 body loops; the order follows the single pre-order walk, which changed
  two files: `c-symbol-rules.lms` (set-level LBC107/LBC108 now precede LBC109)
  and `c-switch-style.lms` on real inputs (the old script ran all switch
  findings, then all if findings; now they interleave by position; its fixture
  has one of each, so the pinned expectation did not move).
  A tree-sitter MISSING placeholder dispatches as its expected kind with empty
  text and zero width; `n.is_missing()` tells it apart. In the Python grammar
  `lambda`, `await`, `yield` and `type` are both a named node and an anonymous
  token, so a rule on those kinds fires twice per construct unless it checks
  `n.is_named()` (the other three grammars have no such spelling).

- **Teaching examples** (`examples/*.lms`) — the same techniques in
  narrative order; read these first.

## Using an lmcode rule

The per-node `.lms` files compile reusable entry rules. Pass your input files
to `check`; the `# check:` header names only the repository test fixture.
`c-cross-file.lms` remains an aggregate body script with explicit file reads.

First follow the [build and setup](../README.md#build-and-run). Run these
commands from the repository root; the fixtures are local files, not fetched
with a raw script URL:

```sh
export PATH="$PWD/build:$PATH"
mkdir -p build/rules-lmcode
lmscript rules/lmcode/c-function-metrics.lms
build/lmok check --lang c build/rules-lmcode/c-function-metrics.dslb tests/fixtures/c-logic-violations.c
```

Expect a findings array containing LBC101 and LBC102, `2 findings`, then
`{"ok": true, "result": null}`. The directory creation is required:
`savedsl` does not create parent directories. Its path in the script chooses
the bytecode filename; passing another argument to `lmscript` does not
change it.

Run all twelve rule demos:

```sh
for source in rules/lmcode/*.lms; do
  lmscript "$source" || exit 1
  bytecode="build/rules-lmcode/$(basename "$source" .lms).dslb"
  spec="$(sed -n 's/^# check: //p' "$source")"
  if [ -n "$spec" ]; then
    read -r lang input <<< "$spec"
    build/lmok check --lang "$lang" "$bytecode" "$input" || exit 1
  else
    build/lmok --lmcode "$bytecode" || exit 1
  fi
done
```

Expected finding counts: function metrics 2, switch style 2, duplicate keys
2, size limits 1, C nesting 1, JSON nesting 1, scanf width 1, same-file
symbols 3, cross-file calls 1, sibling duplicate values 4, python function
metrics 1, bash unquoted variables 3. Exact findings appear in each script's
`# expect:` / `#|` comments. Findings do not make the process fail.

For the data-only packs, no lmscript compilation is needed:

```sh
build/lmok lint rules/c-tier1.json tests/fixtures/c-tier1-clean.c
build/lmok lint rules/json-tier1.json tests/fixtures/json-tier1-clean.json
```

Each prints `0 findings in <fixture path>`. The JSON pack includes LBJ104
(exact password key) as well as LBJ001–LBJ010; both credential-key rules can
report the same `password` key.

`./test.sh` compiles and runs every rule against its violations fixture
(expected finding lines asserted) and a clean fixture (`[]` and `0 findings`
asserted). No separate SDK checkout is required.
