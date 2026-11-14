// Global refactoring engine for `lmok refactor`:
// - Dry-run by default (`diff` with 3 context lines, `sites`, or `json`)
// - Scope-aware tiered `--rename`, exact `--import`, and sequential `--rules`
// - Two-phase in-memory validation + staged atomic per-file `--apply` with rollback

#include "refactor.hpp"

#include "baseline.hpp"
#include "bounded_regex.hpp"
#include "dslvm.hpp"
#include "facts.hpp"
#include "input.hpp"
#include "pattern.hpp"
#include "stdlib/json.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <tree_sitter/api.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace lmok {
namespace {

using dslvm::stdlib::JsonNode;

TSNode ts_of(const Node &node) {
  TSNode n;
  std::memcpy(&n, node.bytes, sizeof n);
  return n;
}

std::string slice(const std::string &src, TSNode n) {
  const uint32_t s = ts_node_start_byte(n);
  const uint32_t e = ts_node_end_byte(n);
  if (s >= src.size() || e <= s) return {};
  return src.substr(s, std::min<uint32_t>(e, static_cast<uint32_t>(src.size())) - s);
}

std::string json_esc(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 0x80) {
      size_t len = c >= 0xc2 && c <= 0xdf ? 2 :
                   c >= 0xe0 && c <= 0xef ? 3 :
                   c >= 0xf0 && c <= 0xf4 ? 4 : 0;
      bool valid = len && len <= s.size() - i;
      for (size_t j = 1; valid && j < len; ++j) {
        const unsigned char nx = static_cast<unsigned char>(s[i + j]);
        valid = nx >= 0x80 && nx <= 0xbf;
      }
      if (valid) { out.append(s, i, len); i += len - 1; }
      else out += "\\ufffd";
      continue;
    }
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (c < 0x20) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "\\u%04x", c);
        out += buf;
      } else {
        out += static_cast<char>(c);
      }
    }
  }
  return out;
}

bool valid_ident(const std::string &s, bool allow_dollar) {
  if (s.empty()) return false;
  auto ok0 = [&](unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || (allow_dollar && c == '$');
  };
  auto okc = [&](unsigned char c) { return ok0(c) || (c >= '0' && c <= '9'); };
  if (!ok0(static_cast<unsigned char>(s[0]))) return false;
  for (size_t i = 1; i < s.size(); ++i)
    if (!okc(static_cast<unsigned char>(s[i]))) return false;
  return true;
}

std::string diff_path_header(const std::string &path) {
  std::string p = path;
  while (p.rfind("./", 0) == 0) p.erase(0, 2);
  return p;
}

std::vector<std::string> split_lines_keep_eol(const std::string &text) {
  std::vector<std::string> lines;
  for (size_t start = 0; start < text.size();) {
    const size_t nl = text.find('\n', start);
    const size_t next = (nl == std::string::npos) ? text.size() : nl + 1;
    lines.push_back(text.substr(start, next - start));
    start = next;
  }
  return lines;
}

// Compute real git-apply-compatible unified diff with context hunks.
std::string unified_refactor_diff(const std::string &path,
                                  const std::string &before,
                                  const std::string &after,
                                  size_t context_lines) {
  if (before == after) return {};
  const auto a = split_lines_keep_eol(before);
  const auto b = split_lines_keep_eol(after);

  // Trim shared prefix/suffix, then Myers / DP LCS over remaining middle slice.
  size_t pref = 0;
  while (pref < a.size() && pref < b.size() && a[pref] == b[pref]) ++pref;
  size_t suf = 0;
  while (pref + suf < a.size() && pref + suf < b.size() &&
         a[a.size() - 1 - suf] == b[b.size() - 1 - suf])
    ++suf;

  const size_t am = a.size() - pref - suf;
  const size_t bm = b.size() - pref - suf;

  struct Op { char kind; size_t ai; size_t bi; };
  std::vector<Op> ops;
  ops.reserve(a.size() + b.size());
  for (size_t i = 0; i < pref; ++i) ops.push_back({' ', i, i});

  if (am * bm <= 4000000ULL) {
    // Classic LCS DP over middle slice
    std::vector<uint32_t> dp((am + 1) * (bm + 1), 0);
    auto at = [&](size_t i, size_t j) -> uint32_t & { return dp[i * (bm + 1) + j]; };
    for (size_t i = am; i-- > 0;) {
      for (size_t j = bm; j-- > 0;) {
        if (a[pref + i] == b[pref + j]) at(i, j) = 1 + at(i + 1, j + 1);
        else at(i, j) = std::max(at(i + 1, j), at(i, j + 1));
      }
    }
    size_t i = 0, j = 0;
    while (i < am || j < bm) {
      if (i < am && j < bm && a[pref + i] == b[pref + j]) {
        ops.push_back({' ', pref + i, pref + j});
        ++i; ++j;
      } else if (i < am && (j == bm || at(i + 1, j) >= at(i, j + 1))) {
        ops.push_back({'-', pref + i, pref + j});
        ++i;
      } else {
        ops.push_back({'+', pref + i, pref + j});
        ++j;
      }
    }
  } else {
    // Huge single-file diff fallback: single combined hunk
    for (size_t i = 0; i < am; ++i) ops.push_back({'-', pref + i, pref});
    for (size_t j = 0; j < bm; ++j) ops.push_back({'+', pref + am, pref + j});
  }
  for (size_t i = 0; i < suf; ++i)
    ops.push_back({' ', a.size() - suf + i, b.size() - suf + i});

  // Group changed ops into hunks with context_lines padding
  std::vector<std::pair<size_t, size_t>> hunks;
  for (size_t idx = 0; idx < ops.size();) {
    if (ops[idx].kind == ' ') { ++idx; continue; }
    size_t hs = idx > context_lines ? idx - context_lines : 0;
    size_t he = idx + 1;
    while (he < ops.size()) {
      size_t gap = 0;
      size_t scan = he;
      while (scan < ops.size() && ops[scan].kind == ' ') { ++gap; ++scan; }
      if (scan < ops.size() && gap <= 2 * context_lines) {
        he = scan + 1;
      } else {
        he = std::min(ops.size(), he + context_lines);
        break;
      }
    }
    if (!hunks.empty() && hs <= hunks.back().second) hunks.back().second = he;
    else hunks.emplace_back(hs, he);
    idx = he;
  }

  const std::string hdr = diff_path_header(path);
  std::ostringstream out;
  out << "--- a/" << hdr << "\n+++ b/" << hdr << "\n";
  for (const auto &hk : hunks) {
    size_t old_count = 0, new_count = 0;
    size_t old_start = 0, new_start = 0;
    bool got_old = false, got_new = false;
    for (size_t k = hk.first; k < hk.second; ++k) {
      if (ops[k].kind != '+') {
        if (!got_old) { old_start = ops[k].ai + 1; got_old = true; }
        ++old_count;
      }
      if (ops[k].kind != '-') {
        if (!got_new) { new_start = ops[k].bi + 1; got_new = true; }
        ++new_count;
      }
    }
    if (!got_old) old_start = ops[hk.first].ai;
    if (!got_new) new_start = ops[hk.first].bi;
    out << "@@ -" << old_start << "," << old_count << " +" << new_start << "," << new_count << " @@\n";
    for (size_t k = hk.first; k < hk.second; ++k) {
      const std::string &line = (ops[k].kind == '+') ? b[ops[k].bi] : a[ops[k].ai];
      const bool has_nl = !line.empty() && line.back() == '\n';
      out << ops[k].kind << line;
      if (!has_nl) out << "\n\\ No newline at end of file\n";
    }
  }
  return out.str();
}

