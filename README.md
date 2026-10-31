# lmok

tree-sitter's incremental parsing engine as a single statically-linked,
scriptable CLI.

lmok embeds [tree-sitter](https://tree-sitter.github.io) 0.27.1 and **11
languages**: JSON, C, Python, Bash, Java, Go, JavaScript, TypeScript, TSX,
lmscript and lmcode. The last two are syntax-analysis grammars; embedded
lmcode bodies in `.lms` files remain opaque. No Node runtime is needed to
run the standalone binary.

- Parse/query source, extract facts, measure local metrics and token duplication.
- Lint with **15 JSON packs (100 rule entries)** and **14 lmcode rule programs**;
  see the [complete pack inventory](rules/README.md#pack-inventory).
- Build a local SQLite knowledge tree with `index`, `symbols`, `refs`,
  `findings` and `status`; `facts` also exports records without a DB.
- Emit SARIF 2.1.0, compare baselines, preview/apply guarded autofixes, scan
  secret-shaped literals with redacted output, and honor inline suppressions.
- Run configured project checks through `ci`, or script the host with
  [lmscript](https://github.com/lmctlhq/lmctl-mono).

These are syntax checks and candidate references, not type inference or taint
analysis. The CLI below describes current main; published artifacts are
identified separately under [install](#published-host-and-raw-urls).

## Build and run

Building needs Bash, Git, curl, tar, SHA-256 tools (`sha256sum` or macOS
`shasum`), Python 3, CMake >= 3.24, Ninja, and C/C++17 compilers. Linux's
static link needs static runtime libraries (`build-essential` on Ubuntu).
macOS needs Xcode Command Line Tools plus CMake/Ninja. Node.js >= 24.15.0
and npm are needed for scripting and the test compiler, not for the binary.
Network access is needed for the initial source/dependency downloads.

```sh
git clone https://github.com/lmctl-ai/lmok.git
cd lmok
scripts/sync-deps.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
export PATH="$PWD/build:$PATH"
build/lmok parse examples/data/sample.json
```

The standalone host build above needs no compiler checkout or Node runtime.
**Scripting and the full test gate currently require Git access to the private
`lmctlhq/lmctl-mono` repository.** With that access configured:

```sh
export LMSCRIPT_BIN="$(scripts/bootstrap-compiler.sh)"
export PATH="$(dirname "$LMSCRIPT_BIN"):$PWD/build:$PATH"
./test.sh
"$LMSCRIPT_BIN" examples/parse-string.lms
```

Run repository examples from this directory. Bootstrap builds the exact
`compiler.lock` source revision once and caches it outside `build/`; reuse
`LMSCRIPT_BIN` in this shell. The npm-only lmscript package lacks the compiler
library needed by fixture export and cannot run the full gate. Compiler
version strings alone do not establish compatibility. No sibling SDK checkout
is needed: its headers are already vendored.

Produces `build/lmok`; `ldd build/lmok` reports "not a dynamic executable" on
Linux (macOS links system libraries). The test suite prints `PASS: N checks`.
The parse-string script constructs and runs a program directly; expect the
JSON text, its tree, `has_error -> false`, and `nodes -> 34`. It does not save
a `.dslb` file. See [all examples](examples/README.md).

## Published host and raw URLs

Verified on 2026-10-31: the latest GitHub Release is **v0.6.11**, newer than
v0.6.0. Main and the rolling `dist` branch can advance independently; use a
release tag for reproducibility. The five assets are `lmok-linux-x64`,
`lmok-linux-arm64`, `lmok-macos-x64`, `lmok-macos-arm64`, and
`lmok-windows-x64.exe`, plus `SHA256SUMS`.

**Compatibility:** the verified v0.6.11 and dist hosts expose `--dsl`,
`--dsl-schema` and `--dsl-signatures`. Current main exposes only `--lmcode`,
`--lmcode-schema` and `--lmcode-signatures`. Use the source-built host with
main's pinned compiler and scripts; installing the latest release alone is
not a substitute. The native parsing, linting and DB journeys were verified on both snapshots.

With npm lmscript installed (the installer below was checked with
`@lmctl-ai/lmctl@0.2.449`):

```sh
npm install -g @lmctl-ai/lmctl@0.2.449
lmscript install github:lmctl-ai/lmok@latest
```

Use **`lmscript install`**, not `lmctl install` (the tested lmctl CLI rejects
that command). It prints the resolved tag, executable path and checksum; add
the printed executable's directory to PATH to invoke `lmok` by name. It does
not add a shell command itself. `@v0.6.11` selects that fixed release.
Installing a host does not install the source compiler needed by this repo's
gate; use the bootstrap above when working from main (it currently requires
private-repository access).

Without Node, download a binary directly. This example is for **Linux ARM64**;
choose the matching asset from the list above for another platform:

```sh
mkdir -p build/lmok-download
curl -fL https://github.com/lmctl-ai/lmok/releases/download/v0.6.11/lmok-linux-arm64 -o build/lmok-download/lmok-linux-arm64
curl -fL https://github.com/lmctl-ai/lmok/releases/download/v0.6.11/SHA256SUMS -o build/lmok-download/SHA256SUMS
(cd build/lmok-download && sha256sum --ignore-missing -c SHA256SUMS)
chmod +x build/lmok-download/lmok-linux-arm64
build/lmok-download/lmok-linux-arm64 --version
```

For a rolling dist snapshot, substitute
`https://raw.githubusercontent.com/lmctl-ai/lmok/dist/` for the release URL
prefix in both downloads. A fixed dist commit in place of `dist` keeps the
binary and checksums on the same snapshot. These flows were executed on Linux
ARM64; other assets were inventoried, not executed in this review.

With npm lmctl installed as above, complete source setup and put the built host on PATH:

```sh
lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmok/main/examples/parse-string.lms
```

`LMSCRIPT_BIN` selects the bootstrapped interpreter for this invocation.
The local form `"$LMSCRIPT_BIN" examples/parse-string.lms` works too.
`run_lmcode` executes the program and forwards its output; there is no second
bytecode-execution step. Raw URLs fetch a script, not its repository data.
Scripts reading `examples/data/` or `tests/fixtures/` still need the clone and
repository-root cwd; artifact-writing examples need `build/` to exist.

## CLI

Use `build/lmok` below after building. These examples run from the repository
root. Prepare scratch outputs first (`mkdir -p build/journey`). Only the
`check` and `--lmcode` rows need these two bytecode fixtures and the restricted
compiler setup above:

```sh
mkdir -p build/journey
node scripts/compile-fixture.mjs examples/parse-string.lms build/journey/parse-string.dslb
node scripts/compile-fixture.mjs rules/lmcode/c-function-metrics.lms build/journey/metrics.dslb
```

`compile-fixture.mjs` is the repository's maintainer-only fixture exporter;
it requires the bootstrapped `LMSCRIPT_BIN`. Ordinary scripts use `run_lmcode`
instead of a public bytecode-export command.

| Verb / option | What it does | Working example |
| --- | --- | --- |
| `parse` | Print the concrete syntax tree. | `build/lmok parse examples/data/sample.json` |
| `query` | Print captured nodes and zero-based ranges. | `build/lmok query '(number) @n' examples/data/sample.json` |
| `lint` | Run a JSON rule pack. | `build/lmok lint rules/c-tier1.json examples/data/sample.c` |
| `ci` | Run project config and enforce its severity gate. | `build/lmok ci tests/fixtures/project-ci` (expected exit **1**, planted finding) |
| `rules` | List rules and metadata as JSONL. | `build/lmok rules rules/c-tier1.json rules/json-tier1.json` |
| `test` | Verify annotated positive/negative pack fixtures. | `build/lmok test examples/self-test/pack.json examples/self-test/fixture.c` |
| `facts` | Extract definitions/references without a DB. | `build/lmok facts tests/fixtures/db/index/main.c` |
| `index` | Persist file facts; skip unchanged files. | `build/lmok index tests/fixtures/db/index --db build/journey/index.db` |
| `symbols` | Query stored symbols by SQL LIKE name pattern. | `build/lmok symbols '%' --lang c --db build/journey/index.db` |
| `refs` | Query stored references by exact spelling. | `build/lmok refs helper --db build/journey/index.db` |
| `findings` | Read live stored findings (empty before recording lint). | `build/lmok findings --db build/journey/index.db` |
| `status` | Print DB location, counts and last run. | `build/lmok status --db build/journey/index.db` |
| `check` | Dispatch compiled entry rules over source inputs. | `build/lmok check --lang c build/journey/metrics.dslb tests/fixtures/c-logic-violations.c` |
| `outline` | Print live definitions, optionally ranked by DB references. | `build/lmok outline tests/fixtures/outline --budget 30` |
| `metrics` | Report lines, functions and maximum nesting. | `build/lmok metrics examples/data/sample.c` |
| `dup` | Find repeated normalized token windows within files. | `build/lmok dup --tokens 3 examples/data/sample.c` |
| `--lmcode` | Run a compiled program body. | `build/lmok --lmcode build/journey/parse-string.dslb` |
| `--version` | Print the binary's build version. | `build/lmok --version` |
| `--notices` | Print embedded dependency licenses. | `build/lmok --notices` |
| `--lmcode-schema` | Inspect the host/standard schema. | `build/lmok --lmcode-schema --json` |
| `--lmcode-signatures` | List registered signatures. | `build/lmok --lmcode-signatures` |

Run the DB rows in table order. `find` remains a design proposal. `outline`
is available on current main; the checked v0.6.11/dist snapshots predate it. `--lang` accepts `json`, `c`, `python`, `bash`, `java`,
`go`, `javascript`, `typescript`, `tsx`, `lmscript`, or `lmcode` for the
source-analysis verbs. `index` infers languages from extensions.

`parse` prints the concrete syntax tree as a single-line s-expression, like
`tree-sitter parse` does. Malformed input still prints its tree — broken
regions appear as `(ERROR ...)` nodes — and still exits 0; tree-sitter always
produces a tree. `query` runs a tree-sitter S-expression pattern over the
document and prints one line per captured node, `kind
start_row:start_col-end_row:end_col` (0-based). Both default to the JSON
grammar; `--lang L` switches grammars (an unknown language exits
2 with the known-languages list). `query --pattern` takes a code-shaped
(semgrep-style) pattern instead of an S-expression — `strcpy($DST, $SRC)`, `$F(...)`, `if ($C) { return $V; }` —
compiles it with the same grammar (first slice: c and python), prints the generated query on a `query:` line and
then the matched nodes; `$NAME` matches any one node (the same name twice means equal text), `...` any call
arguments, everything else is literal code; whitespace and comments in the pattern do not matter. A pattern
that does not parse, holds several statements, or uses `...` outside a call exits 2 naming the fragment.
The same patterns work in rule packs (`pattern`, below). `lint` runs a rule pack (below) over each file and
prints one JSON finding per line plus `N findings in <file>`; exit codes:
0 ok (findings never fail the run — gating is the caller's job, or use
`--fail-on-new`, below), 2 usage or
rule-pack error, 3 input/lmcode error (unreadable file aborts the run fast; an
invalid query pattern names the rule id, byte offset and reason), 4 stdout
failure or new findings under `--fail-on-new`.

```
$ echo '{"a": [1, null]}' | lmok parse -
(document (object (pair key: (string (string_content)) value: (array (number) (null)))))
$ lmok query '(pair key: (string (string_content) @key))' examples/data/sample.json
string_content 1:3-1:7
string_content 2:3-2:10
string_content 3:3-3:5
string_content 4:3-4:12
$ lmok lint examples/rules/c-security.json examples/data/sample.c
{"row":4,"col":2,"end_row":4,"end_col":6,"rule":"LBC001","severity":"critical","message":"unsafe call 'gets'"}
1 findings in examples/data/sample.c
```

The `--lmcode` interface runs the body of lmscript bytecode against the same engine
with structured access to parsed trees. `check RULES.dslb FILE|DIR... [--no-suppress]` is the v6
entry path: it loads one DSLB file, indexes entries by annotation, runs its
body once, then parses and walks each explicit input file once. It dispatches
matching entries in source order, including anonymous tree-sitter kinds, and
emits one output group per input: the `ts.report_json()` findings array, `N findings in FILE`
(the same line `lint` prints; FILE is the path as given) and the standard success record.
Findings reported by the body form their own group, `N findings in body`, printed before the
first file only when the body reported something. If an entry fails mid-walk, the group collected
so far is printed, then the located error record, and the run stops with exit 3 (later inputs are
not processed). File and directory inputs use the shared language-aware
expansion; stdin is not accepted. A fresh host arena and findings sink are used per
input. One invocation-owned return sink survives those per-file resets. Rule and severity metadata come from `ts.report` payloads; entry names
are not silently used as finding ids. Every annotation is checked against the
selected grammar at load: a misspelled kind, a supertype (abstract, e.g. `@expression`) or a hidden
symbol is refused with exit 3 and a `LOAD_UNBOUND` record naming the entry, so a rule can never be
silently dead; a rules file is therefore specific to one `--lang`. lmcode bytecode (DSLB v6) is the only accepted runtime
format, so old bytecode must be recompiled. Use the pinned source compiler from the build instructions for fixtures.

Successful `--lmcode` programs and `check` invocations append a terminal frame after
the existing diagnostics (and after successful SARIF output):

```text
LMCODE_RETURN_BEGIN
{"ok":true,"result":null}
LMCODE_RETURN_END
```

The envelope's result is the program's `$return` value, or `null` when unset. Body and
rule-entry writes share the sink across all nodes and input files; `--lmcode`
continues to run only the body. Repeated `--lmcode FILE` pairs form one invocation, with add-order results and
required `parts`. Each program owns its sealed sink; host state is shared.
`--first FILE` / `--last FILE` roles reorder execution while retaining add order.
Tagged host_summary diagnostics preserve completed-program/input summaries. Runtime or application failures emit one failure envelope without result/parts. Finding
IDs, order, suppression and entry HALT values retain their existing meanings.
The [body/entry return example](examples/invocation-return.lmcode) uses the pinned source compiler from `compiler.lock`. See [invocation ownership and compiler staging](durable-memory/14-invocation-return.md).

## Local database status

```sh
lmok status [--db PATH]
lmok status --init [--db PATH]
```

`status` prints the absolute DB path, schema version, row counts for files,
symbols, refs, edges, runs and findings, and the last run (latest `started_at`,
then id; `none` for an empty DB). Counts include superseded findings.
It opens an existing DB read-only. If absent, it exits 2 with
"run `lmok index` first" and creates neither the DB nor its directory.
`index` is implemented; it populates file facts and text for the read verbs.

Explicit `status --init` creates the empty schema and prints status.
It preserves a compatible existing DB and never overwrites an incompatible
one. This is a bootstrap/diagnostic operation, not an indexing operation.

## Recording lint runs (`lint --db`, `findings`)

```sh
mkdir -p build/journey
build/lmok index tests/fixtures/db/index --db build/journey/index.db
build/lmok symbols '%' --lang c --db build/journey/index.db
build/lmok refs helper --db build/journey/index.db
build/lmok findings --db build/journey/index.db
build/lmok lint examples/rules/c-security.json examples/data/sample.c --db build/journey/index.db
build/lmok findings --rule LBC001 --db build/journey/index.db
build/lmok status --db build/journey/index.db
```

`lint ... --db PATH` leaves stdout and exit codes byte-for-byte unchanged and
additionally records the run: one `runs` row (start time, command string),
then per linted file the file's previous findings are marked superseded and
the fresh set inserted — all in ONE transaction, and only after every target
linted cleanly, so a failed or interrupted lint never masquerades as a clean
result. A missing DB is created through the same resolution chain as `index`
(lint --db is a write operation; explicit paths still need an existing parent
directory). A linted file with no `files` row yet gets a minimal one (real
content hash, mtime, size) so findings never dangle. If recording itself
fails (unwritable path, schema mismatch), the lint still succeeds and a
one-line note goes to stderr — the DB is a side channel, never a lint
failure mode.

`findings` prints the live (non-superseded) findings as the same JSON lines
lint prints plus a `"path"` field, sorted by path then position; `--rule`
and `--file` filter (exact stored path spelling). A missing DB is exit 2
with "run `lmok index` first".

Paths resolve in order: `--db PATH`, nonempty `LMOK_DB`, `.lmok/lmok.db` at
the nearest ancestor with `.git` (directory or worktree file) or `.lmok/`,
then `$XDG_CACHE_HOME/lmok/<sha256-of-absolute-cwd>.db`. Without XDG, use
`$HOME/.cache` (`USERPROFILE` is a Windows fallback if HOME is unset).
The hash uses the UTF-8 absolute cwd with forward slashes. Relative explicit
or environment paths are relative to cwd; their parent directories must
already exist. Only write operations create automatic project/cache dirs.
The tool never edits `.gitignore`.

SQLite 3.54.1 is compiled into the binary; no system SQLite or SQLite CLI is
needed. Writers enable WAL with a five-second busy timeout and foreign-key
checks. Readers see one transaction snapshot. The [schema](docs/tier3/db-design.md#2-db-schema-sqlite--fts5)
is embedded verbatim, including its FTS5 default tokenizer; trigram search
is future work. Schema-version mismatches exit 2 with delete/re-index guidance;
unreadable/corrupt DBs exit 3, and stdout failures exit 4.

## New-code-only linting (`--new-since`, `--baseline`, `--changed`, `--fail-on-new`)

`lint` can report only findings that are NEW relative to a baseline. A finding
is new when its key — (rule, file, message, normalized snippet) — has no
unmatched baseline occurrence. The snippet is the matched node's source text
with whitespace runs collapsed to one space; positions are deliberately NOT
part of the key, so inserting lines above a violation does not resurrect it as
new. Keys are multisets: if a baseline holds one `goto used` for a file and
the file now has two identical ones, the second is new.

```sh
build/lmok lint examples/rules/c-security.json examples/data/sample.c --db build/journey/index.db --new-since last --fail-on-new
build/lmok lint rules/c-tier1.json examples/data/sample.c > build/journey/baseline.jsonl
build/lmok lint rules/c-tier1.json examples/data/sample.c --baseline build/journey/baseline.jsonl --fail-on-new
lmok lint rules/c-tier1.json . --changed          # working tree vs HEAD
lmok lint rules/c-tier1.json . --changed=HEAD~3
```

- `--new-since RUN` reads the findings DB (`--db PATH` when given, else the
  standard resolution chain, opened read-only): RUN is a `runs` id (see `lmok
  status`) or `last` for the latest recorded run. The latest completed scan
  at or before RUN for each file and absolute rule-pack path supplies its
  baseline occurrences. A clean scan clears that file's prior occurrences;
  partial scans preserve observations for unvisited files. Repeated scans
  do not accumulate allowances. Runs are not tagged; pick the run by id or `last`. A missing DB, an unknown run or a bad selector is exit
  2. Within databases that have scan observations, rows recorded before the
  `findings.snippet` column existed match on (rule, file, message) alone.
  Databases without scan observations conservatively supply no allowances;
  record a fresh scan to establish a baseline.
- `--baseline FILE` uses a previous lint JSON-lines output file instead of the
  DB (finding lines attach to the file named by their following `N findings
  in PATH` line). Credential findings include a safe `snippet: "sha256:..."`
  identity, so rotations remain new without exporting source. Ordinary legacy
  lines use the (rule, file, message) fallback. Credential findings never use
  snippet-less fallback entries; regenerate old credential baselines. An unreadable file is exit 3; lines that lack the key
  fields simply never match, so their findings report as new. `--baseline`
  and `--new-since` are mutually exclusive (exit 2). Suppression-bearing
  groups and ambiguous filenames carry a JSON `file` key so the human footer
  cannot corrupt their durable path; old suppression footers remain readable.
- `--changed[=REV]` restricts the targets to the files `git diff --name-only
  REV` reports for the repository containing the working directory (bare
  `--changed` = `HEAD`, i.e. working tree vs HEAD, staged or not; untracked
  files are not listed by git diff). Outside a git repository, or for an
  unknown revision, it fails fast with exit 2.
- `--fail-on-new` (requires `--new-since` or `--baseline`) exits 4 when any
  new finding exists, 0 otherwise. Without it the exit code is unchanged (0;
  findings never fail the run).

Output is unchanged per file (only new findings print, and the `N findings in
FILE` line counts them; suppressed findings are dropped before baseline
matching, so they never become new) plus one trailing summary line
`N new, M baseline`. Recording (`--db`) always stores the unfiltered set:
the DB is reality, the baseline is a view.

## SARIF output (`--sarif FILE`)

```sh
build/lmok lint examples/rules/c-security.json examples/data/sample.c --sarif build/journey/results.sarif
```

This writes one LBC001 result. `lint`, `check` and `ci` accept `--sarif FILE`: the normal stdout is
unchanged and a SARIF 2.1.0 log (the
[examples/sarif-report.lms](examples/sarif-report.lms) mapping, ported into
the host) is written to FILE after the run completes — version 2.1.0 with the
schemastore `$schema`, one run, `tool.driver.name` "lmok". Positions convert
0-based to 1-based (`startLine`/`startColumn`; `endColumn` 1-based exclusive).
Severity maps to level exactly like the example: `critical` → `error`,
`minor` → `note`, everything else → `warning`. For `lint`/`ci`,
`driver.rules` comes from the pack metadata: the rule `id`,
`shortDescription` from the pack message, and `helpUri` from the rule's
top-level `url` when carried. For `check` (lmcode rules carry no pack) the
rule catalog is derived from the results, distinct ids in first-seen order;
`check` body findings (no file) are emitted without `locations`. Under
`--new-since`/`--baseline` the SARIF contains the printed (new) findings
only. With zero findings the file is still written, with an empty `results`
array. An unwritable FILE is exit 3.

## Metrics

`lmok metrics FILE...` prints one JSON object per file with physical `lines`,
`functions`, and maximum control-flow `max_nesting`. Use `--lang` when a file
has no recognized extension. `lmok dup [--tokens N] FILE...` hashes normalized
token windows within each file (identifiers and literals are replaced), and
prints duplicate ranges; the default minimum window is 20 tokens.

## Rule packs

`lmok lint` reads a pack — conditions as data. Format:

```json
{
  "lang": "c",
  "rules": [
    {"id": "LBC001",
     "query": "(call_expression function: (identifier) @fn)",
     "text_matches": "^(gets|strcpy|sprintf)$",
     "message": "unsafe call '{text}'",
     "severity": "critical"}
  ]
}
```

- `id` (required string), `message` (required; `{text}` expands to the
  matched node's text), `severity` (optional, default `"minor"` — free-form,
  the pack owns the vocabulary).
- Selector — exactly one of (both present is a pack error, exit 2):
  - `on`: a tree-sitter node kind, matched against NAMED nodes. This mirrors
    the `@ANNOTATION` value of an lmcode rule fn. The `check` subcommand uses
    the v6 entry table and this same kind-indexed dispatcher.
  - `query`: an S-expression pattern; every captured node of every match is a
    candidate.
  - `pattern`: a code-shaped pattern (`strcpy($DST, $SRC)`, `$F(...)`, see
    `query --pattern`), compiled at pack load with the rule's grammar (c and
    python); the whole match is the candidate, or the node bound by the
    metavariable named in `capture` (`"pattern": "$F(...)", "capture": "F"`
    reports the callee, so `text_matches` filters on it). `pattern-inside` /
    `pattern-not-inside` keep a candidate only if some ancestor is (is not) a
    match of that pattern (`"pattern-inside": "while ($C) $B"`; a lone
    metavariable in a statement slot binds any statement or block). A pattern
    that does not compile, a `capture` naming no metavariable, or an ancestor
    filter without `pattern` is a pack error (exit 2).
- Query rules may set `"capture": "name"` to emit only that capture; other
  captures remain available to predicates. This keeps credential names out of
  the secret finding candidates.
- `text_matches`: optional `std::regex` (ECMAScript) applied with
  `regex_search` (unanchored — anchor with `^`/`$` yourself) to the
  candidate's text as a post-match filter. Regex matching is bounded: node
  texts longer than 16 KiB are not searched (treated as no match, one note
  on stderr), because `std::regex` backtracks recursively and a long comment
  or string would otherwise overflow the stack. The same bound applies to
  query `#match?`.
- Language: `rule.lang` overrides the pack-level `lang`; a rule with neither
  is a pack error. Known languages: `json`, `c`, `python`, `bash`, `java`,
  `go`, `javascript`, `typescript`, `tsx`, `lmscript`, `lmcode`.
- Dispatch: per file, parse once per grammar used. `on` rules share ONE
  pre-order walk over the named nodes with a kind→rules index; `query` rules
  run one query cursor each over the root (merging them into the walk is a
  later optimization — deliberately no Rete). Findings print sorted by
  (row, col), one JSON object per line with the same field order as
  `ts.report_json` (row, col, end_row, end_col, rule, severity, message),
  then `N findings in <file>`.
- **Directories**: a FILE argument that is a directory expands recursively to
  its regular files — hidden files/dirs BELOW the argument skipped (the
  argument itself may live under a dot-path), only `.c`/`.h` (c), `.json`
  (json), `.py` (python), `.sh`/`.bash` (bash), `.java` (java), `.go` (go),
  `.js` (javascript), `.ts` (typescript), `.tsx` (tsx), `.lms` (lmscript) and
  `.lmcode` (lmcode) are recognized, paths sorted
  for byte-stable output, a file whose
  language has no rules in the pack is skipped silently, and a directory
  with no lintable files is a usage error (exit 2). A directory the walk
  cannot read aborts the run (exit 3, like an unreadable file) — never a
  silent partial list. Symlinks to files are followed, symlinks to
  directories are not. File and directory args mix freely. Rule ids must be
  unique within a pack (exit 2 otherwise).

`lmok ci` reads `ROOT/.lmok/config.json` (or `--config FILE`), loads every
configured pack, and emits JSONL findings followed by a nested summary. It
honors `.lmokignore`, configured ignore globs, inline `lmok-disable` markers,
and `--no-suppress`; a finding at or above `severity_floor` returns exit 1
when `fail_on` is `findings`. See [project CI](durable-memory/09-project-ci.md)
for the versioned config format and baseline hook.

Packs live in `examples/rules/` (`c-security.json`, `json-config.json`) as
small demos; the canonical rule library is `rules/` (see below).

## Rule tiers

Rules are classified by the information they need: tier 1 (CST shape + text)
ships as **15 JSON packs with 100 rule entries** in `rules/`: nine language
packs (C, JSON, Python, Bash, Java, Go, JavaScript, TypeScript and TSX), four
secrets packs (Bash, C, JSON, Python), and two autofix packs (Bash, C).
See the [complete filename/count table](rules/README.md#pack-inventory).
lmscript and lmcode have parsing/facts support but no shipped JSON pack.
The **14 lmcode-native**
rules in `rules/lmcode/` cover checks that need logic (function metrics, switch/if style, duplicate JSON
keys and sibling string values, object size, nesting depth via `Node.has_parent()`/`parent()`, scanf width via
query predicates, same-file symbol rules via set algebra, unquoted bash
variables via the parent chain). Since v0.4.0,
trees are incremental: `Tree.edit(start, old_end, new_end)` records a
byte-offset edit, `ts.reparse`/`ts.reparse_c(old, new_text)` re-parse against
the old tree (which stays alive), and `ts.changed_ranges(old, new)` returns
the changed ranges as a JSON array string — the edit → reparse → re-lint loop
is [examples/incremental-reparse.lms](examples/incremental-reparse.lms). Tier-2 extensions E1 (query
predicates) and E2 (parent) are landed; E3 re-scoped to
performance/cross-file after the symbol rules proved scriptable; E4/E5 landed
in v0.3.5/v0.4.0; tier 3 (real semantics) is deferred.
The framework, inventory, and extension plan: [rule tiers](docs/rule-tiers.md).

```sh
lmok lint rules/c-tier1.json examples/data/sample.c
```

## Facts (knowledge tree, tier 3 / E6)

```sh
lmok facts [--lang json|c|python|bash|java|go|javascript|typescript|tsx|lmscript|lmcode] FILE|DIR...
```

Prints one JSON Lines record per definition, reference or edge found in each
file — the per-file half of the tier-3 knowledge tree
(`docs/tier3/deep-analysis.md` §4). Extraction is driven by the embedded
`queries/<lang>/facts.scm` files (tree-sitter query vocabulary:
`@def.<kind>` / `@ref.<kind>` on the fact node, `@name` on the name node,
`(#set! confidence "...")` per pattern) plus the little host logic queries
cannot express: the dotted **scope** chain of enclosing definitions
(`server.tls` for a JSON key, `C.m` for a Python parameter), Python
`def.function` → `def.method` inside a class, and include/source target
normalisation. Record keys, in order: `file, lang, kind, name, scope,
confidence, row, col, end_row, end_col, start_byte, end_byte, name_row,
name_col` (positions 0-based; the extent is the whole fact node, `name_*` is
where the name starts). `confidence` is the honesty contract: `exact`
(definitions, literal edges), `candidate` (name-matched references: calls,
reads), `unresolved` (a `source "$DIR/x.sh"` target), `external`
(`#include <stdio.h>`). Selected kinds (the complete per-language definitions live in
[`queries/`](queries/)):

- C: `def.function def.prototype def.macro def.struct def.union def.enum
  def.enumerator def.typedef def.variable def.parameter def.field def.label
  ref.call ref.include`
- Python: `def.class def.function def.method def.variable def.parameter
  def.import ref.call ref.read ref.import ref.import_from ref.import_wildcard`
- Bash: `def.function def.variable ref.call ref.read ref.source`
- Go: `def.function def.method def.type def.variable def.import ref.call ref.import`
- JSON: `def.key` (every object key; scope = enclosing key chain, arrays
  transparent)

Targets expand like `lint` (directories recursive and sorted, hidden entries
below the argument skipped, fail-fast exit 3). `--lang` forces the grammar of
explicitly named files and filters a directory to that language; without it
the extension decides and an unknown extension is a usage error (exit 2).
Nothing here infers types, expands macros or evaluates `#if`: a reference is
a candidate set, never a resolution.

## Scripting

Save this as `build/show.lms` after the setup above, run
`PATH=build:$PATH lmscript build/show.lms`.
The block constructs an opaque program; run_lmcode executes it.

```lmscript
p = _lmcode_ cli="lmok" install="github:lmctl-ai/lmok@latest";
let tree = ts.parse("{\"name\": \"lmok\", \"tags\": [\"tree-sitter\", \"json\"]}");
println(tree.sexp());
println("has_error -> " + str(tree.has_error()));
println("nodes -> " + str(tree.node_count()));
end _lmcode_
r = run_lmcode(p);
assert(r.ok, "host failed");
```

The lmcode surface is deliberately small: `ts.parse(text)` returns a `Tree` with
`.sexp()`, `.has_error()`, `.node_count()`, and `.root()`. The `root()` handle
is a `Node` with `.kind()`, `.text()`, `.is_named()`, `.is_missing()` (zero-width placeholder tree-sitter inserted for a missing token),
`.start_row()/.start_col()/.end_row()/.end_col()` (all **0-based**,
tree-sitter convention), `.child_count()`, and `.child(i)` (named children
only, bounds-checked), plus upward navigation: `.has_parent()` (false at the
root) and `.parent()` (a located run-time error at the root). The schema also declares the SAM interface
`Rule(n: Node) -> void`: top-level lmcode functions can carry a `@NODE-KIND`
annotation and an `implements Rule` clause (checked at compile time), so
analysis rules are user-redefinable scripts over the CST — see
[examples/cst-rules.lms](examples/cst-rules.lms) and [durable-memory/03-rule-methods.md](durable-memory/03-rule-methods.md). The v6 `check` command
dispatches these entries by annotation; ordinary `--lmcode` execution remains body-driven. Rule methods
require `lmscript` >= 0.2.444. Since v0.2.1, `Tree.nodes()` returns a
`NodeList` (flat pre-order list of named nodes, root first; `.len()` and
bounds-checked `.at(i)`), so `for (n in tree.nodes())` runs a whole-tree scan
in one foreach — see [examples/lint-json.lms](examples/lint-json.lms). Since v0.2.2, the host keeps a
per-run findings sink: `ts.report(node, message)` appends a finding
{row, col, end_row, end_col, message} (0-based positions, capped at 65536 per
run), read back with `ts.report_count()` and `ts.report_json()` (a JSON array
string with stable key order) — the script decides the rendering, see
[examples/report-json.lms](examples/report-json.lms). Since v0.2.5, `ts.report(node, message, rule,
severity)` also attaches rule metadata: `rule`/`severity` are free-form
strings (policy belongs to the script; the host validates nothing), emitted
by `report_json()` only when present (key order row, col, end_row, end_col,
rule, severity, message) — see [examples/rule-catalog.lms](examples/rule-catalog.lms) for per-severity
summaries and a quality gate computed in lmcode, [examples/suppress-findings.lms](examples/suppress-findings.lms)
for host-side `lmok-disable` suppression, and
[examples/sarif-report.lms](examples/sarif-report.lms) for SARIF 2.1.0 emission (GitHub code scanning /
VS Code interop) in pure lmcode. Since v0.2.3, `ts.query(root, pattern)` runs a
tree-sitter S-expression query over the subtree at `root` and returns every
captured node as a `NodeList` (match/byte order; duplicates kept; a pattern
with zero captures yields an empty list; an invalid pattern is a runtime
error naming the byte offset and reason). Since v0.3.3 query predicates are
evaluated per match with upstream tree-sitter's semantics: `#eq?`/`#not-eq?`/
`#any-eq?`/`#any-not-eq?` (capture text vs literal or another capture),
`#match?`/`#not-match?`/`#any-match?`/`#any-not-match?`
(ECMAScript regex over capture text, UNANCHORED like upstream — anchor with
`^...$` yourself; texts over 16 KiB are not searched), and
`#any-of?`/`#not-any-of?`; the non-`any` forms must hold for every node of a
quantified capture. `#is?`/`#is-not?` and directives (`#set!`) are accepted
and ignored. A malformed predicate or an unknown `#…?` predicate is a
runtime error (exit 3) rather than a silent over-match. Published query files
using these predicates work as written. Since v0.2.4, a
second grammar is vendored: tree-sitter-c (pinned in `deps.lock`) with
`ts.parse_c(text)` / `ts.query_c(root, pattern)` mirroring the JSON pair — see
[examples/lint-c.lms](examples/lint-c.lms) for real security rules over C. Two more grammars
follow the same pattern: tree-sitter-python (`ts.parse_python` /
`ts.query_python`) and tree-sitter-bash (`ts.parse_bash` / `ts.query_bash`),
both pinned in `deps.lock` — these are the first grammars with an external
scanner (`src/scanner.c`), wired into the CMake source list. Go (tree-sitter-go v0.25.0) is also available: use
`--lang go` for parse/query/check and `.go` files for lint/facts extension
inference. The lmscript `ts.parse_*` lmcode surface remains unchanged. Example:
`build/lmok lint rules/go-tier1.json tests/fixtures/go-tier1-violations.go`.
The `ts.match_*` code-pattern APIs work for C/Python; see
[examples/pattern-rules.lms](examples/pattern-rules.lms). See `examples/` for runnable scripts.

JavaScript (tree-sitter-javascript v0.25.0) is available as `--lang javascript`
and inferred for `.js` files. Its CLI facts include declarations, imports and
calls; the lmscript `ts.parse_*` lmcode surface remains unchanged. The heuristic
`rules/javascript-tier1.json` pack flags dynamic code, child_process imports,
and innerHTML assignments. TypeScript (tree-sitter-typescript v0.23.2) is a
separate `--lang typescript` grammar for `.ts` files. The distinct TSX grammar
is selected by `--lang tsx` and `.tsx`; both keep independent fact queries and
tier-1 packs.

## Repository layout

- `cpp/` — the CLI host: lmcode VM integration plus the tree-sitter wrapper
- `queries/<lang>/facts.scm` — the embedded facts-extraction queries behind `lmok facts`
- `docs/` — design documents (e.g. `rule-snippets.md`: analysis rules as lmscript snippets)
- `schema/lmok.host.json` — the host-only lmcode schema (merged with the pinned canonical standard into `schema/lmok.dsl.json` and embedded into the binary)
- `examples/` — lmscript examples (plus `examples/rules/` demo packs)
- `rules/` — the canonical rule library: tier-1 packs plus `rules/lmcode/` logic rules (see [rules/README.md](rules/README.md), [docs/rule-tiers.md](docs/rule-tiers.md))
- `tests/fixtures/` — lint fixtures (violations fire every rule; clean guards false positives)
- `scripts/` — vendoring (tree-sitter sources pinned in `deps.lock`, `lmcode-cpp` SDK pinned in `sdk.lock`) and compiler selection
- `third_party/` — downloaded upstream sources (git-ignored, pinned by `deps.lock`)

## License and company

lmok-authored code is MIT licensed (see `LICENSE`). It builds against
tree-sitter and the JSON, C, Python, Bash, Java, Go, JavaScript, TypeScript and
TSX, lmscript and lmcode grammars, all MIT licensed, plus SQLite (public domain). Complete license
texts are in `THIRD_PARTY_NOTICES.md`, also embedded in the binary (see
`lmok --notices`). Made by
**Lattice Systems LLC**.

## Native OS regression checks

Every matrix runner executes tests/check-native-os.py through bash ./test.sh.
The checks use byte output, not Python newline translation: file/stdin parsing,
CRLF/LF/BOM, NUL/0xff/^Z, empty and multi-buffer inputs, UTF-8 paths, drive/separator
handling, diagnostics, executable discovery output and bounded queries. Windows
uses wide CRT arguments converted to UTF-8; filesystem paths cross through u8path.
Symlink assertions run when the OS grants that capability, with an explicit note
when Windows lacks the privilege.

Executable headers must match the matrix's x64/arm64 target. Linux ldd must show
no dynamic dependencies, macOS otool may list only system libraries, and Windows
PE imports may list only system DLLs. Each test group rejects a deliberately
corrupted output or architecture header; Linux also rejects a real dynamically
linked control executable. These tests and the disposable Git repositories used
by tests/release-hygiene.py need no network, models or installed lmok command.

## Rule metadata and fixture tests

`lmok rules PACK...` prints JSON Lines sorted by language/id: id, language,
severity and metadata. Duplicate language/id pairs are refused. Packs may add
`cwe`, `owasp` (string or string list), `tags` (string list), `url` (string),
and `confidence` (`low`, `medium`, `high`) to each rule. Lists are sorted and
deduplicated. Metadata is classification guidance, not proof of exploitability.

`lmok lint PACK FILE --metadata`, `lmok ci --metadata`, and `lmok check --lang c RULES.dslb FILE --metadata`
include a `metadata` object on findings that declare it. Without the flag,
existing JSON bytes remain unchanged. Entry rules use
`ts.report_meta(node, message, id, severity, metadata_json_string)`; the JSON
uses the same keys/validation as packs. `--lmcode ... --metadata` also opts in.
The structured fields are available to SARIF writers in `RuleMetadata`.

`lmok test PACK FIXTURE...` compares finding (line, rule) sets against trailing
CST comments containing `lmok-expect: RULE[,RULE]`. Any supported comment syntax
works; markers in strings do not. An annotation describes its own one-based
line, never the following line. Unannotated reported lines fail. Suppressions,
ignores and baselines do not hide mismatches. JSON stdout has `ok`, `expected`,
`actual`, and a `diff` array; stderr gives `file:line: missing|unexpected RULE`.
Exit 0 passes, 1 means mismatch, 2 means malformed input/annotations.
Rule ID tokens preserve UTF-8 and namespaces; spaces and commas delimit IDs. Repeated
findings for the same rule on one line count once.
## Java

Java source is supported by --lang java and .java directory targets, using the pinned tree-sitter-java v0.23.5 grammar.

    lmok parse --lang java examples/java-sample.java
    lmok lint rules/java-tier1.json examples/java-sample.java
    lmok facts examples/java-sample.java

The nine Java tier-1 rules cover empty catches, println outside methods named main, string-literal reference comparisons, stack traces, credential-looking literal fields, variable Runtime.exec, weak digests, generic catches and TODO/FIXME comments. These are syntax heuristics, not type resolution. Java facts describe classes, interfaces, methods, fields, imports and candidate calls; ts.parse_java/query_java/reparse_java are available in lmcode. See [Java behavior and limits](durable-memory/java.md).


## Secrets packs

`lmok lint rules/secrets-python.json src/` scans string literal content. Packs
for `json`, `bash`, `python` and `c` detect AWS access IDs, GitHub classic/fine
grained tokens, Slack tokens, private-key headers and JWT-shaped values. These
are shape checks, not credential validation. Generic high-entropy candidates
also require a credential assignment/key (`password`, `token`, `secret`,
`api_key`, including underscore suffixes), 24–128 allowed bytes and entropy
above 4.3 bits/byte; random ordinary strings alone are not credentials.

Pack field `"redact": true` replaces `{text}` with the first four content bytes
plus `[REDACTED]` before findings reach stdout, SQLite or downstream formats.
Redacted rules refuse replacement text (`fix`) before output.
Baseline snippets for redacted rules store a SHA-256 digest rather than source
content, so changing a same-prefix credential still becomes new. Do not put
literal credentials in rule messages. Query predicate
`(#entropy-gt? @capture "4.3")` computes byte Shannon entropy with a strict
threshold (finite 0–8); empty or over-16-KiB captures do not match. Quantified
captures must all exceed the threshold. No network checks are made. See
[secrets details](durable-memory/12-secrets-packs.md).

## Token-safe autofix

Pack rules may supply an optional `"fix": "replacement text"`. Findings then
include `"fix":{"start_byte":0,"end_byte":4,"text":"replacement text"}` with
zero-based UTF-8 byte offsets (end exclusive). Text is literal; pattern
metavariable substitution is not performed. lmcode entries can report the same
metadata through `ts.report_fix(node, message, rule, severity, text)`; see
[autofix-report.lms](examples/autofix-report.lms).

```sh
build/lmok lint rules/bash-autofix.json tests/fixtures/autofix-bash-before.sh --diff
# Work on your own copy when applying the pinned fixture:
cp tests/fixtures/autofix-c-before.c build/autofix-example.c
build/lmok lint rules/c-autofix.json build/autofix-example.c --fix
```

`--fix` applies non-overlapping whole-node replacements bottom-up. Before writing,
the host reparses the proposed bytes, refuses ERROR/MISSING nodes or changes to
the CST outside the replaced nodes, and re-lints once using the verified tree.
Remaining findings are printed, including findings exposed by the edit; `K fixed`
is reported per file. Suppressed findings never enter the edit set. CRLF source
keeps its line endings, including newlines inside replacement text. Repeating a
fix run makes no further changes to an already-fixed file.

`--diff` previews the same verified batch without writing source or recording a
DB run; it wins when combined with `--fix`. Stdout shows full-file hunk ranges
with **all source and replacement lines replaced by `[source omitted]`**; the
fixed count goes to stderr. This text-free preview policy also protects
credentials outside edited ranges, suppressed credentials and files scanned
with an autofix-only pack. The preview is informational, not an applyable patch.
`--diff` refuses `--baseline`, `--new-since`, `--fail-on-new` and `--sarif`
(exit 2), rather than silently bypassing those consumers. Files with no edits produce no diff. Overlap or
unsafe parse/shape changes refuse the entire file's batch (exit 3), leaving its
bytes untouched. Whole-file/root replacements, empty/deleting replacements that
cannot form a syntax node, and mixed-grammar edit batches are conservatively
refused, as are byte ranges shared by multiple wrapper nodes. The guard verifies
syntax boundaries, not program behavior; only the
four narrow shipped rules claim their documented semantics.

The initial packs cover exact Bash `` `date` `` / `` `pwd` `` substitutions and C
`01` / `0XFF` literal spelling. Before/after fixture bytes and native OS controls
are in `tests/check-autofix.py`, run by `./test.sh` on every platform. Fix metadata
is available in current lint/check output; historical DB findings keep their
existing position/message schema.

## Local metrics and duplication

`lmok metrics src/` prints JSON Lines with `file`, physical `lines`, `functions`
and `max_nesting`. `lmok dup --tokens 20 src/` reports repeated within-file token
windows: identifiers/literals normalize, keywords/operators remain distinct,
comments are skipped. Both copy ranges are zero-based, end exclusive; adjacent
windows are not merged. `--tokens` accepts a positive integer (default 20).
Both verbs accept `--lang L` and use the existing file/directory expansion.

The C/Python `rules/lmcode/*-function-cognitive-complexity.lms` packs report
scores >15. Controls add 1 plus nesting, else/elif add 1, and boolean operator
runs add 1; nested function bodies are scored separately. These are documented
syntax metrics, with ten hand-computed function values pinned for each language.
See [scoring and limits](durable-memory/11-metrics.md) and run
`bash examples/metrics.sh` after building and setting the compiler PATH.

Native `--sarif FILE` includes declared metadata in rule/result `properties` and
uses `url` as rule `helpUri`, including for lmcode entry rules. It does not require
`--metadata`; that switch controls JSON findings only.

### lmscript and lmcode

The standalone binary includes the experimental lmscript and lmcode Tree-sitter
parsers (analysis syntax, not runtime/type validation):

```sh
lmok parse --lang lmscript tests/fixtures/script-languages/workflow.lms
lmok query --lang lmcode '(function_declaration name: (_) @name)' tests/fixtures/script-languages/rule.lmcode
lmok facts tests/fixtures/script-languages/workflow.lms tests/fixtures/script-languages/rule.lmcode
```

`.lms` selects outer lmscript; `.lmcode` selects standalone inner lmcode.
JSON lint packs use `"lang": "lmscript"` or `"lang": "lmcode"`; compiled rule
packs use `lmok check --lang lmscript|lmcode`. Embedded lmcode bodies stay opaque
in this slice; host-side injection with rebased locations is the next slice.
See [the grammar contract and S5 seam](durable-memory/15-script-languages.md).

## Historical literal names

The documentation calls the language lmcode. `--lmcode`, `--lmcode-schema` and `--lmcode-signatures` are current host flag literals; `DSLB` is the bytecode magic and `DSLB v6` is the current format name; `.dslb` is the current bytecode filename extension; `_lmcode_` is the historical block spelling. Other code-formatted API names, paths, manifest names and diagnostic text retain their current literal spellings.

## Outline

`lmok outline FILE|DIR... [--db PATH] [--budget N]` prints live definitions,
first-line signatures and scopes grouped by file. An available DB ranks
files by reference-name occurrence counts; otherwise paths determine order.
The line budget keeps whole file blocks. See [the output contract](docs/tier3/outline.md).
