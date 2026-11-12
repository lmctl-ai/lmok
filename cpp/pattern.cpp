// Pattern compiler; see pattern.hpp for the contract and durable-memory/11-pattern-rules.md for the
// corner cases this encodes.
#include "pattern.hpp"

#include <cstring>
#include <functional>
#include <set>
#include <tree_sitter/api.h>

namespace lmok {
namespace {

TSNode ts_of(const Node &node) {
  TSNode n;
  std::memcpy(&n, node.bytes, sizeof n);
  return n;
}

const char *kMetaPrefix = "__lmok_mv_";
const char *kEllipsis = "__lmok_ellipsis";

bool ident_start(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
bool ident_char(char c) { return ident_start(c) || (c >= '0' && c <= '9'); }

// `$NAME` -> placeholder identifier, `...` -> placeholder identifier. Both are valid identifiers in C and
// Python, so the fragment parses with the ordinary grammar and the placeholders come back as leaves.
std::string substitute(const std::string &pattern, std::vector<std::string> &names) {
  std::string out;
  for (size_t i = 0; i < pattern.size();) {
    if (pattern[i] == '$' && i + 1 < pattern.size() && ident_start(pattern[i + 1])) {
      size_t j = i + 1;
      while (j < pattern.size() && ident_char(pattern[j])) ++j;
      const std::string name = pattern.substr(i + 1, j - i - 1);
      names.push_back(name);
      out += kMetaPrefix + name;
      i = j;
    } else if (pattern.compare(i, 3, "...") == 0) {
      out += kEllipsis;
      i += 3;
    } else {
      out += pattern[i++];
    }
  }
  return out;
}

std::string quote(const std::string &text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '\\' || c == '"') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else out += c;
  }
  return out + "\"";
}

// Nodes whose text is the match, even though the grammar gives them children (string pieces).
bool text_leaf(Grammar grammar, const std::string &kind) {
  if (grammar == Grammar::C) return kind == "string_literal" || kind == "char_literal" || kind == "concatenated_string" || kind == "system_lib_string";
  if (grammar == Grammar::Python) return kind == "string" || kind == "concatenated_string";
  return false;
}

struct Emitter {
  Grammar grammar;
  const std::string &text;
  std::vector<std::string> predicates;
  std::map<std::string, int> seen; // metavariable -> occurrences
  CompiledPattern out;
  int literals = 0;

  std::string node_text(TSNode n) const { return text.substr(ts_node_start_byte(n), ts_node_end_byte(n) - ts_node_start_byte(n)); }

  bool is_ellipsis(TSNode n) const { return ts_node_named_child_count(n) == 0 && node_text(n) == kEllipsis; }
  bool is_meta(TSNode n, std::string &name) const {
    if (ts_node_named_child_count(n) != 0) return false;
    const std::string t = node_text(n);
    if (t.compare(0, std::strlen(kMetaPrefix), kMetaPrefix) != 0) return false;
    name = t.substr(std::strlen(kMetaPrefix));
    return true;
  }

  std::string emit(TSNode n) {
    std::string name;
    // `if ($C) $S`: a statement slot holding only a metavariable binds the statement itself (a block too),
    // not an expression statement, so pattern-inside shapes like `while ($C) $B` cover every body form.
    if (std::string(ts_node_type(n)) == "expression_statement" && ts_node_named_child_count(n) == 1 && is_meta(ts_node_named_child(n, 0), name))
      return emit(ts_node_named_child(n, 0));
    if (is_meta(n, name)) {
      const int k = ++seen[name];
      const std::string capture = "mv_" + name + "_" + std::to_string(k);
      if (k == 1) { out.metavariables.push_back(name); out.first_capture[name] = capture; }
      else predicates.push_back("(#eq? @" + out.first_capture[name] + " @" + capture + ")");
      return "(_) @" + capture;
    }
    if (is_ellipsis(n)) throw PatternError("'...' is only supported as a call argument in this slice");
    const std::string kind = ts_node_type(n);
    if (ts_node_named_child_count(n) == 0 || text_leaf(grammar, kind)) {
      const std::string capture = "lit_" + std::to_string(++literals);
      predicates.push_back("(#eq? @" + capture + " " + quote(node_text(n)) + ")");
      return "(" + kind + ") @" + capture;
    }
    // Children: named children carry structure (anchored when the parent has no field names, so the
    // sequence is exact); anonymous children matter only when the grammar names their slot (`operator:`).
    const uint32_t count = ts_node_child_count(n);
    bool any_field = false;
    struct Child { TSNode node; const char *field; bool named; };
    std::vector<Child> kids; // in child order: named children, and anonymous children that fill a named slot
    for (uint32_t i = 0; i < count; ++i) {
      TSNode c = ts_node_child(n, i);
      const char *field = ts_node_field_name_for_child(n, i);
      if (ts_node_is_named(c)) {
        if (std::string(ts_node_type(c)) == "comment") continue; // comments in a pattern say nothing
        kids.push_back({c, field, true});
        if (field) any_field = true;
      } else if (field) {
        kids.push_back({c, field, false}); // e.g. `operator: "->"`, kept in place: child order is part of the query
      }
    }
    std::vector<std::pair<TSNode, const char *>> named;
    for (const Child &k : kids) if (k.named) named.emplace_back(k.node, k.field);
    std::string s = "(" + kind;
    if (!any_field && named.empty()) {
      // No structural children at all (e.g. `f()`): match the exact text so `f(1)` does not qualify.
      const std::string capture = "lit_" + std::to_string(++literals);
      predicates.push_back("(#eq? @" + capture + " " + quote(node_text(n)) + ")");
      return s + ") @" + capture;
    }
    bool anchored = !any_field;
    const bool is_args = kind == "argument_list";
    // Anchors (`.`) make the named-child sequence exact; an ellipsis removes the anchor on its side, so
    // `f($A, ...)` is "first argument A, anything after" and `f(...)` is "any arguments".
    std::vector<std::string> parts;
    bool previous_ellipsis = false; // the sequence start counts as a boundary that gets an anchor
    for (const Child &k : kids) {
      if (!k.named) { parts.push_back(std::string(k.field) + ": " + quote(node_text(k.node))); continue; }
      if (is_ellipsis(k.node)) {
        if (!is_args) throw PatternError("'...' is only supported as a call argument in this slice");
        previous_ellipsis = true;
        continue;
      }
      std::string part;
      if (anchored && !previous_ellipsis) part += ". ";
      if (k.field) part += std::string(k.field) + ": ";
      part += emit(k.node);
      parts.push_back(part);
      previous_ellipsis = false;
    }
    for (const auto &p : parts) s += " " + p;
    if (anchored && !previous_ellipsis && !parts.empty()) s += " .";
    return s + ")";
  }
};

bool has_error_node(TSNode n) {
  if (ts_node_is_error(n) || ts_node_is_missing(n)) return true;
  const uint32_t count = ts_node_child_count(n);
  for (uint32_t i = 0; i < count; ++i) if (has_error_node(ts_node_child(n, i))) return true;
  return false;
}

std::string located(TSNode n) {
  const TSPoint p = ts_node_start_point(n);
  return std::to_string(p.row + 1) + ":" + std::to_string(p.column + 1);
}

TSNode first_problem(TSNode n) {
  if (ts_node_is_error(n) || ts_node_is_missing(n)) return n;
  const uint32_t count = ts_node_child_count(n);
  for (uint32_t i = 0; i < count; ++i) { TSNode p = first_problem(ts_node_child(n, i)); if (!ts_node_is_null(p)) return p; }
  return TSNode{};
}

} // namespace