// ---- `--import` path rewriter ----
FileRewritePlan plan_import_file(const Tree &tree, Grammar grammar,
                                 const std::string &old_target,
                                 const std::string &new_target,
                                 bool strict_parse) {
  FileRewritePlan plan;
  const std::string &src = tree.source();
  const auto facts = extract_facts(tree, grammar);
  std::vector<PlannedEdit> edits;

  for (const auto &f : facts) {
    if (f.kind != "ref.include" && f.kind != "ref.import" &&
        f.kind != "ref.import_from" && f.kind != "ref.source")
      continue;
    if (f.name != old_target) continue;

    // Locate the inner path span cleanly inside the fact node `[start_byte, end_byte]`.
    int64_t best_a = -1, best_b = -1;
    walk_nodes(tree, [&](const Node &n, const std::vector<Node> &) {
      if (best_a >= 0) return;
      const int64_t a = node_start_byte(n), b = node_end_byte(n);
      if (a < f.start_byte || b > f.end_byte) return;
      const std::string txt = node_text(n);
      if (txt == old_target) {
        best_a = a;
        best_b = b;
      } else if (txt.size() >= old_target.size() + 2) {
        const char q0 = txt.front(), q1 = txt.back();
        if (((q0 == '"' && q1 == '"') || (q0 == '\'' && q1 == '\'') || (q0 == '<' && q1 == '>')) &&
            txt.substr(1, txt.size() - 2) == old_target) {
          best_a = a + 1;
          best_b = b - 1;
        }
      }
    });
    if (best_a < 0) continue;

    PlannedEdit pe;
    pe.start_byte = best_a;
    pe.end_byte = best_b;
    pe.row = f.name_row;
    pe.col = f.name_col;
    pe.end_row = f.end_row;
    pe.end_col = f.end_col;
    pe.before = src.substr(static_cast<size_t>(best_a), static_cast<size_t>(best_b - best_a));
    pe.after = new_target;
    pe.confidence = "exact";
    edits.push_back(std::move(pe));
  }

  // Deduplicate identical spans and apply bottom-up
  std::sort(edits.begin(), edits.end(), [](const PlannedEdit &a, const PlannedEdit &b) {
    return a.start_byte < b.start_byte;
  });
  edits.erase(std::unique(edits.begin(), edits.end(), [](const PlannedEdit &a, const PlannedEdit &b) {
    return a.start_byte == b.start_byte && a.end_byte == b.end_byte;
  }), edits.end());

  if (edits.empty()) {
    plan.new_text = src;
    return plan;
  }

  std::string out = src;
  for (auto it = edits.rbegin(); it != edits.rend(); ++it)
    out.replace(static_cast<size_t>(it->start_byte), static_cast<size_t>(it->end_byte - it->start_byte), it->after);

  verify_refactor_tree(grammar, tree, out, edits, strict_parse);
  plan.new_text = std::move(out);
  plan.planned = std::move(edits);
  return plan;
}

// ---- `--rename` lexical scope + symbol classifier across languages ----
bool is_scope_node_kind(const std::string &kind) {
  return kind == "translation_unit" || kind == "program" || kind == "module" ||
         kind == "source_file" || kind == "namespace_definition" ||
         kind == "class_specifier" || kind == "struct_specifier" ||
         kind == "union_specifier" || kind == "enum_specifier" ||
         kind == "class_declaration" || kind == "interface_declaration" ||
         kind == "class_definition" || kind == "function_definition" ||
         kind == "function_declaration" || kind == "method_declaration" ||
         kind == "constructor_declaration" || kind == "arrow_function" ||
         kind == "lambda_expression" || kind == "compound_statement" ||
         kind == "statement_block" || kind == "block" ||
         kind == "for_statement" || kind == "for_range_loop" ||
         kind == "for_in_statement" || kind == "for_of_statement" ||
         kind == "catch_clause";
}

bool is_ident_leaf_kind(const std::string &kind) {
  return kind == "identifier" || kind == "field_identifier" ||
         kind == "type_identifier" || kind == "namespace_identifier" ||
         kind == "property_identifier" || kind == "shorthand_property_identifier" ||
         kind == "shorthand_property_identifier_pattern";
}

struct ScopeNode {
  int id = 0;
  int parent = -1;
  int64_t start_byte = 0;
  int64_t end_byte = 0;
  std::string kind;
  std::set<std::string> local_binds; // parameter / local var names declared here
  std::set<std::string> type_binds;
  std::set<std::string> all_idents;  // identifier names appearing directly in this scope
};

struct IdentOcc {
  int64_t start_byte = 0, end_byte = 0;
  int64_t row = 0, col = 0, end_row = 0, end_col = 0;
  int scope_id = 0;
  std::string kind;       // leaf kind
  std::string parent_kind;
  std::string grandparent_kind;
  std::string field_role; // field name on immediate parent
  bool is_member_rhs = false;
  bool is_qualified = false;
  bool is_dtor = false;
  bool is_ctor_or_type = false;
  bool is_decl = false;
  bool in_preproc_body = false;
  std::string qualifier;  // e.g. "Foo" in Foo::bar
  int64_t def_scope_id = -1; // innermost scope declaring this identifier locally
};

struct ParsedFileRenameModel {
  std::vector<ScopeNode> scopes;
  std::vector<IdentOcc> occs;
  std::map<std::pair<int64_t, int64_t>, size_t> by_span;
  std::vector<Fact> facts;
};

// Recursively find declarator identifier leaf(s) of a parameter or variable declaration.
void collect_declarator_idents(TSNode n, const std::string &src, std::vector<std::pair<int64_t, int64_t>> &spans) {
  if (ts_node_is_null(n)) return;
  const std::string k = ts_node_type(n);
  if (k == "identifier" || k == "field_identifier") {
    spans.emplace_back(ts_node_start_byte(n), ts_node_end_byte(n));
    return;
  }
  if (k == "pointer_declarator" || k == "reference_declarator" || k == "array_declarator" ||
      k == "parenthesized_declarator" || k == "structured_binding_declarator" ||
      k == "rest_pattern" || k == "identifier_list" || k == "var_spec") {
    const uint32_t c = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < c; ++i) collect_declarator_idents(ts_node_named_child(n, i), src, spans);
    return;
  }
  if (k == "init_declarator" || k == "variable_declarator" || k == "parameter_declaration" ||
      k == "optional_parameter_declaration" || k == "formal_parameter" ||
      k == "required_parameter" || k == "optional_parameter" || k == "parameter" ||
      k == "default_parameter" || k == "typed_parameter" || k == "typed_default_parameter" ||
      k == "catch_formal_parameter") {
    TSNode d = ts_node_child_by_field_name(n, "declarator", 10);
    if (ts_node_is_null(d)) d = ts_node_child_by_field_name(n, "name", 4);
    if (ts_node_is_null(d) && ts_node_named_child_count(n) > 0) {
      // Fall back for Python `typed_parameter (identifier) (type)` where first child is identifier
      TSNode first = ts_node_named_child(n, 0);
      if (std::string(ts_node_type(first)) == "identifier") d = first;
    }
    collect_declarator_idents(d, src, spans);
  }
}

