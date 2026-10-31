# lmok

tree-sitter's incremental parsing engine as a single statically-linked,
scriptable CLI.

lmok wraps [tree-sitter](https://tree-sitter.github.io) 0.27.1 together
with the [tree-sitter-json](https://github.com/tree-sitter/tree-sitter-json),
[tree-sitter-c](https://github.com/tree-sitter/tree-sitter-c),
[tree-sitter-python](https://github.com/tree-sitter/tree-sitter-python),
[tree-sitter-bash](https://github.com/tree-sitter/tree-sitter-bash),
[tree-sitter-java](https://github.com/tree-sitter/tree-sitter-java), and
[tree-sitter-go](https://github.com/tree-sitter/tree-sitter-go),
[tree-sitter-javascript](https://github.com/tree-sitter/tree-sitter-javascript), and
[tree-sitter-typescript](https://github.com/tree-sitter/tree-sitter-typescript) grammars in one dependency-free executable that can be driven from the command
line or scripted with [lmscript](https://github.com/lmctl-ai). Feed it source text
and it prints the concrete syntax tree; from lmscript you get a typed `Tree`
object to inspect.

## Build and run

The standalone CLI needs no Node runtime. Scripting and the test suite use
[lmctl](https://lmctl.com), which supplies the `lmscript` command.

Prerequisites for the commands below (Bash on Linux or macOS):

- Git, curl, tar, and SHA-256 tools (`sha256sum` or macOS `shasum`); internet
  access for the pinned source archives and npm package.
- CMake >= 3.24, Ninja, and C/C++ compilers with C++17 support (GCC or Clang).
  Linux builds also need static C/C++ runtime libraries; on Ubuntu these
  come with `build-essential`. On macOS install Xcode Command Line Tools.
- Node.js >= 24.15.0 and npm for `@lmctl-ai/lmctl@0.2.444`.
- Python 3 for SQLite archive extraction and `./test.sh`, plus the usual Unix shell utilities. The SDK
  headers are already checked in; no sibling `lmcode-cpp` checkout is needed.

On Ubuntu, `sudo apt-get install build-essential cmake ninja-build git curl
python3` installs the build tools (check CMake's version). Install Node
separately if your distribution's package is older than 24.15.0. On macOS,
install CMake and Ninja as well as the command-line compiler tools.

```sh
git clone https://github.com/lmctl-ai/lmok.git
cd lmok
npm install --prefix /tmp/lmok-tools @lmctl-ai/lmctl@0.2.444
export PATH="/tmp/lmok-tools/node_modules/.bin:$PATH"
lmscript --version   # lmscript 0.2.444
scripts/sync-deps.sh   # pinned tree-sitter sources (third_party/, git-ignored)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
export PATH="$PWD/build:$PATH"
./test.sh
```

Run all repository examples from this directory, in the same shell. The
absolute PATH entry lets lmscript find the locally built host's schema and
makes the `lmok` commands below work. If npm's commands are not found,
add its global executable directory to PATH too.

Produces `build/lmok`: one statically linked executable (`ldd` reports
"not a dynamic executable" on Linux; macOS links only system libraries).
The test suite rebuilds the host, checks dependency pins, and runs examples
and rule fixtures, including native OS byte/path/linkage checks on every matrix runner.

```sh
lmscript examples/parse-string.lms
build/lmok --lmcode build/parse-string.dslb
```

The first command saves bytecode; the second prints the JSON text, its tree,
`has_error -> false`, `nodes -> 34`, then the host's success record
`{"ok": true, "result": null}`. See [all examples](examples/README.md).

## Published host and raw URLs

Every push to `main` builds and tests the five platform binaries. After they pass,
CI compares committed HEAD with the latest `v*` tag and creates a release only if
binary inputs changed: `cpp/`, `rules/`, `schema/`, `CMakeLists.txt`, `cmake/`,
`deps.lock`, `sdk.lock`, embedded notices, or the build/stamp shell scripts.
Tests/docs/CI-only changes since that tag log `SKIP release`; the rolling `dist`
branch still updates. Eligible changes publish a GitHub Release (next patch after
the latest `v*` tag, or the `VERSION` in `CMakeLists.txt` when that is higher;
`lmok --version` in the release prints that tag) and updates the rolling `dist`
branch. Pushing a `v*` tag by hand publishes that tag the same way.
`lmscript install github:lmctl-ai/lmok` fetches the newest release.

To use a published binary without building C++, install Node/npm and lmctl
as above, then:

```sh
lmscript install github:lmctl-ai/lmok@latest
```

The install command resolves a GitHub release, checks its host version, and
prints the executable path and checksum. It does not add `lmok` to your PATH.
The repository rename alone does not rename existing release assets: maintainers
must publish a matching release containing `lmok-linux-x64`, `lmok-linux-arm64`,
`lmok-macos-x64`, `lmok-macos-arm64` and `lmok-windows-x64.exe` plus checksums
before this renamed `@latest` path works. No release is created by this change.
The rolling `dist` branch is separate; `@latest` selects a release, not `dist`.
Use an explicit `@TAG` for a reproducible dependency once that release exists.
Local builds on PATH work now without downloading a host.

From the repository root (or an empty directory for this inline-data example):

```sh
mkdir -p build
lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmok/main/examples/parse-string.lms
lmok --lmcode build/parse-string.dslb
```

With a local build, use `build/lmok` for the second command. The local
form `lmctl autorun examples/parse-string.lms` works too. `autorun` runs the
outer script, whose `savedsl` call writes `build/parse-string.dslb`; it does
not execute that bytecode or print the tree. The `install=` declaration lets
lmscript resolve a host when one is not on PATH. `@latest` is not a fixed
version pin, and initial resolution needs network access.

Raw URLs fetch the script, not a repository checkout or its data files.
Examples reading `examples/data/` or `tests/fixtures/` still need the clone
and repository-root working directory. The examples' output directories
must exist before compilation.

## CLI

```
lmok --version
lmok --notices
lmok --lmcode-schema [--json]
lmok --lmcode-signatures
lmok parse [--lang json|c|python|bash|java|go|javascript|typescript|tsx] FILE       # '-' reads stdin
lmok query [--lang json|c|python|bash|java|go|javascript|typescript|tsx] [--pattern] PATTERN FILE  # '-' reads stdin
lmok lint RULES.json FILE|DIR... [--db PATH] [--no-suppress] [--sarif FILE]
        [--new-since RUN|--baseline FILE] [--fail-on-new] [--changed[=REV]]
lmok ci [--config FILE] [--no-suppress] [--sarif FILE] [ROOT]
lmok rules PACK... | test PACK FIXTURE...
lmok findings [--rule ID] [--file PATH] [--db PATH]
lmok facts [--lang json|c|python|bash|java|go|javascript|typescript|tsx] FILE|DIR...
lmok check [--lang json|c|python|bash|java|go|javascript|typescript|tsx] RULES.dslb FILE|DIR... [--no-suppress] [--sarif FILE]
lmok --lmcode FILE [FILE ...]
```

`parse` prints the concrete syntax tree as a single-line s-expression, like
`tree-sitter parse` does. Malformed input still prints its tree — broken
regions appear as `(ERROR ...)` nodes — and still exits 0; tree-sitter always
produces a tree. `query` runs a tree-sitter S-expression pattern over the
document and prints one line per captured node, `kind
start_row:start_col-end_row:end_col` (0-based). Both default to the JSON
grammar; `--lang c|python|bash|java|go|javascript|typescript|tsx` switches grammars (an unknown language exits
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
not processed). Files are explicit; directories
and stdin are not accepted. A fresh host arena and findings sink are used per
input. One invocation-owned return sink survives those per-file resets. Rule and severity metadata come from `ts.report` payloads; entry names
are not silently used as finding ids. Every annotation is checked against the
selected grammar at load: a misspelled kind, a supertype (abstract, e.g. `@expression`) or a hidden
symbol is refused with exit 3 and a `LOAD_UNBOUND` record naming the entry, so a rule can never be
silently dead; a rules file is therefore specific to one `--lang`. lmcode bytecode (DSLB v6) is the only accepted runtime
format, so old bytecode must be recompiled. Install the private compiler with
`npm install --prefix /tmp/lmok-tools @lmctl-ai/lmctl@0.2.444` when needed.

Successful `--lmcode` programs and `check` invocations append a terminal frame after
the existing diagnostics (and after successful SARIF output):

```text
LMCODE_RETURN_BEGIN
null
LMCODE_RETURN_END
```

The payload is the invocation's `$return` value, or `null` when unset. Body and
rule-entry writes share the sink across all nodes and input files; `--lmcode`
continues to run only the body. Each program in a multi-program `--lmcode` command
gets its own result. Runtime or application failures emit no frame. Finding
IDs, order, suppression and entry HALT values retain their existing meanings.
The [body/entry return example](examples/invocation-return.lmcode) requires the
prepared slice 3 compiler until its release. See [invocation ownership and compiler staging](durable-memory/14-invocation-return.md).

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
`index` is the next spike stage; it is not implemented by this DB layer.

For now, explicit `status --init` creates the empty schema and prints status.
It preserves a compatible existing DB and never overwrites an incompatible
one. This is a bootstrap/diagnostic operation, not an indexing operation.

## Recording lint runs (`lint --db`, `findings`)

```sh
lmok lint rules/c-tier1.json src/ --db .lmok/lmok.db
lmok findings --rule LBC001
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
lmok lint rules/c-tier1.json src/ --db .lmok/lmok.db --new-since last --fail-on-new
lmok lint rules/c-tier1.json src/ --baseline old.lintout
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

`lint`, `check` and `ci` accept `--sarif FILE`: the normal stdout is
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

## Rule packs

## Metrics

`lmok metrics FILE...` prints one JSON object per file with physical `lines`,
`functions`, and maximum control-flow `max_nesting`. Use `--lang` when a file
has no recognized extension. `lmok dup [--tokens N] FILE...` hashes normalized
token windows within each file (identifiers and literals are replaced), and
prints duplicate ranges; the default minimum window is 20 tokens.

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
  `go`, `javascript`, `typescript`, `tsx`.
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
  `.js` (javascript), `.ts` (typescript) and `.tsx` (tsx) are lintable, paths sorted
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
ships as packs in `rules/` — `c-tier1.json` (11 rules: unsafe calls, command
exec, assignment-in-condition, K&R definitions, goto, FIXME/TODO, object-like
macros, register, naming, commented-out code, malloc-result casts) and `json-tier1.json` (11 rules:
credential keys, placeholders, nulls, key naming, numbers-as-strings, exact
password key, sensitive credential keys, HTTP URLs, unpinned versions, empty
containers, long strings), `python-tier1.json` (9 rules: os shell-out,
shell=True, eval/exec, assert, FIXME/TODO, wildcard imports, bare except,
print(), mutable default args), `bash-tier1.json` (6 rules: rm -rf,
FIXME/TODO, backticks, unguarded cd, chmod 777, eval) and `go-tier1.json`
(6 gosec-flavored rules: variable `exec.Command`, unsafe/weak-crypto imports,
0777 file modes, blank assignment results, and `InsecureSkipVerify`), and
`javascript-tier1.json` (4 syntax heuristics: eval, new Function, child_process
imports and innerHTML writes), plus
`typescript-tier1.json` and `tsx-tier1.json` provide three comparable checks for
typed and JSX sources using independent parser registrations. lmcode-native
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
lmok facts [--lang json|c|python|bash|java|go|javascript|typescript|tsx] FILE|DIR...
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
(`#include <stdio.h>`). Kinds per language:

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
`lmscript build/show.lms`, then `build/lmok --lmcode build/show.dslb`.
The script saves bytecode; `--lmcode` executes it.

```lmscript
p = _dsl_ cli="lmok" install="github:lmctl-ai/lmok@latest";
let tree = ts.parse("{\"name\": \"lmok\", \"tags\": [\"tree-sitter\", \"json\"]}");
println(tree.sexp());
println("has_error -> " + str(tree.has_error()));
println("nodes -> " + str(tree.node_count()));
end _dsl_
savedsl(p, "build/show.dslb");
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
scanner (`src/scanner.c`), wired into the CMake source list. Go (tree-sitter-go v0.25.0) is the sixth CLI grammar: use
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
TSX grammars, all MIT licensed, plus SQLite (public domain). Complete license
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
lmok parse --lang lmscript workflow.lms
lmok query --lang lmcode '(function_declaration name: (_) @name)' extracted.lmcode
lmok facts workflow.lms extracted.lmcode
```

`.lms` selects outer lmscript; `.lmcode` selects standalone inner lmcode.
JSON lint packs use `"lang": "lmscript"` or `"lang": "lmcode"`; compiled rule
packs use `lmok check --lang lmscript|lmcode`. Embedded lmcode bodies stay opaque
in this slice; host-side injection with rebased locations is the next slice.
See [the grammar contract and S5 seam](durable-memory/15-script-languages.md).

## Historical literal names

The documentation calls the language lmcode. `--lmcode`, `--lmcode-schema` and `--lmcode-signatures` are current host flag literals; `DSLB` is the bytecode magic and `DSLB v6` is the current format name; `.dslb` is the current bytecode filename extension; `_dsl_` is the historical block spelling. Other code-formatted API names, paths, manifest names and diagnostic text retain their current literal spellings.
