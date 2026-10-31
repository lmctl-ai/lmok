# Purpose and place

lmok repackages tree-sitter as one statically linked, scriptable CLI: the
upstream C library (v0.27.1, pinned in `deps.lock`) plus 11 grammars: JSON,
C, Python, Bash, Java, Go, JavaScript, TypeScript, TSX, lmscript and lmcode.
The CLI includes parsing, linting, facts and a SQLite knowledge tree, plus an
lmscript lmcode host. Sibling of
lmproxy (Squid) and lmcurl (curl); same house conventions: vendored sources
in git-ignored `third_party/`, per-file SDK hashes in `sdk.lock`, schema
embedded by `cmake/embed_schema.cmake`.

The tree-sitter core is compiled through its amalgamation `lib/src/lib.c`
alone (never together with the individual `lib/src/*.c` files); each grammar
contributes `src/parser.c`, plus external scanners where supplied (including Python, Bash, JavaScript, TypeScript and TSX) (Go, C and
JSON have no external scanner). Upstream feature-test defines `-D_POSIX_C_SOURCE=200112L
-D_DEFAULT_SOURCE` apply target-wide.

The lmcode surface is deliberately minimal: context `ts` (type `Ts`)
with `ts.parse(text) -> Tree`; `Tree.sexp() -> string`, `Tree.has_error() ->
bool`, `Tree.node_count() -> int` (total nodes including the root), plus —
since v0.2.0 — `Tree.root() -> Node`, the `Node` navigation methods and the
`Rule` SAM interface (see [03-rule-methods.md](03-rule-methods.md)); since
v0.2.1 `Tree.nodes() -> NodeList`; since v0.2.2 the `ts.report*` findings
sink; since v0.2.3 `ts.query(root, pattern) -> NodeList` (S-expression
selectors); since v0.2.4 the C grammar via `ts.parse_c` / `ts.query_c` and
`--lang json|c` on the CLI; the python and bash grammars via
`ts.parse_python` / `ts.query_python` / `ts.parse_bash` / `ts.query_bash`
(plus `reparse_python` / `reparse_bash`) and `--lang python|bash` landed
after v0.4.0. Go (v0.25.0), JavaScript (v0.25.0), and TypeScript (v0.23.2) are
CLI grammars for `--lang go` / `--lang javascript` / `--lang typescript` /
`--lang tsx`, `.go` / `.js` / `.ts` / `.tsx` lint/facts inference, and their tier-1 packs. They do not add
`ts.parse_go`/`ts.query_go`, `ts.parse_javascript`/`ts.query_javascript`, or
`ts.parse_typescript`/`ts.query_typescript` or `ts.parse_tsx`/`ts.query_tsx`;
the existing explicit lmcode surface stays unchanged for this slice. No cursor API yet.

The lmcode-cpp SDK pin (`sdk.lock`) must match the source compiler selected by
`compiler.lock`. Bootstrap that compiler as described in the [build instructions](../README.md#build-and-run);
npm version strings alone do not establish compatibility. SDK updates use
`LMCODE_CPP=/path/to/lmcode-cpp LMCODE_CPP_REF=<commit> scripts/sync-sdk.sh`.