ParsedFileRenameModel analyze_file_scopes(const Tree &tree, Grammar grammar) {
  ParsedFileRenameModel model;
  model.facts = extract_facts(tree, grammar);
  const std::string &src = tree.source();

  // First build scope tree using walk_nodes
  std::map<const void *, int> scope_of_node;
  std::function<void(TSNode, int, bool)> build_scopes = [&](TSNode n, int cur_scope, bool is_direct_fn_body) {
    const std::string k = ts_node_type(n);
    int next_scope = cur_scope;
    if (ts_node_is_named(n) && is_scope_node_kind(k) && !is_direct_fn_body) {
      ScopeNode sc;
      sc.id = static_cast<int>(model.scopes.size());
      sc.parent = model.scopes.empty() ? -1 : cur_scope;
      sc.start_byte = ts_node_start_byte(n);
      sc.end_byte = ts_node_end_byte(n);
      sc.kind = k;
      next_scope = sc.id;
      model.scopes.push_back(std::move(sc));
    }
    scope_of_node[n.id] = next_scope;

    // Collect local bindings declared in this scope
    if (k == "declaration" || k == "local_variable_declaration" || k == "lexical_declaration" ||
        k == "variable_declaration" || k == "short_var_declaration" || k == "var_declaration") {
      // Only treat as local if inside a non-file/non-class scope
      if (next_scope > 0 && model.scopes[static_cast<size_t>(next_scope)].kind != "namespace_definition" &&
          model.scopes[static_cast<size_t>(next_scope)].kind != "class_specifier" &&
          model.scopes[static_cast<size_t>(next_scope)].kind != "struct_specifier") {
        std::vector<std::pair<int64_t, int64_t>> spans;
        const uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c; ++i)
          collect_declarator_idents(ts_node_named_child(n, i), src, spans);
        for (const auto &sp : spans) {
          model.scopes[static_cast<size_t>(next_scope)].local_binds.insert(
              src.substr(static_cast<size_t>(sp.first), static_cast<size_t>(sp.second - sp.first)));
        }
      }
    } else if (k == "parameter_declaration" || k == "optional_parameter_declaration" ||
               k == "formal_parameter" || k == "required_parameter" ||
               k == "optional_parameter" || k == "default_parameter" ||
               k == "typed_parameter" || k == "typed_default_parameter" ||
               k == "parameter_list" || k == "formal_parameters" ||
               k == "for_range_loop" || k == "catch_clause") {
      if (k != "parameter_list" && k != "formal_parameters") {
        std::vector<std::pair<int64_t, int64_t>> spans;
        collect_declarator_idents(n, src, spans);
        if (k == "for_range_loop") {
          TSNode decl = ts_node_child_by_field_name(n, "declarator", 10);
          collect_declarator_idents(decl, src, spans);
        }
        for (const auto &sp : spans) {
          model.scopes[static_cast<size_t>(next_scope)].local_binds.insert(
              src.substr(static_cast<size_t>(sp.first), static_cast<size_t>(sp.second - sp.first)));
        }
      }
    }

    const uint32_t c = ts_node_child_count(n);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode ch = ts_node_child(n, i);
      const char *fn = ts_node_field_name_for_child(n, i);
      const std::string ck = ts_node_type(ch);
      const bool direct_body = fn && std::string(fn) == "body" &&
          (ck == "compound_statement" || ck == "block" || ck == "statement_block") &&
          (k == "function_definition" || k == "method_declaration" || k == "constructor_declaration" ||
           k == "lambda_expression" || k == "arrow_function" || k == "for_statement" ||
           k == "for_range_loop" || k == "catch_clause");
      build_scopes(ch, next_scope, direct_body);
    }
  };
  build_scopes(ts_of(node_root(tree)), 0, false);

  // Also index def.class/struct/enum/type defs into type_binds
  for (const auto &f : model.facts) {
    if (f.kind == "def.class" || f.kind == "def.struct" || f.kind == "def.union" ||
        f.kind == "def.enum" || f.kind == "def.typedef" || f.kind == "def.type" ||
        f.kind == "def.interface") {
      model.scopes[0].type_binds.insert(f.name);
    }
  }

  // Now collect every identifier-class leaf with full context
  walk_nodes(tree, [&](const Node &n, const std::vector<Node> &anc) {
    TSNode tn = ts_of(n);
    const std::string k = ts_node_type(tn);
    if (!ts_node_is_named(tn)) return;

    const auto sc_it = scope_of_node.find(tn.id);
    const int sc_id = (sc_it == scope_of_node.end()) ? 0 : sc_it->second;

    if (k == "preproc_arg") {
      // Remember preproc_arg text spans in case user asks about macro occurrences
      IdentOcc o;
      o.start_byte = ts_node_start_byte(tn);
      o.end_byte = ts_node_end_byte(tn);
      o.row = ts_node_start_point(tn).row;
      o.col = ts_node_start_point(tn).column;
      o.end_row = ts_node_end_point(tn).row;
      o.end_col = ts_node_end_point(tn).column;
      o.scope_id = sc_id;
      o.kind = k;
      o.in_preproc_body = true;
      model.occs.push_back(std::move(o));
      return;
    }

    if (!is_ident_leaf_kind(k)) return;
    const std::string name = slice(src, tn);
    if (!name.empty() && sc_id >= 0 && static_cast<size_t>(sc_id) < model.scopes.size())
      model.scopes[static_cast<size_t>(sc_id)].all_idents.insert(name);

    IdentOcc o;
    o.start_byte = ts_node_start_byte(tn);
    o.end_byte = ts_node_end_byte(tn);
    o.row = ts_node_start_point(tn).row;
    o.col = ts_node_start_point(tn).column;
    o.end_row = ts_node_end_point(tn).row;
    o.end_col = ts_node_end_point(tn).column;
    o.scope_id = sc_id;
    o.kind = k;

    if (!anc.empty()) {
      TSNode par = ts_of(anc.back());
      o.parent_kind = ts_node_type(par);
      const uint32_t pc = ts_node_child_count(par);
      for (uint32_t i = 0; i < pc; ++i) {
        if (ts_node_child(par, i).id == tn.id) {
          const char *fn = ts_node_field_name_for_child(par, i);
          if (fn) o.field_role = fn;
          break;
        }
      }
      if (anc.size() >= 2) o.grandparent_kind = node_kind(anc[anc.size() - 2]);
      if (o.parent_kind == "destructor_name") o.is_dtor = true;
      if ((o.parent_kind == "field_expression" || o.parent_kind == "member_expression" ||
           o.parent_kind == "selector_expression" || o.parent_kind == "field_access") &&
          o.field_role == "field")
        o.is_member_rhs = true;
      if ((o.parent_kind == "member_expression" || o.parent_kind == "attribute") &&
          (o.field_role == "property" || o.field_role == "attribute"))
        o.is_member_rhs = true;
      if (o.parent_kind == "qualified_identifier" || o.parent_kind == "scoped_identifier" ||
          o.parent_kind == "scoped_type_identifier") {
        o.is_qualified = true;
        TSNode scope_ch = ts_node_child_by_field_name(par, "scope", 5);
        if (!ts_node_is_null(scope_ch) && scope_ch.id != tn.id) o.qualifier = slice(src, scope_ch);
      }
      if (k == "type_identifier" || o.parent_kind == "class_specifier" || o.parent_kind == "struct_specifier" ||
          o.parent_kind == "destructor_name" || o.grandparent_kind == "destructor_name")
        o.is_ctor_or_type = true;
      if (o.field_role == "declarator" || o.parent_kind == "init_declarator" ||
          o.parent_kind == "parameter_declaration" || o.parent_kind == "formal_parameter" ||
          o.parent_kind == "function_declarator")
        o.is_decl = true;
    }

    // Resolve innermost local binding scope for this occurrence
    if (!o.is_member_rhs && !o.is_qualified) {
      int cur = sc_id;
      while (cur > 0) {
        if (model.scopes[static_cast<size_t>(cur)].local_binds.count(name)) {
          o.def_scope_id = cur;
          break;
        }
        cur = model.scopes[static_cast<size_t>(cur)].parent;
      }
    }

    const size_t idx = model.occs.size();
    model.occs.push_back(std::move(o));
    model.by_span[{model.occs[idx].start_byte, model.occs[idx].end_byte}] = idx;
  });

  return model;
}

