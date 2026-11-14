#include "metrics.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace lmok {
namespace {
std::string escape(const std::string &text) {
  static const char hex[] = "0123456789abcdef";
  std::string out;
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
    else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
    else out += char(c);
  }
  return out;
}
bool control(const std::string &kind) {
  return kind == "if_statement" || kind == "for_statement" || kind == "for_range_loop" ||
         kind == "while_statement" || kind == "for_in_statement" || kind == "catch_clause" ||
         kind == "switch_statement" || kind == "do_statement" || kind == "try_statement";
}
bool literal(const std::string &kind) {
  return (kind.size() >= 8 && kind.compare(kind.size() - 8, 8, "_literal") == 0) ||
         kind == "ansi_c_string" || kind == "string" || kind == "string_literal" || kind == "raw_string" ||
         kind == "char_literal" || kind == "character_literal" || kind == "number_literal" ||
         kind == "number" || kind == "integer" || kind == "float" || kind == "true" ||
         kind == "false" || kind == "none" || kind == "null" || kind == "nullptr" || kind == "concatenated_string";
}
struct Token { uint64_t normalized; Node node; };

std::vector<Token> tokens(const Tree &tree) {
  std::vector<Token> result;
  std::map<std::string, uint64_t> ids;
  int64_t skip_until = -1;
  Node previous;
  size_t previous_depth = 0;
  bool pending = false;
  const auto emit = [&](const Node &node) {
    std::string text = node_text(node);
    if (text.empty()) return;
    const auto kind = node_kind(node);
    if (kind.find("identifier") != std::string::npos || kind == "variable_name") text = "$identifier";
    const auto inserted = ids.emplace(text, ids.size() + 3);
    result.push_back({inserted.first->second, node});
  };
  // A node is a leaf when the following pre-order node has no greater depth.
  // Literal parents become one token and hide their children (escaped quotes,
  // string contents and interpolation punctuation must not change the window).
  walk_nodes(tree, [&](const Node &node, const std::vector<Node> &ancestors) {
    const size_t depth = ancestors.size();
    if (pending && depth <= previous_depth) { emit(previous); pending = false; }
    if (node_start_byte(node) < skip_until) return;
    const auto kind = node_kind(node);
    if (kind.find("comment") != std::string::npos) {
      pending = false; skip_until = node_end_byte(node); return;
    }
    if (literal(kind)) {
      pending = false; skip_until = node_end_byte(node);
      result.push_back({1, node}); return;
    }
    previous = node; previous_depth = depth; pending = true;
  });
  if (pending) emit(previous);
  return result;
}
} // namespace

FileMetrics measure_file(const Tree &tree, const std::string &source) {
  FileMetrics metrics;
  metrics.lines = source.empty() ? 0 : static_cast<int64_t>(std::count(source.begin(), source.end(), '\n')) + (source.back() == '\n' ? 0 : 1);
  std::vector<int64_t> depths;
  walk_nodes(tree, [&](const Node &node, const std::vector<Node> &ancestors) {
    const auto kind = node_kind(node);
    if (kind == "function_definition" || kind == "function_declaration" || kind == "generator_function_declaration" ||
        kind == "method_definition" || kind == "method_declaration" || kind == "constructor_declaration") ++metrics.functions;
    depths.resize(ancestors.size());
    const bool else_if = kind == "if_statement" && !ancestors.empty() && node_kind(ancestors.back()) == "else_clause";
    const int64_t depth = (depths.empty() ? 0 : depths.back()) + (control(kind) && !else_if ? 1 : 0);
    metrics.max_nesting = std::max(metrics.max_nesting, depth);
    depths.push_back(depth);
  });
  return metrics;
}

std::vector<Duplicate> duplicate_windows(const Tree &tree, int64_t min_tokens) {
  if (min_tokens < 1) throw std::invalid_argument("--tokens must be positive");
  const auto all = tokens(tree);
  const size_t width = static_cast<size_t>(min_tokens);
  if (width > all.size()) return {};
  // Unsigned wrap is intentional: polynomial rolling hash over token ids.
  constexpr uint64_t base = 1099511628211ULL;
  uint64_t power = 1, hash = 0;
  for (size_t i = 0; i < width; ++i) { power *= base; hash = hash * base + all[i].normalized; }
  std::unordered_map<uint64_t, std::vector<size_t>> representatives;
  std::set<size_t> repeated;
  for (size_t start = 0; start + width <= all.size(); ++start) {
    if (start) hash = hash * base + all[start + width - 1].normalized - all[start - 1].normalized * power;
    auto &bucket = representatives[hash];
    bool equal = false;
    for (size_t earlier : bucket) {
      bool same = true;
      for (size_t j = 0; j < width; ++j)
        if (all[earlier + j].normalized != all[start + j].normalized) { same = false; break; }
      if (!same) continue; // Never report a hash collision as a clone.
      equal = true;
      if (start - earlier >= width) { repeated.insert(earlier); repeated.insert(start); }
      break;
    }
    if (!equal) bucket.push_back(start);
  }
  std::vector<Duplicate> result;
  for (size_t start : repeated) {
    const auto &first = all[start].node, &last = all[start + width - 1].node;
    result.push_back({node_start_row(first), node_start_col(first), node_end_row(last), node_end_col(last), min_tokens});
  }
  return result;
}

std::string metrics_json(const std::string &path, const FileMetrics &metrics) {
  std::ostringstream out;
  out << "{\"file\":\"" << escape(path) << "\",\"lines\":" << metrics.lines << ",\"functions\":" << metrics.functions << ",\"max_nesting\":" << metrics.max_nesting << "}";
  return out.str();
}
std::string duplicate_json(const std::string &path, const Duplicate &duplicate) {
  std::ostringstream out;
  out << "{\"file\":\"" << escape(path) << "\",\"start_row\":" << duplicate.start_row << ",\"start_col\":" << duplicate.start_col
      << ",\"end_row\":" << duplicate.end_row << ",\"end_col\":" << duplicate.end_col << ",\"tokens\":" << duplicate.tokens << "}";
  return out.str();
}
} // namespace lmok
