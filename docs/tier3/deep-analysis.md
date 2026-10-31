# Tier 3 — deep analysis for lmok: stack graphs, CST facts, incremental indexes

Status: design exploration, 2026-10-28. **E6 (facts pass, §4/§5.4 items 1+3) landed for all four languages as `lmok facts` — see README "Facts" and `cpp/facts.hpp`; E7/E8/E9 open.** Author: Fable (lmok team). Scope:
what lmok (binary `lmok`, tree-sitter 0.27.1 + JSON/C/Python/Bash) can
extract *efficiently* that approximates deep semantic knowledge for an
AI-driven CLI — half exact knowledge tree, half search. Not a plan for
SonarQube parity; SonarQube is static too and does not own exact symbol
tables for C either.

Every external claim below is grounded in a source fetched on 2026-10-28 (see
"Sources"). Measurements are from the lmok build in this clone (v0.5.0, Linux
x64, Release).

## 0. Summary

- **Stack graphs are the right *idea* and the wrong *dependency*.** The
  idea: analyse every file in isolation into a graph of definitions,
  references and "every possible target a reference could resolve to", and
  bind across files only at query time, so indexing stays linear and
  per-file incremental. The dependency: `github/stack-graphs` was archived
  read-only on 2025-09-09, carries RustSec advisory RUSTSEC-2026-0303
  ("archived and unmaintained"), last crate release 0.14.1 (Dec 2024), ships
  rules for Java/JavaScript/Python/TypeScript only (none for C, Bash, JSON),
  is Rust-only with no C API, and GitHub's own code-navigation docs now
  describe only tree-sitter search-based navigation. Shipping it inside a
  static C++ CLI would mean writing an FFI shim around an unmaintained Rust
  crate and adding a Rust toolchain to five CI legs, for Python coverage
  alone.
- **What is cheap and exact-ish from CSTs:** per-file definitions (with
  kind, range, signature line, container), call sites, imports/includes/
  `source` edges with literal targets, lexical scope nesting. tree-sitter
  already standardises this as `tags.scm` (`@definition.*`/`@reference.*`,
  shipped in the vendored C and Python grammars) and `locals.scm`
  (`@local.scope`/`@local.definition`/`@local.reference`). A tags-style
  query over a 4,881-line C file runs in 44 ms end to end in lmok today.
- **What stays approximate without a compiler:** anything the preprocessor,
  dynamic dispatch, `eval`, star-imports or the type system decides. We
  should emit *candidates*, mark confidence, and never pretend otherwise.
- **Proposal:** add a `facts` layer — a per-file extraction pass driven by
  per-language `facts.scm` queries plus a small amount of host logic (scope
  paths, signature slices), emitting one JSONL record per fact; a resolver
  that late-binds import/include edges and name references against the
  union of facts (the stack-graph property: a file's facts never depend on
  another file); lmcode builtins so lmcode rules query facts instead of
  re-parsing; and an `outline` verb (aider-style repo map) for AI context.
  Keep the existing dispatcher untouched; `facts` is a sibling pass.
- **Don't build:** a TSG interpreter, a stack-graph path solver, a type
  checker, CFG/dataflow, or a Rust FFI bridge. Section 6 has the list.

## 1. Stack graphs and tree-sitter-graph

### 1.1 What they are

