// Direct CST recursive unifier, variadic template renderer, and bounded
// structural verifier for `lmok refactor` (and `lint` pattern autofixes).
// Leaves cpp/pattern.cpp (`compile_pattern`) untouched so existing query contracts
// remain byte-identical.

#include "refactor.hpp"
#include "pattern.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <tree_sitter/api.h>

namespace lmok {
namespace {

TSNode ts_of(const Node &node) {
  TSNode n;
  std::memcpy(&n, node.bytes, sizeof n);
  return n;
}

const char *kMetaPrefix = "__lmok_mv_";
const char *kVarPrefix = "__lmok_var_";
const char *kEllipsis = "__lmok_ellipsis";

bool ident_start(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
bool ident_char(unsigned char c) {
  return ident_start(c) || (c >= '0' && c <= '9');
}

bool has_error_or_missing(TSNode n) {
  if (ts_node_is_error(n) || ts_node_is_missing(n)) return true;
  const uint32_t count = ts_node_child_count(n);
  for (uint32_t i = 0; i < count; ++i)
    if (has_error_or_missing(ts_node_child(n, i))) return true;
  return false;
}

TSNode first_error_node(TSNode n) {
  if (ts_node_is_error(n) || ts_node_is_missing(n)) return n;
  const uint32_t count = ts_node_child_count(n);
  for (uint32_t i = 0; i < count; ++i) {
    TSNode sub = first_error_node(ts_node_child(n, i));
    if (!ts_node_is_null(sub)) return sub;
  }
  return TSNode{};
}

std::string node_point_str(TSNode n) {
  const TSPoint p = ts_node_start_point(n);
  return std::to_string(p.row + 1) + ":" + std::to_string(p.column + 1);
}

std::string slice(const std::string &src, TSNode n) {
  const uint32_t a = ts_node_start_byte(n);
  const uint32_t b = ts_node_end_byte(n);
  if (b <= a || a >= src.size()) return "";
  return src.substr(a, std::min<size_t>(b, src.size()) - a);
}

bool is_comment_kind(const std::string &kind) {
  return kind == "comment" || kind == "line_comment" || kind == "block_comment";
}

bool is_comment(TSNode n) {
  return ts_node_is_named(n) && is_comment_kind(ts_node_type(n));
}

// Single-child equal-span wrapper kinds that can transparently unwrap when matching placeholders.
bool is_transparent_wrapper(const std::string &kind) {
  return kind == "expression_statement" || kind == "type_descriptor";
}

bool is_sequence_container(const std::string &kind) {
  return kind == "argument_list" || kind == "arguments" ||
         kind == "parameter_list" || kind == "parameters" ||
         kind == "formal_parameters" || kind == "template_argument_list" ||
         kind == "type_arguments" || kind == "initializer_list" ||
         kind == "array" || kind == "list" ||
         kind == "compound_statement" || kind == "block" ||
         kind == "statement_block";
}

bool is_statement_kind(const std::string &kind) {
  return kind == "expression_statement" || kind == "return_statement" ||
         kind == "if_statement" || kind == "for_statement" ||
         kind == "for_range_loop" || kind == "while_statement" ||
         kind == "do_statement" || kind == "switch_statement" ||
         kind == "throw_statement" || kind == "declaration" ||
         kind == "compound_statement" || kind == "block" ||
         kind == "statement_block" || kind == "preproc_include" ||
         kind == "import_statement" || kind == "import_from_statement" ||
         kind == "import_declaration" || kind == "import_spec" ||
         kind == "local_variable_declaration";
}

bool is_expression_kind(const std::string &kind) {
  return kind == "call_expression" || kind == "call" ||
         kind == "method_invocation" || kind == "binary_expression" ||
         kind == "binary_operator" || kind == "unary_expression" ||
         kind == "unary_operator" || kind == "update_expression" ||
         kind == "assignment_expression" || kind == "conditional_expression" ||
         kind == "ternary_expression" || kind == "parenthesized_expression" ||
         kind == "field_expression" || kind == "member_expression" ||
         kind == "selector_expression" || kind == "attribute" ||
         kind == "subscript_expression" || kind == "subscript" ||
         kind == "cast_expression" || kind == "new_expression" ||
         kind == "object_creation_expression" || kind == "identifier" ||
         kind == "number_literal" || kind == "integer" || kind == "float" ||
         kind == "string_literal" || kind == "string" || kind == "null" ||
         kind == "true" || kind == "false" || kind == "boolean_operator" ||
         kind == "comparison_operator" || kind == "pointer_expression" ||
         kind == "await_expression" || kind == "yield_expression" ||
         kind == "template_function" || kind == "qualified_identifier";
}

bool is_text_leaf_kind(Grammar grammar, const std::string &kind) {
  if (kind == "string_literal" || kind == "char_literal" || kind == "character_literal" ||
      kind == "concatenated_string" || kind == "system_lib_string" ||
      kind == "raw_string_literal" || kind == "user_defined_literal" ||
      kind == "string" || kind == "interpreted_string_literal" ||
      kind == "template_string" || kind == "regex")
    return true;
  (void)grammar;
  return false;
}

std::string trim_copy(const std::string &s) {
  size_t a = 0;
  while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
  size_t b = s.size();
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
  return s.substr(a, b - a);
}

bool starts_with_any(const std::string &s, std::initializer_list<const char *> prefixes) {
  for (const char *p : prefixes) {
    const size_t n = std::strlen(p);
    if (s.compare(0, n, p) == 0 &&
        (s.size() == n || !ident_char(static_cast<unsigned char>(s[n])) || !ident_start(static_cast<unsigned char>(p[n - 1]))))
      return true;
  }
  return false;
}

struct SubResult {
  std::string text;
  std::vector<std::string> scalars;
  std::vector<std::string> variadics;
};

// Replaces $NAME -> __lmok_mv_NAME, $...NAME -> __lmok_var_NAME, and standalone ... -> __lmok_ellipsis
// outside string literals, char literals, raw string literals, and comments.
SubResult substitute_pattern_tokens(Grammar grammar, const std::string &pattern) {
  SubResult out;
  std::set<std::string> seen_scalar, seen_var;
  const bool hash_comment = (grammar == Grammar::Python);
  const bool raw_r_string = (grammar == Grammar::Cpp || grammar == Grammar::C || grammar == Grammar::Python);
  const bool backtick_str = (grammar == Grammar::Javascript || grammar == Grammar::Typescript ||
                             grammar == Grammar::Tsx || grammar == Grammar::Go);

  for (size_t i = 0; i < pattern.size();) {
    const char c = pattern[i];
    // Comments
    if (hash_comment && c == '#') {
      while (i < pattern.size() && pattern[i] != '\n') out.text += pattern[i++];
      continue;
    }
    if (!hash_comment && c == '/' && i + 1 < pattern.size() && pattern[i + 1] == '/') {
      while (i < pattern.size() && pattern[i] != '\n') out.text += pattern[i++];
      continue;
    }
    if (!hash_comment && c == '/' && i + 1 < pattern.size() && pattern[i + 1] == '*') {
      out.text += "/*";
      i += 2;
      while (i < pattern.size()) {
        if (pattern[i] == '*' && i + 1 < pattern.size() && pattern[i + 1] == '/') {
          out.text += "*/";
          i += 2;
          break;
        }
        out.text += pattern[i++];
      }
      continue;
    }
    // C++ raw string literal R"delim(...)delim"
    if (raw_r_string && grammar == Grammar::Cpp && c == 'R' && i + 1 < pattern.size() && pattern[i + 1] == '"') {
      size_t paren = pattern.find('(', i + 2);
      if (paren != std::string::npos && paren - (i + 2) <= 16) {
        std::string delim = pattern.substr(i + 2, paren - (i + 2));
        std::string end_seq = ")" + delim + "\"";
        size_t close = pattern.find(end_seq, paren + 1);
        size_t stop = (close == std::string::npos) ? pattern.size() : close + end_seq.size();
        out.text.append(pattern, i, stop - i);
        i = stop;
        continue;
      }
    }
    // Quoted strings & chars (and triple-quoted Python strings)
    if (c == '"' || c == '\'' || (backtick_str && c == '`')) {
      const char q = c;
      if (grammar == Grammar::Python && i + 2 < pattern.size() && pattern[i + 1] == q && pattern[i + 2] == q) {
        out.text.append(pattern, i, 3);
        i += 3;
        while (i < pattern.size()) {
          if (pattern[i] == '\\' && i + 1 < pattern.size()) {
            out.text += pattern[i++];
            out.text += pattern[i++];
          } else if (i + 2 < pattern.size() && pattern[i] == q && pattern[i + 1] == q && pattern[i + 2] == q) {
            out.text.append(3, q);
            i += 3;
            break;
          } else {
            out.text += pattern[i++];
          }
        }
        continue;
      }
      out.text += pattern[i++];
      while (i < pattern.size()) {
        if (pattern[i] == '\\' && q != '`' && i + 1 < pattern.size()) {
          out.text += pattern[i++];
          out.text += pattern[i++];
        } else if (pattern[i] == q) {
          out.text += pattern[i++];
          break;
        } else {
          out.text += pattern[i++];
        }
      }
      continue;
    }
    // Escaped literal $$ -> $
    if (c == '$' && i + 1 < pattern.size() && pattern[i + 1] == '$') {
      out.text += '$';
      i += 2;
      continue;
    }
    // Named variadic $...NAME
    if (pattern.compare(i, 4, "$...") == 0 && i + 4 < pattern.size() &&
        ident_start(static_cast<unsigned char>(pattern[i + 4]))) {
      size_t j = i + 4;
      while (j < pattern.size() && ident_char(static_cast<unsigned char>(pattern[j]))) ++j;
      const std::string name = pattern.substr(i + 4, j - (i + 4));
      if (seen_scalar.count(name))
        throw PatternError("pattern '" + pattern + "': '$" + name + "' cannot be both scalar and variadic");
      if (seen_var.insert(name).second) out.variadics.push_back(name);
      out.text += kVarPrefix + name;
      i = j;
      continue;
    }
    // Scalar $NAME
    if (c == '$' && i + 1 < pattern.size() && ident_start(static_cast<unsigned char>(pattern[i + 1]))) {
      size_t j = i + 1;
      while (j < pattern.size() && ident_char(static_cast<unsigned char>(pattern[j]))) ++j;
      const std::string name = pattern.substr(i + 1, j - (i + 1));
      if (seen_var.count(name))
        throw PatternError("pattern '" + pattern + "': '$" + name + "' cannot be both scalar and variadic");
      if (seen_scalar.insert(name).second) out.scalars.push_back(name);
      out.text += kMetaPrefix + name;
      i = j;
      continue;
    }
    // Anonymous standalone `...` (do not consume Go `xs...` or JS `...xs`)
    if (pattern.compare(i, 3, "...") == 0) {
      const bool prev_id = i > 0 && ident_char(static_cast<unsigned char>(pattern[i - 1]));
      const bool next_id = i + 3 < pattern.size() && ident_char(static_cast<unsigned char>(pattern[i + 3]));
      if (!prev_id && !next_id) {
        out.text += kEllipsis;
        i += 3;
        continue;
      }
    }
    out.text += pattern[i++];
  }
  return out;
}

// Placeholder inspection: direct leaf or single-child equal-span wrapper (not `( $X )`).
bool placeholder_kind_name(TSNode n, const std::string &src, bool &is_var, bool &is_ell, std::string &name) {
  is_var = false;
  is_ell = false;
  TSNode cur = n;
  for (int depth = 0; depth < 3; ++depth) {
    if (ts_node_named_child_count(cur) == 0) {
      const std::string t = slice(src, cur);
      if (t == kEllipsis) { is_ell = true; return true; }
      if (t.compare(0, std::strlen(kMetaPrefix), kMetaPrefix) == 0) {
        name = t.substr(std::strlen(kMetaPrefix));
        return true;
      }
      if (t.compare(0, std::strlen(kVarPrefix), kVarPrefix) == 0) {
        is_var = true;
        name = t.substr(std::strlen(kVarPrefix));
        return true;
      }
      return false;
    }
    const char *tk = ts_node_type(cur);
    if (ts_node_named_child_count(cur) == 1 && is_transparent_wrapper(tk)) {
      TSNode ch = ts_node_named_child(cur, 0);
      if (std::strcmp(tk, "expression_statement") == 0 ||
          (ts_node_start_byte(ch) == ts_node_start_byte(cur) &&
           ts_node_end_byte(ch) == ts_node_end_byte(cur))) {
        cur = ch;
        continue;
      }
    }
    return false;
  }
  return false;
}

bool contains_placeholder(TSNode n, const std::string &src) {
  bool iv = false, ie = false;
  std::string nm;
  if (placeholder_kind_name(n, src, iv, ie, nm)) return true;
  const uint32_t c = ts_node_child_count(n);
  for (uint32_t i = 0; i < c; ++i)
    if (contains_placeholder(ts_node_child(n, i), src)) return true;
  return false;
}

std::vector<TSNode> non_comment_children(TSNode n) {
  std::vector<TSNode> kids;
  const uint32_t c = ts_node_child_count(n);
  kids.reserve(c);
  for (uint32_t i = 0; i < c; ++i) {
    TSNode k = ts_node_child(n, i);
    if (!is_comment(k)) kids.push_back(k);
  }
  return kids;
}

std::vector<TSNode> non_comment_named_children(TSNode n) {
  std::vector<TSNode> kids;
  const uint32_t c = ts_node_named_child_count(n);
  kids.reserve(c);
  for (uint32_t i = 0; i < c; ++i) {
    TSNode k = ts_node_named_child(n, i);
    if (!is_comment(k)) kids.push_back(k);
  }
  return kids;
}

// Validate variadic usage: only allowed inside sequence containers, at most one per container,
// and confirm no placeholder leaked into a string or comment node.
void validate_pattern_tree(TSNode n, const std::string &src, const std::string &raw_pat) {
  const std::string kind = ts_node_type(n);
  if (is_comment_kind(kind) || kind.find("string") != std::string::npos) {
    const std::string txt = slice(src, n);
    if (txt.find(kMetaPrefix) != std::string::npos ||
        txt.find(kVarPrefix) != std::string::npos ||
        txt.find(kEllipsis) != std::string::npos) {
      throw PatternError("pattern '" + raw_pat + "': placeholder inside literal or comment");
    }
  }
  const auto named = non_comment_named_children(n);
  int variadics_here = 0;
  for (TSNode ch : named) {
    bool iv = false, ie = false;
    std::string nm;
    if (placeholder_kind_name(ch, src, iv, ie, nm)) {
      if (iv || ie) {
        ++variadics_here;
        if (!is_sequence_container(kind)) {
          throw PatternError("pattern '" + raw_pat + "': variadic ellipsis is only supported inside argument/parameter/template/initializer/array/block lists");
        }
        if (variadics_here > 1) {
          throw PatternError("pattern '" + raw_pat + "': at most one variadic ellipsis per list");
        }
      }
      continue;
    }
    validate_pattern_tree(ch, src, raw_pat);
  }
}

std::string find_head_identifier(TSNode n, const std::string &src) {
  const std::string kind = ts_node_type(n);
  if (kind == "call_expression" || kind == "call") {
    TSNode fn = ts_node_child_by_field_name(n, "function", 8);
    if (!ts_node_is_null(fn)) {
      const std::string fk = ts_node_type(fn);
      if (fk == "identifier") {
        std::string t = slice(src, fn);
        if (t.compare(0, std::strlen(kMetaPrefix), kMetaPrefix) != 0) return t;
      } else if (fk == "field_expression" || fk == "member_expression" || fk == "selector_expression" || fk == "attribute") {
        const uint32_t nc = ts_node_named_child_count(fn);
        if (nc > 0) {
          std::string t = slice(src, ts_node_named_child(fn, nc - 1));
          if (t.compare(0, std::strlen(kMetaPrefix), kMetaPrefix) != 0) return t;
        }
      }
    }
  } else if (kind == "method_invocation") {
    TSNode nm = ts_node_child_by_field_name(n, "name", 4);
    if (!ts_node_is_null(nm)) return slice(src, nm);
  }
  return "";
}

struct WrapperCandidate {
  std::string code;
  bool wrapped_in_fn = false;
  bool wrapped_in_class = false;
  bool wrapped_in_ns = false;
};

std::vector<WrapperCandidate> wrappers_for(Grammar g, const std::string &sub) {
  const std::string t = trim_copy(sub);
  std::vector<WrapperCandidate> cands;
  if (g == Grammar::C || g == Grammar::Cpp) {
    const bool file_level = starts_with_any(t, {"#", "namespace", "template", "class", "struct", "union", "enum", "using", "typedef", "extern"});
    if (file_level) {
      cands.push_back({sub, false, false, false});
      cands.push_back({sub + ";", false, false, false});
    }
    // For C++ expressions/calls, try inside function body FIRST to avoid the most-vexing-parse!
    cands.push_back({std::string(g == Grammar::C ? "void __lmok_w(void) {" : "void __lmok_w() {") + sub + "}", true, false, false});
    cands.push_back({std::string(g == Grammar::C ? "void __lmok_w(void) {" : "void __lmok_w() {") + sub + ";}", true, false, false});
    if (!file_level) {
      cands.push_back({sub, false, false, false});
      cands.push_back({sub + ";", false, false, false});
    }
    if (g == Grammar::Cpp) {
      cands.push_back({"class __LmokW { public: " + sub + " };", false, true, false});
      cands.push_back({"class __LmokW { public: " + sub + "; };", false, true, false});
    }
  } else if (g == Grammar::Python) {
    cands.push_back({sub, false, false, false});
    cands.push_back({"class __LmokW:\n    " + sub, false, true, false});
  } else if (g == Grammar::Java) {
    const bool file_level = starts_with_any(t, {"import", "package", "public", "class", "interface", "enum", "@interface"});
    if (file_level) cands.push_back({sub, false, false, false});
    cands.push_back({"class __LmokW { void __w() { " + sub + "; } }", true, true, false});
    cands.push_back({"class __LmokW { void __w() { " + sub + " } }", true, true, false});
    cands.push_back({"class __LmokW { " + sub + " }", false, true, false});
    if (!file_level) cands.push_back({sub, false, false, false});
  } else if (g == Grammar::Go) {
    const bool file_level = starts_with_any(t, {"package", "import", "func", "type", "var", "const"});
    if (file_level) cands.push_back({"package __p\n" + sub, false, false, true});
    cands.push_back({"package __p\nfunc __w() {\n" + sub + "\n}", true, false, true});
    if (!file_level) cands.push_back({"package __p\n" + sub, false, false, true});
  } else if (g == Grammar::Javascript || g == Grammar::Typescript || g == Grammar::Tsx) {
    cands.push_back({sub, false, false, false});
    cands.push_back({"class __LmokW { " + sub + " }", false, true, false});
    if (g == Grammar::Tsx) cands.push_back({"<>{" + sub + "}</>", false, false, false});
  }
  return cands;
}

// Walk down named children recording path from root(tree) to pattern root.
std::pair<TSNode, std::vector<int64_t>> unwrap_pattern_root(
    const Tree &tree, const WrapperCandidate &w, bool keep_stmt_semi) {
  TSNode cur = ts_of(node_root(tree));
  std::vector<int64_t> path;
  auto step_child = [&](uint32_t idx) {
    path.push_back(static_cast<int64_t>(idx));
    cur = ts_node_named_child(cur, idx);
  };

  if (w.wrapped_in_ns) {
    // Go: source_file -> skip package_clause (0), pick declaration 1 if single
    std::vector<uint32_t> idxs;
    for (uint32_t i = 0; i < ts_node_named_child_count(cur); ++i) {
      TSNode ch = ts_node_named_child(cur, i);
      if (std::string(ts_node_type(ch)) == "package_clause" || is_comment(ch)) continue;
      idxs.push_back(i);
    }
    if (idxs.size() != 1) throw PatternError("one statement or expression per pattern");
    step_child(idxs[0]);
    if (w.wrapped_in_fn && std::string(ts_node_type(cur)) == "function_declaration") {
      uint32_t body_idx = UINT32_MAX;
      for (uint32_t i = 0; i < ts_node_named_child_count(cur); ++i) {
        if (std::string(ts_node_type(ts_node_named_child(cur, i))) == "block") { body_idx = i; break; }
      }
      if (body_idx == UINT32_MAX) throw PatternError("pattern wrapper malformed");
      step_child(body_idx);
      std::vector<uint32_t> b_idxs;
      for (uint32_t i = 0; i < ts_node_named_child_count(cur); ++i) {
        TSNode ch = ts_node_named_child(cur, i);
        if (std::string(ts_node_type(ch)) == "statement_list") {
          step_child(i);
          break;
        }
      }
      for (uint32_t i = 0; i < ts_node_named_child_count(cur); ++i)
        if (!is_comment(ts_node_named_child(cur, i))) b_idxs.push_back(i);
      if (b_idxs.size() != 1) throw PatternError("one statement or expression per pattern");
      step_child(b_idxs[0]);
    }
  } else if (w.wrapped_in_class) {
    // First named declaration is wrapper class
    if (ts_node_named_child_count(cur) != 1) throw PatternError("one statement or member per pattern");
    step_child(0);
    bool found_body = false;
    for (uint32_t i = 0; i < ts_node_named_child_count(cur); ++i) {
      const std::string k = ts_node_type(ts_node_named_child(cur, i));
      if (k == "field_declaration_list" || k == "class_body" || k == "block") {
        step_child(i);
        found_body = true;
        break;
      }
    }
    if (!found_body) throw PatternError("pattern class wrapper malformed");
    if (w.wrapped_in_fn) {
      // Java class -> class_body -> method_declaration -> block
      step_child(0);
      for (uint32_t i = 0; i < ts_node_named_child_count(cur); ++i) {
        if (std::string(ts_node_type(ts_node_named_child(cur, i))) == "block") {
          step_child(i);
          break;
        }
      }
    }
    std::vector<uint32_t> kids;
    for (uint32_t i = 0; i < ts_node_named_child_count(cur); ++i) {
      TSNode ch = ts_node_named_child(cur, i);
      const std::string ck = ts_node_type(ch);
      if (is_comment(ch) || ck == "access_specifier") continue;
      kids.push_back(i);
    }
    if (kids.size() != 1) throw PatternError("one statement or declaration per pattern");
    step_child(kids[0]);
  } else if (w.wrapped_in_fn) {
    // C/C++: translation_unit -> function_definition -> compound_statement -> child
    if (ts_node_named_child_count(cur) != 1) throw PatternError("one statement or expression per pattern");
    step_child(0);
    for (uint32_t i = 0; i < ts_node_named_child_count(cur); ++i) {
      const std::string k = ts_node_type(ts_node_named_child(cur, i));
      if (k == "compound_statement" || k == "block") { step_child(i); break; }
    }
    std::vector<uint32_t> kids;
    for (uint32_t i = 0; i < ts_node_named_child_count(cur); ++i)
      if (!is_comment(ts_node_named_child(cur, i))) kids.push_back(i);
    if (kids.empty()) throw PatternError("pattern is empty");
    if (kids.size() != 1) throw PatternError("one statement or expression per pattern");
    step_child(kids[0]);
  } else {
    // Direct file-level parse
    std::vector<uint32_t> kids;
    for (uint32_t i = 0; i < ts_node_named_child_count(cur); ++i)
      if (!is_comment(ts_node_named_child(cur, i))) kids.push_back(i);
    if (kids.empty()) throw PatternError("pattern is empty");
    if (kids.size() != 1) throw PatternError("one statement or expression per pattern");
    step_child(kids[0]);
  }

  // Unwrap expression_statement unless pattern explicitly ended with `;`
  if (!keep_stmt_semi && std::string(ts_node_type(cur)) == "expression_statement") {
    std::vector<uint32_t> ek;
    for (uint32_t i = 0; i < ts_node_named_child_count(cur); ++i)
      if (!is_comment(ts_node_named_child(cur, i))) ek.push_back(i);
    if (ek.size() == 1) step_child(ek[0]);
  }
  return {cur, path};
}

Node resolve_pat_root(const RewritePattern &pat) {
  Node cur = node_root(*pat.parsed);
  for (int64_t idx : pat.root_path) cur = node_child(cur, idx);
  return cur;
}

struct UnifyState {
  Grammar grammar;
  const std::string &pat_src;
  const std::string &tgt_src;
  std::map<std::string, RewriteCapture> bindings;
  bool unowned_comment = false;

  bool has_comments_outside(TSNode n, const std::vector<std::pair<int64_t, int64_t>> &covered) const {
    if (is_comment(n)) {
      const int64_t a = ts_node_start_byte(n), b = ts_node_end_byte(n);
      for (const auto &r : covered)
        if (a >= r.first && b <= r.second) return false;
      return true;
    }
    const uint32_t count = ts_node_child_count(n);
    for (uint32_t i = 0; i < count; ++i)
      if (has_comments_outside(ts_node_child(n, i), covered)) return true;
    return false;
  }

  // Unify container sequence containing at most one variadic placeholder.
  bool unify_sequence(TSNode pat_container, TSNode tgt_container) {
    const auto p_named = non_comment_named_children(pat_container);
    const auto t_named = non_comment_named_children(tgt_container);
    size_t vidx = p_named.size();
    bool is_var = false, is_ell = false;
    std::string var_name;
    for (size_t i = 0; i < p_named.size(); ++i) {
      bool v = false, e = false;
      std::string nm;
      if (placeholder_kind_name(p_named[i], pat_src, v, e, nm) && (v || e)) {
        vidx = i;
        is_var = v;
        is_ell = e;
        var_name = nm;
        break;
      }
    }
    if (vidx == p_named.size()) return false;
    const size_t prefix = vidx;
    const size_t suffix = p_named.size() - 1 - vidx;
    if (t_named.size() < prefix + suffix) return false;

    for (size_t i = 0; i < prefix; ++i)
      if (!unify(p_named[i], t_named[i])) return false;
    for (size_t i = 0; i < suffix; ++i)
      if (!unify(p_named[p_named.size() - 1 - i], t_named[t_named.size() - 1 - i])) return false;

    const size_t t_mid_begin = prefix;
    const size_t t_mid_end = t_named.size() - suffix;
    if (is_var) {
      RewriteCapture cap;
      cap.name = var_name;
      cap.variadic = true;
      if (t_mid_begin < t_mid_end) {
        cap.start_byte = ts_node_start_byte(t_named[t_mid_begin]);
        cap.end_byte = ts_node_end_byte(t_named[t_mid_end - 1]);
        cap.text = tgt_src.substr(static_cast<size_t>(cap.start_byte),
                                  static_cast<size_t>(cap.end_byte - cap.start_byte));
        cap.empty = false;
      } else {
        cap.empty = true;
        cap.start_byte = ts_node_start_byte(tgt_container);
        cap.end_byte = cap.start_byte;
      }
      auto prev = bindings.find(var_name);
      if (prev != bindings.end()) {
        if (prev->second.text != cap.text || prev->second.empty != cap.empty) return false;
      } else {
        bindings.emplace(var_name, std::move(cap));
      }
    } else if (!is_ell) {
      return false;
    }
    return true;
  }

  bool container_has_variadic(TSNode pat_node) const {
    if (!is_sequence_container(ts_node_type(pat_node))) return false;
    const auto p_named = non_comment_named_children(pat_node);
    for (TSNode ch : p_named) {
      bool v = false, e = false;
      std::string nm;
      if (placeholder_kind_name(ch, pat_src, v, e, nm) && (v || e)) return true;
    }
    return false;
  }

  bool unify(TSNode pat, TSNode tgt) {
    if (ts_node_is_null(pat) || ts_node_is_null(tgt)) return false;
    if (is_comment(tgt) || ts_node_is_error(tgt) || ts_node_is_missing(tgt)) return false;

    bool is_var = false, is_ell = false;
    std::string mv_name;
    if (placeholder_kind_name(pat, pat_src, is_var, is_ell, mv_name) && !is_var && !is_ell) {
      if (!ts_node_is_named(tgt)) return false;
      RewriteCapture cap;
      cap.name = mv_name;
      cap.variadic = false;
      cap.start_byte = ts_node_start_byte(tgt);
      cap.end_byte = ts_node_end_byte(tgt);
      cap.text = slice(tgt_src, tgt);
      cap.is_expression = is_expression_kind(ts_node_type(tgt));
      auto prev = bindings.find(mv_name);
      if (prev != bindings.end()) {
        return !prev->second.variadic && prev->second.text == cap.text;
      }
      bindings.emplace(mv_name, std::move(cap));
      return true;
    }

    // Transparent equal-span unwrap if one side wrapped a placeholder in type_descriptor etc.
    if (std::string(ts_node_type(pat)) != std::string(ts_node_type(tgt))) {
      if (ts_node_named_child_count(pat) == 1 && is_transparent_wrapper(ts_node_type(pat))) {
        TSNode pc = ts_node_named_child(pat, 0);
        if (ts_node_start_byte(pc) == ts_node_start_byte(pat) &&
            ts_node_end_byte(pc) == ts_node_end_byte(pat))
          return unify(pc, tgt);
      }
      if (ts_node_named_child_count(tgt) == 1 && is_transparent_wrapper(ts_node_type(tgt))) {
        TSNode tc = ts_node_named_child(tgt, 0);
        if (ts_node_start_byte(tc) == ts_node_start_byte(tgt) &&
            ts_node_end_byte(tc) == ts_node_end_byte(tgt))
          return unify(pat, tc);
      }
      return false;
    }

    const std::string pkind = ts_node_type(pat);
    if (is_text_leaf_kind(grammar, pkind) || ts_node_named_child_count(pat) == 0) {
      if (ts_node_named_child_count(tgt) != 0 && !is_text_leaf_kind(grammar, pkind)) return false;
      // Whitespace-insensitive match for empty argument/parameter lists like `f()` vs `f( )`
      if (is_sequence_container(pkind))
        return non_comment_named_children(tgt).empty();
      return slice(pat_src, pat) == slice(tgt_src, tgt);
    }

    if (container_has_variadic(pat)) return unify_sequence(pat, tgt);

    const auto pkids = non_comment_children(pat);
    const auto tkids = non_comment_children(tgt);
    if (pkids.size() != tkids.size()) return false;
    for (size_t i = 0; i < pkids.size(); ++i) {
      TSNode pk = pkids[i], tk = tkids[i];
      if (!ts_node_is_named(pk) || !ts_node_is_named(tk)) {
        if (ts_node_is_named(pk) != ts_node_is_named(tk)) return false;
        if (slice(pat_src, pk) != slice(tgt_src, tk)) return false;
      } else {
        if (!unify(pk, tk)) return false;
      }
    }
    return true;
  }
};

// Detects comments in the matched subtree outside every captured metavariable/variadic slice.
bool match_drops_unowned_comment(TSNode tgt, const std::map<std::string, RewriteCapture> &bindings) {
  std::vector<std::pair<int64_t, int64_t>> covered;
  for (const auto &kv : bindings)
    if (!kv.second.empty) covered.emplace_back(kv.second.start_byte, kv.second.end_byte);
  std::function<bool(TSNode)> check = [&](TSNode n) {
    if (is_comment(n)) {
      const int64_t a = ts_node_start_byte(n), b = ts_node_end_byte(n);
      for (const auto &r : covered)
        if (a >= r.first && b <= r.second) return false;
      return true;
    }
    const uint32_t c = ts_node_child_count(n);
    for (uint32_t i = 0; i < c; ++i)
      if (check(ts_node_child(n, i))) return true;
    return false;
  };
  return check(tgt);
}

// Per-line EOL string (`\r\n` or `\n`) containing byte offset `pos`.
std::string eol_at(const std::string &src, int64_t pos) {
  const size_t p = std::min<size_t>(static_cast<size_t>(std::max<int64_t>(0, pos)), src.size());
  size_t nl = src.find('\n', p);
  if (nl != std::string::npos && nl > 0 && src[nl - 1] == '\r') return "\r\n";
  if (p > 0) {
    size_t prev_nl = src.rfind('\n', p - 1);
    if (prev_nl != std::string::npos && prev_nl > 0 && src[prev_nl - 1] == '\r') return "\r\n";
  }
  return "\n";
}

std::string normalize_newlines_to(const std::string &s, const std::string &eol) {
  if (eol == "\n") return s;
  std::string out;
  out.reserve(s.size() + 8);
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) out += '\r';
    out += s[i];
  }
  return out;
}

// Smart comma elision around an empty variadic expansion `$...NAME`.
void elide_empty_variadic_comma(std::string &out, size_t var_start) {
  // Look backward past horizontal spaces/tabs for `,`
  size_t b = out.size();
  while (b > 0 && (out[b - 1] == ' ' || out[b - 1] == '\t')) --b;
  if (b > 0 && out[b - 1] == ',') {
    out.erase(b - 1); // drop `,   ` before empty variadic
    return;
  }
  (void)var_start;
}

std::string render_template_for_match(
    const CompiledTemplate &tmpl,
    const RewriteMatch &m,
    const std::map<std::string, std::string> &override_texts,
    const std::string &target_eol,
    const std::set<size_t> &wrap_part_indices,
    bool wrap_whole_outer,
    std::vector<PlannedEdit::ExprSpan> *out_expr_spans,
    bool *out_used_parens) {
  if (tmpl.is_delete) return "";
  std::map<std::string, const RewriteCapture *> cap_map;
  for (const auto &c : m.bindings) cap_map[c.name] = &c;

  std::string out;
  bool skip_leading_comma = false;
  if (out_used_parens) *out_used_parens = wrap_whole_outer;
  if (out_expr_spans) out_expr_spans->clear();

  for (size_t idx = 0; idx < tmpl.parts.size(); ++idx) {
    const auto &part = tmpl.parts[idx];
    if (part.kind == TemplatePart::Kind::Literal) {
      std::string lit = normalize_newlines_to(part.text, target_eol);
      if (skip_leading_comma) {
        size_t i = 0;
        while (i < lit.size() && (lit[i] == ' ' || lit[i] == '\t')) ++i;
        if (i < lit.size() && lit[i] == ',') {
          ++i;
          while (i < lit.size() && (lit[i] == ' ' || lit[i] == '\t')) ++i;
          lit.erase(0, i);
        }
        skip_leading_comma = false;
      }
      out += lit;
    } else if (part.kind == TemplatePart::Kind::Meta) {
      skip_leading_comma = false;
      const auto it = override_texts.find(part.text);
      const RewriteCapture *cap = cap_map.at(part.text);
      std::string val = (it != override_texts.end()) ? it->second : cap->text;
      const bool wrap = wrap_part_indices.count(idx) != 0;
      if (wrap) {
        val = "(" + val + ")";
        if (out_used_parens) *out_used_parens = true;
      }
      const int64_t rel_s = static_cast<int64_t>(out.size()) + (wrap ? 1 : 0);
      out += val;
      const int64_t rel_e = static_cast<int64_t>(out.size()) - (wrap ? 1 : 0);
      if (out_expr_spans && cap->is_expression && !part.parenthesized) {
        // Track non-atomic or composite expressions (always safe to track any expr capture)
        out_expr_spans->push_back({rel_s, rel_e, wrap, part.text});
      }
    } else {
      const auto it = override_texts.find(part.text);
      const RewriteCapture *cap = cap_map.at(part.text);
      const bool empty = (it == override_texts.end()) && cap->empty;
      if (empty) {
        // Check if preceding literal in `out` ended with `,` (with optional spaces)
        size_t b = out.size();
        while (b > 0 && (out[b - 1] == ' ' || out[b - 1] == '\t')) --b;
        if (b > 0 && out[b - 1] == ',') {
          elide_empty_variadic_comma(out, out.size());
        } else {
          // Otherwise elide a leading `, ` on the following literal part (`$...ARGS, x` -> `x`)
          skip_leading_comma = true;
        }
      } else {
        skip_leading_comma = false;
        out += (it != override_texts.end()) ? it->second : cap->text;
      }
    }
  }
  if (wrap_whole_outer) {
    if (out_expr_spans) {
      for (auto &es : *out_expr_spans) { es.rel_start += 1; es.rel_end += 1; }
    }
    out = "(" + out + ")";
  }
  return out;
}

// Outside-frame invariance & error-budget preservation check.
struct NodeSig {
  int64_t start = 0, end = 0;
  std::string kind;
  bool named = false, error = false, missing = false;
  std::string leaf_text;
  bool operator==(const NodeSig &o) const {
    return start == o.start && end == o.end && named == o.named &&
           error == o.error && missing == o.missing && kind == o.kind && leaf_text == o.leaf_text;
  }
};

int64_t shift_offset(int64_t byte, const std::vector<PlannedEdit> &edits) {
  int64_t delta = 0;
  for (const auto &e : edits) {
    if (e.end_byte <= byte) delta += static_cast<int64_t>(e.after.size()) - (e.end_byte - e.start_byte);
  }
  return byte + delta;
}

bool overlaps_any_old(int64_t a, int64_t b, const std::vector<PlannedEdit> &edits) {
  for (const auto &e : edits)
    if (a < e.end_byte && e.start_byte < b) return true;
  return false;
}

bool straddles_any_new(int64_t a, int64_t b, const std::vector<std::pair<int64_t, int64_t>> &new_spans) {
  for (const auto &s : new_spans) {
    if ((a < s.first && s.first < b && b < s.second) ||
        (s.first < a && a < s.second && s.second < b))
      return true;
  }
  return false;
}

std::vector<NodeSig> outside_signatures(const Tree &tree,
                                        const std::vector<PlannedEdit> &edits,
                                        const std::vector<std::pair<int64_t, int64_t>> *shift_spans,
                                        bool is_old) {
  std::vector<NodeSig> sigs;
  const auto &src = tree.source();
  walk_nodes(tree, [&](const Node &n, const std::vector<Node> &) {
    TSNode tn = ts_of(n);
    const int64_t a = ts_node_start_byte(tn), b = ts_node_end_byte(tn);
    if (is_old) {
      if (overlaps_any_old(a, b, edits)) return;
      NodeSig s;
      s.start = shift_offset(a, edits);
      s.end = shift_offset(b, edits);
      s.kind = ts_node_type(tn);
      s.named = ts_node_is_named(tn);
      s.error = ts_node_is_error(tn);
      s.missing = ts_node_is_missing(tn);
      if (ts_node_child_count(tn) == 0 && b > a) s.leaf_text = slice(src, tn);
      sigs.push_back(std::move(s));
    } else {
      for (const auto &sp : *shift_spans)
        if (a < sp.second && sp.first < b) return;
      NodeSig s;
      s.start = a;
      s.end = b;
      s.kind = ts_node_type(tn);
      s.named = ts_node_is_named(tn);
      s.error = ts_node_is_error(tn);
      s.missing = ts_node_is_missing(tn);
      if (ts_node_child_count(tn) == 0 && b > a) s.leaf_text = slice(src, tn);
      sigs.push_back(std::move(s));
    }
  });
  return sigs;
}

bool has_exact_span_node(const Tree &tree, int64_t start, int64_t end) {
  bool found = false;
  walk_nodes(tree, [&](const Node &n, const std::vector<Node> &) {
    if (found) return;
    if (node_start_byte(n) == start && node_end_byte(n) == end) {
      if (!node_is_error(n) && !node_is_missing(n)) found = true;
    }
  });
  return found;
}

bool span_contains_error(const Tree &tree, int64_t start, int64_t end) {
  bool err = false;
  walk_nodes(tree, [&](const Node &n, const std::vector<Node> &) {
    if (err) return;
    TSNode tn = ts_of(n);
    const int64_t a = ts_node_start_byte(tn), b = ts_node_end_byte(tn);
    if (!((a < end && start < b) || (start == end && a <= start && start <= b))) return;
    if (ts_node_is_missing(tn)) { err = true; return; }
    if (!ts_node_is_error(tn)) return;
    // Allow edits wholly inside a clean recovered child subtree of an outer ERROR wrapper.
    if (a <= start && end <= b) {
      const uint32_t nc = ts_node_named_child_count(tn);
      for (uint32_t i = 0; i < nc; ++i) {
        TSNode ch = ts_node_named_child(tn, i);
        const int64_t ca = ts_node_start_byte(ch), cb = ts_node_end_byte(ch);
        if (ca <= start && end <= cb && !ts_node_has_error(ch) && !ts_node_is_missing(ch))
          return;
      }
    }
    err = true;
  });
  return err;
}

// Verify edited zone is locally clean, frame outside edits is preserved, and no edit introduced errors.
void check_tree_safety(const Tree &old_tree, const Tree &new_tree,
                       const std::vector<PlannedEdit> &edits,
                       bool strict_parse,
                       std::vector<std::pair<int64_t, int64_t>> *out_new_spans = nullptr) {
  if (strict_parse) {
    if (tree_has_error(old_tree)) throw RefactorError(3, "refused under --strict-parse: input contains parse errors");
    if (tree_has_error(new_tree)) throw RefactorError(3, "refused under --strict-parse: replacement introduces parse errors");
  }

  std::vector<std::pair<int64_t, int64_t>> new_spans;
  new_spans.reserve(edits.size());
  for (const auto &e : edits) {
    const int64_t ns = shift_offset(e.start_byte, edits) - (static_cast<int64_t>(e.after.size()) - (e.end_byte - e.start_byte));
    // Actually shift_offset counts edits BEFORE e (e.end_byte <= e.start_byte), so:
    int64_t acc = 0;
    for (const auto &prev : edits) {
      if (&prev == &e) break;
      acc += static_cast<int64_t>(prev.after.size()) - (prev.end_byte - prev.start_byte);
    }
    const int64_t rs = e.start_byte + acc;
    const int64_t re = rs + static_cast<int64_t>(e.after.size());
    (void)ns;
    new_spans.emplace_back(rs, re);
    if (span_contains_error(old_tree, e.start_byte, e.end_byte))
      throw RefactorError(3, "refactor refused: target site at line " + std::to_string(e.row + 1) + " contains parse errors");
    if (span_contains_error(new_tree, rs, re))
      throw RefactorError(3, "refactor refused: replacement at line " + std::to_string(e.row + 1) + " introduces syntax error");
  }

  // Reject boundary straddling in new_tree
  walk_nodes(new_tree, [&](const Node &n, const std::vector<Node> &) {
    if (straddles_any_new(node_start_byte(n), node_end_byte(n), new_spans))
      throw RefactorError(3, "refactor refused: replacement breaks enclosing syntax boundary");
  });

  // Outside-frame shifted invariance (subsumes error-budget preservation outside edits)
  const auto old_sigs = outside_signatures(old_tree, edits, nullptr, true);
  const auto new_sigs = outside_signatures(new_tree, edits, &new_spans, false);
  if (old_sigs.size() != new_sigs.size())
    throw RefactorError(3, "refactor refused: replacement alters surrounding syntax structure");
  for (size_t i = 0; i < old_sigs.size(); ++i) {
    if (!(old_sigs[i] == new_sigs[i]))
      throw RefactorError(3, "refactor refused: replacement alters surrounding syntax or error recovery");
  }
  if (out_new_spans) *out_new_spans = std::move(new_spans);
}

// Compute enclosing standalone line range if statement deletion sits on its own line.
bool widen_line_deletion(const std::string &src, int64_t &a, int64_t &b) {
  size_t ls = static_cast<size_t>(a);
  while (ls > 0 && src[ls - 1] != '\n') --ls;
  for (size_t i = ls; i < static_cast<size_t>(a); ++i)
    if (src[i] != ' ' && src[i] != '\t') return false;
  size_t le = static_cast<size_t>(b);
  while (le < src.size() && (src[le] == ' ' || src[le] == '\t')) ++le;
  if (le < src.size() && src[le] == '\r') ++le;
  if (le < src.size() && src[le] == '\n') ++le;
  else if (le < src.size()) return false;
  a = static_cast<int64_t>(ls);
  b = static_cast<int64_t>(le);
  return true;
}

bool sole_unbraced_control_body(const Tree &tree, int64_t a, int64_t b) {
  bool unsafe = false;
  walk_nodes(tree, [&](const Node &n, const std::vector<Node> &anc) {
    if (unsafe || node_start_byte(n) != a || node_end_byte(n) != b || anc.empty()) return;
    const std::string pk = node_kind(anc.back());
    if (pk == "if_statement" || pk == "else_clause" || pk == "while_statement" ||
        pk == "for_statement" || pk == "for_range_loop") {
      unsafe = true;
    }
  });
  return unsafe;
}

} // namespace

