# Rule tiers

lmok is deliberately a simple CLI, not a platform. Rules are classified by
what information they need; the tier decides when (or whether) they ship.

See the [rule library and run commands](../rules/README.md) and
[examples](../examples/README.md) for runnable entry points.

Current main supports **11 languages**, with **15 JSON packs (100 entries)**
and **15 lmcode programs**. The [complete pack table](../rules/README.md#pack-inventory)
includes C, JSON, Python, Bash, Java, Go, JavaScript, TypeScript, TSX, secrets
and autofix packs. lmscript/lmcode currently have syntax and facts support
without dedicated shipped JSON packs. The detailed C/JSON/Python/Bash tables
below retain the original tier inventory; they are not the full pack list.
Tier 3 now also includes the shipped local DB/knowledge-tree verbs; type
inference, CFG/dataflow and taint remain deferred.

## The membership test

**What information does the rule need?**

- **Tier 1 — CST shape + text only.** A tree-sitter query pattern, a node kind
  plus a text regex, or a small lmcode predicate over `Node` suffices.
  Implement now (rule packs, or lmcode rules where policy belongs to users).
- **Tier 2 — CST plus one named mechanical extension.** The rule is
  syntactically clear but needs a capability the host does not have yet
  (query predicates, upward navigation, a symbol index, multi-file scope,
  incremental re-parse). Each extension is named (E1–E5 below) and planned,
  then implemented.
- **Tier 3 — real semantics.** Type resolution, control-flow graphs,
  dataflow, taint, symbolic execution. Deferred; tree-sitter alone cannot
  provide this, and faking it with regexes produces false-positive noise.

## Tier 1 — shipped (rules/)

### C ([rules/c-tier1.json](../rules/c-tier1.json), 11 rules)

Real-world validated against sds, kilo and cJSON (see
[durable-memory/06-realworld-validation.md](../durable-memory/06-realworld-validation.md) for the TP/FP triage).

| id | rule | mechanism | reference |
| --- | --- | --- | --- |
| LBC001 | unsafe calls `gets`/`strcpy`/`sprintf`/`strcat` | query `(call_expression function: (identifier) @fn)` + anchored regex | CWE-242 |
| LBC002 | FIXME/TODO comments | `on: comment` + regex `FIXME\|TODO` | cf. SonarQube S1135 |
| LBC003 | `goto` used | `on: goto_statement` | cf. MISRA C:2012 15.1 |
| LBC004 | command-exec calls `system`/`popen`/`execl*`/`execvp` | same query as LBC001 + anchored regex | CWE-78 |
| LBC005 | assignment inside `if`/`while` condition | two query patterns through `parenthesized_expression` | CERT EXP45-C; cf. MISRA C:2012 13.4 |
| LBC006 | K&R function definition | query `((function_definition (declaration)) @kr)` (a definition with direct `declaration` children) | — |
| LBC007 | object-like `#define` macro with a replacement value | `on: preproc_def` + regex requiring a value after the name (value-less include guards / feature macros skipped) | — |
| LBC008 | `register` storage class | `on: storage_class_specifier` + regex `^register$` | — |
| LBC009 | function name not snake_case | two query patterns over `function_definition` declarators (incl. pointer-returning) + regex `[A-Z]`; all-caps names exempt (macro convention) | — (convention rule) |
| LBC010 | commented-out code | `on: comment` + heuristic regex anchored to the comment's FIRST line (`=…;` or trailing `;`) | cf. SonarQube S125 |
| LBC011 | cast of `malloc`/`calloc`/`realloc` result | query `cast_expression` of a call + `#match?` on the callee | CERT MEM02-C |

### JSON ([rules/json-tier1.json](../rules/json-tier1.json), 11 rules)

| id | rule | mechanism |
| --- | --- | --- |
| LBJ001 | credential keys (`password`/`secret`) | query pair key `string_content` + regex |
| LBJ002 | placeholder strings (`TODO`/`FIXME`/`CHANGEME`/`XXX`) | query string content + anchored regex |
| LBJ003 | null value present | `on: null` |
| LBJ004 | key not snake_case | query pair key + regex `[A-Z]` |
| LBJ005 | number encoded as string (`"port": "8080"`) | query pair value string + regex `^[0-9]+$` |
| LBJ006 | sensitive keys (`aws_secret`, `aws_secret_access_key`, `private_key`, `api_key`) | pair key whole string + anchored regex |
| LBJ007 | insecure `http://` URL values | pair/array string values + prefix regex |
| LBJ008 | unpinned version strings (`*`, `latest`) | pair string values + exact regex |
| LBJ009 | empty array/object placeholders | array/object query + whitespace-only contents regex |
| LBJ010 | string source longer than 200 bytes (excluding quotes) | `on: string` + regex `^".{201,}"$` |
| LBJ104 | exact `password` key | pair key query + `#eq?` predicate |

These are opt-in configuration policies: empty containers and repeated values
can be legitimate. LBJ008 treats any pair value equal to `*` or `latest` as a
version placeholder; it does not infer a package schema. LBJ006 is case-sensitive
and intentionally overlaps LBJ001 for `aws_secret`. LBJ007 checks pair values
and direct array elements, not keys or URLs embedded within prose. LBJ009 accepts
whitespace inside empty containers but does not match comment-only containers.
LBJ010 is pack-expressible: regex counts source bytes, including escape spelling,
not decoded Unicode characters. It matches whole strings (keys included), so
escape-split `string_content` nodes do not hide long source strings.

### Python (`rules/python-tier1.json`, 9 rules)

| id | rule | mechanism |
| --- | --- | --- |
| LBP001 | os shell-out (`os.system`/`os.popen`) | query call attribute identifier + anchored `#match?` |
| LBP002 | `shell=True` keyword argument | query `keyword_argument` with `value: (true)` + regex `^shell$` |
| LBP003 | `eval`/`exec` call | query call identifier + anchored `#match?` |
| LBP004 | `assert` used | `on: assert_statement` |
| LBP005 | FIXME/TODO comments | `on: comment` + regex `FIXME\|TODO` |
| LBP006 | wildcard import (`from x import *`) | `on: wildcard_import` |
| LBP007 | bare `except:` | `on: except_clause` + regex `^except\s*:` |
| LBP008 | `print()` left in code | query call identifier + `#eq?` |
| LBP009 | mutable default argument (`def f(a=[])`) | three query patterns over `default_parameter` values (list/dictionary/set) |

### Bash (`rules/bash-tier1.json`, 6 rules)

| id | rule | mechanism |
| --- | --- | --- |
| LBS001 | `rm -rf` / `rm -fr` | query command named `rm` capturing word + command; regex `-[a-zA-Z]*r[a-zA-Z]*f` (or f…r) filters the word capture, keeps the command |
| LBS002 | FIXME/TODO comments | `on: comment` + regex `FIXME\|TODO` |
| LBS003 | backtick command substitution | `on: command_substitution` + regex `` ^` `` ($() parses to the same kind; text distinguishes) |
| LBS004 | `cd` without guard | query command_name + regex `^cd$` (heuristic: a guarded `cd x \|\| exit` still fires — the pack cannot see the enclosing `list`) |
| LBS005 | `chmod 777` | same two-capture trick as LBS001 + regex `\b777\b` |
| LBS006 | `eval` used | query command_name + regex `^eval$` |

### Further tier-1 candidates (not shipped, judged pack/script-able)

- multiple assignment (`a = b = 0`): nested `assignment_expression` query
- empty `if`/`while` body (`;`): `on: expression_statement` + text `;` guard,
  or a query for empty consequences

### Shipped as lmcode rules (rules/lmcode/) — logic over the CST

| id | rule | mechanism |
| --- | --- | --- |
| LBC101 | function too long (>60 rows) | `@function_definition` + `end_row-start_row` arithmetic |
| LBC102 | too many parameters (>7) | child-walk count of `parameter_declaration` |
| LBC103 | switch without default | child-walk; default = `case_statement` text prefix |
| LBC104 | if without braces | consequence child kind != `compound_statement` |
| LBJ101 | duplicate keys in one object | `@object` + per-object StrMap over pair keys |
| LBJ105 | repeated sibling string values | `@object` + per-object StrMap; source-text equality, excludes arrays/non-strings |
| LBJ102 | object too large (>50 entries) | pair-child counting |
| LBC105 | control structure nested >4 deep | parent() chain walk (needs E2) |
| LBJ103 | JSON value nested >6 levels deep | parent() chain walk (needs E2) |
| LBC106 | `scanf`/`fscanf` without field width | query + `#match?` predicate selecting the callee (needs E1); width heuristic in lmcode |
| LBC107 | static function never called | two queries + StrMap set difference in lmcode (no host index) |
| LBC108 | call to undefined function (same-file) | call set minus (defined + script-side libc whitelist) |
| LBC109 | direct recursion | per-definition subtree query for self-calls |
| LBC110 | call to a function defined in none of the input files | cross-file set union in lmcode (E4) |
| LBC111 | call with no indexed C function definition | `ts.db_*` set difference; requires `lmok index` first; candidate calls scanned from C sources |
| LBP101 | python function too long (>60 rows) | `@function_definition` + `end_row-start_row` arithmetic |
| LBS101 | unquoted bash variable (`$x`/`${x}` outside a `string`) | `@simple_expansion`/`@expansion` + parent-chain walk (needs E2) |

(LBC001/LBC004 moved their callee regexes from pack `text_matches` into the
query as `#match?` predicates — same behavior, sharper form.)

## Tier 2 — needs a named extension

- **E1: host-side evaluation of query predicates — LANDED (v0.3.3).**
  `#eq?`/`#not-eq?` (capture text vs literal or another capture) and
  `#match?`/`#not-match?` are evaluated per match in the single query path
  (`node_query`), so lmcode `ts.query*`, pack `query` rules, and the CLI `query`
  subcommand all share it. Semantics follow upstream
  (`satisfies_text_predicates` in the Rust binding): text equality for
  `#eq?`, and **unanchored** regex for `#match?` (upstream uses
  `regex::is_match`; the "anchored" convention is that query AUTHORS write
  `^...$` — verified against the upstream docs' own examples). The `any-*`
  family (`#any-eq?`, `#any-match?`, `#any-of?`, `#not-any-of?`) is
  evaluated too (at-least-one semantics; the plain forms are all-nodes);
  `#is?`/`#is-not?` and `!` directives are accepted and ignored; malformed
  or unknown `?` predicates are refused (exit 3) because an ignored
  predicate silently over-matches. Regex text is capped at 16 KiB (std::regex
  recursion). Shipped with it: LBC106, LBJ104; LBC001/LBC004
  upgraded to predicate form. Note: pack `text_matches` is now partly
  redundant with `#match?` for query rules — kept, since it still applies to
  `on`-kind rules.
- **E2: `Node.parent()` upward navigation — LANDED (v0.3.2).** Contract:
  `Node.has_parent() -> bool` (false at root) and `Node.parent() -> Node`
  (root call is a located run-time error, exit 3 — no fake self-node).
  Shipped with it: LBC105 (control nesting depth), LBJ103 (JSON nesting
  depth). Remaining ideas in this extension's orbit: context-sensitive rules
  ("identifier X only inside function Y"), assignment-in-loop-body (not just
  conditions), commented-out-code refinement (comment inside a function body
  vs. file top level).
- **E3: same-file symbol index — RE-SCOPED (v0.3.4) after reality check.**
  The symbol-ish rules it was meant to unlock turned out to be expressible in
  pure lmcode today: two queries + StrMap set algebra (see
  [rules/lmcode/c-symbol-rules.lms](../rules/lmcode/c-symbol-rules.lms): LBC107 unused static function, LBC108
  undefined call, LBC109 direct recursion). No host index was needed. E3
  remains only as a PERFORMANCE optimization for very large files (the
  script-side approach is O(defs × subtree) for recursion) and as an E4
  adjunct. Cross-file defined/called sets are now scriptable (E4 below).
  Function-pointer/callback targets and typedef-aware name resolution remain
  outside this model (tier-3 territory). Conservative choices shipped: LBC107 flags STATIC
  functions only (their calls must be same-file); LBC108 considers plain
  identifier callees only and uses a script-owned libc whitelist.
- **E4: multi-file / directory lint — LANDED (v0.3.5), minus include
  resolution.** `lmok lint` accepts directories: recursive expansion of
  regular files, hidden paths skipped, only known extensions (`.c`/`.h` → c,
  `.json` → json, `.py` → python, `.sh`/`.bash` → bash), paths sorted for
  byte-stable output, files whose language
  has no pack rules skipped silently, an empty expansion is exit 2, and
  unreadable files still fail fast (exit 3). The cross-file half is
  script-side: [rules/lmcode/c-cross-file.lms](../rules/lmcode/c-cross-file.lms) (LBC110, call to a function
  defined in NO file) unions defined/called sets over multiple parsed files
  in pure lmcode. Still out of scope: include-graph resolution and
  header/source pairing heuristics (fine-grained C project semantics stay
  tier 3).
- **E5: incremental re-parse — LANDED (v0.4.0).** `Tree.edit(start, old_end,
  new_end)` records a byte-offset edit (applied at reparse time, when the new
  text — and thus the new-end point — is known; one pending edit per reparse;
  bad offsets are a located run-time error). `ts.reparse`/`ts.reparse_c(old,
  new_text)` return the new tree (contract: an edit must be pending unless
  the text is unchanged, and the new text's length must equal what the edit
  implies — both are exit-3 errors, since tree-sitter would otherwise reuse
  the old tree and return it silently for different text); the old tree
  stays alive for
  `ts.changed_ranges(old, new) -> string` (JSON array of [start,end] pairs).
  [examples/incremental-reparse.lms](../examples/incremental-reparse.lms) shows the edit → reparse → re-lint loop.

## Tier-2 retrospective (all extensions landed or re-scoped, v0.4.0)

- **E1 query predicates**: landed as specified (v0.3.3), unanchored `#match?`
  matching upstream.
- **E2 Node.parent()**: landed (v0.3.2); unlocked nesting-depth rules.
- **E3 same-file symbol index**: dissolved — the symbol-ish rules proved to
  be pure-lmcode set algebra (v0.3.4); a host index would be performance
  only.
- **E4 multi-file**: directory expansion landed host-side (v0.3.5); the
  cross-file analysis half dissolved into script-side set unions; only
  include-graph resolution stays out.
- **E5 incremental re-parse**: landed (v0.4.0).

Pattern worth noting: three of five extensions (E3, the analysis half of E4,
and the rendering/policy halves throughout) dissolved into lmcode. The host
grew only where tree-sitter semantics genuinely required it (predicates,
parent navigation, edit/reparse).

## Tier 3 — deferred (real semantics)

Re-scoped in `docs/tier3/` (knowledge tree + local DB). Landed from that plan:

- **E6: per-file facts extraction — LANDED.** `lmok facts FILE|DIR...` emits
  JSON Lines records (definitions, references, include/import/source edges;
  dotted scope chains; `exact|candidate|unresolved|external` confidence) for
  C, Python, Bash and JSON, driven by embedded `queries/<lang>/facts.scm`
  files evaluated through the same `node_query` predicate path as rule packs
  (`cpp/facts.cpp`). Fixtures: `tests/fixtures/facts/`.

Still deferred (needs a compiler, not a parser):

- Type-aware rules (wrong argument types, format-string type mismatches):
  need a type system and symbol resolution.
- Null dereference: needs CFG construction and path-sensitive analysis.
- Taint / injection tracking (user input → `system`): needs dataflow.
- Dead store (assigned value never read): needs dataflow liveness.
- Use-after-free / double-free: needs alias and lifetime analysis.
- Full MISRA/CERT conformance: many rules need types, CFG, or translation-
  unit-wide semantics — only the syntactic subset lands in tier 1/2.