Stack graphs extend *scope graphs* (Néron, Tolmach, Visser, Wachsmuth, "A
Theory of Name Resolution", ESOP 2015): name binding is encoded as a graph,
a valid binding is a path, and resolving a reference is a path search.
GitHub's extension (Creager & van Antwerpen, "Stack graphs: Name resolution
at scale", arXiv:2211.01224) adds a **symbol stack**: reference nodes push a
symbol, definition nodes pop one, and the search "cannot enter a pop node
unless its symbol matches the top of the stack". Type-directed lookups
(`a.b.c`) pause one lookup to resolve another, hence a *stack* of pending
lookups. Edges carry a **precedence** value so shadowing (Python's
last-definition-wins) is a path preference, not special code.

The property that matters for lmok is stated in the abstract: "for each
source file, we create an isolated subgraph without any knowledge of, or
visibility into, any other file in the program." The 2021 GitHub blog post
puts it operationally: at index time each file is analysed completely in
isolation into "a graph of its definitions and references, plus every
possible target a reference could resolve to"; imports are represented but
not checked; graphs for a commit are merged at query time and a path search
runs from the clicked reference. This is what makes indexing linear in the
number of changed files instead of quadratic in dependents.

### 1.2 How the rules are written (TSG)

`tree-sitter-graph` (tree-sitter org, Rust, crate 0.12, not archived) is a
lmcode for "constructing arbitrary graph structures from source code that has
been parsed using tree-sitter". A TSG file is a list of **stanzas**: a
tree-sitter query pattern followed by statements that run once per match:

```
(identifier) @id
{
  node @id.ref            ; create a graph node, stored as a scoped variable
  attr (@id.ref) type = "push_symbol", symbol = (source-text @id), is_reference, source_node = @id
  edge @id.ref -> @id.before_scope
}
```

Values are null/bool/string/u32/syntax node/graph node/list/set; variables
are `global` (supplied by the host), `let`/`var` locals, or **scoped**
(`@node.name`, carried across stanzas, how one stanza hands a scope node to
another). `edge a -> b` is unique per pair; `attr` cannot overwrite; the
docs recommend the *lazy* evaluation strategy and say it "will likely become
the only supported strategy".

`tree-sitter-stack-graphs` interprets the resulting graph. Node `type` is
one of `scope` (default), `push_symbol`, `pop_symbol`, `push_scoped_symbol`,
`pop_scoped_symbol`, `drop_scopes`; `symbol` is required on push/pop nodes;
`is_definition`/`is_reference` mark endpoints and then require a
`source_node` for location; `is_exported` lets a scope be captured by a
`push_scoped_symbol`; `definiens_node` spans the body; edges take
`precedence` (default 0). Two singleton globals exist in every graph:
`ROOT_NODE` and `JUMP_TO_SCOPE_NODE`; `FILE_PATH` is a global too. The
Python rule set is roughly 1,250 lines of TSG organised as Modules /
Imports / Blocks / Statements / Expressions / Patterns / Syntax Types.
Non-relative imports connect to `ROOT_NODE`; dotted names become symbol
chains joined by a `"."` symbol, so `a.b.c` matches a module path; relative
imports walk `parent_module`/`grandparent_module`; builtins are a
`"<builtins>"` push reaching `ROOT_NODE`.

Cost of adoption for a *new* language is therefore not the engine; it is the
~1,000+ lines of carefully ordered binding rules per language plus a
builtins corpus, written by someone who knows both the grammar and the
language's scoping rules intimately.

### 1.3 Maturity and language coverage (as fetched 2026-10-28)

| fact | source |
| --- | --- |
| `github/stack-graphs` "archived by the owner on Sep 9, 2025 and is now read-only", "no longer supported or updated by GitHub", "we recommend you fork it" | repo page |
| RUSTSEC-2026-0303 (2026-09-22): `stack-graphs` crate "archived and unmaintained"; issues/PRs disabled; last crates.io release 0.14.1, Dec 2024; no successor | rustsec.org |
| Language rule sets in `languages/`: `tree-sitter-stack-graphs-java`, `-javascript`, `-python`, `-typescript` — nothing for C, Bash, JSON | repo tree |
| GitHub shipped precise navigation for Python (2021 blog) and TypeScript (changelog 2024-03-14) | github.blog |
| GitHub's current "Navigating code on GitHub" docs list 21 languages (incl. Bash, C, Python) all described as tree-sitter powered, "searches all definitions and references across a repository to find entities with a given name"; no precise tier is mentioned | docs.github.com |
| `tree-sitter-graph` itself: not archived, 500 commits, Rust-only, no C API mentioned | repo page |
| Dual-licensed Apache-2.0 / MIT (both crates) | repo pages |

Reading: the framework is sound and published, GitHub stopped maintaining
the implementation, and even at its peak it covered four dynamic/JVM
languages whose scoping rules suit syntactic resolution. C was never
attempted upstream, and the preprocessor is the reason (see 2.2).

### 1.4 Could lmok ship stack-graph evaluation inside a static C++ CLI?

Three routes, assessed against lmok's constraints (one static binary, five
CI legs incl. MinGW, vendored C/C++ only, lmcode as the user-facing logic):

