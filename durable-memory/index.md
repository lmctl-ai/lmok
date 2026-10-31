# lmok durable memory

Technical documentation and operational notes for `lmok`.
`lmok` embeds tree-sitter 0.27.1 with 11 grammars: JSON, C, Python, Bash, Java, Go, JavaScript, TypeScript, TSX, lmscript and lmcode as a standalone CLI host for lmscript.
Repository: https://github.com/lmctl-ai/lmok.

## Document map

| File | Contents |
| --- | --- |
| [01-purpose-and-place.md](01-purpose-and-place.md) | What lmok is, the pinned parts, the minimal lmcode surface |
| [02-build-run-test.md](02-build-run-test.md) | Build, run, and test commands; adding examples and functions |
| [03-rule-methods.md](03-rule-methods.md) | Rule methods: @annotation + implements Rule, Node navigation, limitations |
| [04-dispatcher.md](04-dispatcher.md) | lint rule packs, the kind-indexed walk dispatcher, v6 entry-point trajectory |
| [05-rule-tiers.md](05-rule-tiers.md) | Three-tier rule strategy, shipped packs, dropped candidates, tier-2 plan |
| [06-realworld-validation.md](06-realworld-validation.md) | c-tier1 validated against real-world C codebases (sds, kilo, cJSON) |
| [07-robustness.md](07-robustness.md) | Bounded queries/regexes, input hardening, finding output escaping |
| [08-v6-entry-dispatch.md](08-v6-entry-dispatch.md) | v6 lmcode entry loading, CST dispatch, findings and CLI contract |
| [09-project-ci.md](09-project-ci.md) | .lmokignore, inline suppression, project config, and `lmok ci` |
| [09-facts-layer.md](09-facts-layer.md) | E6 facts extraction: facts.scm vocabulary, query_matches, scope chains, confidence, known edges |
| [10-lint-db-findings.md](10-lint-db-findings.md) | lint --db run recording + supersession, findings verb, DB-create/side-channel decisions |
| [11-pattern-rules.md](11-pattern-rules.md) | Code-shaped (semgrep-style) patterns compiled to queries: `query --pattern`, pack `pattern` fields, `ts.match_*`, corner cases |
| [12-autofix.md](12-autofix.md) | Whole-node fix metadata, verified edits and unified previews |
| [13-baseline-sarif.md](13-baseline-sarif.md) | lint --new-since/--baseline/--changed/--fail-on-new keying, findings.snippet column, host --sarif on lint/check/ci |
| [16-review-mainline-wave.md](16-review-mainline-wave.md) | Independent review of the bef3003..065a833 wave: index-after-lint fix, --changed hardening, bootstrap publish, import bindings, L notes |
| [11-metrics.md](11-metrics.md) | Local metrics, token duplication and C/Python cognitive complexity |
| [12-rule-contracts.md](12-rule-contracts.md) | Rule metadata and annotated pack-author tests |
| [12-secrets-packs.md](12-secrets-packs.md) | Secret shapes, contextual entropy, redaction and limits |
| [14-invocation-return.md](14-invocation-return.md) | Invocation return ownership and terminal frames |
| [15-script-languages.md](15-script-languages.md) | lmscript/lmcode grammars and the S5 injection seam |
| [java.md](java.md) | Java parser, facts, nine tier-1 rules and syntax limits |

Operational name: **lmok** (binary, schemas, CMake and install paths).
The [current CLI and setup](../README.md) and [pack inventory](../rules/README.md#pack-inventory)
are the user entry points. Tier-3 design documents distinguish proposals from
shipped verbs; historical milestone sections describe their original scope.
