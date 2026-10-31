# New-user journey review

Reviewed 2026-10-28 on branch `docs/user-journey`, starting at
`dd3690ddbd0bfe3e706f2f2f2d965e5f3c095d45`.
The [published README](https://github.com/lmctl-ai/lmok/blob/main/README.md)
was fetched from GitHub's raw endpoint and matched the starting checkout.
All changes in this review are documentation or example header comments;
script behavior and `# expect:` / `#|` lines are unchanged.

## Findings

High = prevents completing a documented path; medium = misleading behavior
or missing setup; low = inventory/navigation or nonblocking diagnostic.

| Severity | Friction and evidence | Resolution |
| --- | --- | --- |
| High | No prerequisite list: the build requires CMake >= 3.24, Ninja, C/C++17 compilers and Linux static runtime libraries; dependency sync requires Bash, curl, tar and SHA-256 utilities. | Fixed: explicit prerequisites and Ubuntu/macOS setup notes. All tools were present on the review machine; these were missing from docs, not missing locally. |
| High | npm setup omitted Node's version floor. Published `@lmctl-ai/lmctl@0.2.442` metadata requires Node >= 24.15.0; tests additionally require Python 3. | Fixed: tested npm version and runtime floor, Python requirement, compiler version check. |
| High | No clone/cd instruction or PATH export; `lmok` is not installed by CMake. lmscript also needs a host for schema discovery. | Fixed: clone, repo-root working directory, absolute build PATH, explicit `build/lmok` example commands. |
| High | Raw URL in an empty directory fails at `savedsl`: `cannot write build/parse-string.dslb: ENOENT`. | Fixed: `mkdir -p build` before raw-URL compilation. |
| High | The exact rules README command fails at `savedsl`: `cannot write build/rules-lmcode/c-function-metrics.dslb: ENOENT`. | Fixed: create `build/rules-lmcode` before compiling rules. |
| High | SARIF header says to execute `build/sarif-results.dslb`; the script actually saves `build/sarif-report.dslb`. | Fixed: header command only. The `.sarif` artifact remains `build/sarif-results.sarif`. |
| Medium | README presents `lmctl autorun` as the complete example run, but it only runs the outer script and saves bytecode; no JSON tree is printed. The standalone Scripting snippet likewise lacked execution instructions. | Fixed: separate compile/save and host execution steps, expected output including the success record. |
| Medium | `lmscript SOURCE OUTPUT.dslb` appears to select an output filename. Probe with `build/ignored-argument.dslb` produced no such file; the script's literal `savedsl` target was used. | Fixed: remove misleading positional arguments and explain that these are script arguments. |
| Medium | "Each compiles to bytecode" is false for `lint-repo.lms`, which runs subprocesses and never saves a bytecode file. | Fixed: all-example loop special-cases it; documented its one-line result. |
| Medium | "Nothing (offline)" headers omit the compiler, host/schema, output directory and cwd requirements. | Fixed: example headers refer to setup; docs distinguish offline-after-setup from first network resolution. Rule header shorthand is qualified by rules/README setup. |
| Medium | Raw URL execution does not fetch data files. `parse-file.lms` compiles in an empty directory but the host exits 3 with `file_read: examples/data/sample.json: No such file or directory`. | Fixed: explain repository fixtures and cwd requirements. Inline-data parse-string works without a checkout once the output directory exists. |
| Medium | `install="github:lmctl-ai/lmok@latest"` is described as a pin; it follows a moving release. Installation also does not put the executable on shell PATH. | Fixed: explain release resolution, printed executable path, PATH setup, fixed `@v0.4.0`, and the separate rolling dist branch. No packaging/code fix needed on tested platform. |
| Medium | Rule reuse instructions imply every check is one annotated SAM function. scanf pairing and symbol/cross-file checks require helper/set/dispatch logic too. | Fixed: describe both forms and copying their calling logic. |
| Low | JSON pack says five rules but contains six including LBJ104; tier inventory puts LBJ104 under lmcode; cross-file demo/LBC110 is omitted and E3 still calls cross-file references impossible. | Fixed: counts, placement, cross-file inventory and stale E3 wording. |
| Low | Paths to tier docs and examples are plain code text, and relative navigation is unclear from subdirectories. | Fixed: clickable repository-relative links across the four requested documents; link targets checked locally. |
| Low | Intro and license summary omit the shipped C grammar. | Fixed: name and link tree-sitter-c. |
| Low | `test.sh` check 38 still says "all 5 rules" for the six-rule JSON pack. | Fixed by the concurrent JSON-rule expansion merged during publication; the original review found no behavioral defect. |
| Low | GCC emits an unused-function warning for `ts(Node&)` in `cpp/host.cpp`. | Needs optional code cleanup; build succeeds. No code changed. |

## Published install verification

The release flow works on Linux ARM64; there is **no observed dist/release
layout blocker**. With no `lmok` on PATH, raw-URL compilation resolved
and downloaded the host into a fresh cache under the review workspace.
Its release metadata identifies `v0.4.0`, asset `lmok-linux-arm64`.
The executable reports `lmok 0.4.0`; its actual SHA-256 is:

```
725352922dec53b4ceaef138eb697fa4cfbae66e3f97b910e6b849ae64555c5f
```

That matches the ARM64 entry in the published
[dist SHA256SUMS](https://github.com/lmctl-ai/lmok/blob/dist/SHA256SUMS).
Both `lmscript install github:lmctl-ai/lmok@latest` and the explicit
`@v0.4.0` selector resolved successfully. The
[v0.4.0 release](https://github.com/lmctl-ai/lmok/releases/tag/v0.4.0)
and [dist branch](https://github.com/lmctl-ai/lmok/tree/dist) each expose
five platform executables plus SHA256SUMS. Only Linux ARM64 was executed;
asset presence does not establish runtime correctness on the other four.

Verified local and raw-URL `lmctl autorun` forms, followed by `--lmcode`.
After creating the output directory, the raw parse-string example also ran
successfully in an otherwise empty directory within the workspace.

## Build and example results

Environment: Linux aarch64, GCC 13.3.0, CMake 3.28.3, Ninja 1.11.1,
Node 24.22.0, Python 3.12.3, lmscript 0.2.442.

The original `scripts/sync-deps.sh`, CMake configure and CMake build
commands succeeded unchanged. No extra SDK checkout/sync was necessary.
All 13 examples and all nine lmcode rule demos passed their declared
expected output against the downloaded v0.4.0 executable after the documented
setup omissions were supplied. `lint-repo.lms` ran as an outer script;
the other 21 programs were compiled and executed with `--lmcode`.

The local test suite passed **53/53 checks**, including all examples, nine
violation/clean rule pairs, SARIF structure and fully static linkage.
A second `./test.sh` run started with no `build/` directory and passed
**53/53 checks** again; downloaded, hash-verified dependencies were retained.
The revised documented all-example and all-rule loops were also exercised.

No blocking code defect was found. Remaining follow-ups are the unused-helper warning above and execution on
other platforms. The counts above describe the original review revision.
No workflow or executable source was edited by this documentation change.

## Publication integration

After push authorization, main advanced to `eec706f` (JSON rule expansion).
The documentation conflicts were reconciled to retain all new rules: 11 JSON
pack rules and ten lmcode demos, including LBJ105 (four fixture findings).
The concurrent implementation and all its checks were retained unchanged.
The integrated suite passed **53/53 checks** from a fresh build directory,
including all ten lmcode violation/clean pairs, before pushing.
