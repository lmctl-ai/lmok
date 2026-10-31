# Autofix (Wave 3 item 7)

Autofix is an optional structured edit on a finding. A pack rule may provide a
plain `fix` replacement string. The host reports it as `fix: {start_byte,
end_byte,text}` and applies edits only when the range is valid, non-overlapping,
and the replacement reparses with the same tree shape outside the replaced
node. Edits are applied bottom-up. `build/lmok lint PACK FILE --diff` prints a
unified diff on stdout with a fixed count on stderr.
`--fix` verifies and re-lints the candidate once before writing; the verified
parse is reused by that lint pass. A second run is
idempotent. Suppressed findings never supply edits.

The first content slice keeps replacements deliberately literal: the shell
pack converts exact `` `date` `` and `` `pwd` `` nodes to `$(date)` and
`$(pwd)`, while the C pack canonicalizes exact `01` and `0XFF` number literals
to `1` and `0xff`. Both old and new values remain number literal nodes with
the same value. Metavariable substitution is a follow-up for pattern rules
and is not assumed by these packs. The `examples/autofix-report.lms` example
exercises `ts.report_fix` directly.

Fixtures in `tests/fixtures/autofix-*-before.*` and `*-after.*` pin bytes. The
host preserves CRLF when writing a CRLF input. Overlapping fixes are refused as
a group, and parse/error/tree-shape verification failure leaves the original
file untouched.


The source must already parse cleanly. The verifier masks the reported node in
both CSTs and compares the full remaining topology, including anonymous tokens,
ancestor kinds and unaffected leaf bytes. Every new range must be exactly a
unambiguous syntax node; equal-range wrappers are refused rather than masked as
an ancestor. Root/full-file and mixed-grammar replacements are refused in this
slice. CRLF normalization precedes both offset mapping and no-op detection.
Unsafe or overlapping batches fail with exit 3 before a write. This is syntax
safety, not a semantic proof for arbitrary custom rules.

Previewing never writes source or a findings DB run, even with `--db`. Applying
fixes records the final text/findings when a DB is requested; the DB schema still
stores positions/message, not replayable fixes. `check`/`--lmcode` expose report_fix
metadata but do not apply edits.
