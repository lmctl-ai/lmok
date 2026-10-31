# Rule metadata and pack author tests

Optional pack fields cwe/owasp/tags/url/confidence are parsed in
cpp/rule_metadata.hpp and carried by LintRule/LintFinding and entry Finding.
The five-argument ts.report_meta takes metadata as a JSON string. Strictly
validate types; sort/deduplicate list values; never enrich default JSON bytes.
Only `--metadata` opts `lint`/`ci`/`check`/`--lmcode`/`report_json` into the additional field.
SARIF integration seam: use the structured RuleMetadata fields for rule
properties/helpUri instead of parsing finding prose.

cpp/rule_test.hpp implements lmok test PACK FIXTURE... using actual CST comment
tokens and exact one-based line/rule sets, with no ignore/suppression/baseline.
It emits machine-readable differences plus located stderr. A string containing
the marker is not an annotation. Node report rows are zero-based internally.

Native-OS controls: tests/check-rule-contracts.py, wired into test.sh. Initial
control on the original host failed all six cases (missing verbs/flag/method).
Tests cover unchanged default shape, deterministic metadata, invalid fields,
CRLF/UTF-8 paths, strings vs comments, off-by-one, missing/extra rules and entries.

The tier-1 packs (including the concurrently added Java/Go packs) carry language/quality tags, medium confidence (syntactic
candidates), help URLs and selected CWE/OWASP mappings where applicable. Do not
assign a security taxonomy to style rules merely to fill every field. The C and
Python annotated fixtures combine the existing violations/clean pairs without
changing those older byte-oracle inputs; test.sh runs both plus examples/self-test.
C preprocessor values are opaque CST tokens: reparse their contents to recognize
trailing comments without accepting a marker in a macro string.

OWASP values are explicitly versioned taxonomy hints, not a blanket security tag.
The 2021 A03 mapped CWE list includes CWE-78/95, not CWE-120; unsafe buffer-copy
rule LBC001 therefore carries CWE-120 without A03. Source: https://top10.owasp.org/2021/A03_2021-Injection/

SARIF integration: c8cbab3 introduced the host writer; RuleMetadata now passes
through SarifRuleMeta and SarifResult for lint, check and ci. Explicit --sarif
always carries properties and helpUri even if JSON --metadata is absent. Entry
catalogs retain first declared metadata, with metadata also kept per result.
Do not rebuild SARIF by parsing default JSON, which deliberately omits these fields.
