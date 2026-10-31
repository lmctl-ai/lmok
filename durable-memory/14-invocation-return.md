# Invocation envelopes (R6)

The SDK InvocationResults collector owns independent per-program sealed sinks and
strict private metadata. --lmcode accepts repeated flag/file pairs; --first and
--last roles reorder dispatch while output stays in add order. Shared Host/context
preserves effects (including report_count) across programs. Standard regions retain
their per-run lifetime. Do not restore the prior per-file Host reset.

check retains one selected slot across the body and selected entries for all input
files. Input-local findings/Node resets retain their original semantics. Findings
and SARIF rule/result ordering, IDs and HALT results are unchanged. Tagged
host_summary diagnostics replace old duplicate unframed success records. Write one
compact terminal success/failure envelope after global validation/SARIF. Failed
SARIF is app/APP_FAIL exit4. Known builder failures carry part; global/single omit.
No result or parts on failure. First observed failure wins; no replay or fallback.

Schema is generated through the canonical merge helper, never edited by hand. All
24 returns and raw-schema hashing stay intact. Fixture-only bytecode generation
uses scripts/compile-fixture.mjs through the source compiler library: it evaluates
construction/splices, captures internal compiled bytes, and never runs a host body.
No public export function or legacy marker is reintroduced. Public examples use
run_lmcode; CLI-only lint examples invoke the lint command with explicit argv.

Full test.sh includes real public single/builder/role/reuse/part/global flows,
findings/SARIF exact oracles and retained return/entry controls. tests/mutate-invocations.py
kills lost-summary/reset-context/wrong-slot plus real subprocess-per-file faults.
scripts/test-return-controls.py retains five compiled framing/entry/SARIF mutants.
No releases while other sample adapters remain on the previous envelope protocol.
