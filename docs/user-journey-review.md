# New-user journey review

Reviewed **2026-10-31**, starting from main `b3a6537`, then rebased through `f904436` (outline) onto `5440a29`, on Linux ARM64.
This replaces the October 28 pre-rename review: its `savedsl`, two-step
script execution, v0.4.0 install and 53-check claims no longer describe main.
Changes here are documentation and the compiler invocation in
`examples/metrics.sh`; no host, rule logic, workflow or version changes.

## Findings

High = blocks a documented path; medium = misleading setup/behavior;
low = inventory or navigation.

| Severity | Friction / observed evidence | Resolution |
| --- | --- | --- |
| High | Bootstrap fetches private `lmctlhq/lmctl-mono` (GitHub API confirms private visibility). Main CI run [41230275702](https://github.com/lmctl-ai/lmok/actions/runs/41230275702) fails all five compiler-install jobs with Git credential errors. | **Needs packaging/access work outside this docs-only task.** This machine's authenticated bootstrap succeeds; public users cannot reproduce the full gate without access. README now separates the standalone native build from restricted scripting/test setup. No workflow or credential changes made. |
| High | npm-only lmscript cannot export the repository fixtures; package version is not the compiler contract. | Fixed docs: bootstrap `compiler.lock` once, export `LMSCRIPT_BIN` and its directory plus the built host on PATH. Bootstrap completed and its smoke test passed. |
| High | Published v0.6.11 and dist accept `--dsl*`, while main accepts only `--lmcode*`. `--lmcode-schema --json` exits 2 on both published hosts. Raw-URL `lmctl autorun` without the source-built host likewise exits 2 after selecting the published host. | Documented compatibility boundary. **Publication work remains** to ship main's renamed host contract; main scripts must use the matching source-built host. No code workaround added. |
| High | `lmctl install github:lmctl-ai/lmok` is an unknown command. | Fixed docs: `lmscript install github:lmctl-ai/lmok@latest` resolves v0.6.11, validates SHA-256 and prints its cached executable path. Installation does not put `lmok` on PATH. |
| High | Positional `lmscript SOURCE OUTPUT.dslb` and old `savedsl` instructions no longer export bytecode. Direct entry-only scripts do not dispatch rules over source inputs. | Fixed docs: ordinary scripts use `run_lmcode`; entry fixtures use `node scripts/compile-fixture.mjs` followed by `check`. Explicitly label exporter as a maintainer helper. Updated `examples/metrics.sh`, which previously tried to check a nonexistent exported file. |
| Medium | Build prerequisites, compiler/host PATH, output directories and repo-root cwd were scattered or missing. | Consolidated source setup, compiler cache explanation, prerequisites and scratch-directory commands. No missing local prerequisite was encountered. |
| Medium | Raw URLs fetch only the script, not referenced fixture files. `@latest` is moving, not a pin. | Fixed docs: distinguish source/raw URL, release install and dist download; explain cwd, PATH and fixed tags/commits. Raw parse-string invocation passed with the source setup. |
| Medium | Status documentation said index did not exist; CLI overview omitted shipped verbs. | Fixed: table covers all 16 named verbs and host inspection/execution options; runnable DB sequence and baseline/SARIF examples. `outline` landed during review and was added; `find` and `ts.db_*` remain proposals. |
| Medium | Older four-language inventories and a partial pack list concealed current coverage. | Fixed current-facing docs: 11 grammars, 15 JSON packs / 100 rule entries, 14 lmcode programs. Historical four-language milestone sections are identified as historical. |
| Medium | The supplied v0.6.0 release assumption was stale. | Live release API/installer resolve v0.6.11; README names the checked date and platform. Release and dist can lag main independently. |
| Low | Memory index omitted metrics, rule contracts, secrets and Java; invocation-return/script-language notes were loose bullets instead of table entries. | Added all six to the document map and checked every current memory file has a table link. |
| Low | `check` documentation claimed explicit files only; current dispatcher accepts directories. | Corrected accepted input forms. |

## Published artifacts

The GitHub latest-release API returned **v0.6.11**, published
2026-10-31T11:29:37Z. Dist was
`cf2c06db495d7923d430dd014c823341f6702e9d`.
Both Linux ARM64 downloads passed their own SHA256SUMS manifest:

```text
6e3466da21448371a61f6cd797353982be1c2b61ac81e7820cba95f897a999e3
```

Both report `lmok 0.6.11`; the install cache has the same hash. Release asset
inventory contains Linux x64/ARM64, macOS x64/ARM64 and Windows x64 plus the
checksum manifest. Only Linux ARM64 was executed in this review.

Each published binary passed parse/facts for all 11 languages, query (native
and code-shaped), lint, rules, annotated fixture test, metrics, dup, project
ci, and the full DB/baseline/SARIF/autofix/suppression sequence below. `ci`
exits **1** on `tests/fixtures/project-ci` as intended for its planted finding.
Their schema/signature probes pass under the older `--dsl*` spellings.
This is not a claim that release bytecode/compiler compatibility matches main.

## Tier-3 journey, end to end

Run from the clone root after building. Use a fresh DB path for these counts:

```sh
mkdir -p build/journey
build/lmok index tests/fixtures/db/index --db build/journey/index.db
build/lmok index tests/fixtures/db/index --db build/journey/index.db
build/lmok symbols '%' --lang c --db build/journey/index.db
build/lmok refs helper --db build/journey/index.db
build/lmok findings --db build/journey/index.db
build/lmok status --db build/journey/index.db
build/lmok lint examples/rules/c-security.json examples/data/sample.c --db build/journey/index.db
build/lmok findings --rule LBC001 --db build/journey/index.db
build/lmok status --db build/journey/index.db
```

Observed on main and both published binaries:

- First index: `indexed 5 files (0 unchanged, 0 removed)`; second: zero
  indexed, five unchanged.
- Symbols include `helper`, `main`, the `add` prototype and parameters;
  refs reports the `helper` call at `main.c 4:24-4:33` (zero-based).
- Before lint, findings is empty. Status: schema **2**, five files, 13
  symbols, nine refs, two edges, zero runs/findings, `last_run: none`.
- Recording lint prints one LBC001 finding. `findings --rule LBC001` returns
  it with `path: examples/data/sample.c`. Status then has six files, one run
  and one finding; last_run includes the recorded command.
- A repeat with `--new-since last --fail-on-new` reports no new findings and
  exits 0. Index records syntax facts; lint records findings separately.

Names/references are syntax candidates, not cross-file semantic resolution.
Explicit DB paths make this journey independent of an existing project DB.

## Examples and feature checks

Source compiler: `compiler.lock` revision
`8e334dda3bb821f3bc34be42d598259cb231411c`. Its reported `0.2.38` version is
not comparable to the npm installer's `0.2.449`; the source pin and successful
fixture compilation establish the tested pairing.

Executed all README CLI table examples on the source-built host, including
fixture-exported `--lmcode` parse-string and `check` C function metrics.
Ran all **20 `examples/*.lms`**, `examples/metrics.sh`, `examples/secrets.sh`,
and the README `lmctl autorun` raw URL with the pinned compiler and built host.
All exited 0. Scripts: `autofix-report.lms`, `cst-rules.lms`, `detect-error.lms`, `incremental-reparse.lms`, `lint-c.lms`, `lint-go.lms`, `lint-javascript.lms`, `lint-json.lms`, `lint-repo.lms`, `lint-tsx.lms`, `lint-typescript.lms`, `parse-file.lms`, `parse-string.lms`, `pattern-rules.lms`, `query-json.lms`, `report-json.lms`, `report-metadata.lms`, `rule-catalog.lms`, `sarif-report.lms`, `suppress-findings.lms`.

After rebasing onto `f904436`, also ran `build/lmok outline
tests/fixtures/outline --budget 30`, then indexed that fixture and reran with
`--db build/journey/outline.db`: both exited 0, with live signatures and DB
reference counts respectively. The published snapshots do not have this verb.

Ran all **14 `rules/lmcode/*.lms`** via the documented exporter
and `check`/body dispatcher loop; all exited 0. Findings and `gate=FAIL` in
demonstration output are not process failures.

Additional assertions on main, release and dist:

- Baseline captured from lint then reused with `--fail-on-new`: exit 0.
- Native SARIF for the C security demo: one result in valid JSON.
- Autofix preview, then apply to a scratch copy: bytes match the committed
  after fixture; repository fixtures remain untouched.
- Inline `lmok-disable: LBC001`: zero findings; `--no-suppress`: one finding.
- Secrets JSON pack executes; metrics/dup and all language facts execute.

## Clean gate

The pinned compiler was bootstrapped once and reused through `LMSCRIPT_BIN`.
Generated `build/` and `third_party/` were moved to ignored scratch backups,
leaving both absent before `scripts/sync-deps.sh && ./test.sh` (the same clean
starting state as deleting them). Initial clean gate on `b3a6537`: **PASS: 123 checks**. No skipped checks or relaxed expectations were introduced.

After rebasing onto `f904436`, repeated the full clean dependency sync/build/gate:
**PASS: 127 checks**, including four new outline checks. Linux static-link
verification passed. No test count changes were introduced by these docs.

Final base `5440a29` includes the concurrent mainline review fixes and its
three additional checks. Repeated the full clean sync/build/gate: **PASS: 130
checks**. Re-ran the README command sequence and outline on that final binary;
all expected exit codes and feature assertions passed. Preserved the new
`16-review-mainline-wave.md` memory-index entry.
