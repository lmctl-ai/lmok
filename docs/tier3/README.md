# Tier 3 synthesis: knowledge tree + local DB (Lead's consolidation)

Status (2026-10-31): the storage spike is shipped. `facts`, `index`, `symbols`,
`refs`, `findings`, `status`, `outline` and `lint --db` work on current main; facts/parser
coverage is 11 languages. `find`, trigram search and the `ts.db_*`
builtins below remain proposals. See the [verified journey](../user-journey-review.md).
The rest of this document records the original architecture and phasing. Inputs:
`deep-analysis.md` (Fable), `local-index-survey.md` (Astra1), `db-design.md`
(Kimi2). Owner framing: NOT SonarQube parity; half exact knowledge tree, half
search; local DB for storage/retrieval; local-first, CLI-first, no server.

## The converged architecture

Three layers, each independently useful:

1. **Facts layer (extraction)** — per-file, isolated, driven by embedded
   `queries/<lang>/facts.scm` query files (tree-sitter's standard `tags.scm`/
   `locals.scm` vocabulary as the starting point). Emits symbols, refs,
   include/import edges, and scope paths with exact ranges. Every record
   carries a `confidence: exact|candidate|unresolved|external` field — the
   honesty contract that replaces pretending to have semantics. This is the
   "half exact knowledge tree".
2. **Local DB (storage/retrieval)** — one embedded SQLite (amalgamation
   vendored via deps.lock, `-DSQLITE_ENABLE_FTS5`, WAL, static link stays
   clean) at `.lmok/lmok.db` (resolution: `--db` > `LMOK_DB` > `.lmok/` at
   git root > XDG cache fallback). Tables: files (sha256 invalidation),
   symbols, refs, edges, runs, findings (superseded flag), FTS5 `file_text`.
   Facts are the persistent artifact; tree-sitter trees are a session cache
   and are never serialized.
3. **Search half** — FTS5 trigram over stored file text (`find` verb,
   grep-shaped output) with exact-verify after candidate narrowing and a
   correct direct-scan fallback. Broad regexes degrade to O(bytes) scan,
   reported as such.

Access surfaces: CLI verbs (`index`, `symbols`, `refs`, `find`, `findings`,
`status`, `lint --db`) AND lmcode builtins (`ts.db_files/db_symbols/db_refs`,
JSON-array-string returns per the `ts.report_json` precedent) so user rules
become repo-wide without host changes — LBC108 goes from same-file to
whole-repo in ~3 lines of lmcode set algebra.

## Reconciliations between the three docs

- **Fable: "no DB inside the binary, emit JSONL" vs Kimi2/Astra1: "SQLite in
  the binary".** Resolved by the owner's explicit requirement (local DB,
  fast retrieval, CLI-only). Fable's underlying caution — don't pretend
  precision, don't serialize trees, facts are the artifact — is kept: the DB
  stores Fable's fact records verbatim, and `facts`/JSONL export stays
  available for piping to other tools.
- **Astra1's immutable scan generations vs Kimi2's superseded flag.** Spike
  takes the simpler superseded-flag model, with Astra1's two hard rules
  adopted: a failed/interrupted scan is never a successful empty result, and
  project-wide rules rerun conservatively until dependency invalidation
  exists. Generational staging tables are the upgrade path if history
  queries matter.
- **Extraction tables format.** Converged on `facts.scm` query files
  (tree-sitter-standard, C and Python vocabularies already vendored as
  `tags.scm`; nvim-treesitter `locals.scm` covers C/Python/Bash) over a
  bespoke JSON table. Same evaluation path as tier-1 packs.
- **Scope of resolution.** Cross-file = literal edge resolution (quoted
  includes, relative imports) + name JOIN with all candidates kept and a
  `via` path shown. Nearest-scope/nearest-file precedence is data in the
  record, never silently applied. No type inference, no preprocessor
  evaluation, no macro expansion, no CFG/dataflow/taint, no `eval`/PATH
  lookup. Documented, matching "never fake semantics".
- **Stack graphs**: right architecture (per-file isolation + late binding),
  wrong dependency (archived upstream, RUSTSEC-2026-0303, no C API, no
  C/Bash/JSON rule sets). We borrow the architecture; we do not link or
  port it.

## Phasing

**v0.6.0 spike (smallest useful slice)** — from db-design.md §6, merged with
deep-analysis.md sequencing:

1. Vendor SQLite amalgamation (deps.lock pin, CMake, FTS5 on, `ldd`-clean
   check in test.sh).
2. `facts.scm` for C + Python (from vendored `tags.scm` / nvim `locals.scm`);
   `index` verb: walk (lint's expansion rules) → parse changed files
   (stat-then-sha256) → extract → write files/symbols/refs/edges
   transactionally per file. `.lmok/` resolution + `--db`.
3. Read verbs: `symbols`, `refs`, `findings`, `status`; `lint --db` with run
   rows + supersession.
4. test.sh: byte-exact fixture-tree checks (index → symbols → lint --db →
   findings → edit one file → re-index → only that file's rows change).

**v0.6.x**: `find` (FTS5 table already populated by the spike); the three
`ts.db_*` builtins + repo-wide LBC108 as the flagship; `facts.scm` for Bash
+ JSON; Python scope paths; `outline` verb (aider-style repo map with raw
reference/import counts — the AI-facing knowledge-tree view); confidence
labels wired end-to-end.

**Deferred indefinitely**: type inference, preprocessor evaluation, macro
expansion, CFG/dataflow/taint, cross-language resolution beyond name join,
watch mode/daemon/server, schema migrations in 0.x (delete + re-index).

## Open questions for the owner

1. Spike approved as scoped above? (Estimate from the survey: 15–25
   engineer-days for the full storage/search foundation; the spike slice is
   roughly the first week of that.)
The binary/product rename to `lmok` is complete; release assets, `test.sh`
and CI artifact names use `lmok`.

**E9 landed:** [`lmok outline`](outline.md) uses live facts for definitions
and an optional read-only DB for reference-name ranking, with a whole-block
line budget. No daemon or DB is required.

## Measured scale

[Scale validation](scale-validation.md) records cold/warm/1% indexing, query
latency, resource stress, and a subsecond timestamp invalidation fix. Cold curl
indexing exceeded 10 seconds; the mixed corpus exceeded the DB-size target.