1. **Link the Rust crates.** Neither `tree-sitter-graph` nor
   `tree-sitter-stack-graphs` exposes a C API; we would write a `cdylib`/
   `staticlib` shim, add cargo to every CI leg (the Windows leg is MinGW
   today), and depend on an archived crate with an open RustSec advisory.
   Payoff: Python only. **Reject.**
2. **Reimplement stack graphs in C++.** The graph model (six node kinds,
   symbol stack, precedence, partial-path search) is a few thousand lines
   and we have the tree-sitter C API. But the *value* is in the per-language
   TSG rule sets, which we do not have for C/Bash/JSON and would have to
   author from scratch, plus a TSG interpreter (or a C++ transliteration of
   each rule set, which freezes it). Twelve months of work before the first
   rule fires on C, and the C result would still be wrong under `#ifdef`.
   **Reject for now; revisit only if a maintained successor appears.**
3. **Borrow the architecture, not the engine.** Keep the two load-bearing
   ideas — *per-file isolation with late binding* and *emit every candidate,
   decide at query time* — and implement them over a much simpler facts
   model: named definitions, references, scope paths, and import edges,
   extracted by tree-sitter queries we already run. Resolution becomes a
   table join on (name, kind, scope-path prefix) rather than a path search.
   This loses type-directed lookup (`a.b.c` through assignments) and gains
   four languages this quarter. **Build this.** (Section 4.)

## 2. What is exact-ish from CSTs alone

### 2.1 The existing conventions we can reuse

tree-sitter already defines two query conventions that are exactly the
"facts" we want, and our vendored grammars ship some of them:

- **`tags.scm`** (code navigation): captures `@definition.<kind>` /
  `@reference.<kind>` plus `@name` (and optional `@doc`); standard kinds
  `class function interface method module` and `call class implementation`.
  Vendored: `tree-sitter-c/queries/tags.scm` (struct/union → class,
  `function_declarator` → function, typedef/enum → type) and
  `tree-sitter-python/queries/tags.scm` (module-level constants, classes,
  functions, `call` → `reference.call`). Bash and JSON ship none.
- **`locals.scm`** (scope tracking): `@local.scope` "introduces a new local
  scope", `@local.definition` "contains the *name* of a definition within the
  current local scope", `@local.reference` "*may* refer to an earlier
  definition within some enclosing scope". Not vendored, but nvim-treesitter
  maintains them: C (scopes `translation_unit function_definition
  compound_statement struct_specifier for/if/while_statement`; definitions by
  kind function/macro/var/parameter/type/field/label; references on every
  `identifier`, `field_identifier`, `type_identifier`), Python (94 lines;
  scopes `module class_definition function_definition` + comprehensions;
  definitions parameter/function/method/type/import/field/var; `for`/`while`
  deliberately *not* scopes), Bash (10 lines: `function_definition` scope,
  `variable_assignment`/`function_definition` definitions, references on
  `variable_name` and `word`).

Both are plain tree-sitter queries, so lmok's existing `node_query`
(predicates included) executes them unchanged; what is missing is the host
pass that turns captures into records and computes scope paths.

### 2.2 Per-language inventory

Node shapes below were confirmed with `lmok parse --lang …` on small
probes in this clone.

**C.** Exact: function definitions and prototypes
(`function_definition`/`declaration` → `function_declarator declarator:
(identifier)`), `storage_class_specifier` (static/extern) for linkage,
struct/union/enum/typedef names, globals, parameters, labels, call sites
(`call_expression function: (identifier)`), includes (`preproc_include
path: (system_lib_string)` vs `(string_literal)` — the quote/angle
distinction is in the CST), object/function macros (`preproc_def`,
`preproc_function_def`), lexical blocks. Approximate or impossible:
anything inside `#if`/`#ifdef` (tree-sitter parses *all* branches; a symbol
may be defined twice or never, depending on flags), macro-generated
definitions (`DEFINE_FOO(x)` expands to a function the CST never shows),
field/typedef resolution (`p->x` needs the type of `p`), function pointers
and callbacks, which header an `#include <x.h>` actually resolves to without
the `-I` list, and overloaded-by-macro names. Rule of thumb: *names and
spans are exact; linkage is exact; targets of references are candidate
sets.*

