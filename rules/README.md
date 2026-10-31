# rules/ — the lmok rule library

Three delivery forms, by how much logic a rule needs:

- **Rule packs** (`rules/*.json`) — conditions as data for the `lmok lint`
  dispatcher: a tree-sitter `query` pattern or an `on` node kind, an optional
  `text_matches` regex, message, severity. No code. See [rule tiers](../docs/rule-tiers.md)
  for the framework and [Rule packs](../README.md#rule-packs) for the format.
  The complete current inventory is below: nine tier-1 packs, four secrets
  packs and two autofix packs. Pack `fix` text is a literal node replacement,
  not metavariable substitution.
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
  functions defined in none of the input files), `c-repowide-undefined-call.lms`
  (LBC111 calls with no indexed C definition; requires `lmok index` first), plus
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
  `n.is_named()` (check namedness when subscribing to a spelling shared by tokens and nodes).

- **Teaching examples** (`examples/*.lms`) — the same techniques in
  narrative order; read these first.

## Pack inventory

Current main has **15 packs / 100 rule entries**. Counts are entries per pack,
not globally distinct IDs: secret checks intentionally recur across languages.
The binary supports 11 languages; lmscript and lmcode have parsing/facts support
but no shipped tier-1 JSON packs yet.

| Pack | Language | Rules |
| --- | --- | ---: |
| [bash-autofix.json](bash-autofix.json) | bash | 2 |
| [bash-tier1.json](bash-tier1.json) | bash | 6 |
| [c-autofix.json](c-autofix.json) | c | 2 |
| [c-tier1.json](c-tier1.json) | c | 11 |
| [go-tier1.json](go-tier1.json) | go | 6 |
| [java-tier1.json](java-tier1.json) | java | 9 |
| [javascript-tier1.json](javascript-tier1.json) | javascript | 4 |
| [json-tier1.json](json-tier1.json) | json | 11 |
| [python-tier1.json](python-tier1.json) | python | 9 |
| [secrets-bash.json](secrets-bash.json) | bash | 13 |
| [secrets-c.json](secrets-c.json) | c | 7 |
| [secrets-json.json](secrets-json.json) | json | 7 |
| [secrets-python.json](secrets-python.json) | python | 7 |
| [tsx-tier1.json](tsx-tier1.json) | tsx | 3 |
| [typescript-tier1.json](typescript-tier1.json) | typescript | 3 |

The **15 lmcode programs** add function metrics, C/Python cognitive complexity,
switch/branch style, nesting, scanf-width checks, same-/cross-file and indexed
repository-wide symbol sets,
JSON duplicate keys/values and object size, and Bash quoting checks. Secrets
are heuristic, redacted shape checks; autofixes are deliberately narrow.

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
node scripts/compile-fixture.mjs rules/lmcode/c-function-metrics.lms build/rules-lmcode/c-function-metrics.dslb
build/lmok check --lang c build/rules-lmcode/c-function-metrics.dslb tests/fixtures/c-logic-violations.c
```

`LMSCRIPT_BIN` must point to the compiler built by `scripts/bootstrap-compiler.sh`.
The fixture exporter is a repository maintainer helper, not a public lmscript
bytecode-export API. Running an entry-only `.lms` directly invokes its body;
it neither saves bytecode nor dispatches rules over an input file.

Expect LBC101 and LBC102 findings, `2 findings in tests/fixtures/c-logic-violations.c`,
and a terminal `LMCODE_RETURN_BEGIN` / JSON envelope / `LMCODE_RETURN_END` frame.
Output directories must exist before fixture export.

Run all 15 rule programs against their fixtures:

```sh
for source in rules/lmcode/*.lms; do
  bytecode="build/rules-lmcode/$(basename "$source" .lms).dslb"
  node scripts/compile-fixture.mjs "$source" "$bytecode" || exit 1
  spec="$(sed -n 's/^# check: //p' "$source")"
  case "$source" in
    *c-repowide-undefined-call.lms)
      build/lmok index --db build/rule-demo.db tests/fixtures/db-rule || exit 1
      build/lmok --lmcode "$bytecode" --db build/rule-demo.db || exit 1
      continue ;;
    *c-function-cognitive-complexity.lms) spec="c tests/fixtures/c-logic-violations.c" ;;
    *python-function-cognitive-complexity.lms) spec="python tests/fixtures/python-logic-violations.py" ;;
  esac
  if [ -n "$spec" ]; then
    read -r lang input <<< "$spec"
    build/lmok check --lang "$lang" "$bytecode" "$input" || exit 1
  else
    build/lmok --lmcode "$bytecode" || exit 1
  fi
done
```

Exact domain output for the fixture-backed examples is in their `# expect:` /
`#|` comments. Cognitive-complexity scoring has dedicated hand-computed oracles
in `tests/check-metrics.py`; these commands only exercise its entry path.
Findings alone do not make `check` exit nonzero.

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

### Indexed C calls (LBC111)

`c-repowide-undefined-call.lms` requires `lmok index` first. It reads C file
paths and function definitions from the DB, discovers candidate call names
from those C sources, and queries indexed call sites by exact name. The three
DB methods do not include an all-references iterator, so candidate discovery
still parses C inputs; definition resolution and reported positions use the DB.
Run from the same directory used to index relative paths, and re-index after edits:

```sh
build/lmok index --db build/repo.db tests/fixtures/db-rule
node scripts/compile-fixture.mjs rules/lmcode/c-repowide-undefined-call.lms build/rules-lmcode/c-repowide-undefined-call.dslb
build/lmok --lmcode build/rules-lmcode/c-repowide-undefined-call.dslb --db build/repo.db
```

This emits path-aware JSONL (LBC111, minor) and a finding count, followed by the
normal invocation frame. It does not populate `ts.report`, SARIF, or a failing
quality gate. The fixture reports `ghost_fn`, while `shared_helper` resolves in
another C file and `printf` is covered by LBC108's unchanged libc whitelist.
Python definitions do not satisfy C calls. This remains a name-set heuristic:
static linkage, macros, conditional compilation, callbacks and external
libraries beyond the whitelist are not resolved. An indexed definition with the
same name can hide an unresolved call in a different scope or translation unit.