CompiledPattern compile_pattern(Grammar grammar, const std::string &pattern) {
  if (grammar != Grammar::C && grammar != Grammar::Python)
    throw PatternError("pattern '" + pattern + "': code patterns are supported for c and python in this slice");
  std::vector<std::string> names;
  const std::string source = substitute(pattern, names);
  // Parse; a C expression without ';' is retried as a statement so `strcpy(a, b)` reads naturally.
  Tree tree = parse(grammar, source);
  std::string text = source;
  bool wrapped = false;
  if (has_error_node(ts_of(node_root(tree))) && grammar == Grammar::C) {
    // C forbids most statements and bare expressions at file scope: retry with ';' and then inside a function body.
    for (const std::string &candidate : {source + ";", "void __lmok_wrap(void) {" + source + "}", "void __lmok_wrap(void) {" + source + ";}"}) {
      Tree retry = parse(grammar, candidate);
      if (!has_error_node(ts_of(node_root(retry)))) { tree = std::move(retry); text = candidate; wrapped = candidate.compare(0, 5, "void ") == 0; break; }
    }
  }
  TSNode root = ts_of(node_root(tree));
  if (wrapped && !has_error_node(root)) {
    // translation_unit -> function_definition -> body: compound_statement; the pattern is its one statement.
    TSNode body = ts_node_child_by_field_name(ts_node_named_child(root, 0), "body", 4);
    if (!ts_node_is_null(body)) root = body;
  }
  if (has_error_node(root)) {
    const TSNode p = first_problem(root);
    throw PatternError("pattern '" + pattern + "' does not parse as " + grammar_name(grammar) + " (" +
                       (ts_node_is_missing(p) ? std::string("missing ") + ts_node_type(p) : "syntax error") + " at " + located(p) + ")");
  }
  // Unwrap the file wrapper and a single expression statement so the pattern names the construct itself.
  TSNode target = root;
  for (int depth = 0; depth < 2; ++depth) {
    std::vector<TSNode> kids;
    const uint32_t count = ts_node_named_child_count(target);
    for (uint32_t i = 0; i < count; ++i) { TSNode c = ts_node_named_child(target, i); if (std::string(ts_node_type(c)) != "comment") kids.push_back(c); }
    const std::string kind = ts_node_type(target);
    if (depth == 0) {
      if (kids.empty()) throw PatternError("pattern '" + pattern + "' is empty");
      if (kids.size() != 1) throw PatternError("pattern '" + pattern + "': one statement or expression per pattern (found " + std::to_string(kids.size()) + ")");
      target = kids[0];
    } else if (kind == "expression_statement" && kids.size() == 1) {
      target = kids[0];
    }
  }
  Emitter e{grammar, text, {}, {}, {}, 0};
  std::string body = e.emit(target);
  std::string query = "(" + body + " @root";
  for (const auto &p : e.predicates) query += " " + p;
  query += ")";
  e.out.query = query;
  return e.out;
}

NodeList pattern_matches(const Node &root, const CompiledPattern &compiled, Grammar grammar, const std::string &capture) {
  const std::string wanted = capture.empty() ? "root" : compiled.first_capture.at(capture);
  NodeList out;
  const QueryMatches matches = query_matches(root, compiled.query, grammar);
  for (const QueryMatch &m : matches.matches)
    for (const QueryCapture &c : m.captures)
      if (c.name == wanted) { out.items.push_back(c.node); break; }
  return out;
}

} // namespace lmok
