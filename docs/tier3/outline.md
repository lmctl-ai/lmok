# Outline (E9)

`lmok outline FILE|DIR... [--db PATH] [--budget N]` produces a compact
repo map. Directory traversal, supported languages and ignore rules match
`facts`. There must be at least one input. `--` ends option parsing.

Definitions and signatures **always come from live source through
extract_facts**, in both DB and non-DB invocations. This is the sole source
of definitions; there is no cached-symbol mode and no automatic indexing.
Files without definitions are omitted. Repeated lexically normalized paths
are deduplicated (symlink aliases are not resolved).

Each block starts with the normalized input path, followed by one line per
definition, ordered by source offset, kind, name and end offset:

```text
src/main.c (refs: 12)
  main  function  0:0  signature: int main(void) {
```

Positions are zero-based definition-start row and byte column, as in
`facts`. Nonempty containers appear as `scope: outer.inner` before
`signature:`. A signature is the definition's first physical source line,
clipped at the definition end and stripped of trailing spaces/tabs; it is
not a reconstructed type signature. Names, scopes, paths and signatures
use JSON string escaping without surrounding quotes, so embedded tabs,
newlines and control characters cannot add output lines.

DB resolution follows `index`: explicit `--db`, then `LMOK_DB`, then the
default DB. If the resolved implicit DB is absent, no DB is used. An explicit
missing DB is an error. An existing DB is opened read-only and supplies
**ranking only**. For each file we count rows in `refs` whose name exactly
matches any unique live definition name in that file. All reference kinds
and all files in that DB participate. A reference can contribute to several
files with ambiguous names; repeated definitions in one file do not double
count it. These are name-match occurrence counts, not resolved dependencies.
The DB may be stale: run `index` to refresh ranking. Definitions remain live.

With a DB, headers include `(refs: N)` and files sort by decreasing count,
then path. Without a DB, headers omit the count and files sort by path.
Definition lines are identical for the same source either way.

`--budget N` accepts a positive integer limiting **all output lines**,
including `... (M more files)`. Blocks are indivisible. If the full map
exceeds the budget, reserve one line for the marker and print the ranked
prefix of whole blocks that fits. The first non-fitting block and all later
blocks are omitted. Thus a small budget can print only the marker. Empty
maps print nothing. This is a line budget, not a token or byte budget.
