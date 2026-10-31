# lmscript and lmcode syntax analysis (S4–S5)

Both grammars are independently pinned to immutable MIT source archives in
`deps.lock`. `scripts/sync-deps.sh` checks archive SHA-256 and extracted-tree
hashes. CMake compiles **both parser.c and scanner.c** for each grammar. Do not
omit scanners: lmscript blocks/command literals and lmcode heredocs depend on
external scanner state. No Tree-sitter generator or Node runtime is needed to
run the installed binary.

`--lang lmscript` selects the outer language; `.lms` inference selects it too.
`--lang lmcode` selects standalone inner language; `.lmcode` is the local
standalone convention. Existing `.lms` rule packs remain outer lmscript files,
outer containers whose inner bodies are analyzed as described below. parse/query, JSON lint packs,
compiled `check --lang`, and facts use the same registered grammars.

Facts include function definitions/calls, lmscript imports/packages/schema
loads and markers, and lmcode implements edges. Inner lmcode has no import/schema declaration;
those descriptors belong to the outer lmscript context. Ordinary map keys do
not establish schema references. These are syntactic candidate
references, not schema/type checking. Grammar limitations and source parser
pins are recorded in the upstream grammars' DIVERGENCES.md and MONO-PIN.

## S5: independent injected trees and absolute coordinates

Outer lmscript parsing stays unchanged and exposes named lmcode_body nodes.
The host walks these nodes (including bodies nested in functions/branches) and
parses each range with the lmcode grammar. Tree-sitter included ranges retain
absolute byte offsets and points over a full owned source snapshot. No body
rewriting, splice evaluation, substring-relative arithmetic or combined grammar.
CRLF contributes one row; columns and ranges count UTF-8 bytes, not characters.
Empty bodies have no inner tree; other data blocks and marker text in strings
are not injections. Multiple bodies are independent, in source order.

| Command | Behavior on a .lms file |
| --- | --- |
| parse / query --lang lmscript | Outer syntax only; lmcode_body stays opaque in the outer CST. |
| parse / query --lang lmcode | One inner tree per body; query spans are absolute. |
| facts (inferred or --lang lmscript) | Outer facts plus inner facts labelled lmcode, globally source-sorted. |
| facts --lang lmcode | Inner facts only; directory selection includes .lms containers. |
| lint / ci / test with lmcode JSON rules | Inner nodes, absolute positions; lmscript rules still see only outer syntax. |
| check --lang lmcode RULES FILE or DIR | Dispatches compiled entries over inner trees; one findings group per file. |

A line inserted before a body moves reported rows by one. Splices remain parsed
outer-expression syntax inside the inner grammar; they are never substituted or
executed. Query their splice_* node kinds explicitly. Scope chains are local to
the parsed language; there is no cross-language schema/name resolution.

Facts consumed by index/outline also include inner definitions. Existing indexed
files retain the usual content cache: use index --force once to refresh unchanged
.lms files indexed before S5. DB file language remains lmscript; JSON fact language
identifies the inner grammar. No database schema change is introduced.

Nodes retain full-file backing for text(), snippets, SARIF and absolute edits.
Each injected tree currently owns a source copy: memory scales with body count
and file size. This is one-shot analysis, not an incremental injection API.
Injected fix coordinates are reported, but --fix/--diff still use the existing
whole-language verifier, which can refuse wrappers; injection-aware autofix
verification belongs to the later autofix slice. It never bypasses that guard.
Compiled checks share suppression state across a file's bodies, so warnings are
emitted once. A missing inner body produces no entries, not a fabricated tree.

Tests: check-injections.py has hand-counted byte/point oracles, a CRLF line-shift
control, nested/multiple bodies, UTF-8 before tokens, splices, compiled dispatch,
JSON query/kind rules, ci and pack-author annotations. Native-OS and rule-OS suites
also exercise injection. test.sh runs them on the five native platforms. Optional
scripts/mutate-injections.py builds production copies and challenges missing
injection, relative origin, first-body-only traversal, fact language, suppression
sharing and compiled dispatch. Set LMSCRIPT_BIN and LMCODE_COMPILER_MODULE as for
test.sh; the working sources are never mutated.