bool refactor_pattern_supports(Grammar grammar) {
  switch (grammar) {
  case Grammar::C:
  case Grammar::Cpp:
  case Grammar::Python:
  case Grammar::Java:
  case Grammar::Go:
  case Grammar::Javascript:
  case Grammar::Typescript:
  case Grammar::Tsx:
    return true;
  default:
    return false;
  }
}

RewritePattern compile_rewrite_pattern(Grammar grammar, const std::string &pattern) {
  if (!refactor_pattern_supports(grammar))
    throw PatternError(std::string("pattern '") + pattern + "': code patterns are not supported for " + grammar_name(grammar));
  const std::string trimmed = trim_copy(pattern);
  if (trimmed.empty()) throw PatternError("pattern is empty");
  const bool keep_semi = !trimmed.empty() && trimmed.back() == ';';

  const SubResult sub = substitute_pattern_tokens(grammar, pattern);
  const auto cands = wrappers_for(grammar, sub.text);

  std::shared_ptr<Tree> best_tree;
  WrapperCandidate best_wrap;
  bool parsed_clean = false;
  for (const auto &w : cands) {
    Tree candidate = parse(grammar, w.code);
    if (!has_error_or_missing(ts_of(node_root(candidate)))) {
      best_tree = std::make_shared<Tree>(std::move(candidate));
      best_wrap = w;
      parsed_clean = true;
      break;
    }
  }
  if (!parsed_clean) {
    Tree err_tree = parse(grammar, cands.empty() ? sub.text : cands.front().code);
    TSNode err = first_error_node(ts_of(node_root(err_tree)));
    throw PatternError("pattern '" + pattern + "' does not parse as " + grammar_name(grammar) +
                       (ts_node_is_null(err) ? "" : " (syntax error at " + node_point_str(err) + ")"));
  }

  auto unwrapped = unwrap_pattern_root(*best_tree, best_wrap, keep_semi);
  validate_pattern_tree(unwrapped.first, best_tree->source(), pattern);

  RewritePattern out;
  out.grammar = grammar;
  out.raw = pattern;
  out.parsed = std::move(best_tree);
  out.root_path = std::move(unwrapped.second);
  out.scalars = sub.scalars;
  out.variadics = sub.variadics;
  const std::string rkind = ts_node_type(unwrapped.first);
  out.is_statement = is_statement_kind(rkind);
  out.is_expression = is_expression_kind(rkind);
  out.head_identifier = find_head_identifier(unwrapped.first, out.parsed->source());
  return out;
}