bool qualifier_matches_scope(const std::string &qual, const std::string &target_scope) {
  if (target_scope.empty()) return true;
  // Normalize `::` -> `.` for comparison
  auto norm = [](std::string s) {
    for (size_t p = 0; (p = s.find("::", p)) != std::string::npos;) s.replace(p, 2, ".");
    return s;
  };
  const std::string q = norm(qual), t = norm(target_scope);
  return q == t || q.rfind("." + t) != std::string::npos || t.rfind("." + q) != std::string::npos;
}

bool contains_whole_word(const std::string &haystack, const std::string &word) {
  for (size_t pos = 0; (pos = haystack.find(word, pos)) != std::string::npos; pos += word.size()) {
    const bool left = pos == 0 || !((haystack[pos - 1] >= 'A' && haystack[pos - 1] <= 'Z') ||
                                    (haystack[pos - 1] >= 'a' && haystack[pos - 1] <= 'z') ||
                                    (haystack[pos - 1] >= '0' && haystack[pos - 1] <= '9') ||
                                    haystack[pos - 1] == '_');
    const size_t end = pos + word.size();
    const bool right = end == haystack.size() || !((haystack[end] >= 'A' && haystack[end] <= 'Z') ||
                                                   (haystack[end] >= 'a' && haystack[end] <= 'z') ||
                                                   (haystack[end] >= '0' && haystack[end] <= '9') ||
                                                   haystack[end] == '_');
    if (left && right) return true;
  }
  return false;
}

} // namespace

// Global helpers exposed cleanly for multi-file plan / transaction
struct FileResult {
  std::string path;
  Grammar grammar = Grammar::Json;
  std::string before_bytes;
  std::string after_bytes;
  std::string before_sha256;
  FileRewritePlan plan;
};

