# lmcode bytecode (DSLB v6) entry dispatch

`lmok check [--lang json|c|python|bash|java|go|javascript|typescript|tsx] RULES.dslb FILE...` is the host
entry-dispatch path. It loads one DSLB v6 program and rejects v5 files through
the SDK's version check. The host indexes `Program::entries()` by annotation, preserving entry source order, then runs the body once for configuration.

For every explicitly named input file, the host resets transient trees, node
handles and findings, parses once with the selected grammar, and performs one
pre-order cursor walk over all CST nodes, including anonymous kinds. Every
entry whose annotation equals the node kind is called with a `Node` handle.
Directories and stdin are intentionally outside this command; cross-file
aggregation remains a body-level `--lmcode` responsibility.

Each annotation must name a kind the walk can visit in the selected grammar (a named node, an
anonymous token or `ERROR`, checked with `ts_language_symbol_for_name`/`ts_language_symbol_type`);
unknown spellings, supertypes and hidden symbols are refused at load (`LOAD_UNBOUND`, exit 3) so a
misspelled rule cannot be silently dead (WP14 F3).

Rule entries must implement the schema SAM `Rule(n: Node) -> void`. A rule's
finding id and severity come from its `ts.report` payload; the entry name is a
callable identity only and is not inserted as a default finding id. Two-argument
reports therefore retain absent `rule`/`severity` fields. Existing report JSON
ordering and SARIF examples remain script-owned.

Each input emits the report JSON array, `N findings in FILE` (FILE as given on the command line,
the same line `lint` prints) and the standard success record; body findings, when any, are a
group of their own (`N findings in body`) before the first file. Inputs are processed in argument
order. An entry failure prints the partial group collected so far, then the located run-phase
error record, and stops the run with exit 3 (WP14 F2/F5/F6). `--lmcode` continues to run only the body, preserving
existing scripts and rule files that perform their own traversal.

The host schema already declares `Rule` and `Node`; changing those exact schema
bytes changes the DSLB hash, so all rule files must be recompiled together.
The SDK pin is lmcode-cpp `267e9a0` or newer, with the 73-vector v6 corrections.
The private compiler can be installed without a global link:

```sh
npm install --prefix /tmp/lmok-tools @lmctl-ai/lmctl@0.2.444
```

Rules remain subject to lmcode constraints: no recursion, void rules have no
return, value rules return once at the end, helpers are inline, and body locals
cannot be captured by entries. Duplicate annotation values are legal and cause
multiple entries to fire in source order. Annotation names may be anonymous or
keyword-like tree-sitter kinds.

Local Linux gate: PASS 77 checks, including seven entry-dispatch cases. The
73-vector SDK fixes also reject duplicate entry names and hidden-local stores.