CompiledTemplate compile_rewrite_template(const RewritePattern &pat, const std::string &tmpl) {
  CompiledTemplate out;
  out.raw = tmpl;
  if (tmpl.empty()) {
    out.is_delete = true;
    out.drops = pat.scalars;
    out.drops.insert(out.drops.end(), pat.variadics.begin(), pat.variadics.end());
    return out;
  }

  std::set<std::string> scalars(pat.scalars.begin(), pat.scalars.end());
  std::set<std::string> vars(pat.variadics.begin(), pat.variadics.end());
  std::map<std::string, int> used_count;

  std::string lit;
  auto flush_lit = [&]() {
    if (!lit.empty()) {
      out.parts.push_back({TemplatePart::Kind::Literal, lit, false});
      lit.clear();
    }
  };

  for (size_t i = 0; i < tmpl.size();) {
    if (tmpl[i] == '$' && i + 1 < tmpl.size() && tmpl[i + 1] == '$') {
      lit += '$';
      i += 2;
      continue;
    }
    if (tmpl.compare(i, 4, "$...") == 0 && i + 4 < tmpl.size() &&
        ident_start(static_cast<unsigned char>(tmpl[i + 4]))) {
      flush_lit();
      size_t j = i + 4;
      while (j < tmpl.size() && ident_char(static_cast<unsigned char>(tmpl[j]))) ++j;
      const std::string name = tmpl.substr(i + 4, j - (i + 4));
      if (!vars.count(name)) {
        if (scalars.count(name))
          throw PatternError("template '" + tmpl + "': '$..." + name + "' used as variadic but '$" + name + "' is scalar in pattern");
        throw PatternError("template '" + tmpl + "': unbound variadic '$..." + name + "'");
      }
      ++used_count[name];
      out.parts.push_back({TemplatePart::Kind::Variadic, name, false});
      i = j;
      continue;
    }
    if (tmpl[i] == '$' && i + 1 < tmpl.size() && ident_start(static_cast<unsigned char>(tmpl[i + 1]))) {
      size_t j = i + 1;
      while (j < tmpl.size() && ident_char(static_cast<unsigned char>(tmpl[j]))) ++j;
      const std::string name = tmpl.substr(i + 1, j - (i + 1));
      if (!scalars.count(name)) {
        if (vars.count(name))
          throw PatternError("template '" + tmpl + "': '$" + name + "' must be written '$..." + name + "' (variadic in pattern)");
        throw PatternError("template '" + tmpl + "': unbound metavariable '$" + name + "'");
      }
      // Check if directly surrounded by `(` ... `)` already
      bool explicit_parens = !lit.empty() && lit.back() == '(' && j < tmpl.size() && tmpl[j] == ')';
      flush_lit();
      ++used_count[name];
      out.parts.push_back({TemplatePart::Kind::Meta, name, explicit_parens});
      i = j;
      continue;
    }
    lit += tmpl[i++];
  }
  flush_lit();

  for (const auto &s : pat.scalars) {
    if (!used_count.count(s)) out.drops.push_back(s);
    else if (used_count[s] > 1) out.duplicates.push_back(s);
  }
  for (const auto &v : pat.variadics) {
    if (!used_count.count(v)) out.drops.push_back(v);
    else if (used_count[v] > 1) out.duplicates.push_back(v);
  }
  return out;
}