**Python.** Exact: `function_definition`/`class_definition` names and
nesting (method = function directly in class body), parameters incl.
`*args`/`**kwargs`, imports in all three shapes (`import_statement
(dotted_name)`, `import_from_statement module_name: (dotted_name) name:
(aliased_import|dotted_name)`, `relative_import (import_prefix)`), module-
level assignments, decorators, call sites (`call function: (identifier)` and
`call function: (attribute object: … attribute: …)`), comprehension scopes.
Approximate: attribute targets (`obj.m()` needs the type of `obj`; stack
graphs attempt this through assignments, we would not), `from x import *`,
dynamic `getattr`/`setattr`/`globals()`, monkey-patching, metaclasses and
decorators that rewrite the function, `self.x` fields (nvim's locals treat
assignment to `self.x` in a class as a field — a good, honest heuristic to
copy, tagged as inferred).

**Bash.** Exact: `function_definition name: (word)`, `variable_assignment
name: (variable_name)` incl. `declaration_command` (`export`/`local`/
`declare`), `command name: (command_name (word))` call sites, `source` and
`.` as commands whose first `argument: (word)` is the target *when it is a
literal word*. Approximate: `source "$HOME/x.sh"` (argument is a `string`
with `simple_expansion`; the target is unknowable without running), `eval`,
indirect expansion `${!var}`, command resolution through `PATH` (is `foo` a
local function, an alias, or `/usr/bin/foo`? We can only say "defined in
this file / in a sourced file / external"), word splitting. Bash has no
`tags.scm` upstream; we write ours.

**JSON.** No symbols in the language. Exact and useful: key paths (JSON
Pointer `/a/b/0/c`), value kinds, duplicate keys (already a rule), `$ref`
strings (JSON Schema / OpenAPI) as *edges* when the target is a local
fragment or a relative file. Everything else is schema-specific policy and
belongs in lmcode rules, not the facts layer.

### 2.3 What stays approximate without a real type checker (all languages)

Types, overload/field/method resolution, control flow (reachability, dead
stores), dataflow (taint, null), aliasing/lifetimes, and conditional
compilation. These are the Tier 3 bullets already listed in
`docs/rule-tiers.md`; nothing in this document changes that. The facts
layer must therefore carry a **confidence** field (`exact` for definitions
and literal edges, `candidate` for name-matched references, `unresolved`
for dynamic targets) so an AI consumer can weigh them, and so a rule author
can opt in to candidates deliberately.

## 3. Incremental parsing as an index

tree-sitter's contract (api.h, "Advanced Parsing" docs): edit the old tree
with `ts_tree_edit` "in a way that exactly matches the source code changes",
reparse with the old tree so "the unchanged parts of it can be reused", and
`ts_tree_get_changed_ranges` returns "areas where the hierarchical structure
of syntax nodes (from root to leaf) has changed … Characters outside these
ranges have identical ancestor nodes in both trees", possibly "slightly
larger than the exact changed areas". lmok exposes exactly this since
v0.4.0 (`Tree.edit`, `ts.reparse*`, `ts.changed_ranges`), with the
contract checks added in the v0.4.0 review (an edit must be pending unless
the text is unchanged; lengths must agree).

What this buys an index, and what it does not:

- **Trees are not a persistent format.** tree-sitter has no serialisation;
  a `TSTree` lives in one process. A codebase knowledge tree therefore
  persists *facts*, not trees. Trees are an in-session cache (editor/agent
  loop) that makes re-extraction cheap; cold start is a full parse, which is
  cheap anyway (measured below).
- **Edit-invalidation semantics for facts.** Every fact carries its byte
  range and the range of its enclosing top-level node. After a reparse,
  a fact is stale iff its top-level node's range intersects a changed range
  (or the node identity changed; `ts_node_descendant_for_byte_range` on the
  new tree finds the replacement). Re-extract only those top-level nodes;
  shift the ranges of the rest by the edit delta (positions after
  `old_end_byte` move by `new_end - old_end`). This is the same granularity
  tree-sitter itself reuses, so it is consistent by construction.
- **Cross-file invalidation is a join, not a re-extraction.** Because facts
  are per-file and edges are late-bound by name/path, editing `a.h` never
  requires re-extracting `b.c`; only the resolution of `b.c`'s references
  into `a.h`'s definitions changes, and that is recomputed by the resolver
  (a DB query), not by parsing. This is the stack-graph incrementality
  property applied to a simpler model.
- **Limits.** Byte offsets are `uint32_t` (lmok refuses >4 GiB inputs); a
  retained tree costs memory proportional to the file (two trees during a
  reparse, documented in `host.hpp`); the Python/Bash grammars have external
  scanners whose state is part of the tree, so reuse is still exact but
  less aggressive around strings/heredocs.

Measured on this machine (lmok v0.5.0, Release, tree-sitter's own sources):

| operation | input | time |
| --- | --- | --- |
| `parse --lang c` incl. printing the s-expression | `lib/src/query.c`, 4,881 lines / 172 KB | ~48 ms per run (5 runs 243 ms) |
| tags-style `query` (`function_declarator declarator: (identifier)`) | same file, 114 hits | 44 ms end to end |
| `(identifier) @id` | same file | 5,060 captures |
| `lint rules/c-tier1.json` (11 rules, 7 query cursors + 1 walk + regex) | `lib/src/` 74 files, 22,621 lines / 811 KB | 2.55 s (≈320 KB/s) |

Parsing is not the bottleneck; running one cursor per rule per file is
(a fold-into-one-walk optimisation is already noted in
`durable-memory/04-dispatcher.md`). A facts pass that runs one combined
query per language per file would land near parse speed, i.e. tens of
milliseconds per large file, which makes "re-extract the whole file" an
acceptable fallback whenever the incremental path is in doubt.

## 4. "Enhanced tree-sitter" extensions lmok could add

Numbering continues the E1–E5 series in `docs/rule-tiers.md`.

### E6 — facts extraction pass (per file, isolated)

A host pass, `cpp/facts.cpp` (tree-sitter headers stay in `host.cpp`'s
TU family), driven by one query file per language, `queries/<lang>/facts.scm`,
vendored *in lmok* (not in `third_party/`) so we own the capture vocabulary.
Capture vocabulary = tags.scm ∪ locals.scm, extended:

- `@def.<kind>` + `@name` (+ optional `@sig`, `@doc`): kind ∈ `function
  method class struct union enum typedef macro variable constant parameter
  field label module key`. (`@definition.*` from tags.scm is accepted as an
  alias so upstream files work unmodified.)
- `@ref.<kind>` + `@name`: kind ∈ `call type field variable label command`.
- `@scope` on scope-introducing nodes (per locals.scm lists), `@scope.name`
  optional (functions/classes/modules name their scope; blocks are
  anonymous `#n`).
- `@edge.<kind>` + `@target`: kind ∈ `include import import_from source
  ref`; the host records the literal target text and whether it was a
  literal (`exact`) or contained expansions/computed parts (`unresolved`).
- `@linkage` (C `storage_class_specifier`), `@export` (bash `export`,
  python `__all__` membership — the latter candidate-only).

Host logic (the part queries cannot express): the lexical **scope path**
of each fact (`file::outer::inner`, computed from the `@scope` capture
stack by byte containment — one pre-order pass, same as `tree_nodes`), the
**signature slice** (text of the definition node up to its body's start
byte, whitespace-normalised), the **container** (enclosing `@def` of kind
class/struct/function), and byte/row/col ranges. One record per fact:

```json
{"file":"src/a.c","lang":"c","kind":"def.function","name":"helper",
 "scope":"src/a.c","container":null,"linkage":"static",
 "range":[52,105],"start":[2,11],"end":[4,1],
 "sig":"static int helper(int a)","confidence":"exact","top":[52,105]}
{"file":"src/a.c","lang":"c","kind":"ref.call","name":"helper",
 "scope":"src/a.c::main","range":[89,95],"confidence":"candidate","top":[70,110]}
{"file":"src/a.c","lang":"c","kind":"edge.include","target":"local.h",
 "quoted":true,"range":[19,37],"confidence":"exact","top":[19,37]}
```

`top` is the enclosing top-level node's range — the invalidation unit from
section 3. Output: JSON Lines on stdout (`lmok facts [--lang L] FILE|DIR…`),
one line per fact, files in the same sorted/forward-slash order as `lint`.
No database inside the binary: JSONL is the contract, and SQLite/DuckDB/a
vector store/whatever the agent uses ingests it. (An optional `--sqlite
PATH` writer is a later convenience, not a dependency we should take on
now.)

### E7 — cross-file edge resolution (the only "multi-file" host logic)

A resolver that reads a facts stream and emits `resolved` records. Rules,
deliberately literal:

- **C `#include "x.h"`:** search the includer's directory, then `-I` roots
  passed on the command line (`--include-dir`, repeatable), in that order;
  `<x.h>` searches only `-I` roots and otherwise resolves to `external`.
  No macro-path includes.
- **Python:** `a.b.c` → `<root>/a/b/c.py` or `<root>/a/b/c/__init__.py`
  for each `--root`; relative imports from the importing file's package
  directory (one dot per level, exactly what `import_prefix` carries);
  unresolvable → `external` (stdlib/site-packages are external by design).
- **Bash:** `source`/`.` with a literal word target → relative to the
  script's directory then cwd; any expansion in the target → `unresolved`
  with the raw text preserved.
- **JSON `$ref`:** local fragment (`#/…`) → in-file key path; relative file
  → sibling file if present.
- **Name references** (`ref.*`): join on `name` (+ kind compatibility: a
  `ref.call` joins `def.function|def.method|def.macro`) against definitions
  reachable through the file's own scope chain first, then through its
  resolved edges transitively (one hop for C includes, module graph for
  Python), then globally. Every hit is a *candidate* with a `via` path;
  precedence is "nearest scope wins, then nearest file wins", stated in
  the record, never applied silently. This is where stack graphs would do a
  path search with a symbol stack; we do a bounded join and keep all
  candidates. An AI consumer that sees two candidates can read both; a
  linter rule can require exactly one.

### E8 — lmcode builtins so lmcode rules can query facts

Keep the schema surface small and typed (per `durable-memory/01`: a third
grammar generalises to a language parameter — the same applies here, one
API for all four):

- `ts.facts(tree) -> FactList` — run the language's `facts.scm` + host pass
  over an already-parsed tree (so a rule that parsed for other reasons pays
  nothing extra).
- `FactList.len()/at(i)`, `Fact.kind()/name()/scope()/container()/sig()/
  confidence()/node() -> Node` (the defining/referencing node, so existing
  `ts.report(node, …)` works unchanged).
- `FactList.where(kind, name) -> FactList` (both may be `""` wildcards) —
  enough for set algebra in lmcode without a query language.
- `ts.resolve(facts_a, facts_b, …)`? No: multi-file joins stay script-side
  (as LBC110 does today) *or* go through the external DB. The lmcode language gets
  per-file facts; the resolver is a CLI verb. This mirrors E3's lesson
  ("symbol-ish rules were pure lmcode set algebra").

Existing lmcode rules that would simplify immediately: `c-symbol-rules.lms`
(LBC107/108/109: defined/called sets become `facts.where("def.function","")`
and `facts.where("ref.call","")`), `c-cross-file.lms` (LBC110), and both
`*-function-metrics.lms` (signature/body ranges come for free).

### E9 — `outline` verb (the AI-facing half)

`lmok outline FILE|DIR… [--budget N]` prints definitions grouped by file
with their signature line and container — the "repo map" aider builds with
tree-sitter tags (aider: "the most important classes and functions along
with their types and call signatures", ranked on "a graph where each source
file is a node and edges connect files which have dependencies", default
"1k tokens"). Ranking input is E7's edge list (file-level import graph +
reference counts); the ranking itself (PageRank or a simpler in-degree
score) is ~50 lines in C++ or, better, left to the consumer with the raw
counts exposed. This verb is what makes the "half exact knowledge tree"
usable in a prompt.

## 5. Concrete proposal

### 5.1 Compute per file

1. Definitions with kind, name, scope path, container, linkage/visibility,
   signature slice, ranges, `top` range. Exact.
2. References with kind, name, scope path, ranges. Exact as *occurrences*;
   their *targets* are candidates (E7).
3. Edges: include/import/source/`$ref` with literal target and
   quoted/relative flags. Exact when literal, `unresolved` otherwise.
4. Scopes: the nested scope tree (name, kind, range) — lets a consumer ask
   "what is visible at byte N" without re-parsing.
5. Metrics already used by rules: body/signature ranges, parameter counts,
   nesting depth — cheap by-products of 1 and 4, so rules stop recomputing
   them in lmcode loops.
6. Per-file digest (sha256 of the text) and the lmok version + `facts.scm`
   hash, so a consumer DB can skip unchanged files and invalidate on rule
   changes. (The `top` ranges support the finer, in-session incremental
   path from section 3; the digest is the cold-start path.)

### 5.2 Do not attempt (in the host)

Type inference of any kind; preprocessor evaluation (`#if` branches are
reported as-is, both sides); macro expansion; attribute/method target
resolution through values (`obj.m()` resolves to *all* `def.method m`
candidates, flagged); Python star-imports and `__getattr__`; Bash `eval`,
`PATH` lookup, or sourcing through variables; control-flow or dataflow
graphs; a persistent store inside the binary; a general graph query
language; and any "precision" claim beyond `exact|candidate|unresolved`.
Each of these is either a Tier 3 bullet already deferred or a dynamic
property that a static tool must not pretend to know.

### 5.3 Where it plugs in

- **`cpp/host.cpp` / `host.hpp`:** unchanged contract; `node_query` already
  evaluates the predicates `facts.scm` files need (`#match?`, `#eq?`,
  `#any-of?` after the v0.4.0 fix). Add `queries/<lang>/facts.scm` loading
  (embedded at build time like the schema, via `cmake/embed_schema.cmake`,
  so the binary stays single-file).
- **New `cpp/facts.cpp` / `facts.hpp`:** `extract_facts(const Tree&,
  Grammar) -> std::vector<Fact>` (scope-path pass + capture-to-record
  mapping) and `resolve_edges(...)` for E7. Pure functions over `Tree` and
  the existing `Node`/`NodeList`; no tree-sitter headers (keeps the one-TU
  rule).
- **`cpp/main.cpp`:** two verbs, `facts` and `outline`, sharing `lint`'s
  target expansion (directories, hidden filter, extension → grammar, sorted
  forward-slash paths, fail-fast exit 3) — factor that expansion into a
  helper first; the dispatcher itself is not touched. Exit codes keep the
  house convention: 0 ok, 2 usage/pack, 3 input/runtime, 4 output failure.
- **`cpp/dslvm` registry (`register_host`):** the E8 methods; `Fact`/
  `FactList` are two more arena-held types using the existing `hold`
  pattern and per-run reset. Schema bump invalidates compiled bytecode (as
  every host-schema change does); note it in `rules/README.md`.
- **Tests:** `facts` output pinned byte-exact per fixture language (like the
  tier-1 packs), a resolver fixture tree (`tests/fixtures/facts/` with an
  include chain, a Python package with relative imports, a sourced bash
  lib), and the incremental check: facts after `edit`+`reparse` must equal
  facts from a cold parse of the new text.

### 5.4 Sequencing

1. E6 for C and Python (vendored `tags.scm` + nvim-derived `locals.scm`
   as the starting vocabulary), JSONL output, pinned fixtures. 2–3 days.
2. E8 builtins; port LBC107/108/109/110 to facts; delete the duplicated
   set-building code in lmcode. 1–2 days.
3. E6 for Bash and JSON (write `facts.scm` from scratch; small). 1 day.
4. E7 resolver with the literal rules above; `external|unresolved`
   accounting in output. 2–3 days.
5. E9 `outline`. 1 day.
6. Incremental re-extraction over `top` ranges inside a `--lmcode` session
   (optional; the cold path is already fast enough for CLI use).

## 6. Build / don't build

**Build**

- E6 facts pass (per-file, isolated, `facts.scm`-driven, JSONL), C + Python
  first, then Bash + JSON.
- E7 literal edge resolver with explicit `exact|candidate|unresolved|
  external` outcomes and every candidate kept.
- E8 `ts.facts`/`FactList`/`Fact` lmcode builtins; migrate the four lmcode
  symbol rules onto them.
- E9 `outline` verb with raw reference/import counts (ranking optional).
- Scope-path and `top`-range bookkeeping so in-session incremental
  re-extraction is possible later without a format change.
- Confidence field from day one; a `facts.scm` hash in every record.

**Don't build**

- A TSG interpreter or a C++ port of stack graphs' path search: engine
  without rules, rules do not exist for C/Bash/JSON, upstream is archived
  (RUSTSEC-2026-0303).
- A Rust FFI bridge to `tree-sitter-stack-graphs`/`tree-sitter-graph`:
  no C API, archived dependency, toolchain cost on five CI legs, Python-only
  payoff.
- Type inference, attribute resolution through values, preprocessor
  evaluation, CFG/dataflow/taint: Tier 3 as already deferred; a static
  tool pretending here produces exactly the false-positive noise the tier
  framework exists to avoid.
- A database or query language inside the binary: emit JSONL; let the
  consumer own storage.
- Serialising tree-sitter trees: no upstream format; facts are the
  persistent artefact, trees are a session cache.
- "Precise" in any user-facing name. Call it `facts`.

## Sources (fetched 2026-10-28)

- github/stack-graphs repository page (archive notice, crates, licence):
  https://github.com/github/stack-graphs
- Language rule sets directory: https://github.com/github/stack-graphs/tree/main/languages
- `tree-sitter-stack-graphs` README (CLI: `init`, `index`, `status`,
  `clean`, `query definition`): https://github.com/github/stack-graphs/blob/main/tree-sitter-stack-graphs/README.md
- `tree-sitter-stack-graphs` API docs (node types/attributes, globals):
  https://docs.rs/tree-sitter-stack-graphs/latest/tree_sitter_stack_graphs/
- Python rule set (`stack-graphs.tsg`, ≈1,250 lines):
  https://raw.githubusercontent.com/github/stack-graphs/main/languages/tree-sitter-stack-graphs-python/src/stack-graphs.tsg
- `tree-sitter-graph` language reference: https://docs.rs/tree-sitter-graph/latest/tree_sitter_graph/reference/index.html
  and repository: https://github.com/tree-sitter/tree-sitter-graph
- RUSTSEC-2026-0303, "`stack-graphs` is archived and unmaintained":
  https://rustsec.org/advisories/RUSTSEC-2026-0303.html
- GitHub blog, "Introducing stack graphs" (2021):
  https://github.blog/open-source/introducing-stack-graphs/ ;
  "Precise code navigation for Python":
  https://github.blog/news-insights/product-news/precise-code-navigation-python-code-navigation-pull-requests/ ;
  changelog "Precise code navigation for TypeScript projects" (2024-03-14):
  https://github.blog/changelog/2024-03-14-precise-code-navigation-for-typescript-projects/
- GitHub Docs, "Navigating code on GitHub" (current language list, tree-sitter
  search-based description):
  https://docs.github.com/en/repositories/working-with-files/using-files/navigating-code-on-github
- Creager & van Antwerpen, "Stack graphs: Name resolution at scale",
  arXiv:2211.01224 (Eelco Visser Commemorative Symposium 2023):
  https://arxiv.org/abs/2211.01224
- Néron, Tolmach, Visser, Wachsmuth, "A Theory of Name Resolution", ESOP 2015:
  https://research.tudelft.nl/en/publications/a-theory-of-name-resolution
- tree-sitter docs, "Code Navigation Systems" (tags.scm):
  https://tree-sitter.github.io/tree-sitter/4-code-navigation.html ;
  "Syntax Highlighting" → Local Variables (locals.scm):
  https://tree-sitter.github.io/tree-sitter/3-syntax-highlighting.html ;
  "Advanced Parsing" → Editing (vendored copy `docs/src/using-parsers/3-advanced-parsing.md`,
  tree-sitter v0.27.1) and `lib/include/tree_sitter/api.h` comments on
  `ts_parser_parse`, `ts_tree_get_changed_ranges`, `ts_node_edit`.
- nvim-treesitter `locals.scm` for bash, c, python:
  https://github.com/nvim-treesitter/nvim-treesitter/tree/master/queries
- Vendored `queries/tags.scm` in `third_party/tree-sitter-c` (b780e47) and
  `third_party/tree-sitter-python` (v0.23.6); no `tags.scm` in
  `tree-sitter-bash` (v0.25.1) or `tree-sitter-json` (254c42a).
- aider, "Repository map": https://aider.chat/docs/repomap.html
- Sourcegraph, "SCIP - a better code indexing format than LSIF" (search-based
  navigation "powered by tools like ctags and tree-sitter"; precise navigation
  needs a compiler-backed indexer): https://sourcegraph.com/blog/announcing-scip
