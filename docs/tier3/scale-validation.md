# Tier-3 scale validation (2026-10-31)

This is a measured capacity report, not a throughput guarantee. The baseline is
`b3a6537f0c72976ee0f28d3d77a5004bd74eba09`; the accompanying fix preserves
subsecond mtimes so quick same-size edits cannot silently retain stale facts.

**Targets missed on integrated main: curl cold indexing takes 40.75 seconds
(>10 seconds for roughly 1,000 files); the mixed DB is 173.37 MiB for just
2,327 files (>100 MB before 10,000 files); and unchanged mixed reindexing takes
22.23 seconds, not near-instant.** The last result is a regression introduced
by the mainline extraction-state lookup during this validation wave. The
subsecond invalidation bug is fixed here; slow-but-correct behavior is reported
without a performance patch, as requested.

## Methodology and corpora

Machine: four ARM Neoverse-N1 CPUs (`nproc`: 4), 15,759 MiB RAM, no swap,
Ubuntu/GCC 13.3, Release CMake/Ninja build, fully static `lmok`. `uname -a`:

```text
Linux ip-172-31-80-6 7.0.0-1014-aws #14~24.04.1-Ubuntu SMP PREEMPT Fri Sep 18 21:50:22 UTC 2026 aarch64 aarch64 aarch64 GNU/Linux
```

This is a shared development machine, not an isolated benchmark host. Other
team builds were active, so wall times include contention. No CPU affinity,
cache dropping, or privileged tuning was used. A cold run means a new SQLite
DB; source files can already be in the OS page cache. Warm runs reuse the DB
and unchanged sources. Measurements include CLI startup and the time/timeout
wrapper; query medians are five warm process invocations, not in-process SQL
microbenchmarks. Output goes to ordinary files, not a terminal.

The binary was built from a deleted `build/` and `third_party/`, using
`scripts/sync-deps.sh`, CMake Release, and `cmake --build build -j2`. The compiler
library was bootstrapped once with
`export LMSCRIPT_BIN="$(scripts/bootstrap-compiler.sh)"`; its pinned revision is
`8e334dda3bb821f3bc34be42d598259cb231411c` (`compiler.lock`). The final corrected
binary and its first full gate use a separate initially empty `build-fixed/`
directory. After integrating the latest main, the pre-push gate is repeated in
an initially empty `build-final/` directory.

Corpora are copies; the original checkouts and pinned dependencies were not
edited. Hidden paths and symlinks were excluded. Absolute paths in DB rows
include this workspace's scratch directory, so path storage contributes to size.

- **C tree:** curl `31c911efe5652725e930f4d2e60c9060e5f6646d`, shallow clone,
  all 1,061 `.c`/`.h` files, 9,450,904 source bytes. This is real source,
  not 1,000 repeated synthetic files.
- **Mixed pinned tree:** all recognized source extensions from every
  `third_party/` source in `deps.lock`, plus tracked source files from the
  compiler revision above (excluding generated build output and npm modules).
  Includes tree-sitter core, every grammar's generated parser, SQLite's
  amalgamation, and the compiler's TypeScript/JavaScript/scripts/JSON. The
  copied pool has 2,533 files / 77,477,317 bytes; `.mjs` and `.cjs` are not
  recognized by this version of the indexer and are excluded from indexed counts.
- **Earlier real-world projects:** sds `5347739b1581fcba74fd5cab1fc21d2aef317d71`,
  kilo `323d93b29bd89a2cb446de90c4ed4fea1764176e`, and cJSON
  `6d9f2443ab071f86e5d9b43025a40929ec41c46c`, recovered from
  `/tmp/lmbetter-rw/`; copied C/header files only, 104 files total.

