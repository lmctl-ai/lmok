#pragma once
// E6: per-file facts extraction (docs/tier3/deep-analysis.md §4, §5).
//
// A Fact is one record of the knowledge tree: a definition, a reference or
// an edge, with its name as written, the dotted chain of enclosing
// scope-defining definitions, exact ranges, and a confidence label — the
// honesty contract (exact|candidate|unresolved|external) that replaces
// pretending to have semantics. Extraction is driven by the embedded
// queries/<lang>/facts.scm files (tree-sitter query vocabulary:
// `@def.<kind>`/`@ref.<kind>` on the fact node, `@name` on the name node,
// `(#set! confidence "...")` per pattern) plus the small amount of host logic
// queries cannot express (scope chains, Python method detection, include /
// source target normalisation). Pure functions over Tree/Node; no tree-sitter
// headers here (host.cpp owns that TU).
#include "host.hpp"

#include <functional>
#include <string>
#include <vector>

namespace lmok {

struct Fact {
  std::string language; // injected facts retain their inner grammar; file ownership stays outer
  std::string kind;       // def.function, def.key, ref.call, ref.include, ...
  std::string name;       // as written (quotes/angle brackets stripped for edges)
  std::string scope;      // "" at top level, else "outer.inner"
  std::string confidence; // exact | candidate | unresolved | external
  // Extent of the fact node (the whole definition / call / directive).
  int64_t row = 0, col = 0, end_row = 0, end_col = 0; // 0-based, end exclusive
  int64_t start_byte = 0, end_byte = 0;
  // Where the name is (start of the @name node), for navigation.
  int64_t name_row = 0, name_col = 0;
};

// The embedded facts.scm source for a grammar.
const std::string &facts_query(Grammar grammar);

// Extract every fact of one tree, sorted by (start_byte, kind, name,
// end_byte) with exact duplicates removed. Throws QueryError if the embedded
// query fails to compile (a build bug, not a user error).
std::vector<Fact> extract_facts(const Tree &tree, Grammar grammar);

// One JSON Lines record (no trailing newline). Key order is fixed: file,
// lang, kind, name, scope, confidence, row, col, end_row, end_col,
// start_byte, end_byte, name_row, name_col. `escape` is the host's JSON
// string escaper (invalid UTF-8 handling is the host's policy).
std::string fact_json(const Fact &fact, const std::string &file, const std::string &lang,
                      const std::function<std::string(const std::string &)> &escape);

} // namespace lmok
