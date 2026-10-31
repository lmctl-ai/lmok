# Tier 3: local DB + knowledge tree (design sketch)

Status: design only, no implementation. Target reader: whoever codes the
v0.6.0 spike. Ground rules from the owner: **local-first, CLI-first, single
static binary, no server/platform/daemon**. Tier 1 (packs) and tier 2
(mechanical extensions) are done; tier 3 is re-scoped from "real semantics"
(types/CFG/dataflow — still deferred) to something tree-sitter CAN do exactly:
a per-file fact extraction from CSTs, persisted in a local DB, chained
cross-file by name, plus a full-text search half.

The product shape: `lmok` becomes the CLI front-end to a project-scale
knowledge base. Half the value is the exact "deep knowledge tree" (every def,
call, import, key path, with exact ranges), half is search (FTS5 over the same
files). lmcode snippets stay the policy layer and gain DB access.

## 1. CLI shape

New/changed verbs (existing `parse`/`query`/`lint`/`--lmcode` behavior unchanged):

```
lmok index [DIR...] [--db PATH] [--force]
lmok symbols [PATTERN] [--lang L] [--kind K] [--db PATH]
lmok refs NAME [--db PATH]
lmok find PATTERN [--lang L] [--db PATH]        # the search half (FTS5)
lmok findings [--rule ID] [--file PATH] [--db PATH]
lmok status [--db PATH]
lmok lint RULES.json FILE... [--db PATH]        # also writes findings
```

- `index` walks DIR (default `.`) with the SAME expansion rules lint uses
  (extensions `.c/.h/.py/.sh/.bash/.json`, hidden-below-arg skipped, sorted
  for stable output), parses each changed file, extracts facts, writes the
  DB. Prints one summary line: `indexed N files (M unchanged, K removed)`.
- `symbols` prints one line per symbol:
  `name kind path start_row:start_col-end_row:end_col` (the `query` output
  convention, plus name/kind). PATTERN is a SQL `LIKE` pattern (`%` wildcard),
  default `%`.
- `refs NAME` prints reference sites in the same shape.
- `find PATTERN` runs FTS5 over stored file text, prints
  `path:row:col: line-text` (grep-shaped, sorted by path then row).
- `findings` prints stored findings as the same JSON lines lint prints
  (row, col, end_row, end_col, rule, severity, message + `"path"` field —
  the one deliberate format extension, since findings outlive a single run).
- `lint ... --db` keeps stdout output unchanged AND records the run in the
  DB (exit codes unchanged: 0/2/3/4).
- `status` prints DB stats: files/symbols/refs/findings counts, DB path,
  schema version, last run.
- Exit codes reuse the existing vocabulary: 2 usage/DB-schema error,
  3 unreadable file/DB, 4 stdout failure. A missing DB on a read verb is
  exit 2 with "run `lmok index` first".

### Where the DB lives

Resolution order (first hit wins):

1. `--db PATH` flag (explicit always wins; parent dirs must exist).
2. `LMOK_DB` env var.
3. `.lmok/lmok.db` at the project root — walk up from cwd for a `.git` dir
   or an existing `.lmok/`; `index` creates `.lmok/` there.
4. Fallback outside a repo: `$XDG_CACHE_HOME/lmok/<sha256-of-abs-cwd>.db`
   (so `lmok index /tmp/thing` still works without polluting it).