Each measured command runs through `/usr/bin/time -v` and GNU `timeout`, with a
6 GiB process address-space ceiling (`RLIMIT_AS`) and core dumps disabled. The
initial timeout is 180 seconds; longer complete-run allowances are identified
below. Exit 124 means **the harness timed out**, not that `lmok` enforced an
internal budget. Peak RSS is the `Maximum resident set size` field (KiB);
wall seconds are Python `perf_counter` around the subprocess. DB sizes are the
main database file after command termination; successful close checkpoints WAL.
Interrupted runs are partial and must not be mistaken for successful indexing.
Row counts use Python `sqlite3`, and every measured DB passed `PRAGMA quick_check`.

For the 1% edit test, choose the first `round(file_count * .01)` indexed paths in
lexical order and append one newline. This changes size, guarantees a stat miss,
and preserves facts. Touch-only and same-size subsecond changes are separately
covered by the new regression. No edits are made to the original sources.

## Integrated main: final measurements

Code: `41ea6c1f79f162a4bd201ca7e307bf56d8364887`, based on `ff35b1c`, including
review `5440a29` and this change's timestamp fix. Clean Release binary:
`build-final/lmok` (`lmok 0.6.0`). Both cold DBs below were new; cold, warm, and
edit trials allowed 600 seconds and all completed with exit 0 and empty stderr.
These results supersede the earlier snapshot for claims about current main.

| Corpus | Files | Source MiB | Cold s | Warm s | 1% edit s | Cold RSS MiB | DB MiB | Symbols | Refs | Edges |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| curl C/header tree | 1,061 | 9.01 | 40.752 | 2.318 | 2.808 | 13.75 | 21.60 | 55,739 | 49,865 | 4,218 |
| all pinned mixed sources | 2,327 | 72.93 | 152.368 | 22.230 | 24.826 | 366.01 | 173.37 | 263,747 | 929,098 | 7,681 |

These copied inputs include the preceding whitespace-edit rounds: curl is
9,450,926 bytes and mixed is 76,472,492 bytes before these trials. The summary
lines are still exactly 1,061/2,327 cold files, zero warm re-extractions, and
11/23 edited files respectively (1,050/2,304 unchanged). Row counts do not change
on the whitespace edits. All DBs pass `quick_check`.

The mixed symbol count is 68 lower than the earlier snapshot: precisely one
Java, six JavaScript, and 61 TypeScript import symbols disappeared after the
mainline binding fixes. Counts for all other language/kind groups are unchanged.
This is an upstream correctness change, not an extraction timeout.

**Warm regression:** curl went from 0.096 to 2.318 seconds and mixed from 0.327
to 22.230 seconds while reporting zero re-extracted files. The mainline review
correctly prevents `lint --db` placeholder rows from being treated as fully
indexed, but its known-files query now contains:

```sql
SELECT f.id, f.path, f.sha256, f.mtime, f.size,
       EXISTS(SELECT 1 FROM file_text t WHERE t.path = f.path)
FROM files f;
```

SQLite reports `SCAN f`, `CORRELATED SCALAR SUBQUERY 1`, and
`SCAN t VIRTUAL TABLE INDEX 0:`. This adds a repeated unindexed FTS path scan
before stat skipping. The timings and query plan identify a concrete follow-up
for performance work; this report does not weaken the correctness check.

The current mixed DB was then populated with the same 6,954 benchmark findings
as the earlier snapshot; its size became **181.63 MiB**. Five-run query timings
include startup and output rendering, with identical result counts across runs:

| Command (plus `--db mixed-integrated.db`) | Output rows | Median ms | Min–max ms |
| --- | ---: | ---: | ---: |
| `symbols 'ts_%'` | 2,080 | 40.08 | 39.68–41.30 |
| `symbols` | 263,747 | 629.97 | 611.70–758.67 |
| `refs malloc` | 34 | 10.54 | 9.38–19.29 |
| `findings` | 6,954 | 37.35 | 36.67–39.67 |
| `findings --rule SCALE001` | 6,954 | 38.38 | 37.31–40.47 |
| `findings --file …/tree-sitter/lib/src/parser.c` | 51 | 11.23 | 10.93–11.63 |