RewriteSearchResult find_rewrite_matches(
    const Tree &tree,
    const RewritePattern &pat,
    const RewritePattern *inside,
    const RewritePattern *not_inside,
    const std::vector<std::pair<std::string, std::regex>> &where_filters) {
  RewriteSearchResult res;
  const Node pat_root_node = resolve_pat_root(pat);
  const TSNode pat_root = ts_of(pat_root_node);
  const std::string pat_kind = ts_node_type(pat_root);
  const std::string &tgt_src = tree.source();
  const std::string &pat_src = pat.parsed->source();

  // Compute ancestor filters once if requested
  auto ancestor_id_set = [&](const RewritePattern *filt) {
    std::set<std::pair<int64_t, int64_t>> spans;
    if (!filt) return spans;
    auto sub = find_rewrite_matches(tree, *filt);
    for (const auto &m : sub.matches) spans.emplace(m.start_byte, m.end_byte);
    return spans;
  };
  const auto inside_spans = ancestor_id_set(inside);
  const auto not_inside_spans = ancestor_id_set(not_inside);

  walk_nodes(tree, [&](const Node &n, const std::vector<Node> &anc) {
    TSNode tn = ts_of(n);
    if (!ts_node_is_named(tn) || ts_node_is_error(tn) || ts_node_is_missing(tn)) return;

    // Candidate prefilter & ancestor check
    const std::string tkind = ts_node_type(tn);
    if (tkind != pat_kind) {
      // Allow transparent equal-span unwrap check when pat_kind is a wrapper or target is one
      if (!(ts_node_named_child_count(pat_root) == 1 && is_transparent_wrapper(pat_kind)) &&
          !(ts_node_named_child_count(tn) == 1 && is_transparent_wrapper(tkind)))
        return;
    }

    if (inside || not_inside) {
      bool in_ok = !inside;
      bool not_in_ok = true;
      for (const Node &a : anc) {
        const std::pair<int64_t, int64_t> sp{node_start_byte(a), node_end_byte(a)};
        if (inside && inside_spans.count(sp)) in_ok = true;
        if (not_inside && not_inside_spans.count(sp)) not_in_ok = false;
      }
      if (!in_ok || !not_in_ok) return;
    }

    UnifyState state{pat.grammar, pat_src, tgt_src, {}, false};
    if (!state.unify(pat_root, tn)) {
      if (!pat.head_identifier.empty() && find_head_identifier(tn, tgt_src) == pat.head_identifier)
        ++res.near_misses;
      return;
    }

    // Apply --where filters on captured variables
    for (const auto &wf : where_filters) {
      const auto it = state.bindings.find(wf.first);
      if (it == state.bindings.end() || !regex_search_bounded(it->second.text, wf.second))
        return;
    }

    RewriteMatch m;
    m.start_byte = ts_node_start_byte(tn);
    m.end_byte = ts_node_end_byte(tn);
    m.row = ts_node_start_point(tn).row;
    m.col = ts_node_start_point(tn).column;
    m.end_row = ts_node_end_point(tn).row;
    m.end_col = ts_node_end_point(tn).column;
    m.text = slice(tgt_src, tn);
    m.is_statement = is_statement_kind(tkind);
    m.is_expression = is_expression_kind(tkind);
    m.has_unowned_comment = match_drops_unowned_comment(tn, state.bindings);
    for (const auto &kv : state.bindings) m.bindings.push_back(kv.second);
    res.matches.push_back(std::move(m));
  });
  return res;
}