`.lmok/` holds ONLY the DB (and SQLite's `-wal`/`-shm` sidecars). Users add
`.lmok/` to `.gitignore` themselves — we never write into VCS files. The DB
is disposable: delete `.lmok` and re-index; no migrations promised across
minor versions in the spike (schema_version in `meta`, mismatch = exit 2
telling the user to re-index).

## 2. DB schema (SQLite + FTS5)

SQLite amalgamation vendored via `deps.lock` like the tree-sitter pins,
compiled with `-DSQLITE_ENABLE_FTS5 -DSQLITE_THREADSAFE=1`, linked into the
same static binary (`ldd` stays clean — SQLite is C, no new runtime deps).
Journal mode WAL (read verbs don't block a concurrent `index`).

```sql
CREATE TABLE meta (
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
-- rows: schema_version (=2), lmok_version, created_at

CREATE TABLE files (
  id         INTEGER PRIMARY KEY,
  path       TEXT NOT NULL UNIQUE,   -- repo-relative, forward slashes
  lang       TEXT NOT NULL,          -- json | c | python | bash
  sha256     TEXT NOT NULL,          -- content hash (invalidation key)
  mtime      INTEGER NOT NULL,
  size       INTEGER NOT NULL,
  indexed_at INTEGER NOT NULL
);

CREATE TABLE symbols (
  id         INTEGER PRIMARY KEY,
  file_id    INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
  name       TEXT NOT NULL,
  kind       TEXT NOT NULL,           -- function|class|variable|macro|struct|key
  start_row  INTEGER NOT NULL, start_col INTEGER NOT NULL,
  end_row    INTEGER NOT NULL, end_col INTEGER NOT NULL,
  scope      TEXT NOT NULL DEFAULT '', -- enclosing path, e.g. "Server.handle"; '' = top level
  confidence TEXT NOT NULL DEFAULT 'exact' -- exact|candidate|unresolved|external
);
CREATE INDEX idx_symbols_name ON symbols(name);
CREATE INDEX idx_symbols_file ON symbols(file_id);

CREATE TABLE refs (
  id         INTEGER PRIMARY KEY,
  file_id    INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
  name       TEXT NOT NULL,           -- identifier being referenced
  kind       TEXT NOT NULL,           -- call|read|write|include|import
  start_row  INTEGER NOT NULL, start_col INTEGER NOT NULL,
  end_row    INTEGER NOT NULL, end_col INTEGER NOT NULL,
  confidence TEXT NOT NULL DEFAULT 'exact'
);
CREATE INDEX idx_refs_name ON refs(name);

CREATE TABLE edges (                 -- import/include graph
  from_file     INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
  to_name       TEXT NOT NULL,       -- as written: "util.h", "os.path", "./lib.sh"
  kind          TEXT NOT NULL,       -- include|import|from_import|source
  resolved_file INTEGER REFERENCES files(id),  -- NULL = external/unresolved
  confidence    TEXT NOT NULL DEFAULT 'exact'
);

CREATE TABLE runs (
  id         INTEGER PRIMARY KEY,
  started_at INTEGER NOT NULL,
  command    TEXT NOT NULL           -- e.g. "lint rules/c-tier1.json src/"
);

-- Every file visited by a recorded lint run, including clean files. Scope is
-- the rule-pack path, so a scan with a different pack cannot reuse findings.
CREATE TABLE run_files (
  run_id  INTEGER NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
  file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
  scope   TEXT NOT NULL,
  PRIMARY KEY (run_id, file_id, scope)
);

CREATE INDEX idx_run_files_scope ON run_files(file_id, scope, run_id);

CREATE TABLE findings (
  id         INTEGER PRIMARY KEY,
  run_id     INTEGER NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
  file_id    INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
  rule       TEXT NOT NULL,
  severity   TEXT NOT NULL,
  message    TEXT NOT NULL,
  start_row  INTEGER NOT NULL, start_col INTEGER NOT NULL,
  end_row    INTEGER NOT NULL, end_col INTEGER NOT NULL,
  superseded INTEGER NOT NULL DEFAULT 0,
  snippet    TEXT                      -- whitespace-collapsed node text (baseline key); NULL in pre-baseline rows
);
CREATE INDEX idx_findings_file ON findings(file_id, superseded);
CREATE INDEX idx_findings_rule ON findings(rule, superseded);

-- the search half: full text mirror of every indexed file
CREATE VIRTUAL TABLE file_text USING fts5(
  path UNINDEXED, lang UNINDEXED, text
);
```

Notes:

- Wave 2A added the `confidence` column to symbols/refs/edges
  (schema_version 2; a v1 DB is refused with exit 2 — delete + re-index).
  `files.path` stores the path AS WALKED (cwd-relative, forward slashes —
  `./main.c` for `index .`), matching what `lint` prints for the same run.
- The baseline wave (lmok 0.7) added `findings.snippet` (nullable) WITHIN
  schema_version 2: an additive nullable column is compatible both ways, so
  writers `ALTER TABLE` it in when missing and readers tolerate its absence
  (`PRAGMA table_info`). Rows recorded before the column existed keep NULL
  and the baseline matcher falls back to keying on (rule, file, message).
- All ranges stay 0-based tree-sitter points, identical to every other
  surface (parse/query/lint) — no 1-based conversion anywhere in the DB.
- `file_text` doubles the DB size (source is on disk AND in FTS). That is the
  accepted cost of `find` being a single indexed query with no re-read; the
  alternative (FTS external-content tables) saves space but breaks when a
  file changes between index and query. Spike takes the simple correct one.
- Row counts scale linearly: ~1 files row, ~5–30 symbols/refs per file. WAL
  + per-file transactions keep a 10k-file index in the seconds range.

## 3. Knowledge-tree model

Extraction is a FIXED SET of tree-sitter queries per language (data, like
rule packs — a `extract/<lang>.json` table the binary embeds), run once per
changed file. This reuses the tier-1 machinery exactly: `query` patterns with
captures, evaluated by the same `node_query` path. Nothing heuristic enters
the per-file facts.

### Per-language facts (all exact from the CST)

**C** (grammar tree-sitter-c):
- symbols/function: `function_definition` declarator identifier; scope = ''
  (C has no nesting).
- symbols/macro: `preproc_def` name.
- symbols/struct: `struct_specifier` name identifier.
- refs/call: `call_expression function: (identifier)`.
- refs/include → edges/include: `preproc_include` path (`string_literal` =
  quoted, `system_lib_string` = angle-bracket).

**Python** (tree-sitter-python):
- symbols/function: `function_definition` name (async included); scope =
  dotted enclosing `class_definition`/`function_definition` names (walk
  `parent()` chain — E2 machinery, already in the binary).
- symbols/class: `class_definition` name.
- symbols/variable: module- and class-level `assignment` left identifier.
- refs/call: `call function: (identifier)` and
  `call function: (attribute attribute: (identifier))` (name = attribute,
  e.g. `system` from `os.system` — the OBJECT is recorded as a separate
  refs/read so `refs os` still finds the module use).
- edges/import / from_import: `import_statement` dotted_name,
  `import_from_statement` module_name (+ `wildcard_import` marker).

**Bash** (tree-sitter-bash):
- symbols/function: `function_definition` name.
- symbols/variable: `variable_assignment` name.
- refs/call: `command name: (command_name (word))` (static words only —
  `"$CMD"` is a runtime dispatch, recorded as refs/read of `CMD` instead).
- refs/read: `simple_expansion`/`expansion` variable_name.
- edges/source: `source` / `.` command argument literal.

**JSON** (tree-sitter-json) — no symbols in the code sense; the knowledge
tree treats key paths as structure:
- symbols/key: every `pair` key `string_content`, scope = dotted parent key
  chain (`server.tls.port`), giving `symbols --lang json` real value for
  config exploration.
- No refs, no edges.

### Chaining cross-file (exact vs heuristic — stated plainly)

- **Exact**: every row's file, range, kind, name-as-written; include/import
  edge existence; relative quoted includes (`"util.h"`) resolved to a
  `files` row when the target is indexed.
- **Heuristic (name-join)**: resolving a refs row to a symbols row is
  `symbols.name = refs.name` — a JOIN, not resolution. Collisions are
  visible, not hidden: `refs parse` in C returns all static `parse`
  functions across files; the caller disambiguates by file. Python dynamic
  dispatch (`getattr`, decorators), bash PATH lookup, and C function
  pointers stay invisible — documented, matching the tier-3 philosophy of
  never faking semantics.
- The tree shape consumers see: file → scope path → symbol → refs (same
  name, all files) → files. `lmok symbols` / `refs` are the two
  navigation primitives; lmcode composes the rest.

## 4. lmcode integration

Scripts run under `lmok --lmcode rule.dslb`; the DB is opened from the same
resolution chain (`--db` flag added to `--lmcode`, then `LMOK_DB`, then
`.lmok/`). New builtins are methods on the existing `Ts` context, declared in
`schema/lmok.host.json` and registered with `reg.method` in
`cpp/main.cpp` — same style as `ts.query_c`. Structured results come back as
JSON ARRAY STRINGS (the `ts.report_json` / `ts.changed_ranges` precedent —
lmcode has no record type, `json_parse` is the decoder):

```json
"db_files":   [{"params": ["string"], "returns": "string"}],
"db_symbols": [{"params": ["string"], "returns": "string"}],
"db_refs":    [{"params": ["string"], "returns": "string"}]
```

- `ts.db_files(lang) -> string` — `[{"path":..., "lang":..., "sha256":...}]`;
  empty string = all languages.
- `ts.db_symbols(pattern) -> string` —
  `[{"name","kind","path","start_row","start_col","end_row","end_col","scope"}]`;
  pattern is SQL LIKE (`%` wildcard), so `"handle_%"` works without a regex
  engine in the SQL layer.
- `ts.db_refs(name) -> string` —
  `[{"name","kind","path","start_row","start_col"}]`, exact name match.

What this unlocks script-side (the point of the design): LBC108 ("call to
undefined function") is currently same-file with a libc whitelist
(`rules/lmcode/c-symbol-rules.lms`). Against the DB it becomes
repo-wide in three lines of lmcode: `json_parse(ts.db_symbols("%"))` for the
defined set, `json_parse(ts.db_refs(...))` per call name for the used set,
StrMap difference as today — the E3 set-algebra pattern, just fed from disk
instead of re-parsing. A DB miss (no `.lmok/`) is a located run-time error
naming the fix ("run `lmok index` first"), exit 3.

## 5. Incrementality

- **Invalidation key**: `files.sha256`. Walk computes `stat` first —
  unchanged (mtime,size) skips the hash; changed stat → re-hash; changed
  hash → re-extract. No content is re-parsed unless the bytes changed.
- **Re-extract per file, transactionally**: one SQLite transaction per
  changed file: `DELETE FROM symbols/refs/edges WHERE file_id = ?`, delete
  the `file_text` row, re-parse, re-insert. Files that vanished from disk
  are deleted (CASCADE cleans symbols/refs/edges/findings).
- **Findings supersession**: `lint --db` inserts a `runs` row, then per
  linted file marks old findings `superseded = 1` and inserts the fresh set
  (superseded = 0). History per run is kept; `findings` defaults to
  `superseded = 0` only, `--run ID` (later) inspects history. A re-linted
  clean file therefore clears its own findings automatically — no stale
  results, the common failure mode of cached linters.
- **`index` after `lint` is safe in either order**: findings are keyed by
  file content only through re-linting; the doc/spec contract is "findings
  reflect the last lint run over each file", not the indexed bytes. Drift is
  visible via `status` (files changed since last lint run — a join on
  indexed_at vs runs.started_at; spike may defer this to a warning line).

## 6. Phasing

**v0.6.0 spike (smallest useful slice)** — one binary, one DB, read back:

1. Vendor sqlite3 amalgamation (`deps.lock` pin, CMake `TS_SOURCES`-style
   source list, `-DSQLITE_ENABLE_FTS5`), static link verified (`ldd` clean
   stays a test.sh check).
2. `index` + `.lmok/` resolution + `--db`: files/symbols/refs/edges for all
   11 current languages via embedded extraction-query tables (the original spike covered four).
3. `symbols`, `refs`, `findings`, `status` read verbs.
4. `lint --db` with run rows + supersession.
5. test.sh: byte-exact checks on a small fixture tree (index → symbols →
   lint --db → findings → edit one file → re-index → only that file's rows
   change). Fixtures under `tests/fixtures/db/`.

Explicitly NOT in the spike: `find`/FTS5 (schema ships, verb lands next),
lmcode `db_*` builtins, scope paths for Python (store '' — name join works
without them), `status` drift warning.

**v0.6.x next**: `find` (FTS5, the search half — the `file_text` table is
already populated by the spike, so this is one query path); the three
`ts.db_*` builtins + one flagship lmcode rule (repo-wide LBC108); Python
scope paths via the existing parent() walk.

**Later / non-goals**: no watch mode, no daemon, no server, no background
indexing (re-index on demand is the whole contract); no cross-language
resolution beyond name join; no SARIF-from-DB until someone asks; no schema
migrations in 0.x (delete + re-index).

Completed lint scans also record `run_files` observations keyed by file and absolute
rule-pack path, including clean files. This additive schema-v2 table is created
on writable opens. DBs without observations conservatively contribute no baseline
allowances; record a fresh scan to establish one. Historical clean scans from
older binaries cannot be reconstructed.