## Earlier snapshot before the mainline DB review

The corrected binary uses baseline `b3a6537` plus the subsecond timestamp fix
described below (built before rebasing onto the subsequent `outline` change).
The C and mixed results below use that binary and fresh databases with a
600-second allowance. No extraction queries or SQLite settings were changed. The first mixed
baseline attempt hit 180 seconds after 2,026 files / 227,555 symbols /
553,290 refs / 7,157 edges, exit 124, with an intact partial DB; it is not included
as a completed cold result. The small recovered-project row uses the baseline.

| Corpus | Indexed files | Source MiB | Cold seconds | Warm seconds | 1% edit seconds | Cold peak RSS MiB | DB MiB | Symbols | Refs | Edges |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| curl C/header tree | 1,061 | 9.01 | 44.052 | 0.096 | 0.626 | 13.68 | 21.60 | 55,739 | 49,865 | 4,218 |
| all pinned mixed sources | 2,327 | 72.93 | 170.017 | 0.327 | 5.174 | 365.95 | 173.35 | 263,815 | 929,098 | 7,681 |
| sds + kilo + cJSON | 104 | 0.79 | 7.311 | 0.028 | 0.126 | 12.55 | 2.13 | 5,489 | 5,227 | 409 |

All completed runs exit 0 with empty stderr. Curl's corrected cold corpus includes
the eleven newlines added in the baseline edit test (9,450,915 bytes). The mixed
corpus has 2,327 recognized files / 76,472,469 bytes, spanning C, JSON, Java,
Python, Bash, Go, JavaScript, TypeScript, lmscript, and lmcode. There are no TSX
source files in this particular pinned corpus. DB row counts are unchanged by
the whitespace edits. Findings are initially zero; they are populated separately
for the latency test below.

Actual complete-run summaries:

```text
indexed 1061 files (0 unchanged, 0 removed)
indexed 0 files (1061 unchanged, 0 removed)
indexed 11 files (1050 unchanged, 0 removed)

indexed 2327 files (0 unchanged, 0 removed)
indexed 0 files (2327 unchanged, 0 removed)
indexed 23 files (2304 unchanged, 0 removed)
```

These earlier warm runs are subsecond but not constant-time: they enumerate/stat the files,
load known rows, and resolve edges. The 1% test verifies the extraction count;
it does not mean all other DB maintenance is skipped.

## Correctness fix: subsecond invalidation

Reproduction on the baseline:

1. Write `int before;\n`, set mtime to `1800000000.100000000`, index.
2. Replace it with equally sized `int after_;\n`, set mtime to
   `1800000000.200000000`, index again.
3. The baseline prints `indexed 0 files (1 unchanged, 0 removed)`; `symbols`
   still returns `before`. The filesystem timestamps differ, but the indexer
   discarded their fractional seconds.

The fix stores nanosecond file-clock timestamps in both index and lint DB
records. Existing whole-second records miss the stat fast path once, get
content-hashed, and refresh without unnecessary fact extraction. The regression
`tests/check-index-invalidation.py` checks facts and stored text after the edit,
then unchanged, touch-only, legacy-timestamp, and forced reindex behavior.

This does **not** make stat skipping hash-verified: deliberately restoring the
same full timestamp and size, or using a coarse-timestamp filesystem, can still
hide a content change. Use `index --force` when that freshness guarantee matters.

## Resource-budget behavior

A pathological identifier regex `^(a+)+$` over 30 `a`s followed by `X` exits **3**
with `regex work limit exceeded (65536)`. A query containing 257 nested arrays
exits **3** with `query error at byte offset 1792: nesting limit exceeded (256)`.
Neither crashes or exhausts memory.

A regex applied to the roughly 10 MiB comment exits **0**, emits a stderr note
that the 16,384-byte text cap was exceeded, and treats it as no match. That is the
existing documented cap contract, not a successful complete regex search.
These guards constrain query-pattern depth and regex work; they are **not** a
general index wall-time, tree-depth, fact-count, or SQLite-size budget.

