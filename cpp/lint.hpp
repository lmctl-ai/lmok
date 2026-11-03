#pragma once
// Rule-pack loading and the lint dispatcher behind `lmok lint`.
//
// A rule pack is conditions-as-data: each rule declares an `on` node kind
// (mirrors the @annotation value used by the v6 check command) or a
// tree-sitter `query` pattern, plus optional text_matches regex and the
// finding message/severity. See README "Rule packs" for the format.

#include "host.hpp"
#include "fix.hpp"
#include <optional>
#include "pattern.hpp"
#include "rule_metadata.hpp"
#include "suppression.hpp"

#include <regex>
#include <string>
#include <vector>

namespace lmok {

struct LintRule {
  std::string id;
  RuleMetadata metadata; // SARIF: rule properties and helpUri; JSON only under --metadata

  bool redact = false; // Render only the first four content bytes, before any output/DB sink.
  Grammar grammar = Grammar::Json;
  std::string on;          // CST node kind; empty = not set
  std::string query;       // S-expression pattern; empty = not set
  std::string pattern;     // code-shaped pattern (pattern.hpp); empty = not set; compiled into `compiled`
  std::string capture;     // pattern metavariable / query capture to report; empty = default candidates
  std::string pattern_inside, pattern_not_inside; // ancestor filters, also code-shaped; empty = not set
  CompiledPattern compiled, compiled_inside, compiled_not_inside;
  std::string guard; // Optional validated structural refinement for lmscript migration rules.
  std::string message;     // "{text}" expands to the matched node's text
  std::optional<std::string> fix; // plain whole-node replacement; no metavariable expansion
  std::string severity = "minor"; // free-form; the pack owns the vocabulary
  std::string text_matches;      // ECMAScript regex source; empty = not set
                                 // (applied through regex_search_bounded: see host.hpp kRegexTextCap)
  std::regex text_regex;         // valid only when text_matches is non-empty
};

struct LintFinding {
  int64_t row = 0, col = 0, end_row = 0, end_col = 0; // 0-based
  std::string rule, severity, message;
  RuleMetadata metadata;
  std::optional<FixEdit> fix;
  // The matched node's source text with whitespace runs collapsed to one
  // space (lmok::normalize_snippet). Never printed in lint output; recorded
  // in the findings table as the position-independent baseline key part.
  // Redacted rules store sha256:<digest> of that normalized value instead.
  std::string snippet;
  bool redacted = false; // Only identities hashed at the rule boundary may be exported.
};

// Parse and validate a rule pack file. Throws dslvm::Error (plain message,
// naming the rule id where one exists) for malformed JSON, missing fields,
// duplicate ids, unknown languages, bad regexes, and on/query violations.
std::vector<LintRule> load_rule_pack(const std::string &path);

// Run the dispatcher over one file's text. Rules are grouped by grammar and
// the text is parsed once per grammar used. `on` rules dispatch through ONE
// pre-order CST walk with a kind→rules index (the dispatcher core v6
// reuses); `query` rules run one ts_query cursor per rule over the root
// (merging them into the single walk is a later optimization — deliberately
// no Rete); `pattern` rules run their compiled query and, when
// `pattern-inside`/`pattern-not-inside` are set, keep a candidate only if some
// ancestor is (is not) the root of a match of that pattern, over one parent map
// per file built from a single cursor walk. Findings are sorted by (row, col).
// A query compile failure throws QueryError with the rule id prepended.
std::vector<LintFinding> lint_text(const std::vector<LintRule> &rules, const std::string &text, Suppression *suppression = nullptr, bool no_suppress = false, const std::map<Grammar, Tree> *parsed = nullptr, bool lmscript_container = false);

class Database;

// One linted file's contribution to a recorded run (lint --db). `path` is the
// path exactly as printed in lint output (forward slashes); `sha256` is the
// lowercase hex content hash of the linted bytes.
struct LintFileRecord {
  std::string path;
  std::string lang; // grammar name from the extension, "unknown" if not inferable
  std::string sha256;
  int64_t mtime = 0, size = 0;
  std::vector<LintFinding> findings;
};

// Record one completed lint run (docs/tier3/db-design.md §1/§5): insert a
// runs row, then per file mark that file's old findings superseded=1 and
// insert the fresh set superseded=0. A file with no files row yet gets a
// minimal one (real content hash/mtime/size) so findings never dangle. All
// in ONE transaction — a failure rolls back and no partial run is ever
// visible (a failed/interrupted scan must never look like a clean result).
// Throws DbError.
void record_lint_run(Database &db, int64_t started_at, const std::string &command,
                     const std::vector<LintFileRecord> &files,
                     const std::string &scope = ""); // rule-pack scope; explicit when recording

// Whitespace-collapsed source text: every run of ASCII whitespace becomes
// one space, ends trimmed. This is the position-independent part of the
// baseline key (lint --new-since/--baseline), so edits ABOVE a finding do
// not change its key.
std::string normalize_snippet(const std::string &text);

} // namespace lmok