FileRewritePlan apply_pattern_rewrites(
    const Tree &old_tree,
    const RewritePattern &pat,
    const CompiledTemplate &tmpl,
    const RewriteSearchResult &search,
    bool drop_comments,
    bool skip_unsafe,
    bool strict_parse) {
  FileRewritePlan plan;
  plan.near_misses = search.near_misses;
  const std::string &src = old_tree.source();
  if (search.matches.empty()) {
    plan.new_text = src;
    return plan;
  }

  // Sort matches: earliest start first, widest end first (outer before inner).
  std::vector<RewriteMatch> ordered = search.matches;
  std::sort(ordered.begin(), ordered.end(), [](const RewriteMatch &a, const RewriteMatch &b) {
    return a.start_byte < b.start_byte || (a.start_byte == b.start_byte && a.end_byte > b.end_byte);
  });

  // Deduplicate identical byte spans and detect partial overlaps.
  std::vector<RewriteMatch> deduped;
  for (const auto &m : ordered) {
    if (!deduped.empty() && deduped.back().start_byte == m.start_byte && deduped.back().end_byte == m.end_byte)
      continue;
    deduped.push_back(m);
  }
  const size_t n = deduped.size();
  std::vector<int> parent(n, -1);
  std::vector<std::vector<size_t>> children(n);
  for (size_t i = 0; i < n; ++i) {
    for (size_t p = i; p-- > 0;) {
      if (deduped[p].start_byte <= deduped[i].start_byte && deduped[i].end_byte <= deduped[p].end_byte) {
        parent[i] = static_cast<int>(p);
        children[p].push_back(i);
        break;
      }
      if (deduped[p].end_byte > deduped[i].start_byte && deduped[p].end_byte < deduped[i].end_byte) {
        throw RefactorError(3, "refactor refused: partial overlap at " +
                               std::to_string(deduped[i].row + 1) + ":" + std::to_string(deduped[i].col + 1));
      }
    }
  }

  // Recursive bottom-up expansion for tree-nested matches.
  struct ExpandedNode {
    std::string rendered;
    bool valid = true;
    bool used_parens = false;
    std::vector<PlannedEdit::ExprSpan> expr_spans;
  };
  std::vector<ExpandedNode> expanded(n);
  std::vector<bool> include_in_toplevel(n, false);

  std::function<void(size_t)> expand = [&](size_t idx) {
    const RewriteMatch &m = deduped[idx];
    for (size_t ch : children[idx]) expand(ch);

    if (m.has_unowned_comment && !drop_comments && !tmpl.is_delete) {
      PlannedEdit sk;
      sk.start_byte = m.start_byte;
      sk.end_byte = m.end_byte;
      sk.row = m.row;
      sk.col = m.col;
      sk.end_row = m.end_row;
      sk.end_col = m.end_col;
      sk.before = m.text;
      sk.status = "skipped";
      sk.reason = "unowned comment inside match (pass --drop-comments to override)";
      plan.skipped.push_back(std::move(sk));
      expanded[idx].valid = false;
      return;
    }

    // Splice rendered nested children that fall strictly inside a bound capture.
    std::map<std::string, std::string> cap_overrides;
    for (const auto &cap : m.bindings) {
      if (cap.empty) continue;
      std::vector<size_t> inside_cap;
      for (size_t ch : children[idx]) {
        if (!expanded[ch].valid) continue;
        if (deduped[ch].start_byte >= cap.start_byte && deduped[ch].end_byte <= cap.end_byte) {
          inside_cap.push_back(ch);
        } else if (deduped[ch].start_byte < cap.end_byte && cap.start_byte < deduped[ch].end_byte) {
          throw RefactorError(3, "refactor refused: partial metavariable overlap at " +
                                 std::to_string(deduped[ch].row + 1) + ":" + std::to_string(deduped[ch].col + 1));
        }
      }
      if (!inside_cap.empty()) {
        std::string composed = cap.text;
        for (size_t k = inside_cap.size(); k-- > 0;) {
          const auto &cm = deduped[inside_cap[k]];
          const size_t rel_s = static_cast<size_t>(cm.start_byte - cap.start_byte);
          const size_t rel_e = static_cast<size_t>(cm.end_byte - cap.start_byte);
          composed.replace(rel_s, rel_e - rel_s, expanded[inside_cap[k]].rendered);
        }
        cap_overrides[cap.name] = std::move(composed);
      }
    }
    // Check if any nested match hit the fixed skeleton outside all captures
    for (size_t ch : children[idx]) {
      if (!expanded[ch].valid) continue;
      bool in_any_cap = false;
      for (const auto &cap : m.bindings) {
        if (!cap.empty && deduped[ch].start_byte >= cap.start_byte && deduped[ch].end_byte <= cap.end_byte) {
          in_any_cap = true;
          break;
        }
      }
      if (!in_any_cap) {
        // Outermost subsumes skeleton overlap
      }
    }

    const std::string eol = eol_at(src, m.start_byte);
    std::set<size_t> no_wrap;
    expanded[idx].rendered = render_template_for_match(
        tmpl, m, cap_overrides, eol, no_wrap, false,
        &expanded[idx].expr_spans, &expanded[idx].used_parens);
  };

  for (size_t i = 0; i < n; ++i) {
    if (parent[i] == -1) {
      expand(i);
      if (expanded[i].valid) include_in_toplevel[i] = true;
    }
  }

  // Emit logical PlannedEdit entries (every valid match, ordered by start_byte)
  // AND build disjoint top-level edits for splicing/verification.
  std::vector<PlannedEdit> top_edits;
  for (size_t i = 0; i < n; ++i) {
    if (!expanded[i].valid) continue;
    const RewriteMatch &m = deduped[i];
    PlannedEdit pe;
    pe.start_byte = m.start_byte;
    pe.end_byte = m.end_byte;
    pe.row = m.row;
    pe.col = m.col;
    pe.end_row = m.end_row;
    pe.end_col = m.end_col;
    pe.before = m.text;
    pe.after = expanded[i].rendered;
    pe.parenthesized = expanded[i].used_parens;
    pe.is_expression = m.is_expression;
    pe.drops = tmpl.drops;
    pe.duplicates = tmpl.duplicates;
    pe.expr_spans = expanded[i].expr_spans;
    for (const auto &b : m.bindings) pe.bindings[b.name] = b.text;

    if (tmpl.is_delete) {
      if (sole_unbraced_control_body(old_tree, pe.start_byte, pe.end_byte)) {
        PlannedEdit sk = std::move(pe);
        sk.status = "skipped";
        sk.reason = "cannot delete sole body of unbraced control-flow statement";
        plan.skipped.push_back(std::move(sk));
        continue;
      }
      widen_line_deletion(src, pe.start_byte, pe.end_byte);
      pe.before = src.substr(static_cast<size_t>(pe.start_byte), static_cast<size_t>(pe.end_byte - pe.start_byte));
    }

    if (include_in_toplevel[i]) {
      // Only include top-level non-no-op edits for splicing, unless nested edit inside was needed
      top_edits.push_back(pe);
    }
    if (pe.before != pe.after) plan.planned.push_back(std::move(pe));
  }

  if (!plan.skipped.empty() && !skip_unsafe) {
    throw RefactorError(3, "refactor refused: " + std::to_string(plan.skipped.size()) +
                           " skipped site(s): " + plan.skipped.front().reason);
  }

  if (top_edits.empty()) {
    plan.new_text = src;
    return plan;
  }

  // Ensure top_edits sorted ascending and rebuild new_text, checking precedence & conditional parens.
  std::sort(top_edits.begin(), top_edits.end(), [](const PlannedEdit &a, const PlannedEdit &b) {
    return a.start_byte < b.start_byte;
  });

  auto assemble = [&](const std::vector<PlannedEdit> &eds) {
    std::string result = src;
    for (auto it = eds.rbegin(); it != eds.rend(); ++it) {
      result.replace(static_cast<size_t>(it->start_byte),
                     static_cast<size_t>(it->end_byte - it->start_byte), it->after);
    }
    return result;
  };

  std::string candidate_text = assemble(top_edits);
  Tree new_tree = parse(pat.grammar, candidate_text);

  const auto compute_spans = [](const std::vector<PlannedEdit> &eds) {
    std::vector<std::pair<int64_t, int64_t>> spans;
    spans.reserve(eds.size());
    int64_t acc = 0;
    for (const auto &e : eds) {
      const int64_t rs = e.start_byte + acc;
      const int64_t re = rs + static_cast<int64_t>(e.after.size());
      spans.emplace_back(rs, re);
      acc += static_cast<int64_t>(e.after.size()) - (e.end_byte - e.start_byte);
    }
    return spans;
  };

  // Conditional auto-parenthesization pass (up to 2 iterations: inner operands, then outer site).
  // Runs before check_tree_safety so precedence-torn sites get wrapped first.
  bool any_rerendered = false;
  for (int pass = 0; pass < 2; ++pass) {
    std::vector<std::pair<int64_t, int64_t>> new_spans = compute_spans(top_edits);
    bool pass_changed = false;
    for (size_t i = 0; i < top_edits.size(); ++i) {
      PlannedEdit &e = top_edits[i];
      if (e.after.empty()) continue;
      const int64_t edit_s = new_spans[i].first;
      const int64_t edit_e = new_spans[i].second;

      std::set<size_t> wrap_parts;
      size_t match_idx = n;
      for (size_t j = 0; j < n; ++j) {
        if (deduped[j].start_byte == e.start_byte && deduped[j].end_byte == e.end_byte) { match_idx = j; break; }
      }

      for (const auto &es : e.expr_spans) {
        if (es.wrap_parens) {
          for (size_t pi = 0; pi < tmpl.parts.size(); ++pi) {
            if (tmpl.parts[pi].kind == TemplatePart::Kind::Meta && tmpl.parts[pi].text == es.meta_name)
              wrap_parts.insert(pi);
          }
          continue;
        }
        const int64_t abs_s = edit_s + es.rel_start;
        const int64_t abs_e = edit_s + es.rel_end;
        if (abs_e > abs_s && !has_exact_span_node(new_tree, abs_s, abs_e)) {
          for (size_t pi = 0; pi < tmpl.parts.size(); ++pi) {
            if (tmpl.parts[pi].kind == TemplatePart::Kind::Meta && tmpl.parts[pi].text == es.meta_name)
              wrap_parts.insert(pi);
          }
        }
      }
      const bool already_outer = e.force_outer_parens ||
          (e.after.size() >= 2 && e.after.front() == '(' && e.after.back() == ')');
      const bool wrap_outer = !already_outer && e.is_expression && edit_e > edit_s &&
          !has_exact_span_node(new_tree, edit_s, edit_e);
      if (e.force_outer_parens || !wrap_parts.empty() || wrap_outer) {
        bool need_render = wrap_outer;
        if (!need_render) {
          for (const auto &es : e.expr_spans)
            if (!es.wrap_parens) {
              const int64_t abs_s = edit_s + es.rel_start;
              const int64_t abs_e = edit_s + es.rel_end;
              if (abs_e > abs_s && !has_exact_span_node(new_tree, abs_s, abs_e)) { need_render = true; break; }
            }
        }
        if (!need_render) continue;
        if (wrap_outer) e.force_outer_parens = true;
        if (match_idx < n) {
          std::map<std::string, std::string> cap_ov;
          for (const auto &cap : deduped[match_idx].bindings) {
            if (cap.empty) continue;
            std::vector<size_t> inside_cap;
            for (size_t ch : children[match_idx])
              if (expanded[ch].valid && deduped[ch].start_byte >= cap.start_byte && deduped[ch].end_byte <= cap.end_byte)
                inside_cap.push_back(ch);
            if (!inside_cap.empty()) {
              std::string comp = cap.text;
              for (size_t k = inside_cap.size(); k-- > 0;) {
                const auto &cm = deduped[inside_cap[k]];
                comp.replace(static_cast<size_t>(cm.start_byte - cap.start_byte),
                             static_cast<size_t>(cm.end_byte - cm.start_byte),
                             expanded[inside_cap[k]].rendered);
              }
              cap_ov[cap.name] = std::move(comp);
            }
          }
          bool used_p = false;
          e.after = render_template_for_match(
              tmpl, deduped[match_idx], cap_ov, eol_at(src, e.start_byte),
              wrap_parts, e.force_outer_parens, &e.expr_spans, &used_p);
          e.parenthesized = used_p;
        } else {
          if (wrap_outer && !(e.after.size() >= 2 && e.after.front() == '(' && e.after.back() == ')')) {
            e.after = "(" + e.after + ")";
            e.parenthesized = true;
          }
        }
        pass_changed = true;
        any_rerendered = true;
      }
    }
    if (!pass_changed) break;
    candidate_text = assemble(top_edits);
    new_tree = parse(pat.grammar, candidate_text);
  }

  std::vector<std::pair<int64_t, int64_t>> new_spans;
  check_tree_safety(old_tree, new_tree, top_edits, strict_parse, &new_spans);
  if (any_rerendered) {
    for (size_t i = 0; i < top_edits.size(); ++i) {
      const auto &e = top_edits[i];
      const int64_t edit_s = new_spans[i].first;
      const int64_t edit_e = new_spans[i].second;
      if (e.is_expression && edit_e > edit_s && !has_exact_span_node(new_tree, edit_s, edit_e)) {
        throw RefactorError(3, "refactor refused: replacement at line " + std::to_string(e.row + 1) +
                               " breaks operator precedence even after parenthesizing");
      }
      for (const auto &es : e.expr_spans) {
        const int64_t abs_s = edit_s + es.rel_start;
        const int64_t abs_e = edit_s + es.rel_end;
        if (abs_e > abs_s && !has_exact_span_node(new_tree, abs_s, abs_e)) {
          throw RefactorError(3, "refactor refused: $" + es.meta_name + " broke operator precedence at line " +
                                 std::to_string(e.row + 1) + "; wrap as '($" + es.meta_name + ")'");
        }
      }
    }
    for (const auto &te : top_edits) {
      for (auto &pe : plan.planned) {
        if (pe.start_byte == te.start_byte && pe.end_byte == te.end_byte) {
          pe.after = te.after;
          pe.parenthesized = te.parenthesized;
        }
      }
    }
  }

  plan.new_text = std::move(candidate_text);
  return plan;
}