## Initial stress results

These rows use the baseline binary. MiB = 1,048,576 bytes. Zero exit means the
operation completed, not that a deliberately malformed source has no parse errors.

| Case | Source bytes | Wall seconds | Peak RSS MiB | DB MiB | files / symbols / refs / edges | Exit |
| --- | ---: | ---: | ---: | ---: | --- | ---: |
| 10 MiB comment-heavy C | 10,485,762 | 0.397 | 75.95 | 10.14 | 1 / 1 / 0 / 0 | 0 |
| 100,000 C variables | 1,900,000 | 1.977 | 126.99 | 11.26 | 1 / 100,000 / 0 / 0 | 0 |
| 1,000 nested Python functions | 517,399 | 4.375 | 9.93 | 0.96 | 1 / 232 / 0 / 0 | 0 |
| 50,000 tiny C files: initial attempt | 850,000 | 180.048 | 18.14 | 2.25 | 4,266 / 4,266 / 0 / 0 | 124 |
| 50,000 tiny C files: resume | 850,000 | 180.019 | 19.05 | 4.25 | 8,172 / 8,172 / 0 / 0 | 124 |

The variable input is `int symbol_000000;\n` through `int symbol_099999;\n`;
all 100,000 definitions were stored. The comment-heavy C input has one function
and a large block comment; it tests byte size, not CST-node density. A dense
10 MiB counterpart is reported separately below.

The Python input contains `i` spaces followed by `def level_i():` for
`i=0..999`, ending with an indented `return 1`. The parser emits one `ERROR`
region and 232 function-definition nodes; indexing stores those 232 symbols.
Exit 0 follows the existing error-tolerant parser contract, **not** complete
recognition of all 1,000 intended definitions.

The directory contains 50 subdirectories with 1,000 distinct 17-byte C files
each (`int value_00000;\n`, etc.). Completing 50,000 files was not feasible
within the two 180-second trials: the first stored 4,266 files and a resume
advanced to 8,172. Both were explicitly terminated by the harness (124), not
by an internal resource limit. Progress and intact partial DBs distinguish this
from a demonstrated hang. There is no successful 50,000-file cold or warm claim.

A possible performance contributor, not a profile-based attribution: per-file
replacement executes `DELETE FROM file_text WHERE path=?`, including for new
files. `EXPLAIN QUERY PLAN` reports `SCAN file_text VIRTUAL TABLE INDEX 0:`;
`path` is an unindexed FTS5 column. The growing repeated scans and per-file
transactions deserve a separate optimization effort. Warm indexing also always
reruns edge resolution, even when it extracts zero files.

## Dense 10 MiB result

The corrected binary indexed an exactly 10,485,760-byte C file containing
291,270 definitions of the form `int f000000(void){return target();}` (unique
six-digit names), padded to size with a trailing block comment. It stored all
291,270 symbols and 291,270 call refs, with zero edges; exit 0, empty stderr,
`PRAGMA quick_check = ok`.

Wall time **11.783 seconds**, peak RSS **974.76 MiB**, DB **51.28 MiB**.

This is materially more expensive than the comment-heavy input despite the same
byte size. No tested case crashed or reached the harness's 6 GiB address-space
ceiling. This does not establish safety for arbitrarily large CSTs.

## Earlier-snapshot query latency on the complete mixed DB

The same corrected binary ran `lint --db` with a deliberately simple benchmark
policy: C `on: function_definition`, rule `SCALE001`, severity `info`, message
`function definition (benchmark policy)`. These are benchmark findings, not
6,954 diagnosed bugs. It recorded 6,954 findings in one run; files/symbols/refs/
edges were unchanged. The DB then occupied **181.62 MiB**.

