# Global Refactoring (`lmok refactor`)

`lmok refactor` provides IDE-grade multi-file refactoring on top of Tree-sitter CSTs (`cpp/refactor_pattern.cpp`, `cpp/refactor.cpp`, CLI entry in `cpp/main.cpp`):

- **Dry-run default**: all modes compute and verify every file plan in memory and emit a `git apply`-compatible unified diff (`--format diff`, default), site list (`--format sites` / `--sites`), or JSON summary (`--format json` / `--json`) plus a 16-hex plan digest. `--check` exits `1` when edits are pending (CI drift check).
- **`--apply`**: runs full two-phase execution — in-memory planning + tree safety verification for ALL target files first, optional `--expect DIGEST` / `--expect-sites N` / `--git-clean` checks, then temp-file staged atomic writes preserving permissions with rollback on any write error.
- **Pattern unifier (`--pattern` / `-p`, `--to`)**: direct CST unifier across `c`, `cpp`, `python`, `java`, `go`, `javascript`, `typescript`, `tsx`. Supports `$X` metavariables, `$...ARGS` variadics with smart comma elision, `--inside`/`--not-inside`/`--where VAR~REGEX`, bottom-up nested composition, standalone statement line deletion (refusing unbraced control-flow sole bodies unless `--skip-unsafe`), comment-loss refusal unless `--drop-comments`, conditional expression auto-parenthesization when torn by precedence, and per-line EOL preservation.
- **Safety model**: local edit-zone `ERROR`/`MISSING` refusal (allowing edits wholly inside clean recovered subtrees of outer `ERROR` wrappers unless `--strict-parse`), boundary-straddling refusal, and outside-frame shifted CST invariance.
- **Scope-aware `--rename`**:
  - Tier A (`--at FILE:LINE:COL`): lexical local/parameter rename within the cursor's declaring block/function, excluding inner shadows and member RHS (`obj.x` / `obj->x`), refusing new-name collisions.
  - Tier B (type/class): renames C++ `class`/`struct`, constructors, destructors (`~Foo`, `Foo::~Foo`), and type identifiers across `--header-lang cpp` headers and sources.
  - Tier C (`--kind method|field` with `--scope C`, or free functions): distinguishes `exact` sites from heuristic `candidate` member calls (`ptr->m()`), refusing `--apply` on candidates without `--allow-candidates` or `--exact-only`.
- **`--import` and `--rules FILE.json`**: exact `#include` / `import` specifier rewrites and sequential multi-rule JSON codemod packs.
- Verified by `tests/check-refactor.py` (with `--prove-controls`).