void verify_refactor_tree(Grammar grammar, const Tree &old_tree, const std::string &new_text,
                          const std::vector<PlannedEdit> &applied_edits,
                          bool strict_parse) {
  if (applied_edits.empty()) return;
  Tree new_tree = parse(grammar, new_text);
  check_tree_safety(old_tree, new_tree, applied_edits, strict_parse, nullptr);
}

std::optional<std::string> expand_pattern_fix(Grammar grammar, const Tree &tree,
                                              const Node &match_root,
                                              const std::string &pattern,
                                              const std::string &fix_template) {
  if (!refactor_pattern_supports(grammar)) return std::nullopt;
  if (fix_template.find('$') == std::string::npos) return std::nullopt;
  const RewritePattern pat = compile_rewrite_pattern(grammar, pattern);
  const CompiledTemplate tmpl = compile_rewrite_template(pat, fix_template);
  const auto res = find_rewrite_matches(tree, pat);
  const int64_t rs = node_start_byte(match_root), re = node_end_byte(match_root);
  for (const auto &m : res.matches) {
    if (m.start_byte == rs && m.end_byte == re) {
      return render_template_for_match(
          tmpl, m, {}, eol_at(tree.source(), m.start_byte), {}, false, nullptr, nullptr);
    }
    for (const auto &b : m.bindings) {
      if (!b.empty && b.start_byte == rs && b.end_byte == re) {
        return render_template_for_match(
            tmpl, m, {}, eol_at(tree.source(), m.start_byte), {}, false, nullptr, nullptr);
      }
    }
  }
  return std::nullopt;
}

} // namespace lmok