Each row below is five separate warm CLI invocations with stdout redirected to
files. Timing includes process startup and rendering the indicated rows.
All exits were 0, stderr empty, and result counts identical across repetitions.

| Command (plus `--db mixed-complete.db`) | Output rows | Median ms | Min–max ms |
| --- | ---: | ---: | ---: |
| `symbols 'ts_%'` | 2,080 | 39.49 | 38.50–40.27 |
| `symbols` | 263,815 | 602.66 | 598.10–618.43 |
| `refs malloc` | 34 | 9.15 | 8.82–9.41 |
| `findings` | 6,954 | 36.44 | 36.19–36.75 |
| `findings --rule SCALE001` | 6,954 | 38.24 | 37.71–45.59 |
| `findings --file …/tree-sitter/lib/src/parser.c` | 51 | 11.83 | 11.30–12.59 |

`symbols` uses SQL LIKE matching, not a regex; `_` in `ts_%` is a LIKE
single-character wildcard. `refs malloc` is exact-name lookup. The rule filter
selects every benchmark finding, whereas the file filter selects 51. Returning
all 263,815 symbols takes substantially longer than a selective lookup; these
numbers include that output cost rather than hiding it.

## Reproduction outline

```sh
export LMSCRIPT_BIN="$(scripts/bootstrap-compiler.sh)"
scripts/sync-deps.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
# CORPUS is a copied corpus assembled from the revisions above; DB is absent.
/usr/bin/time -v -o cold.time timeout --kill-after=5 600 \
  build/lmok index "$CORPUS" --db "$DB" > cold.out 2> cold.err
/usr/bin/time -v -o warm.time timeout --kill-after=5 180 \
  build/lmok index "$CORPUS" --db "$DB" > warm.out 2> warm.err
# Append a newline to the first round(N*.01) paths from files ORDER BY path.
/usr/bin/time -v -o edit.time timeout --kill-after=5 180 \
  build/lmok index "$CORPUS" --db "$DB" > edit.out 2> edit.err
```

The measurement harness additionally sets `resource.setrlimit(resource.RLIMIT_AS,
(6*1024**3, 6*1024**3))` and `RLIMIT_CORE=(0,0)` before launching the command.
Count `files`, `symbols`, `refs`, `edges`, and `findings` with `SELECT count(*)`;
use `PRAGMA quick_check` and measure DB size after the process has closed it.
For the mixed copy, enumerate all regular, non-symlink files in `third_party/`
and `git ls-files` in the pinned compiler checkout. Copy extensions `.c`, `.h`,
`.json`, `.py`, `.sh`, `.bash`, `.java`, `.go`, `.js`, `.ts`, `.tsx`, `.lms`, and
`.lmcode`, preserving relative paths; exclude paths with dot-prefixed components.
The original preparation also copied `.mjs`/`.cjs`, which the indexer skipped.
Do not accidentally copy compiler `node_modules` or generated `dist` trees.

Raw commands, `/usr/bin/time -v` logs, JSONL measurements, copied corpora, and DBs
remain locally under `.scale-validation/` (excluded from Git); no generated
corpora or DB binaries are committed. The source revisions, input recipes,
complete counts, and observed failures above are the durable report.

## Validation and disposition

The final code was rebuilt from an empty `build-final/` with the bootstrapped
compiler library and passed the complete `./test.sh`: **131 checks**. This
includes the new subsecond regression, the mainline lint-before-index regression,
and existing bounded-regex/query and native-platform checks. The full-static
check passed (12,952,152-byte host). No workflows or CMake version were changed.

Fixed: lost same-size edits within a filesystem timestamp's whole second.
No crashes, OOMs, or incorrect resource-limit exit codes were observed in the
reported corpus. Open performance findings: slow cold indexing, oversized mixed
DB, regressed warm indexing, and incomplete 50,000-file trials. Deep Python
remains explicitly error-tolerant and only partially extracted. These limits
are not hidden behind a successful small-fixture gate.
