# Rules as lmscript snippets (SonarQube study)

Goal: code-analysis rules expressed as small lmscript snippets running over a
tree-sitter AST inside `lmok`, instead of SonarQube's compiled, rigid rule
classes.

## What SonarQube actually does (study summary)

Sources: SonarSource/sonarqube, sonar-java, sonar-plugin-api, SonarJS, sonar-text.

- **A rule is a compiled Java class.** Most extend `IssuableSubscriptionVisitor`:
  declare AST node kinds in `nodesToVisit()`, get callbacks in `visitNode(Tree)`,
  raise with `reportIssue(tree, message)`. The engine walks the AST once and
  dispatches by node-kind membership. Example: `ContinueInLoopCheck` (S909) is a
  15-line class — subscribe to `CONTINUE_STATEMENT`, report every hit.
- **Metadata is split from logic**: `@Rule(key = "S909")` on the class, plus
  `S909.json` (title, type BUG/CODE_SMELL/VULNERABILITY, severity, remediation
  cost, tags) and `S909.html` (description) per rule, loaded by
  `RulesDefinition`/`RuleMetadataLoader`.
- **Pipeline**: Sensor → CheckFactory (activates rules from the quality profile)
  → parser (Eclipse JDT for Java) → VisitorsBridge (single merged AST walk) →
  `NewIssue` (primary location, secondary locations, flows, severity/cost
  overrides) → server persistence.
- **What rules can inspect**: 129 `Tree.Kind`s, tokens/trivia, plus full
  semantics — resolved types, symbols (`MethodMatchers`), control-flow graphs,
  and a symbolic-execution engine for bug/vulnerability rules.
- **Configuration**: `@RuleProperty` parameters injected from quality profiles;
  `// NOSONAR` suppression platform-side.
- **Scale/cost**: sonar-java has ~772 rule classes. A custom-rule plugin needs a
  213-line Maven pom, Plugin/RulesDefinition/CheckRegistrar glue classes, jar
  packaging, and a server restart to deploy.

### SonarSource is already converging on the minimal kernel

- **sonar-text secrets rules**: empty Java subclass per rule; all logic lives in
  a YAML spec (pre-filter strings, a matching regex, context patterns, entropy
  post-filter, inline examples as tests) interpreted by a generic matcher.
- **SonarJS**: rules are plain ESLint rule modules over ESTree ASTs in a Node
  bridge process; the subscription is an **esquery CSS-like selector string**,
  the check is a small JS function calling `context.report(...)`.

## The essential kernel of "a rule"

Everything above reduces to four parts:

1. **Metadata record** — key, title, severity, type, tags. Pure data.
2. **Selector/subscription** — node-kind list, esquery selector, or regex
   pre-filter.
3. **Check function** — `f(node, report) -> ()`, interpreted.
4. **Issue record** — {rule key, location/range, message, optional secondary
   locations}.

The plugin packaging, profile plumbing, reflection factories, and server are
incidental weight. What a pure tree-sitter setup genuinely loses vs. sonar-java
is semantic resolution (types, symbols) and CFG/symbolic execution — so scope
snippet rules to syntax-level checks, or feed a separately-built symbol index.

## The lmok mapping

tree-sitter's query engine (`ts_query_*`, already compiled into our binary via
`lib/src/lib.c`) is the native selector — S-expression patterns with captures,
the direct analog of esquery selectors and `nodesToVisit()` combined.

- **Rules = annotated functions in a `.lms` snippet.** Compile it to a v6 `.dslb`;
  `lmok check rule.dslb FILE...` dispatches annotated entries by
  tree-sitter node kind. `lmok --lmcode rule.dslb` remains the body/configuration
  path, and `lmctl autorun rules/no-foo.lms` remains useful for scripts without
  host entry dispatch.
- **Selector**: `@kind` for host entry dispatch; `ts.query(node, pattern)`
  for captures inside a rule or body script (implemented).
- **Check**: typed lmcode over matches/nodes — node accessors
  (`kind`, `text`, `start/end row/col`, child navigation) exposed on a `Node`
  type.
- **Report**: `ts.report(node, message, rule, severity)` stores explicit
  finding metadata. `check` prints the existing JSON array/count per input;
  scripts may use `ts.report_json()` to build SARIF or apply policy.
- **Metadata = explicit report data**: rule IDs and severity belong to
  `ts.report` arguments or a companion catalog. Entry names are callable
  identities, annotations are node-kind subscriptions. Body locals cannot be
  captured by rules; outer LMScript values can be inserted with `${...}` at
  compile time. Host-side inline suppression is available; see the current README.
- **Testing**: the existing examples convention (`# expect:` / `#|` header
  lines, inline positive/negative fixtures) is exactly sonar-text's
  inline-examples model.

### Runnable rule (JSON grammar)

After the [source setup](../README.md#build-and-run), save the following as
`build/lmb001.lms`. Export this entry fixture and dispatch it from the repo root:

```sh
node scripts/compile-fixture.mjs build/lmb001.lms build/lmb001.dslb
build/lmok check --lang json build/lmb001.dslb examples/data/sample.json
```

The maintainer exporter requires the bootstrapped `LMSCRIPT_BIN`.

```lmscript
p = _lmcode_ cli="lmok";
@pair
fn no_password(n: Node) -> void implements Rule {
  if (n.child_count() > 0 && n.child(0).text() == "\"password\"") {
    ts.report(n, "do not store passwords in config", "LMB001", "major");
  }
}
end _lmcode_
r = run_lmcode(p);
assert(r.ok, "host failed");
```

The host walks once and supplies matching nodes. Finding IDs remain the explicit
report payload, not the function name. Body-oriented examples are still valid
with `--lmcode`, including scripts that build SARIF files from `ts.report_json()`.

## Non-goals (v1)

- No quality profiles / severity matrix — the snippet is the policy.
- No semantic resolution, CFG, or symbolic execution.
- The original v1 excluded fixes; current main supports guarded whole-node
  autofixes through `ts.report_fix` and JSON packs (see the README).