int refactor_run(const RefactorOptions &opts, const std::vector<RefactorTarget> &targets,
                 std::ostream &out, std::ostream &err) {
  if (targets.empty()) {
    err << "refactor: no target files\n";
    return 2;
  }

  // Deduplicate canonical target paths while preserving order
  std::vector<RefactorTarget> unique_targets;
  std::set<std::string> seen_paths;
  for (const auto &t : targets) {
    if (seen_paths.insert(t.path).second) unique_targets.push_back(t);
  }

  // 1. Validate mode & upfront configuration before touching files (exit 2)
  if (opts.expect_digest && opts.expect_digest->empty()) {
    err << "--expect needs a non-empty plan digest\n";
    return 2;
  }

  if (opts.mode == RefactorMode::Import) {
    if (opts.from.empty() || opts.to.empty() || opts.from == opts.to) {
      err << "--import requires distinct non-empty <OLD_PATH> and <NEW_PATH>\n";
      return 2;
    }
  } else if (opts.mode == RefactorMode::Rename) {
    if (opts.to.empty() || opts.from == opts.to) {
      err << "--rename requires distinct non-empty <OLD> and <NEW>\n";
      return 2;
    }
    const bool allow_dollar = opts.forced_lang && (*opts.forced_lang == Grammar::Javascript ||
                                                    *opts.forced_lang == Grammar::Typescript ||
                                                    *opts.forced_lang == Grammar::Tsx);
    if (!valid_ident(opts.to, allow_dollar) || (!opts.from.empty() && !valid_ident(opts.from, allow_dollar))) {
      err << "--rename identifier must be a valid identifier\n";
      return 2;
    }
    static const std::set<std::string> kKinds{
        "", "local", "parameter", "variable", "function", "method",
        "field", "type", "class", "struct", "namespace", "macro"};
    if (!kKinds.count(opts.rename_kind)) {
      err << "unknown --kind '" << opts.rename_kind << "'\n";
      return 2;
    }
    if ((opts.rename_kind == "method" || opts.rename_kind == "field") && opts.rename_scope.empty()) {
      err << "--kind " << opts.rename_kind << " requires --scope <Class>\n";
      return 2;
    }
  }

  // Compile --where regexes
  std::vector<std::pair<std::string, std::regex>> where_filters;
  for (const auto &wc : opts.where_clauses) {
    try {
      where_filters.emplace_back(wc.first, bounded_regex(wc.second));
    } catch (const std::exception &e) {
      err << "--where " << wc.first << ": " << e.what() << "\n";
      return 2;
    }
  }

  // Rule struct for --rules pack
  struct CodemodRule {
    std::string id;
    std::optional<Grammar> grammar;
    std::string pattern;
    std::string to;
    std::string inside;
    std::string not_inside;
    std::vector<std::pair<std::string, std::regex>> where;
    bool drop_comments = false;
  };
  std::vector<CodemodRule> codemod_rules;
  if (opts.mode == RefactorMode::Rules) {
    try {
      const std::string raw_rules = read_input_file(opts.from);
      dslvm::stdlib::json_detail::Parser parser(raw_rules);
      const JsonNode root = parser.val();
      parser.ws();
      if (parser.p != raw_rules.size()) throw RefactorError(2, "rules pack has trailing bytes");
      if (root.kind != JsonNode::Kind::Object) throw RefactorError(2, "rules pack must be a JSON object");
      std::optional<Grammar> pack_lang;
      for (const auto &m : root.object) {
        if (m.key == "lang" && m.value->kind == JsonNode::Kind::String) {
          const std::string &l = m.value->text;
          for (int gi = 0; gi <= static_cast<int>(Grammar::Lmcode); ++gi) {
            if (l == grammar_name(static_cast<Grammar>(gi))) { pack_lang = static_cast<Grammar>(gi); break; }
          }
          if (!pack_lang) throw RefactorError(2, "unknown lang '" + l + "' in refactor rules");
        }
      }
      const JsonNode *arr = nullptr;
      for (const auto &m : root.object) if (m.key == "rules") arr = m.value.get();
      if (!arr || arr->kind != JsonNode::Kind::Array || arr->array.empty())
        throw RefactorError(2, "rules pack must have a non-empty 'rules' array");
      for (const auto &item : arr->array) {
        if (item.kind != JsonNode::Kind::Object) throw RefactorError(2, "rule entry must be object");
        CodemodRule cr;
        cr.grammar = pack_lang;
        for (const auto &f : item.object) {
          if (f.key == "id" && f.value->kind == JsonNode::Kind::String) cr.id = f.value->text;
          else if (f.key == "pattern" && f.value->kind == JsonNode::Kind::String) cr.pattern = f.value->text;
          else if ((f.key == "to" || f.key == "replace") && f.value->kind == JsonNode::Kind::String) cr.to = f.value->text;
          else if (f.key == "inside" && f.value->kind == JsonNode::Kind::String) cr.inside = f.value->text;
          else if (f.key == "not-inside" && f.value->kind == JsonNode::Kind::String) cr.not_inside = f.value->text;
          else if (f.key == "drop_comments" && f.value->kind == JsonNode::Kind::Bool) cr.drop_comments = f.value->boolean;
          else if (f.key == "where" && f.value->kind == JsonNode::Kind::Object) {
            for (const auto &wm : f.value->object) {
              if (wm.value->kind != JsonNode::Kind::String)
                throw RefactorError(2, "rule '" + cr.id + "' where value for '" + wm.key + "' must be a regex string");
              cr.where.emplace_back(wm.key, bounded_regex(wm.value->text));
            }
          } else if (f.key == "lang" && f.value->kind == JsonNode::Kind::String) {
            std::optional<Grammar> rg;
            for (int gi = 0; gi <= static_cast<int>(Grammar::Lmcode); ++gi)
              if (f.value->text == grammar_name(static_cast<Grammar>(gi))) { rg = static_cast<Grammar>(gi); break; }
            if (!rg) throw RefactorError(2, "unknown rule lang '" + f.value->text + "'");
            cr.grammar = rg;
          }
        }
        if (cr.pattern.empty()) throw RefactorError(2, "rule '" + cr.id + "' missing 'pattern'");
        if (cr.grammar) {
          const auto rp = compile_rewrite_pattern(*cr.grammar, cr.pattern);
          (void)compile_rewrite_template(rp, cr.to);
        }
        codemod_rules.push_back(std::move(cr));
      }
    } catch (const RefactorError &e) {
      err << e.what() << "\n";
      return e.exit_code;
    } catch (const std::exception &e) {
      err << e.what() << "\n";
      return 2;
    }
  }

  // Per-grammar cached pattern compilations
  struct CompiledPair {
    RewritePattern pat;
    CompiledTemplate tmpl;
    std::optional<RewritePattern> inside;
    std::optional<RewritePattern> not_inside;
  };
  std::map<Grammar, CompiledPair> compiled_cache;
  auto get_compiled = [&](Grammar g) -> const CompiledPair & {
    auto it = compiled_cache.find(g);
    if (it != compiled_cache.end()) return it->second;
    RewritePattern pat = compile_rewrite_pattern(g, opts.from);
    CompiledTemplate tmpl = compile_rewrite_template(pat, opts.to);
    std::optional<RewritePattern> in_p, nin_p;
    if (!opts.inside.empty()) in_p = compile_rewrite_pattern(g, opts.inside);
    if (!opts.not_inside.empty()) nin_p = compile_rewrite_pattern(g, opts.not_inside);
    auto inserted = compiled_cache.emplace(g, CompiledPair{std::move(pat), std::move(tmpl), std::move(in_p), std::move(nin_p)});
    return inserted.first->second;
  };

  // Eagerly validate if --lang was specified for Pattern mode
  if (opts.mode == RefactorMode::Pattern && opts.forced_lang) {
    try {
      (void)get_compiled(*opts.forced_lang);
    } catch (const std::exception &e) {
      err << e.what() << "\n";
      return 2;
    }
  }

  // 2. Read & parse all targets; preflight plan & verify ALL files in memory (never touch disk)
  std::vector<FileResult> file_results;
  file_results.reserve(unique_targets.size());

  // For cursor `--rename --at FILE:LINE:COL`, resolve target cursor first
  std::string at_old_name = opts.from;
  bool at_is_local = false;
  bool at_is_type = false;
  int at_decl_scope = -1;
  std::string at_resolved_path;

  if (opts.mode == RefactorMode::Rename && !opts.rename_at_file.empty()) {
    try {
      at_resolved_path = std::filesystem::canonical(std::filesystem::u8path(opts.rename_at_file)).generic_u8string();
    } catch (const std::exception &) {
      at_resolved_path = std::filesystem::u8path(opts.rename_at_file).lexically_normal().generic_u8string();
    }
    const RefactorTarget *anchor = nullptr;
    for (const auto &t : unique_targets) {
      std::string tc;
      try { tc = std::filesystem::canonical(std::filesystem::u8path(t.path)).generic_u8string(); }
      catch (const std::exception &) { tc = std::filesystem::u8path(t.path).lexically_normal().generic_u8string(); }
      if (tc == at_resolved_path) { anchor = &t; break; }
    }
    if (!anchor) {
      err << "--at file is not in refactor target set: " << opts.rename_at_file << "\n";
      return 2;
    }
    std::string raw;
    try { raw = read_input_file(anchor->path); }
    catch (const std::exception &e) { err << e.what() << "\n"; return 3; }
    Tree tree = parse(anchor->grammar, raw);
    const auto model = analyze_file_scopes(tree, anchor->grammar);
    const int64_t r0 = opts.rename_at_line - 1;
    const int64_t c0 = opts.rename_at_col - 1;
    const IdentOcc *hit = nullptr;
    for (const auto &occ : model.occs) {
      if (occ.in_preproc_body) continue;
      if (occ.row <= r0 && r0 <= occ.end_row &&
          (r0 > occ.row || c0 >= occ.col) &&
          (r0 < occ.end_row || c0 < occ.end_col)) {
        hit = &occ;
        break;
      }
    }
    if (!hit) {
      err << "--at " << opts.rename_at_file << ":" << opts.rename_at_line << ":" << opts.rename_at_col
          << ": no identifier at cursor\n";
      return 2;
    }
    const std::string sym = raw.substr(static_cast<size_t>(hit->start_byte),
                                       static_cast<size_t>(hit->end_byte - hit->start_byte));
    if (!at_old_name.empty() && at_old_name != sym) {
      err << "--at cursor identifier is '" << sym << "', expected '" << at_old_name << "'\n";
      return 2;
    }
    at_old_name = sym;
    if (hit->def_scope_id > 0) {
      at_is_local = true;
      at_decl_scope = hit->def_scope_id;
    } else if (hit->is_ctor_or_type || model.scopes[0].type_binds.count(sym)) {
      at_is_type = true;
    }
  }

  try {
    for (const auto &t : unique_targets) {
      if (opts.mode == RefactorMode::Pattern && t.from_dir && !refactor_pattern_supports(t.grammar))
        continue;
      FileResult fr;
      fr.path = t.path;
      fr.grammar = t.grammar;
      fr.before_bytes = read_input_file(t.path);
      fr.before_sha256 = dslvm::hex(dslvm::sha256(fr.before_bytes));
      Tree old_tree = parse(t.grammar, fr.before_bytes);

      if (opts.mode == RefactorMode::Pattern) {
        const auto &cp = get_compiled(t.grammar);
        const auto search = find_rewrite_matches(
            old_tree, cp.pat,
            cp.inside ? &*cp.inside : nullptr,
            cp.not_inside ? &*cp.not_inside : nullptr,
            where_filters);
        fr.plan = apply_pattern_rewrites(old_tree, cp.pat, cp.tmpl, search,
                                         opts.drop_comments, opts.skip_unsafe, opts.strict_parse);
        fr.after_bytes = fr.plan.new_text;
      } else if (opts.mode == RefactorMode::Import) {
        fr.plan = plan_import_file(old_tree, t.grammar, opts.from, opts.to, opts.strict_parse);
        fr.after_bytes = fr.plan.new_text;
      } else if (opts.mode == RefactorMode::Rules) {
        std::string cur_text = fr.before_bytes;
        std::vector<PlannedEdit> combined_planned;
        for (const auto &cr : codemod_rules) {
          if (cr.grammar && *cr.grammar != t.grammar) continue;
          if (!refactor_pattern_supports(t.grammar)) continue;
          const auto pat = compile_rewrite_pattern(t.grammar, cr.pattern);
          const auto tmpl = compile_rewrite_template(pat, cr.to);
          std::optional<RewritePattern> in_p, nin_p;
          if (!cr.inside.empty()) in_p = compile_rewrite_pattern(t.grammar, cr.inside);
          if (!cr.not_inside.empty()) nin_p = compile_rewrite_pattern(t.grammar, cr.not_inside);
          Tree cur_tree = parse(t.grammar, cur_text);
          const auto search = find_rewrite_matches(
              cur_tree, pat, in_p ? &*in_p : nullptr, nin_p ? &*nin_p : nullptr, cr.where);
          auto sub = apply_pattern_rewrites(
              cur_tree, pat, tmpl, search, cr.drop_comments || opts.drop_comments,
              opts.skip_unsafe, opts.strict_parse);
          cur_text = sub.new_text;
          for (auto &pe : sub.planned) combined_planned.push_back(std::move(pe));
          fr.plan.near_misses += sub.near_misses;
        }
        fr.plan.new_text = cur_text;
        fr.plan.planned = std::move(combined_planned);
        fr.after_bytes = cur_text;
      } else { // RefactorMode::Rename
        // Check --at locality: Tier A renames ONLY inside the cursor's file/scope!
        std::string cur_canon;
        try { cur_canon = std::filesystem::canonical(std::filesystem::u8path(t.path)).generic_u8string(); }
        catch (const std::exception &) { cur_canon = std::filesystem::u8path(t.path).lexically_normal().generic_u8string(); }
        if (at_is_local && cur_canon != at_resolved_path) {
          fr.after_bytes = fr.before_bytes;
          fr.plan.new_text = fr.before_bytes;
          file_results.push_back(std::move(fr));
          continue;
        }

        const auto model = analyze_file_scopes(old_tree, t.grammar);
        const std::string &old_name = at_old_name;
        const std::string &new_name = opts.to;
        const bool type_mode = at_is_type || opts.rename_kind == "type" ||
                               opts.rename_kind == "class" || opts.rename_kind == "struct" ||
                               (opts.rename_kind.empty() && !at_is_local && model.scopes[0].type_binds.count(old_name));
        const bool member_mode = (opts.rename_kind == "method" || opts.rename_kind == "field");

        std::vector<PlannedEdit> edits;
        for (const auto &occ : model.occs) {
          if (occ.in_preproc_body) {
            const std::string mtxt = fr.before_bytes.substr(static_cast<size_t>(occ.start_byte),
                                                            static_cast<size_t>(occ.end_byte - occ.start_byte));
            if (contains_whole_word(mtxt, old_name)) {
              fr.plan.warnings.push_back("macro body at line " + std::to_string(occ.row + 1) +
                                         " mentions '" + old_name + "' (not rewritten)");
            }
            continue;
          }
          const std::string text = fr.before_bytes.substr(static_cast<size_t>(occ.start_byte),
                                                          static_cast<size_t>(occ.end_byte - occ.start_byte));
          if (text != old_name) continue;

          if (at_is_local) {
            // Tier A: only occurrences whose lexical def_scope_id matches cursor's declaring scope
            if (occ.def_scope_id != at_decl_scope) continue;
            // Check NEW collision inside target scope or enclosing child scope reaching occ
            if (model.scopes[static_cast<size_t>(occ.scope_id)].local_binds.count(new_name) ||
                model.scopes[static_cast<size_t>(at_decl_scope)].local_binds.count(new_name) ||
                model.scopes[static_cast<size_t>(occ.scope_id)].all_idents.count(new_name) ||
                model.scopes[static_cast<size_t>(at_decl_scope)].all_idents.count(new_name)) {
              throw RefactorError(3, "rename collision in " + t.path + ":" + std::to_string(occ.row + 1) +
                                     ": '" + new_name + "' already exists in target scope");
            }
            PlannedEdit pe;
            pe.start_byte = occ.start_byte;
            pe.end_byte = occ.end_byte;
            pe.row = occ.row; pe.col = occ.col; pe.end_row = occ.end_row; pe.end_col = occ.end_col;
            pe.before = old_name;
            pe.after = new_name;
            pe.confidence = "exact";
            edits.push_back(std::move(pe));
          } else if (type_mode) {
            // Tier B: type / C++ class + ctor + dtor + qualifier, skipping local variable shadows!
            if (occ.def_scope_id > 0) continue; // local var shadows class name
            if (occ.is_member_rhs && !occ.is_dtor) continue; // obj.field != type name unless dtor `p->~Foo()`
            // Check collision with existing type definition in file
            if (model.scopes[0].type_binds.count(new_name) ||
                model.scopes[static_cast<size_t>(occ.scope_id)].all_idents.count(new_name)) {
              // Allow if new_name isn't a conflicting type decl, check type_binds cleanly:
              if (model.scopes[0].type_binds.count(new_name)) {
                throw RefactorError(3, "rename collision in " + t.path + ": type '" + new_name + "' already declared");
              }
            }
            PlannedEdit pe;
            pe.start_byte = occ.start_byte;
            pe.end_byte = occ.end_byte;
            pe.row = occ.row; pe.col = occ.col; pe.end_row = occ.end_row; pe.end_col = occ.end_col;
            pe.before = old_name;
            pe.after = new_name;
            pe.confidence = "exact";
            edits.push_back(std::move(pe));
          } else if (member_mode) {
            // Tier C: gated method/field rename (`--scope C` required)
            if (occ.def_scope_id > 0 && !occ.is_member_rhs) continue; // unrelated local var
            bool is_exact_site = false;
            bool is_cand_site = false;
            if (occ.is_member_rhs) {
              is_cand_site = true; // untyped receiver `x.foo()` or `p->foo()`
            } else if (occ.is_qualified && !occ.qualifier.empty()) {
              if (!qualifier_matches_scope(occ.qualifier, opts.rename_scope)) continue;
              is_exact_site = true; // out-of-line `C::foo`
            } else {
              // Check enclosing class/struct/decl scope
              bool in_target_class = false;
              int cur = occ.scope_id;
              while (cur >= 0) {
                if (model.scopes[static_cast<size_t>(cur)].kind == "class_specifier" ||
                    model.scopes[static_cast<size_t>(cur)].kind == "struct_specifier" ||
                    model.scopes[static_cast<size_t>(cur)].kind == "class_declaration") {
                  // Match via facts scope at this node span
                  for (const auto &fc : model.facts) {
                    if ((fc.kind == "def.method" || fc.kind == "def.field" || fc.kind == "def.prototype") &&
                        fc.name == old_name && fc.start_byte <= occ.start_byte && occ.end_byte <= fc.end_byte &&
                        qualifier_matches_scope(fc.scope, opts.rename_scope)) {
                      in_target_class = true;
                      break;
                    }
                  }
                }
                cur = model.scopes[static_cast<size_t>(cur)].parent;
              }
              if (!in_target_class) continue;
              is_exact_site = true;
            }
            if (opts.exact_only && !is_exact_site) continue;
            PlannedEdit pe;
            pe.start_byte = occ.start_byte;
            pe.end_byte = occ.end_byte;
            pe.row = occ.row; pe.col = occ.col; pe.end_row = occ.end_row; pe.end_col = occ.end_col;
            pe.before = old_name;
            pe.after = new_name;
            pe.confidence = is_cand_site ? "candidate" : "exact";
            edits.push_back(std::move(pe));
          } else {
            // Tier C: function / variable / macro / namespace by name, excluding shadowed locals and member RHS
            if (occ.def_scope_id > 0) continue;
            if (occ.is_member_rhs) continue;
            if (!opts.rename_scope.empty() && occ.is_qualified && !qualifier_matches_scope(occ.qualifier, opts.rename_scope))
              continue;
            // Check NEW collision in enclosing scope
            if (model.scopes[static_cast<size_t>(occ.scope_id)].local_binds.count(new_name)) {
              throw RefactorError(3, "rename collision in " + t.path + ":" + std::to_string(occ.row + 1) +
                                     ": '" + new_name + "' is locally bound in scope");
            }
            PlannedEdit pe;
            pe.start_byte = occ.start_byte;
            pe.end_byte = occ.end_byte;
            pe.row = occ.row; pe.col = occ.col; pe.end_row = occ.end_row; pe.end_col = occ.end_col;
            pe.before = old_name;
            pe.after = new_name;
            // Function/global defs and direct non-member calls are exact when matching declared symbol
            pe.confidence = (opts.rename_kind == "variable" || opts.rename_kind == "macro") ? "candidate" : "exact";
            edits.push_back(std::move(pe));
          }
        }

        if (!edits.empty()) {
          std::sort(edits.begin(), edits.end(), [](const PlannedEdit &a, const PlannedEdit &b) {
            return a.start_byte < b.start_byte;
          });
          std::string out_bytes = fr.before_bytes;
          for (auto it = edits.rbegin(); it != edits.rend(); ++it)
            out_bytes.replace(static_cast<size_t>(it->start_byte),
                              static_cast<size_t>(it->end_byte - it->start_byte), it->after);
          verify_refactor_tree(t.grammar, old_tree, out_bytes, edits, opts.strict_parse);
          fr.after_bytes = std::move(out_bytes);
          fr.plan.new_text = fr.after_bytes;
          fr.plan.planned = std::move(edits);
        } else {
          fr.after_bytes = fr.before_bytes;
          fr.plan.new_text = fr.before_bytes;
        }
      }

      file_results.push_back(std::move(fr));
    }
  } catch (const PatternError &e) {
    err << e.what() << "\n";
    return 2;
  } catch (const RefactorError &e) {
    err << e.what() << "\n";
    return e.exit_code;
  } catch (const dslvm::Error &e) {
    err << e.what() << "\n";
    return 3;
  } catch (const std::exception &e) {
    err << e.what() << "\n";
    return 3;
  }

  // Totals & deterministic digest
  size_t total_sites = 0, changed_files = 0, total_skipped = 0, total_near = 0, total_candidates = 0;
  std::ostringstream digest_input;
  digest_input << "lmok-refactor-v1:" << static_cast<int>(opts.mode) << "\n";
  for (const auto &fr : file_results) {
    total_sites += fr.plan.planned.size();
    total_skipped += fr.plan.skipped.size();
    total_near += fr.plan.near_misses;
    for (const auto &pe : fr.plan.planned)
      if (pe.confidence == "candidate") ++total_candidates;
    if (fr.before_bytes != fr.after_bytes) ++changed_files;
    digest_input << fr.path << "|" << fr.before_sha256 << "|";
    digest_input << dslvm::hex(dslvm::sha256(fr.after_bytes)) << "\n";
  }
  const std::string plan_digest = dslvm::hex(dslvm::sha256(digest_input.str())).substr(0, 16);

  if (opts.expect_sites && total_sites != *opts.expect_sites) {
    err << "refactor refused: expected " << *opts.expect_sites << " site(s), found " << total_sites << "\n";
    return 3;
  }

  if (opts.apply && total_candidates > 0 && !opts.allow_candidates) {
    err << "refactor refused: " << total_candidates
        << " candidate site(s) require --allow-candidates (or pass --exact-only)\n";
    return 3;
  }

  if (opts.expect_digest && *opts.expect_digest != plan_digest) {
    err << "refactor refused: plan digest mismatch (expected " << *opts.expect_digest
        << ", got " << plan_digest << ")\n";
    return 3;
  }

  if (opts.apply && opts.git_clean && changed_files > 0) {
    try {
      const auto dirty = git_changed_files("HEAD");
      for (const auto &fr : file_results) {
        if (fr.before_bytes == fr.after_bytes) continue;
        std::string abs_p;
        try { abs_p = std::filesystem::canonical(std::filesystem::u8path(fr.path)).generic_u8string(); }
        catch (const std::exception &) { abs_p = std::filesystem::absolute(std::filesystem::u8path(fr.path)).generic_u8string(); }
        if (dirty.count(abs_p)) {
          err << "refactor refused under --git-clean: modified target has uncommitted git changes: " << fr.path << "\n";
          return 3;
        }
      }
    } catch (const BaselineError &e) {
      err << "refactor --git-clean: " << e.what() << "\n";
      return 2;
    }
  }

  // 3. Emit requested preview / output
  if (opts.format == RefactorFormat::Diff) {
    for (const auto &fr : file_results) {
      if (fr.before_bytes != fr.after_bytes)
        out << unified_refactor_diff(fr.path, fr.before_bytes, fr.after_bytes, opts.context_lines);
    }
  } else if (opts.format == RefactorFormat::Sites) {
    for (const auto &fr : file_results) {
      for (const auto &pe : fr.plan.planned) {
        out << fr.path << ":" << (pe.row + 1) << ":" << (pe.col + 1) << ": "
            << pe.before << " -> " << pe.after << " [" << pe.confidence << "]\n";
      }
      for (const auto &sk : fr.plan.skipped) {
        out << fr.path << ":" << (sk.row + 1) << ":" << (sk.col + 1) << ": skipped (" << sk.reason << ")\n";
      }
    }
  } else { // RefactorFormat::Json
    out << "{\"digest\":\"" << plan_digest << "\""
        << ",\"apply\":" << (opts.apply ? "true" : "false")
        << ",\"sites_count\":" << total_sites
        << ",\"files_count\":" << changed_files
        << ",\"skipped_count\":" << total_skipped
        << ",\"near_misses\":" << total_near
        << ",\"sites\":[";
    bool first = true;
    for (const auto &fr : file_results) {
      auto emit_site = [&](const PlannedEdit &pe) {
        if (!first) out << ",";
        first = false;
        out << "{\"file\":\"" << json_esc(fr.path) << "\""
            << ",\"lang\":\"" << grammar_name(fr.grammar) << "\""
            << ",\"row\":" << pe.row << ",\"col\":" << pe.col
            << ",\"end_row\":" << pe.end_row << ",\"end_col\":" << pe.end_col
            << ",\"start_byte\":" << pe.start_byte << ",\"end_byte\":" << pe.end_byte
            << ",\"before\":\"" << json_esc(pe.before) << "\""
            << ",\"after\":\"" << json_esc(pe.after) << "\""
            << ",\"confidence\":\"" << pe.confidence << "\""
            << ",\"status\":\"" << pe.status << "\"";
        if (!pe.reason.empty()) out << ",\"reason\":\"" << json_esc(pe.reason) << "\"";
        if (pe.parenthesized) out << ",\"parenthesized\":true";
        if (!pe.drops.empty()) {
          out << ",\"drops\":[";
          for (size_t i = 0; i < pe.drops.size(); ++i)
            out << (i ? "," : "") << "\"" << json_esc(pe.drops[i]) << "\"";
          out << "]";
        }
        if (!pe.duplicates.empty()) {
          out << ",\"duplicates\":[";
          for (size_t i = 0; i < pe.duplicates.size(); ++i)
            out << (i ? "," : "") << "\"" << json_esc(pe.duplicates[i]) << "\"";
          out << "]";
        }
        out << "}";
      };
      for (const auto &pe : fr.plan.planned) emit_site(pe);
      for (const auto &sk : fr.plan.skipped) emit_site(sk);
    }
    out << "]}\n";
  }

  for (const auto &fr : file_results)
    for (const auto &w : fr.plan.warnings)
      err << "warning (" << fr.path << "): " << w << "\n";

  // 4. Execute `--apply` transactionally (all preflight already verified!)
  if (opts.apply && changed_files > 0) {
    namespace fs = std::filesystem;
    struct Staged {
      const FileResult *fr = nullptr;
      fs::path target;
      fs::path tmp;
      bool replaced = false;
    };
    std::vector<Staged> staged;
    staged.reserve(changed_files);

    auto cleanup_temps = [&]() {
      std::error_code ec;
      for (const auto &s : staged) {
        if (!s.tmp.empty() && fs::exists(s.tmp, ec)) fs::remove(s.tmp, ec);
      }
    };

#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif

    try {
      size_t idx = 0;
      for (const auto &fr : file_results) {
        if (fr.before_bytes == fr.after_bytes) continue;
        fs::path tgt = fs::u8path(fr.path);
        std::error_code ec;
        if (fs::is_symlink(tgt, ec))
          throw RefactorError(3, "refactor refused: target is a symlink: " + fr.path);
        if (read_input_file(fr.path) != fr.before_bytes)
          throw RefactorError(3, "refactor refused: file modified during run: " + fr.path);

        const fs::file_status st = fs::status(tgt, ec);
        if (ec) throw RefactorError(3, "cannot stat " + fr.path + ": " + ec.message());

        fs::path dir = tgt.parent_path();
        if (dir.empty()) dir = ".";
        fs::path tmp = dir / (".lmok-refactor-" + std::to_string(pid) + "-" + std::to_string(idx++) + ".tmp");

        {
          std::ofstream tf(tmp, std::ios::binary | std::ios::trunc);
          if (!tf) throw RefactorError(3, "cannot stage temp file for " + fr.path);
          tf.write(fr.after_bytes.data(), static_cast<std::streamsize>(fr.after_bytes.size()));
          tf.flush();
          if (!tf) throw RefactorError(3, "failed writing temp file for " + fr.path);
        }
        fs::permissions(tmp, st.permissions(), ec);
        staged.push_back({&fr, tgt, tmp, false});
      }

      for (size_t i = 0; i < staged.size(); ++i) {
        auto &s = staged[i];
#ifdef _WIN32
        if (!MoveFileExW(s.tmp.wstring().c_str(), s.target.wstring().c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
          throw RefactorError(3, "cannot replace " + s.fr->path);
        }
#else
        std::error_code ec;
        fs::rename(s.tmp, s.target, ec);
        if (ec) throw RefactorError(3, "cannot replace " + s.fr->path + ": " + ec.message());
#endif
        s.replaced = true;
      }
    } catch (const RefactorError &e) {
      // Roll back any already-replaced files from retained original bytes
      for (auto &s : staged) {
        if (!s.replaced) continue;
        std::ofstream rb(s.target, std::ios::binary | std::ios::trunc);
        if (rb) rb.write(s.fr->before_bytes.data(), static_cast<std::streamsize>(s.fr->before_bytes.size()));
      }
      cleanup_temps();
      err << e.what() << "\n";
      return e.exit_code;
    } catch (const std::exception &e) {
      cleanup_temps();
      err << e.what() << "\n";
      return 3;
    }
  }

  err << (opts.apply ? "applied " : "dry-run: ")
      << total_sites << " replacement(s) in " << changed_files << " file(s)"
      << " (" << total_skipped << " skipped";
  if (total_near > 0) err << ", " << total_near << " near-miss(es)";
  err << ") digest:" << plan_digest << "\n";

  if (!out) return 4;
  if (opts.check && total_sites > 0) return 1;
  return 0;
}

} // namespace lmok
