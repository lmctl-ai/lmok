# The lint dispatcher (v0.2.8)

`lmok lint RULES.json FILE...` is the host-side tree-walk dispatcher from
the architecture review ("condition as data, action as annotated function").
Today the conditions are JSON rule packs; the dispatch core is built so the
v6 bytecode rule entry table (host-callable annotated lmcode fns) plugs into
the same kind-indexed walk later.

## Structure (cpp/lint.hpp / cpp/lint.cpp)

- Pack loading uses the vendored SDK's own JSON parser
  (`dslvm::stdlib::json_detail::Parser` → `JsonNode`) — no new dependency.
  Validation throws plain `dslvm::Error` (exit 2) naming the rule id; a bad
  query pattern surfaces at dispatch time as `QueryError` (exit 3) with the
  rule id prepended to the usual "query error at byte offset N: reason".
- Precedence: `rule.lang` > pack `lang` > pack error (exit 2). `on` and
  `query` are mutually exclusive per rule (both present = exit 2) — one
  selector per rule keeps the data shape aligned with the v6 @annotation
  (one node kind) vs host-query split.
- Dispatch per file, once per grammar used: `on` rules share ONE pre-order
  walk — `tree_nodes()` flatten, then a `std::map<string, vector<rule*>>`
  kind index (document order preserved within a kind; findings globally
  sorted by (row, col) at the end). `query` rules run one
  `ts_query_cursor` per rule over the root — per-rule cursors today; folding
  query evaluation into the single walk is a later optimization,
  deliberately no Rete.
- `text_matches` is `std::regex` ECMAScript via `regex_search` (unanchored;
  packs anchor with `^`/`$` themselves), applied post-match to the node's
  text through `regex_search_bounded` (host.hpp): texts over 16 KiB are not
  searched (no match + one stderr note). Reason: libstdc++/libc++ regex
  backtracks recursively; the shipped LBC010 rule segfaulted on a 40 KB
  comment with an 8 MB stack (a few KB on Windows' 1 MB). The same bound
  covers query `#match?`. Rule ids must be unique within a pack (exit 2).
  `on` matches NAMED nodes only (the flatten skips anonymous tokens).
- Output is one JSON object per line with the `ts.report_json` field order
  (row, col, end_row, end_col, rule, severity, message) plus `N findings in
  <file>` per file. Exit 0 regardless of findings (gating is the caller's
  job); the first unreadable file aborts the run with exit 3 (fail fast —
  earlier files' output already printed).

## v6 trajectory

The `on` field value IS the `@ANNOTATION` value: a node kind. When v6
bytecode carries a rule entry table (annotation + interface per rule), a
`--lmcode` chunk can export `implements Rule` fns indexed by kind; the
dispatcher then walks once and calls the exported fn for each hit instead of
emitting a finding directly — the lmcode side supplies the action (report,
suppress, SARIF, all already demonstrated script-side). Until then,
`text_matches` covers the condition half and `message` the action half.
