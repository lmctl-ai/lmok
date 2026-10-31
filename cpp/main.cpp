// lmok: tree-sitter's incremental parsing engine as one statically
// linked, scriptable CLI. Host for the lmscript DSL plus a small parse
// subcommand (JSON grammar).
#include "host.hpp"
#include "lint.hpp"
#include "rule_test.hpp"
#include "autofix.hpp"
#include "autofix_io.hpp"
#include "ignore.hpp"
#include "project_config.hpp"
#include "project_ci.hpp"
#include "pattern.hpp"
#include "facts.hpp"
#include "metrics.hpp"
#include "input.hpp"
#include "db.hpp"
#include "baseline.hpp"
#include "sarif.hpp"
#include "dslvm.hpp"
#include "stdlib/string.hpp"
#include "stdlib/collections.hpp"
#include "schema_embedded.hpp"
#include "notices_embedded.hpp"

#include <cstdio>
#include <charconv>
#include <limits>
#include <chrono>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <map>
#include <set>
#include <unordered_map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif

namespace {
using dslvm::Args;
using dslvm::Value;

// ---- arena for schema object handles ----
struct Finding {
  int64_t row, col, end_row, end_col; // 0-based, tree-sitter convention
  std::string message;
  // Optional rule metadata (4-arg ts.report overload). Free-form strings —
  // severity policy belongs to the script, the host validates nothing.
  std::string rule, severity;
  lmok::RuleMetadata metadata; // SARIF rule properties/helpUri: use structured metadata, not message parsing.
  std::optional<lmok::FixEdit> fix;
};

// Every arena is append-only for one program run and cleared by reset()
// before the next --lmcode file (like the SDK's per-run reset of standard
// collection handles), so handles never dangle and the caps are per run, not
// per process. Trees and NodeLists are few (65536); Node handles are many
// (one per root()/child()/parent() call; NodeList.at() hands out pointers
// into the list itself and allocates nothing), and a Node is ~40 bytes, so
// their cap is 1<<20 (~40 MB worst case) in a deque (stable addresses, no
// per-node heap allocation).
struct Host {
  static constexpr size_t kHandleCap = 65536;
  static constexpr size_t kNodeCap = size_t(1) << 20;
  std::vector<std::unique_ptr<lmok::Tree>> trees;
  std::deque<lmok::Node> nodes;
  // One handle per distinct node per run (WP14 F4): a rule that climbs n.parent() for every node used to
  // allocate depth^2 handles and hit kNodeCap on inputs nested a few thousand deep; keyed by tree-sitter's
  // node identity, so repeated root()/child()/parent() answers reuse the same stable handle.
  std::unordered_map<const void *, lmok::Node *> node_index;
  // Parent of every node a rule can reach by climbing: filled from the walk's ancestor path when a node is
  // dispatched (each ancestor once per file), so Node.parent()/has_parent() are O(1) map hits instead of
  // tree-sitter's O(depth) ts_node_parent, which made nesting-depth rules cubic on deep inputs (WP14 F4).
  std::unordered_map<const void *, lmok::Node> parent_of;
  bool remember_parent(const lmok::Node &child, const lmok::Node &parent) {
    return parent_of.emplace(lmok::node_id(child), parent).second;
  }
  const lmok::Node *cached_parent(const lmok::Node &node) const {
    const auto it = parent_of.find(lmok::node_id(node));
    return it == parent_of.end() ? nullptr : &it->second;
  }
  std::vector<std::unique_ptr<lmok::NodeList>> node_lists;
  std::map<std::string, lmok::CompiledPattern> compiled_patterns; // "<grammar>\n<pattern>" -> compiled (ts.match_*)
  // Findings collected via ts.report share a multi-program invocation; check resets per input.
  std::vector<Finding> findings;
  bool include_metadata = false;
  bool no_suppress=false;
  std::map<const lmok::Tree *, lmok::Suppression> suppressions;
  size_t suppressed_count() const {size_t n=0;for(const auto &s:suppressions)n+=s.second.suppressed;return n;}
  void suppression_warnings(const std::string &path) {for(auto &s:suppressions)s.second.warn(path,{},true);}
  template <class T>
  Value hold(std::vector<std::unique_ptr<T>> &arena, const char *type, T value) {
    if (arena.size() >= kHandleCap)
      throw dslvm::Error(std::string(type) + " object limit exceeded (" + std::to_string(kHandleCap) + ")");
    arena.push_back(std::make_unique<T>(std::move(value)));
    return Value::object(type, arena.back().get());
  }
  Value hold_node(lmok::Node node) {
    const auto known = node_index.find(lmok::node_id(node));
    if (known != node_index.end()) return Value::object("Node", known->second);
    if (nodes.size() >= kNodeCap)
      throw dslvm::Error("Node object limit exceeded (" + std::to_string(kNodeCap) + ")");
    nodes.push_back(node);
    node_index.emplace(lmok::node_id(nodes.back()), &nodes.back());
    return Value::object("Node", &nodes.back());
  }
  void reset() {
    trees.clear();
    nodes.clear();
    node_index.clear();
    parent_of.clear();
    node_lists.clear();
    findings.clear();
    suppressions.clear();
  }
};

template <class T> T *self(void *p) { return static_cast<T *>(p); }

lmok::Tree *tree_of(void *p, const char *who) {
  auto *t = self<lmok::Tree>(p);
  if (!t) throw dslvm::Error(std::string(who) + ": stale Tree handle");
  return t;
}

lmok::Node *node_of(void *p, const char *who) {
  auto *n = self<lmok::Node>(p);
  if (!n) throw dslvm::Error(std::string(who) + ": stale Node handle");
  return n;
}

lmok::NodeList *nodelist_of(void *p, const char *who) {
  auto *l = self<lmok::NodeList>(p);
  if (!l) throw dslvm::Error(std::string(who) + ": stale NodeList handle");
  return l;
}

// The CLI has UTF-8 paths; leave the pinned SDK reader unchanged.
std::string read_bytecode(const std::string &path) {
  try { return lmok::read_input_file(path); }
  catch (const dslvm::Error &e) { throw dslvm::Error("LOAD_BAD_BYTECODE", e.what()); }
}

std::string read_file(const std::string &path) {
  return lmok::read_input_file(path);
}

// JSON string escaping for ts.report_json output.
std::string json_escape(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 0x80) {
      size_t length = c >= 0xc2 && c <= 0xdf ? 2 :
                      c >= 0xe0 && c <= 0xef ? 3 :
                      c >= 0xf0 && c <= 0xf4 ? 4 : 0;
      bool valid = length && length <= s.size() - i;
      for (size_t j = 1; valid && j < length; ++j) {
        const unsigned char next = static_cast<unsigned char>(s[i + j]);
        valid = next >= 0x80 && next <= 0xbf;
        if (j == 1 && ((c == 0xe0 && next < 0xa0) || (c == 0xed && next >= 0xa0) ||
                       (c == 0xf0 && next < 0x90) || (c == 0xf4 && next >= 0x90))) valid = false;
      }
      if (valid) { out.append(s, i, length); i += length - 1; }
      else out += "\\ufffd"; // malformed source bytes must not corrupt JSON output
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
      } else out += static_cast<char>(c);
    }
  }
  return out;
}

// One JSON representation for script reports and host-dispatched findings.
// IDs/severity come from ts.report, never from the entry's dispatch name.
std::string findings_json(const std::vector<Finding> &findings, bool include_metadata = false) {
    std::ostringstream out;
    out << "[";
    bool first = true;
    for (const auto &f : findings) {
      if (!first) out << ",";
      first = false;
      out << "{\"row\":" << f.row << ",\"col\":" << f.col
          << ",\"end_row\":" << f.end_row << ",\"end_col\":" << f.end_col;
      if (!f.rule.empty()) out << ",\"rule\":\"" << json_escape(f.rule) << "\"";
      if (!f.severity.empty()) out << ",\"severity\":\"" << json_escape(f.severity) << "\"";
      out << ",\"message\":\"" << json_escape(f.message) << "\"";
      if (f.fix) out << ",\"fix\":{\"start_byte\":" << f.fix->start_byte << ",\"end_byte\":" << f.fix->end_byte << ",\"text\":\"" << json_escape(f.fix->text) << "\"}";
      out << lmok::metadata_suffix(f.metadata,include_metadata) << "}";
    }
    out << "]";
    return out.str();
}

// ---- DSL registry: the schema implementation ----
void register_host(dslvm::Registry &reg, Host &host) {
  reg.context("ts", "Ts");

  reg.function("println", {"string"}, "void", [](Args &args) {
    std::cout << args[0].s << "\n";
    return Value::none();
  });

  // The "ts" context object (type Ts) is the parse entry point: the DSL
  // compiler resolves ts.parse(...) as a method call on the context object.
  const auto parse_method = [&reg, &host](const char *name, lmok::Grammar grammar) {
    reg.method("Ts", name, {"string"}, "Tree", [&host, grammar](void *, Args &args) {
      try {
        return host.hold(host.trees, "Tree", lmok::parse(grammar, args[0].s));
      } catch (const std::length_error &error) {
        throw dslvm::Error(error.what());
      }
    });
  };
  parse_method("parse", lmok::Grammar::Json);
  parse_method("parse_c", lmok::Grammar::C);
  parse_method("parse_python", lmok::Grammar::Python);
  parse_method("parse_java", lmok::Grammar::Java);
  parse_method("parse_bash", lmok::Grammar::Bash);
  // The findings sink: rules are void fns, so they report into the host and
  // the chunk body reads the collection back (count / JSON array string).
  const auto report_impl = [&host](void *, Args &args, bool with_fix) {
    const lmok::Node *n = node_of(args[0].obj, "report");
    Finding f{lmok::node_start_row(*n), lmok::node_start_col(*n),
              lmok::node_end_row(*n), lmok::node_end_col(*n), args[1].s, "", "", {}, std::nullopt};
    if (args.size() >= 4) { f.rule = args[2].s; f.severity = args[3].s; }
    if (args.size() == 5) {
      if (with_fix) f.fix = lmok::FixEdit{lmok::node_start_byte(*n), lmok::node_end_byte(*n), args[4].s};
      else f.metadata = lmok::parse_rule_metadata(args[4].s);
    }
    auto &suppression=host.suppressions.try_emplace(n->owner,n->owner->source()).first->second;
    if(suppression.drop(static_cast<size_t>(f.row),f.rule,host.no_suppress))return Value::none();
    if (host.findings.size() >= 65536)
      throw dslvm::Error("report limit exceeded (65536)");
    host.findings.push_back(std::move(f));
    return Value::none();
  };
  const auto normal_report = [report_impl](void *p, Args &args) { return report_impl(p, args, false); };
  reg.method("Ts", "report", {"Node", "string"}, "void", normal_report);
  reg.method("Ts", "report", {"Node", "string", "string", "string"}, "void", normal_report);
  reg.method("Ts", "report_meta", {"Node", "string", "string", "string", "string"}, "void", normal_report);
  reg.method("Ts", "report_fix", {"Node", "string", "string", "string", "string"}, "void", [report_impl](void *p, Args &args) { return report_impl(p, args, true); });
  reg.method("Ts", "report_count", {}, "int", [&host](void *, Args &) {
    return Value::integer(static_cast<int64_t>(host.findings.size()));
  });
  reg.method("Ts", "report_json", {}, "string", [&host](void *, Args &) {
    return Value::string(findings_json(host.findings,host.include_metadata));
  });
  const auto query_method = [&reg, &host](const char *name, lmok::Grammar grammar) {
    reg.method("Ts", name, {"Node", "string"}, "NodeList", [&host, grammar](void *, Args &args) {
      try {
        return host.hold(host.node_lists, "NodeList",
                         lmok::node_query(*node_of(args[0].obj, "query"), args[1].s, grammar));
      } catch (const lmok::QueryError &error) {
        throw dslvm::Error(error.what());
      }
    });
  };
  query_method("query", lmok::Grammar::Json);
  query_method("query_c", lmok::Grammar::C);
  query_method("query_python", lmok::Grammar::Python);
  query_method("query_java", lmok::Grammar::Java);
  query_method("query_bash", lmok::Grammar::Bash);
  // ts.match_c / ts.match_python: code-shaped patterns (pattern.hpp), compiled once per distinct
  // pattern text per run and cached, so a rule calling match in a loop pays the parse once.
  const auto match_method = [&reg, &host](const char *name, lmok::Grammar grammar) {
    reg.method("Ts", name, {"Node", "string"}, "NodeList", [&host, grammar](void *, Args &args) {
      try {
        auto &cache = host.compiled_patterns;
        const std::string key = std::string(lmok::grammar_name(grammar)) + "\n" + args[1].s;
        auto it = cache.find(key);
        if (it == cache.end()) it = cache.emplace(key, lmok::compile_pattern(grammar, args[1].s)).first;
        return host.hold(host.node_lists, "NodeList",
                         lmok::pattern_matches(*node_of(args[0].obj, "match"), it->second, grammar));
      } catch (const lmok::PatternError &error) {
        throw dslvm::Error(error.what());
      } catch (const lmok::QueryError &error) {
        throw dslvm::Error(error.what());
      }
    });
  };
  match_method("match_c", lmok::Grammar::C);
  match_method("match_python", lmok::Grammar::Python);
  const auto reparse_method = [&reg, &host](const char *name, lmok::Grammar grammar) {
    reg.method("Ts", name, {"Tree", "string"}, "Tree", [&host, grammar](void *, Args &args) {
      try {
        return host.hold(host.trees, "Tree",
                         lmok::reparse(grammar, *tree_of(args[0].obj, "reparse"), args[1].s));
      } catch (const std::out_of_range &error) {
        throw dslvm::Error(error.what());
      } catch (const std::length_error &error) {
        throw dslvm::Error(error.what());
      }
    });
  };
  reparse_method("reparse", lmok::Grammar::Json);
  reparse_method("reparse_c", lmok::Grammar::C);
  reparse_method("reparse_python", lmok::Grammar::Python);
  reparse_method("reparse_java", lmok::Grammar::Java);
  reparse_method("reparse_bash", lmok::Grammar::Bash);
  reg.method("Ts", "changed_ranges", {"Tree", "Tree"}, "string", [](void *, Args &args) {
    return Value::string(lmok::changed_ranges_json(*tree_of(args[0].obj, "changed_ranges"),
                                                       *tree_of(args[1].obj, "changed_ranges")));
  });

  reg.method("Tree", "edit", {"int", "int", "int"}, "void", [](void *s, Args &args) {
    try {
      lmok::tree_edit(*tree_of(s, "edit"), args[0].i, args[1].i, args[2].i);
    } catch (const std::out_of_range &error) {
      throw dslvm::Error(error.what());
    }
    return Value::none();
  });

  reg.method("Tree", "sexp", {}, "string", [](void *s, Args &) {
    return Value::string(lmok::tree_sexp(*tree_of(s, "sexp")));
  });
  reg.method("Tree", "has_error", {}, "bool", [](void *s, Args &) {
    return Value::boolean(lmok::tree_has_error(*tree_of(s, "has_error")));
  });
  reg.method("Tree", "node_count", {}, "int", [](void *s, Args &) {
    return Value::integer(lmok::tree_node_count(*tree_of(s, "node_count")));
  });
  reg.method("Tree", "root", {}, "Node", [&host](void *s, Args &) {
    return host.hold_node(lmok::node_root(*tree_of(s, "root")));
  });
  reg.method("Tree", "nodes", {}, "NodeList", [&host](void *s, Args &) {
    return host.hold(host.node_lists, "NodeList", lmok::tree_nodes(*tree_of(s, "nodes")));
  });

  const auto node_string_method = [&reg](const char *name, auto impl) {
    reg.method("Node", name, {}, "string", [name, impl](void *s, Args &) {
      return Value::string(impl(node_of(s, name)));
    });
  };
  node_string_method("kind", [](const lmok::Node *n) { return lmok::node_kind(*n); });
  node_string_method("text", [](const lmok::Node *n) { return lmok::node_text(*n); });
  reg.method("Node", "is_named", {}, "bool", [](void *s, Args &) {
    return Value::boolean(lmok::node_is_named(*node_of(s, "is_named")));
  });
  reg.method("Node", "is_missing", {}, "bool", [](void *s, Args &) {
    return Value::boolean(lmok::node_is_missing(*node_of(s, "is_missing")));
  });
  const auto node_point_method = [&reg](const char *name, auto impl) {
    reg.method("Node", name, {}, "int", [name, impl](void *s, Args &) {
      return Value::integer(impl(node_of(s, name)));
    });
  };
  node_point_method("start_row", [](const lmok::Node *n) { return lmok::node_start_row(*n); });
  node_point_method("start_col", [](const lmok::Node *n) { return lmok::node_start_col(*n); });
  node_point_method("end_row", [](const lmok::Node *n) { return lmok::node_end_row(*n); });
  node_point_method("end_col", [](const lmok::Node *n) { return lmok::node_end_col(*n); });
  reg.method("Node", "child_count", {}, "int", [](void *s, Args &) {
    return Value::integer(lmok::node_child_count(*node_of(s, "child_count")));
  });
  reg.method("Node", "child", {"int"}, "Node", [&host](void *s, Args &args) {
    const lmok::Node *n = node_of(s, "child");
    const int64_t count = lmok::node_child_count(*n);
    if (args[0].i < 0 || args[0].i >= count)
      throw dslvm::Error("Node.child index " + std::to_string(args[0].i) +
                         " out of range (named child count " + std::to_string(count) + ")");
    return host.hold_node(lmok::node_child(*n, args[0].i));
  });
  reg.method("Node", "has_parent", {}, "bool", [&host](void *s, Args &) {
    const lmok::Node *n = node_of(s, "has_parent");
    return Value::boolean(host.cached_parent(*n) != nullptr || lmok::node_has_parent(*n));
  });
  reg.method("Node", "parent", {}, "Node", [&host](void *s, Args &) {
    const lmok::Node *n = node_of(s, "parent");
    if (const lmok::Node *cached = host.cached_parent(*n)) return host.hold_node(*cached);
    if (!lmok::node_has_parent(*n))
      throw dslvm::Error("Node.parent: root node has no parent");
    return host.hold_node(lmok::node_parent(*n));
  });

  reg.method("NodeList", "len", {}, "int", [](void *s, Args &) {
    return Value::integer(static_cast<int64_t>(nodelist_of(s, "len")->items.size()));
  });
  reg.method("NodeList", "at", {"int"}, "Node", [](void *s, Args &args) {
    lmok::NodeList *l = nodelist_of(s, "at");
    const int64_t count = static_cast<int64_t>(l->items.size());
    if (args[0].i < 0 || args[0].i >= count)
      throw dslvm::Error("NodeList.at index " + std::to_string(args[0].i) +
                         " out of range (length " + std::to_string(count) + ")");
    // The list lives in the arena for the run and its vector is never
    // resized, so a pointer into it is a stable Node handle: iterating a
    // 200k-node tree allocates nothing (and no longer burns the Node cap).
    return Value::object("Node", &l->items[static_cast<size_t>(args[0].i)]);
  });
}

// ---- FILE|DIR target expansion shared by `lint` and `facts` ----
// Directory args expand RECURSIVELY to their regular files: hidden files and
// dirs BELOW the argument are skipped (a project under ~/.local or passed as
// ./.proj is still processed), only known extensions (.c/.h -> c, .json ->
// json, .py -> python, .sh/.bash -> bash, .go -> go, .js -> javascript) are kept, and paths are sorted for
// byte-stable output. An explicit file keeps its argument order. All printed
// paths use forward slashes (fs::path::generic_u8string) on every OS.
struct Target { std::string path; bool from_dir; };

std::string canonical_path(const std::string &path) {
  return std::filesystem::u8path(path).generic_u8string();
}

bool grammar_for_extension(const std::string &ext, lmok::Grammar &grammar) {
  if (ext == ".c" || ext == ".h") { grammar = lmok::Grammar::C; return true; }
  if (ext == ".json") { grammar = lmok::Grammar::Json; return true; }
  if (ext == ".java") { grammar = lmok::Grammar::Java; return true; }
  if (ext == ".py") { grammar = lmok::Grammar::Python; return true; }
  if (ext == ".sh" || ext == ".bash") { grammar = lmok::Grammar::Bash; return true; }
  if (ext == ".go") { grammar = lmok::Grammar::Go; return true; }
  if (ext == ".js") { grammar = lmok::Grammar::Javascript; return true; }
  if (ext == ".ts") { grammar = lmok::Grammar::Typescript; return true; }
  if (ext == ".tsx") { grammar = lmok::Grammar::Tsx; return true; }
  // Rule packs remain outer .lms scripts; .lmcode denotes standalone inner code.
  if (ext == ".lms") { grammar = lmok::Grammar::Lmscript; return true; }
  if (ext == ".lmcode") { grammar = lmok::Grammar::Lmcode; return true; }
  return false;
}

// Expands args[first..args.size()). Returns 0, or the exit code (2 usage,
// 3 I/O) after printing the diagnostic. With `only` set, directory expansion
// keeps that grammar's extensions alone (explicit files are kept regardless).
int expand_targets(const std::vector<std::string> &args, size_t first, std::vector<Target> &targets,
                   const lmok::Grammar *only = nullptr, const std::vector<std::string> &extra_ignore = {}) {
  namespace fs = std::filesystem;
  for (size_t i = first; i < args.size(); ++i) {
    std::error_code ec;
    const fs::path root = fs::u8path(args[i]);
    if (fs::is_directory(root, ec)) {
      std::optional<lmok::IgnoreRules> ignore;
      try {ignore.emplace(root,extra_ignore);}catch(const std::exception &e){std::cerr<<"ignore: "<<e.what()<<"\n";return 2;}
      bool eligible=false;
      std::vector<std::string> found;
      const fs::recursive_directory_iterator end;
      // Iterator errors (an unreadable subdirectory, a vanished entry) must
      // not end the walk silently with a partial file list: that is the
      // same fail-fast contract as an unreadable file (exit 3).
      fs::recursive_directory_iterator it(root, ec);
      fs::path failed_at = root;
      for (; !ec && it != end; failed_at = it->path(), it.increment(ec)) {
        const fs::path &p = it->path();
        // Hidden check FIRST: directories must be pruned (never descended
        // into) before the regular-file filter would skip them.
        bool hidden = false;
        for (const auto &part : p.lexically_relative(root)) {
          const std::string name = part.u8string();
          if (name != "." && name != ".." && name.rfind(".", 0) == 0) hidden = true; // C++17: no starts_with
        }
        if (hidden) { it.disable_recursion_pending(); continue; } // prune hidden dirs
        std::error_code file_ec;
        if (!it->is_regular_file(file_ec)) continue;
        lmok::Grammar grammar;
        if (!grammar_for_extension(p.extension().u8string(), grammar)) continue;
        if (only && grammar != *only) continue;
        eligible=true;
        if(ignore->ignored(fs::absolute(p)))continue;
        found.push_back(canonical_path(p.u8string()));
      }
      if (ec) {
        std::cerr << "cannot read " << canonical_path(failed_at.u8string()) << ": " << ec.message() << "\n";
        return 3;
      }
      if (found.empty() && !eligible) {
        std::cerr << "no lintable files in " << canonical_path(args[i]) << " (want .c/.h/.json/.py/.sh/.bash/.java/.go/.js)\n";
        return 2;
      }
      std::sort(found.begin(), found.end());
      for (auto &f : found) targets.push_back({f, true});
    } else {
      targets.push_back({canonical_path(args[i]), false});
    }
  }
  return 0;
}

bool parse_lang(const std::string &lang, lmok::Grammar &grammar) {
  if (lang == "json") grammar = lmok::Grammar::Json;
  else if (lang == "c") grammar = lmok::Grammar::C;
  else if (lang == "python") grammar = lmok::Grammar::Python;
  else if (lang == "bash") grammar = lmok::Grammar::Bash;
  else if (lang == "java") grammar = lmok::Grammar::Java;
  else if (lang == "go") grammar = lmok::Grammar::Go;
  else if (lang == "javascript") grammar = lmok::Grammar::Javascript;
  else if (lang == "typescript") grammar = lmok::Grammar::Typescript;
  else if (lang == "tsx") grammar = lmok::Grammar::Tsx;
  else if (lang == "lmscript") grammar = lmok::Grammar::Lmscript;
  else if (lang == "lmcode") grammar = lmok::Grammar::Lmcode;
  else return false;
  return true;
}

// ---- index helpers (facts -> DB, wave 2A) ----
// Edge-like fact kinds land in `edges` under these names (in addition to the
// refs row every ref.* fact gets).
std::string edge_kind_of(const std::string &fact_kind) {
  if (fact_kind == "ref.include") return "include";
  if (fact_kind == "ref.import") return "import";
  if (fact_kind == "ref.import_from" || fact_kind == "ref.import_wildcard") return "from_import";
  if (fact_kind == "ref.source") return "source";
  return "";
}

// Join dir + edge target and collapse interior "./" and resolvable ".."
// components, keeping a leading "./", so the result string-compares against
// the walked path form expand_targets stores ("./main.c" for root ".").
std::string normalize_join(const std::string &dir, const std::string &name) {
  const std::string both = dir.empty() ? name : dir + "/" + name;
  std::vector<std::string> parts;
  size_t at = 0;
  bool first = true;
  while (at <= both.size()) {
    const size_t slash = both.find('/', at);
    const std::string part = both.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
    if (part == ".") {
      if (first) parts.push_back(".");
    } else if (part == "..") {
      if (!parts.empty() && parts.back() != ".." && parts.back() != ".") parts.pop_back();
      else if (!parts.empty() && parts.back() == ".") { parts.pop_back(); parts.push_back(".."); }
      else parts.push_back("..");
    } else if (!part.empty()) parts.push_back(part);
    first = false;
    if (slash == std::string::npos) break;
    at = slash + 1;
  }
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) out += "/";
    out += parts[i];
  }
  return out;
}

// Write a SARIF log to FILE (binary, truncated). Returns 0, or 3 after a
// stderr diagnostic — the SARIF file is a requested output, so a failed
// write fails the run (unlike DB recording, which is a side channel).
int write_sarif_file(const std::string &path, const std::string &log) {
  std::ofstream out(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
  if (!out) { std::cerr << "cannot write " << canonical_path(path) << "\n"; return 3; }
  out << log;
  out.flush();
  if (!out) { std::cerr << "cannot write " << canonical_path(path) << "\n"; return 3; }
  return 0;
}

// driver.rules entries from a loaded pack: the id always, shortDescription
// from the pack message, helpUri from rule metadata `url` when carried.
std::vector<lmok::SarifRuleMeta> sarif_rules_of(const std::vector<lmok::LintRule> &rules) {
  std::vector<lmok::SarifRuleMeta> out;
  for (const auto &rule : rules) {
    const auto url = rule.metadata.strings.find("url");
    out.push_back({rule.id, rule.message, url == rule.metadata.strings.end() ? "" : url->second, rule.metadata});
  }
  return out;
}

void print_schema_summary(const dslvm::Registry &reg) {
  for (const auto &line : reg.signatures()) std::cout << line << "\n";
}
} // namespace

static int run_cli(int argc, char **argv) {
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY); // `parse -` must not CRLF-translate or stop at ^Z
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stderr), _O_BINARY);
#endif
  Host host;
  std::vector<char *> metadata_argv;
  const bool metadata_verb=argc>=2&&(std::string(argv[1])=="lint"||std::string(argv[1])=="ci"||std::string(argv[1])=="check"||std::string(argv[1])=="--lmcode"||std::string(argv[1])=="--first"||std::string(argv[1])=="--last");
  for(int i=0;i<argc;++i){if(metadata_verb&&i>=2&&std::string(argv[i])=="--metadata")host.include_metadata=true;else metadata_argv.push_back(argv[i]);}
  argc=static_cast<int>(metadata_argv.size());argv=metadata_argv.data();
  dslvm::Registry reg;
  register_host(reg, host);
  const std::string usage =
      "usage: lmok --version | --notices | --lmcode-schema [--json] | --lmcode-signatures\n"
      "       lmok status [--db PATH] [--init]\n"
      "       lmok index [--db PATH] [--force] [DIR...]\n"
      "       lmok symbols [PATTERN] [--lang L] [--kind K] [--db PATH]\n"
      "       lmok refs NAME [--db PATH]\n"
      "       lmok findings [--rule ID] [--file PATH] [--db PATH]\n"
      "       lmok parse [--lang json|c|python|bash|java|go|javascript|typescript|tsx|lmscript|lmcode] FILE\n"
      "       lmok query [--lang json|c|python|bash|java|go|javascript|typescript|tsx|lmscript|lmcode] [--pattern] PATTERN FILE\n"
      "       lmok ci [--config FILE] [--no-suppress] [--sarif FILE] [ROOT]\n"
      "       lmok rules PACK... | test PACK FIXTURE...\n"
      "       lmok lint RULES.json FILE|DIR... [--db PATH] [--no-suppress] [--sarif FILE] [--fix|--diff]\n"
      "               [--new-since RUN|--baseline FILE] [--fail-on-new] [--changed[=REV]]\n"
      "       lmok outline FILE|DIR... [--db PATH] [--budget N]\n"
      "       lmok facts [--lang json|c|python|bash|java|go|javascript|typescript|tsx|lmscript|lmcode] FILE|DIR...\n"
      "       lmok check [--lang json|c|python|bash|java|go|javascript|typescript|tsx|lmscript|lmcode] RULES.dslb FILE|DIR... [--no-suppress] [--sarif FILE]\n"
      "       lmok metrics [--lang json|c|python|bash|java|go|javascript|typescript|tsx|lmscript|lmcode] FILE|DIR...\n"
      "       lmok dup [--lang json|c|python|bash|java|go|javascript|typescript|tsx|lmscript|lmcode] [--tokens N] FILE|DIR...\n"
      "       lmok --lmcode FILE [--lmcode FILE ...]\n";
  if (argc>=2 && std::string(argv[1])=="rules") {
    if(argc<3){std::cerr<<"rules needs PACK...\n";return 2;}
    try {
      std::map<std::string,lmok::LintRule> catalog;
      for(int i=2;i<argc;++i)for(const auto &rule:lmok::load_rule_pack(argv[i])){
        const auto key=std::string(lmok::grammar_name(rule.grammar))+"/"+rule.id;
        auto found=catalog.find(key);
        if(found!=catalog.end())throw dslvm::Error("duplicate catalog rule "+key);
        catalog.emplace(key,rule);
      }
      for(const auto &item:catalog){const auto &r=item.second;std::cout<<"{\"id\":"<<lmok::meta_quote(r.id)<<",\"lang\":"<<lmok::meta_quote(lmok::grammar_name(r.grammar))<<",\"severity\":"<<lmok::meta_quote(r.severity)<<",\"metadata\":"<<r.metadata.json()<<"}\n";}
      return std::cout?0:4;
    }catch(const std::exception &e){std::cerr<<e.what()<<"\n";return 2;}
  }
  if(argc>=2 && std::string(argv[1])=="test"){
    if(argc<4){std::cerr<<"test needs PACK FIXTURE...\n";return 2;}
    try {
      const auto rules=lmok::load_rule_pack(argv[2]);
      if(rules.empty())throw dslvm::Error("test needs a non-empty pack");
      lmok::RuleTestResult total;
      for(int i=3;i<argc;++i){
        lmok::Grammar grammar=rules.front().grammar;
        grammar_for_extension(std::filesystem::u8path(argv[i]).extension().u8string(),grammar);
        std::vector<lmok::LintRule> active;for(const auto &r:rules)if(r.grammar==grammar)active.push_back(r);
        if(active.empty())throw dslvm::Error("test fixture has no rules for its language: "+std::string(argv[i]));
        const auto result=lmok::test_rule_file(active,grammar,argv[i],read_file(argv[i]));
        total.expected+=result.expected;total.actual+=result.actual;total.diff.insert(total.diff.end(),result.diff.begin(),result.diff.end());
      }
      for(const auto &d:total.diff)std::cerr<<d.file<<":"<<d.line<<": "<<d.kind<<" "<<d.rule<<"\n";
      std::cout<<lmok::rule_test_json(total)<<"\n";return std::cout?(total.diff.empty()?0:1):4;
    }catch(const std::exception &e){std::cerr<<e.what()<<"\n";return 2;}
  }
  if (argc >= 2 && std::string(argv[1]) == "ci") {
    std::string root = ".", config_path;
    std::optional<std::string> sarif_path;
    bool root_seen = false, no_suppress = false;
    for (int i = 2; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--no-suppress") no_suppress = true;
      else if (arg == "--config" && i + 1 < argc && config_path.empty()) config_path = argv[++i];
      else if (arg == "--sarif" && !sarif_path && i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0)
        sarif_path = argv[++i];
      else if (arg.rfind("--", 0) == 0 || root_seen) { std::cerr << usage; return 2; }
      else { root = arg; root_seen = true; }
    }
    if (!std::filesystem::is_directory(std::filesystem::u8path(root))) {
      std::cerr << "ci root must be a directory: " << root << "\n"; return 2;
    }
    if (config_path.empty()) config_path = (std::filesystem::u8path(root) / ".lmok" / "config.json").generic_u8string();
    try {
      const auto config = lmok::load_project_config(config_path);
      std::vector<lmok::SarifRuleMeta> sarif_rules;
      std::vector<lmok::SarifResult> sarif_results;
      lmok::SarifSink sarif_sink;
      if (sarif_path) { sarif_sink.rules = &sarif_rules; sarif_sink.results = &sarif_results; }
      const int rc = lmok::run_project_ci<Target>(config, root, no_suppress,
        [](const std::vector<std::string> &roots, std::vector<Target> &out, const std::vector<std::string> &ignore) {
          return expand_targets(roots, 0, out, nullptr, ignore);
        }, grammar_for_extension, read_file, json_escape, parse_lang, sarif_path ? &sarif_sink : nullptr, host.include_metadata);
      // The SARIF artifact is written once the run completed (even with
      // findings); rc 1 (threshold hit) still counts as a completed run.
      if (sarif_path && (rc == 0 || rc == 1)) {
        const int write_rc = write_sarif_file(*sarif_path,
          lmok::sarif_log(LMOK_VERSION, sarif_rules, sarif_results, json_escape));
        if (write_rc) return write_rc;
      }
      return rc;
    } catch (const std::exception &e) { std::cerr << e.what() << "\n"; return 2; }
  }
  if (argc >= 2 && std::string(argv[1]) == "status") {
    std::optional<std::string> path;
    bool init = false;
    for (int i = 2; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--db" && !path && i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
        path = argv[++i];
      } else if (arg == "--init" && !init) {
        init = true;
      } else { std::cerr << usage; return 2; }
    }
    try {
      const auto resolved = lmok::resolve_db_path(path, init);
      lmok::Database db(resolved, init ? lmok::Database::Mode::Create : lmok::Database::Mode::ReadOnly);
      std::cout << lmok::db_status(db, resolved) << std::flush;
      return std::cout ? 0 : 4;
    } catch (const lmok::DbError &error) {
      std::cerr << error.what() << "\n";
      return error.exit_code;
    } catch (const std::exception &error) {
      std::cerr << "DB error: " << error.what() << "\n";
      return 3;
    }
  }
  // Wave 2A: facts -> DB pipeline (docs/tier3/db-design.md §1/§5). Walks like
  // lint, invalidates by stat (mtime+size) then sha256, re-extracts each
  // changed file in ONE transaction, and reconciles vanished files under the
  // indexed roots afterwards.
  if (argc >= 2 && std::string(argv[1]) == "index") {
    std::optional<std::string> db_path;
    bool force = false;
    std::vector<std::string> roots;
    for (int i = 2; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--db" && !db_path && i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
        db_path = argv[++i];
      } else if (arg == "--force" && !force) {
        force = true;
      } else if (arg.empty() || arg.rfind("--", 0) == 0) {
        std::cerr << usage; return 2;
      } else roots.push_back(arg);
    }
    if (roots.empty()) roots.push_back(".");
    std::vector<Target> targets;
    if (const int rc = expand_targets(roots, 0, targets)) return rc;
    namespace fs = std::filesystem;
    try {
      const auto resolved = lmok::resolve_db_path(db_path, true);
      lmok::Database db(resolved, lmok::Database::Mode::Create);
      // `extracted` distinguishes a row written by `index` (facts + file_text
      // present) from one `lint --db` created only so findings have a file_id:
      // the latter has the real hash/stat but NO extraction, so "unchanged"
      // must never skip it (review finding: lint --db then index left files
      // silently un-indexed with "N unchanged").
      struct FileRow { int64_t id; std::string sha256; int64_t mtime; int64_t size; bool extracted; };
      std::map<std::string, FileRow> known;
      {
        auto q = db.prepare(lmok::index_files_sql);
        while (q.step())
          known.emplace(q.text(1), FileRow{q.integer(0), q.text(2), q.integer(3), q.integer(4), q.integer(5) != 0});
      }
      const int64_t now = static_cast<int64_t>(std::time(nullptr));
      int64_t n_indexed = 0, n_unchanged = 0;
      std::set<std::string> seen;
      for (const auto &target : targets) {
        const std::string &path = target.path;
        seen.insert(path);
        lmok::Grammar grammar;
        if (!grammar_for_extension(fs::u8path(path).extension().u8string(), grammar)) {
          std::cerr << "cannot infer the language of " << path << " from its extension\n";
          return 3;
        }
        std::error_code ec;
        const auto fsize = fs::file_size(fs::u8path(path), ec);
        if (ec) { std::cerr << "cannot stat " << path << ": " << ec.message() << "\n"; return 3; }
        const auto ftime = fs::last_write_time(fs::u8path(path), ec);
        if (ec) { std::cerr << "cannot stat " << path << ": " << ec.message() << "\n"; return 3; }
        // Preserve subsecond edits; old whole-second DB stamps miss this fast
        // path once and are refreshed by the content-hash check below.
        const int64_t mtime =
            std::chrono::duration_cast<std::chrono::nanoseconds>(ftime.time_since_epoch()).count();
        const auto it = known.find(path);
        const bool reusable = !force && it != known.end() && it->second.extracted;
        if (reusable && it->second.mtime == mtime &&
            it->second.size == static_cast<int64_t>(fsize)) { ++n_unchanged; continue; }
        std::string raw;
        try { raw = read_file(path); }
        catch (const dslvm::Error &e) { std::cerr << e.what() << "\n"; return 3; }
        const std::string hash = dslvm::hex(dslvm::sha256(raw));
        if (reusable && it->second.sha256 == hash) {
          // Stat changed but content did not: refresh the stat fields only.
          auto upd = db.prepare("UPDATE files SET mtime=?, size=?, indexed_at=? WHERE id=?");
          upd.bind(1, mtime); upd.bind(2, static_cast<int64_t>(fsize)); upd.bind(3, now);
          upd.bind(4, it->second.id); upd.step();
          ++n_unchanged; continue;
        }
        std::vector<lmok::Fact> facts;
        try {
          const lmok::Tree tree = lmok::parse(grammar, raw);
          facts = lmok::extract_facts(tree, grammar);
        } catch (const std::exception &e) {
          std::cerr << "index " << path << ": " << e.what() << "\n"; return 3;
        }
        const std::string lang = lmok::grammar_name(grammar);
        int64_t file_id = -1;
        {
          lmok::Transaction transaction(db);
          if (it != known.end()) {
            file_id = it->second.id;
            auto upd = db.prepare("UPDATE files SET lang=?, sha256=?, mtime=?, size=?, indexed_at=? WHERE id=?");
            upd.bind(1, lang); upd.bind(2, hash); upd.bind(3, mtime);
            upd.bind(4, static_cast<int64_t>(fsize)); upd.bind(5, now); upd.bind(6, file_id); upd.step();
          } else {
            auto ins = db.prepare("INSERT INTO files(path, lang, sha256, mtime, size, indexed_at) VALUES (?,?,?,?,?,?)");
            ins.bind(1, path); ins.bind(2, lang); ins.bind(3, hash); ins.bind(4, mtime);
            ins.bind(5, static_cast<int64_t>(fsize)); ins.bind(6, now); ins.step();
            auto idq = db.prepare("SELECT id FROM files WHERE path=?");
            idq.bind(1, path);
            if (!idq.step()) throw lmok::DbError(3, "index: lost files row for " + path);
            file_id = idq.integer(0);
          }
          const auto del_where = [&](const char *sql) {
            auto del = db.prepare(sql); del.bind(1, file_id); del.step();
          };
          del_where("DELETE FROM symbols WHERE file_id=?");
          del_where("DELETE FROM refs WHERE file_id=?");
          del_where("DELETE FROM edges WHERE from_file=?");
          auto del_text = db.prepare("DELETE FROM file_text WHERE path=?");
          del_text.bind(1, path); del_text.step();
          auto ins_sym = db.prepare("INSERT INTO symbols(file_id, name, kind, start_row, start_col, end_row, end_col, scope, confidence) VALUES (?,?,?,?,?,?,?,?,?)");
          auto ins_ref = db.prepare("INSERT INTO refs(file_id, name, kind, start_row, start_col, end_row, end_col, confidence) VALUES (?,?,?,?,?,?,?,?)");
          auto ins_edge = db.prepare("INSERT INTO edges(from_file, to_name, kind, resolved_file, confidence) VALUES (?,?,?,NULL,?)");
          for (const auto &f : facts) {
            if (f.kind.rfind("def.", 0) == 0) {
              ins_sym.bind(1, file_id); ins_sym.bind(2, f.name); ins_sym.bind(3, f.kind.substr(4));
              ins_sym.bind(4, f.row); ins_sym.bind(5, f.col); ins_sym.bind(6, f.end_row);
              ins_sym.bind(7, f.end_col); ins_sym.bind(8, f.scope); ins_sym.bind(9, f.confidence);
              ins_sym.step(); ins_sym.reset();
            } else if (f.kind.rfind("ref.", 0) == 0) {
              ins_ref.bind(1, file_id); ins_ref.bind(2, f.name); ins_ref.bind(3, f.kind.substr(4));
              ins_ref.bind(4, f.row); ins_ref.bind(5, f.col); ins_ref.bind(6, f.end_row);
              ins_ref.bind(7, f.end_col); ins_ref.bind(8, f.confidence);
              ins_ref.step(); ins_ref.reset();
              const std::string ek = edge_kind_of(f.kind);
              if (!ek.empty()) {
                ins_edge.bind(1, file_id); ins_edge.bind(2, f.name); ins_edge.bind(3, ek);
                ins_edge.bind(4, f.confidence); ins_edge.step(); ins_edge.reset();
              }
            }
          }
          auto ins_text = db.prepare("INSERT INTO file_text(path, lang, text) VALUES (?,?,?)");
          ins_text.bind(1, path); ins_text.bind(2, lang); ins_text.bind(3, raw); ins_text.step();
          transaction.commit();
        }
        ++n_indexed;
      }
      // Vanished files: previously indexed rows under an indexed directory
      // root that the walk did not produce this run. Rows from other roots
      // are left alone (indexing a subset must not drop the rest).
      int64_t n_removed = 0;
      {
        std::vector<std::string> prefixes;
        for (const auto &root : roots) {
          std::error_code ec;
          if (fs::is_directory(fs::u8path(root), ec)) {
            std::string prefix = canonical_path(root);
            if (prefix.empty() || prefix.back() != '/') prefix += "/";
            prefixes.push_back(prefix);
          }
        }
        lmok::Transaction transaction(db);
        for (const auto &[path, row] : known) {
          if (seen.count(path)) continue;
          bool under = false;
          for (const auto &prefix : prefixes)
            if (path.rfind(prefix, 0) == 0) { under = true; break; }
          if (!under) continue;
          auto del = db.prepare("DELETE FROM files WHERE id=?"); // CASCADE: symbols/refs/edges
          del.bind(1, row.id); del.step();
          auto del_text = db.prepare("DELETE FROM file_text WHERE path=?");
          del_text.bind(1, path); del_text.step();
          ++n_removed;
        }
        transaction.commit();
      }
      // Edge resolution (literal targets only): dir-of-source join first,
      // then a UNIQUE whole-DB suffix match; ambiguous or absent = NULL
      // (external/unresolved stays honest, never guessed).
      {
        std::map<std::string, int64_t> file_ids;
        std::map<int64_t, std::string> id_path;
        auto q = db.prepare("SELECT id, path FROM files");
        while (q.step()) { file_ids.emplace(q.text(1), q.integer(0)); id_path.emplace(q.integer(0), q.text(1)); }
        struct EdgeRow { int64_t id; int64_t from; std::string to; };
        std::vector<EdgeRow> edge_rows;
        auto eq = db.prepare("SELECT rowid, from_file, to_name FROM edges");
        while (eq.step()) edge_rows.push_back({eq.integer(0), eq.integer(1), eq.text(2)});
        lmok::Transaction transaction(db);
        auto upd = db.prepare("UPDATE edges SET resolved_file=? WHERE rowid=?");
        for (const auto &e : edge_rows) {
          const std::string &from = id_path[e.from];
          const size_t slash = from.rfind('/');
          const std::string dir = slash == std::string::npos ? "" : from.substr(0, slash);
          int64_t resolved_id = -1;
          const auto found = file_ids.find(normalize_join(dir, e.to));
          if (found != file_ids.end()) resolved_id = found->second;
          else {
            const std::string suffix = "/" + e.to;
            int64_t match = -1;
            int count = 0;
            for (const auto &[p, id] : file_ids)
              if (p.size() >= suffix.size() &&
                  p.compare(p.size() - suffix.size(), std::string::npos, suffix) == 0) { match = id; ++count; }
            if (count == 1) resolved_id = match;
          }
          if (resolved_id >= 0) upd.bind(1, resolved_id); else upd.bind_null(1);
          upd.bind(2, e.id); upd.step(); upd.reset();
        }
        transaction.commit();
      }
      std::cout << "indexed " << n_indexed << " files (" << n_unchanged << " unchanged, "
                << n_removed << " removed)\n";
      return std::cout ? 0 : 4;
    } catch (const lmok::DbError &error) {
      std::cerr << error.what() << "\n";
      return error.exit_code;
    } catch (const std::exception &error) {
      std::cerr << "index: " << error.what() << "\n";
      return 3;
    }
  }
  // Read verbs over the facts DB. Output shape mirrors `query`:
  // `name kind path start_row:start_col-end_row:end_col`, sorted path-then-row.
  if (argc >= 2 && (std::string(argv[1]) == "symbols" || std::string(argv[1]) == "refs")) {
    const bool is_refs = std::string(argv[1]) == "refs";
    std::optional<std::string> db_path, pattern, lang, kind;
    for (int i = 2; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--db" && !db_path && i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
        db_path = argv[++i];
      } else if (!is_refs && arg == "--lang" && !lang && i + 1 < argc &&
                 std::string(argv[i + 1]).rfind("--", 0) != 0) {
        lang = argv[++i];
      } else if (!is_refs && arg == "--kind" && !kind && i + 1 < argc &&
                 std::string(argv[i + 1]).rfind("--", 0) != 0) {
        kind = argv[++i];
      } else if (!arg.empty() && arg.rfind("--", 0) == 0) {
        std::cerr << usage; return 2;
      } else if (!pattern) pattern = arg;
      else { std::cerr << usage; return 2; }
    }
    if (is_refs && !pattern) { std::cerr << usage; return 2; }
    if (lang) {
      lmok::Grammar g;
      if (!parse_lang(*lang, g)) {
        std::cerr << "unknown language '" << *lang << "' (known: json, c, python, bash, java, go, javascript, typescript, tsx, lmscript, lmcode)\n";
        return 2;
      }
    }
    try {
      const auto resolved = lmok::resolve_db_path(db_path, false);
      lmok::Database db(resolved, lmok::Database::Mode::ReadOnly);
      std::string sql = is_refs
          ? "SELECT r.name, r.kind, f.path, r.start_row, r.start_col, r.end_row, r.end_col"
            " FROM refs r JOIN files f ON f.id = r.file_id WHERE r.name = ?"
          : "SELECT s.name, s.kind, f.path, s.start_row, s.start_col, s.end_row, s.end_col"
            " FROM symbols s JOIN files f ON f.id = s.file_id WHERE s.name LIKE ?";
      if (lang) sql += " AND f.lang = ?";
      if (kind && !is_refs) sql += " AND s.kind = ?";
      sql += is_refs ? " ORDER BY f.path, r.start_row, r.start_col, r.kind"
                     : " ORDER BY f.path, s.start_row, s.start_col, s.name, s.kind";
      auto q = db.prepare(sql);
      int at = 1;
      q.bind(at++, pattern ? *pattern : "%"); // refs requires NAME; symbols defaults to %
      if (lang) q.bind(at++, *lang);
      if (kind && !is_refs) q.bind(at++, *kind);
      while (q.step())
        std::cout << q.text(0) << " " << q.text(1) << " " << q.text(2) << " "
                  << q.integer(3) << ":" << q.integer(4) << "-" << q.integer(5) << ":" << q.integer(6) << "\n";
      return std::cout ? 0 : 4;
    } catch (const lmok::DbError &error) {
      std::cerr << error.what() << "\n";
      return error.exit_code;
    } catch (const std::exception &error) {
      std::cerr << "DB error: " << error.what() << "\n";
      return 3;
    }
  }
  if (argc == 2 && std::string(argv[1]) == "--version") {
    std::cout << "lmok " LMOK_VERSION "\n";
    return std::cout ? 0 : 4;
  }
  if (argc == 2 && std::string(argv[1]) == "--notices") {
    std::cout.write(reinterpret_cast<const char *>(lmok::embedded_notices),
                    static_cast<std::streamsize>(lmok::embedded_notices_size));
    return std::cout ? 0 : 4;
  }
  if (argc >= 2 && std::string(argv[1]) == "--lmcode-schema") {
    if (argc == 2) {
      print_schema_summary(reg);
      return std::cout ? 0 : 4;
    }
    if (argc == 3 && std::string(argv[2]) == "--json") {
      std::cout.write(reinterpret_cast<const char *>(lmok::embedded_schema),
                      static_cast<std::streamsize>(lmok::embedded_schema_size));
      return std::cout ? 0 : 4;
    }
    std::cerr << usage;
    return 2;
  }
  if (argc == 2 && std::string(argv[1]) == "--lmcode-signatures") {
    for (const auto &signature : reg.signatures()) std::cout << signature << "\n";
    return 0;
  }
  if (argc >= 2 && (std::string(argv[1]) == "parse" || std::string(argv[1]) == "query")) {
    const bool is_query = std::string(argv[1]) == "query";
    lmok::Grammar grammar = lmok::Grammar::Json;
    int argi = 2;
    const bool explicit_lang = argi < argc && std::string(argv[argi]) == "--lang";
    if (argi < argc && std::string(argv[argi]) == "--lang" && argi + 1 >= argc) {
      std::cerr << "--lang needs a value (json|c)\n" << usage;
      return 2;
    }
    if (argi + 1 < argc && std::string(argv[argi]) == "--lang") {
      const std::string lang = argv[argi + 1];
      if (lang == "json") grammar = lmok::Grammar::Json;
      else if (lang == "c") grammar = lmok::Grammar::C;
      else if (lang == "python") grammar = lmok::Grammar::Python;
      else if (lang == "bash") grammar = lmok::Grammar::Bash;
      else if (lang == "java") grammar = lmok::Grammar::Java;
      else if (lang == "go") grammar = lmok::Grammar::Go;
      else if (lang == "javascript") grammar = lmok::Grammar::Javascript;
      else if (lang == "typescript") grammar = lmok::Grammar::Typescript;
      else if (lang == "tsx") grammar = lmok::Grammar::Tsx;
      else if (lang == "lmscript") grammar = lmok::Grammar::Lmscript;
      else if (lang == "lmcode") grammar = lmok::Grammar::Lmcode;
      else {
        std::cerr << "unknown language '" << lang << "' (known: json, c, python, bash, java, go, javascript, typescript, tsx, lmscript, lmcode)\n" << usage;
        return 2;
      }
      argi += 2;
    }
    const char *pattern = nullptr;
    bool code_pattern = false; // --pattern: PATTERN is code-shaped; print the generated query, then the root matches
    lmok::CompiledPattern compiled;
    if (is_query) {
      if (argi < argc && std::string(argv[argi]) == "--pattern") { code_pattern = true; ++argi; }
      if (argi >= argc) { std::cerr << usage; return 2; }
      pattern = argv[argi++];
      if (code_pattern) {
        try { compiled = lmok::compile_pattern(grammar, pattern); }
        catch (const lmok::PatternError &e) { std::cerr << e.what() << "\n"; return 2; }
      }
    }
    if (argi != argc - 1) { std::cerr << usage; return 2; }
    std::string raw;
    const std::string path = argv[argi];
    if (!explicit_lang) {
      const auto ext = std::filesystem::u8path(path).extension().u8string();
      if (ext == ".lms" || ext == ".lmcode") grammar_for_extension(ext, grammar);
    }
    if (path == "-") {
      std::ostringstream data;
      data << std::cin.rdbuf();
      raw = data.str();
    } else {
      try { raw = read_file(path); }
      catch (const dslvm::Error &e) { std::cerr << e.what() << "\n"; return 3; }
    }
    lmok::Tree tree;
    try { tree = lmok::parse(grammar, raw); }
    catch (const std::length_error &e) { std::cerr << e.what() << "\n"; return 3; }
    if (!is_query) {
      std::cout << lmok::tree_sexp(tree) << "\n";
      return std::cout ? 0 : 4;
    }
    lmok::NodeList captures;
    try {
      if (code_pattern) {
        std::cout << "query: " << compiled.query << "\n";
        captures = lmok::pattern_matches(lmok::node_root(tree), compiled, grammar);
      } else {
        captures = lmok::node_query(lmok::node_root(tree), pattern, grammar);
      }
    } catch (const lmok::QueryError &e) {
      std::cerr << e.what() << "\n";
      return 3;
    }
    for (const auto &node : captures.items)
      std::cout << lmok::node_kind(node) << " "
                << lmok::node_start_row(node) << ":" << lmok::node_start_col(node)
                << "-" << lmok::node_end_row(node) << ":" << lmok::node_end_col(node) << "\n";
    return std::cout ? 0 : 4;
  }
  if (argc >= 2 && std::string(argv[1]) == "findings") {
    std::optional<std::string> db_path, rule, file;
    for (int i = 2; i < argc; ++i) {
      const std::string arg = argv[i];
      std::optional<std::string> *slot = nullptr;
      if (arg == "--db") slot = &db_path;
      else if (arg == "--rule") slot = &rule;
      else if (arg == "--file") slot = &file;
      if (!slot || *slot || i + 1 >= argc || std::string(argv[i + 1]).rfind("--", 0) == 0) {
        std::cerr << usage;
        return 2;
      }
      *slot = argv[++i];
    }
    try {
      // ReadOnly: a missing DB is exit 2 with "run `lmok index` first".
      const auto resolved = lmok::resolve_db_path(db_path);
      lmok::Database db(resolved, lmok::Database::Mode::ReadOnly);
      std::string sql =
          "SELECT f.start_row,f.start_col,f.end_row,f.end_col,f.rule,f.severity,f.message,files.path"
          " FROM findings f JOIN files ON files.id = f.file_id WHERE f.superseded = 0";
      if (rule) sql += " AND f.rule = ?";
      if (file) sql += " AND files.path = ?";
      sql += " ORDER BY files.path,f.start_row,f.start_col,f.end_row,f.end_col,f.rule";
      auto query = db.prepare(sql);
      int bind_at = 1;
      if (rule) query.bind(bind_at++, *rule);
      if (file) query.bind(bind_at++, *file);
      // Same JSON-lines shape lint prints, plus the one deliberate extension:
      // a "path" field, since findings outlive a single run.
      while (query.step())
        std::cout << "{\"row\":" << query.integer(0) << ",\"col\":" << query.integer(1)
                  << ",\"end_row\":" << query.integer(2) << ",\"end_col\":" << query.integer(3)
                  << ",\"rule\":\"" << json_escape(query.text(4))
                  << "\",\"severity\":\"" << json_escape(query.text(5))
                  << "\",\"message\":\"" << json_escape(query.text(6))
                  << "\",\"path\":\"" << json_escape(query.text(7)) << "\"}\n";
      return std::cout ? 0 : 4;
    } catch (const lmok::DbError &error) {
      std::cerr << error.what() << "\n";
      return error.exit_code;
    }
  }
  if (argc >= 2 && std::string(argv[1]) == "lint") {
    if (argc < 4) { std::cerr << usage; return 2; }
    std::vector<lmok::LintRule> rules;
    try {
      rules = lmok::load_rule_pack(argv[2]);
    } catch (const dslvm::Error &e) {
      std::cerr << e.what() << "\n";
      return 2;
    }
    // Flags may appear anywhere after the pack; the rest are targets.
    std::optional<std::string> db_path, sarif_path, baseline_path, new_since;
    bool no_suppress = false, fix_files = false, show_diff = false, changed = false, fail_on_new = false;
    std::string changed_rev = "HEAD"; // bare --changed: working tree vs HEAD
    std::vector<std::string> target_args;
    for (int i = 3; i < argc; ++i) {
      const std::string arg = argv[i];
      std::optional<std::string> *slot = nullptr;
      if (arg == "--db") slot = &db_path;
      else if (arg == "--sarif") slot = &sarif_path;
      else if (arg == "--baseline") slot = &baseline_path;
      else if (arg == "--new-since") slot = &new_since;
      if (slot) {
        if (*slot || i + 1 >= argc || std::string(argv[i + 1]).rfind("--", 0) == 0) {
          std::cerr << usage;
          return 2;
        }
        *slot = argv[++i];
      } else if (arg == "--fix") {
        fix_files = true;
      } else if (arg == "--diff") {
        show_diff = true;
      } else if (arg == "--no-suppress") {
        no_suppress = true;
      } else if (arg == "--changed" && !changed) {
        changed = true;
      } else if (arg.rfind("--changed=", 0) == 0 && !changed && arg.size() > 10) {
        changed = true;
        changed_rev = arg.substr(10);
      } else if (arg == "--fail-on-new" && !fail_on_new) {
        fail_on_new = true;
      } else {
        target_args.push_back(arg);
      }
    }
    if (target_args.empty()) { std::cerr << usage; return 2; }
    if (baseline_path && new_since) {
      std::cerr << "lint: --baseline and --new-since are mutually exclusive\n";
      return 2;
    }
    if (fail_on_new && !baseline_path && !new_since) {
      std::cerr << "lint: --fail-on-new needs --new-since or --baseline\n";
      return 2;
    }
    // Preview output has a distinct, text-free format. Refuse combinations
    // whose gate or SARIF results would otherwise be silently skipped.
    if (show_diff && (baseline_path || new_since || sarif_path || fail_on_new)) {
      std::cerr << "lint: --diff cannot be combined with baseline gating or --sarif\n";
      return 2;
    }
    const int64_t started_at = std::time(nullptr);
    std::string command;
    for (int i = 1; i < argc; ++i) {
      if (i > 1) command += ' ';
      command += argv[i];
    }
    // Pack paths are stable scan scopes, including paths containing spaces.
    const std::string baseline_scope = std::filesystem::absolute(
        std::filesystem::u8path(argv[2])).lexically_normal().generic_u8string();
    // The baseline (when asked for) loads BEFORE the run: with --new-since
    // the current run is recorded only at the end, so run_id <= RUN never
    // sees this run's own rows.
    const bool filtering = baseline_path.has_value() || new_since.has_value();
    lmok::Baseline baseline;
    if (baseline_path) {
      try { baseline = lmok::load_baseline_file(*baseline_path); }
      catch (const dslvm::Error &e) { std::cerr << e.what() << "\n"; return 3; }
    }
    if (new_since) {
      try {
        // The comparison DB is --db when given, else the standard resolution
        // chain; ReadOnly: a never-recorded DB is exit 2, not "all new".
        const auto resolved = lmok::resolve_db_path(db_path, false);
        lmok::Database db(resolved, lmok::Database::Mode::ReadOnly);
        const int64_t run_id = lmok::resolve_baseline_run(db, *new_since);
        baseline = lmok::load_baseline_db(db, run_id, lmok::db_has_column(db, "findings", "snippet"), baseline_scope);
      } catch (const lmok::DbError &e) {
        std::cerr << e.what() << "\n";
        return e.exit_code;
      } catch (const lmok::BaselineError &e) {
        std::cerr << e.what() << "\n";
        return 2;
      }
    }
    // --changed: keep only targets `git diff --name-only REV` reports for the
    // repository containing the cwd (bare --changed = REV HEAD, working tree
    // vs HEAD). Fail fast outside a repository (exit 2), like Semgrep.
    std::set<std::string> changed_files;
    if (changed) {
      try { changed_files = lmok::git_changed_files(changed_rev); }
      catch (const lmok::BaselineError &e) { std::cerr << e.what() << "\n"; return 2; }
    }
    // Per-grammar rule lists for extension-based filtering.
    std::vector<lmok::LintRule> c_rules, json_rules, python_rules, bash_rules, java_rules, go_rules, javascript_rules, typescript_rules, tsx_rules, lmscript_rules, lmcode_rules;
    for (const auto &rule : rules) {
      if (rule.grammar == lmok::Grammar::C) c_rules.push_back(rule);
      else if (rule.grammar == lmok::Grammar::Python) python_rules.push_back(rule);
      else if (rule.grammar == lmok::Grammar::Bash) bash_rules.push_back(rule);
      else if (rule.grammar == lmok::Grammar::Java) java_rules.push_back(rule);
      else if (rule.grammar == lmok::Grammar::Go) go_rules.push_back(rule);
      else if (rule.grammar == lmok::Grammar::Javascript) javascript_rules.push_back(rule);
      else if (rule.grammar == lmok::Grammar::Typescript) typescript_rules.push_back(rule);
      else if (rule.grammar == lmok::Grammar::Tsx) tsx_rules.push_back(rule);
      else if (rule.grammar == lmok::Grammar::Lmscript) lmscript_rules.push_back(rule);
      else if (rule.grammar == lmok::Grammar::Lmcode) lmcode_rules.push_back(rule);
      else json_rules.push_back(rule);
    }
    namespace fs = std::filesystem;
    std::vector<Target> targets;
    if (const int rc = expand_targets(target_args, 0, targets)) return rc;
    // With --db the run is recorded only after EVERY target linted cleanly: a
    // failed/interrupted scan must never look like a successful empty result,
    // so findings are collected per file and written in one transaction at
    // the end (docs/tier3/local-index-survey.md §4).
    std::vector<lmok::LintFileRecord> records;
    std::vector<lmok::SarifResult> sarif_results;
    int64_t n_new = 0, n_baseline = 0;
    // Fail fast: the first unreadable file aborts the whole run (exit 3);
    // earlier files' findings were already printed.
    for (const auto &target : targets) {
      if (changed) {
        const std::string absolute =
            fs::absolute(fs::u8path(target.path)).lexically_normal().generic_u8string();
        if (!changed_files.count(absolute)) continue;
      }
      const std::vector<lmok::LintRule> *active = &rules;
      const std::string ext = fs::u8path(target.path).extension().u8string();
      if (ext == ".c" || ext == ".h") active = &c_rules;
      else if (ext == ".json") active = &json_rules;
      else if (ext == ".java") active = &java_rules;
      else if (ext == ".py") active = &python_rules;
      else if (ext == ".sh" || ext == ".bash") active = &bash_rules;
      else if (ext == ".go") active = &go_rules;
      else if (ext == ".js") active = &javascript_rules;
      else if (ext == ".ts") active = &typescript_rules;
      else if (ext == ".tsx") active = &tsx_rules;
      else if (ext == ".lms") active = &lmscript_rules;
      else if (ext == ".lmcode") active = &lmcode_rules;
      // A file whose language has no rules in the pack: skipped silently when
      // it came from a directory; an explicitly named file still reports.
      if (active->empty() && target.from_dir) continue;
      std::string raw;
      try { raw = read_file(target.path); }
      catch (const dslvm::Error &e) { std::cerr << e.what() << "\n"; return 3; }
      lmok::Suppression suppression(raw);
      std::vector<lmok::LintFinding> findings;
      try {
        findings = lmok::lint_text(*active, raw, &suppression, no_suppress);
      } catch (const lmok::QueryError &e) {
        std::cerr << e.what() << "\n";
        return 3;
      } catch (const std::exception &e) { // oversized input, regex engine limits, allocation
        std::cerr << "lint " << target.path << ": " << e.what() << "\n";
        return 3;
      }
      if (fix_files || show_diff) {
        try {
          std::vector<lmok::FixEdit> edits;
          for (const auto &finding : findings) if (finding.fix) edits.push_back(*finding.fix);
          auto result = lmok::apply_fixes(raw, edits);
          if (result.fixed) {
            // Verify + re-lint once in memory before any file write. Reuse the
            // verifier's parsed trees, so the modified bytes are parsed once.
            lmok::Suppression after_suppression(result.text);
            auto after = lmok::lint_text(*active, result.text, &after_suppression, no_suppress, &result.verified_trees);
            if (show_diff) std::cout << lmok::unified_fix_diff(target.path, raw, result.text);
            else {
              lmok::write_fixed_file(target.path, raw, result.text);
              raw = std::move(result.text);
              findings = std::move(after);
              suppression = std::move(after_suppression);
            }
          }
          (show_diff ? std::cerr : std::cout) << result.fixed << " fixed in " << target.path << "\n";
        } catch (const std::exception &e) {
          std::cerr << "lint " << target.path << ": " << e.what() << "\n";
          return 3;
        }
      }
      if (show_diff) continue; // previews never write findings DB records
      std::set<std::string> known_ids;for(const auto &rule:rules)known_ids.insert(rule.id);
      suppression.warn(target.path,known_ids);
      int64_t printed = 0;
      // Preserve ordinary JSON bytes, but carry an escaped durable file key
      // when a human footer contains suppression metadata or ambiguous paths.
      const bool explicit_file = suppression.suppressed > 0 ||
          target.path.find_first_of("\r\n") != std::string::npos ||
          (target.path.size() >= 12 && target.path.compare(target.path.size() - 12, 12, " suppressed)") == 0);
      const auto sarif_source = sarif_path ? std::make_shared<const std::string>(raw) : nullptr;
      for (const auto &f : findings) {
        // Suppressed findings never reach this loop, so they can never become
        // "new". Under --new-since/--baseline only NEW findings print (the
        // baseline matches on rule/file/message/snippet, never positions);
        // the DB still records the unfiltered set below: recording is
        // reality, filtering is the view.
        const bool is_new = !filtering || !baseline.match(f.rule, target.path, f.message, f.snippet);
        if (!is_new) { ++n_baseline; continue; }
        ++n_new;
        ++printed;
        std::cout << "{\"row\":" << f.row << ",\"col\":" << f.col
                  << ",\"end_row\":" << f.end_row << ",\"end_col\":" << f.end_col
                  << ",\"rule\":\"" << json_escape(f.rule)
                  << "\",\"severity\":\"" << json_escape(f.severity)
                  << "\",\"message\":\"" << json_escape(f.message) << "\"";
        if (explicit_file) std::cout << ",\"file\":\"" << json_escape(target.path) << "\"";
        // Only safe digests cross the JSON-lines baseline boundary. Ordinary
        // findings retain their established byte format and legacy fallback.
        if (f.redacted)
          std::cout << ",\"snippet\":\"" << json_escape(f.snippet) << "\"";
        if (f.fix) std::cout << ",\"fix\":{\"start_byte\":" << f.fix->start_byte << ",\"end_byte\":" << f.fix->end_byte << ",\"text\":\"" << json_escape(f.fix->text) << "\"}";
        std::cout << lmok::metadata_suffix(f.metadata,host.include_metadata) << "}\n";
        if (sarif_path)
          sarif_results.push_back({f.rule, f.severity, f.message, target.path,
                                   f.row, f.col, f.end_row, f.end_col, f.metadata, sarif_source});
      }
      std::cout << printed << " findings in " << target.path << lmok::suppressed_suffix(suppression.suppressed) << "\n";
      if (db_path) {
        lmok::LintFileRecord record;
        record.path = target.path;
        lmok::Grammar file_grammar;
        record.lang = grammar_for_extension(ext, file_grammar) ? lmok::grammar_name(file_grammar) : "unknown";
        record.sha256 = dslvm::hex(dslvm::sha256(raw));
        record.size = static_cast<int64_t>(raw.size());
        std::error_code mtime_ec;
        const auto written = fs::last_write_time(fs::u8path(target.path), mtime_ec);
        if (!mtime_ec)
          record.mtime = std::chrono::duration_cast<std::chrono::nanoseconds>(written.time_since_epoch()).count();
        record.findings = std::move(findings);
        records.push_back(std::move(record));
      }
    }
    if (filtering) std::cout << n_new << " new, " << n_baseline << " baseline\n";
    if (sarif_path) {
      const std::string log = lmok::sarif_log(LMOK_VERSION, sarif_rules_of(rules), sarif_results, json_escape);
      if (const int rc = write_sarif_file(*sarif_path, log)) return rc;
    }
    if (db_path && !show_diff) {
      try {
        // Same resolution chain as `index` (docs/tier3/db-design.md §1): the
        // DB is CREATED when missing — lint --db is an explicit recording
        // request and a write operation, so automatic project/cache paths may
        // be created (explicit paths still require an existing parent).
        const auto resolved = lmok::resolve_db_path(db_path, /*write=*/true);
        lmok::Database db(resolved, lmok::Database::Mode::Create);
        lmok::record_lint_run(db, started_at, command, records, baseline_scope);
      } catch (const lmok::DbError &e) {
        // Recording is a side channel: the lint already succeeded and its
        // stdout/exit-code contract is unchanged; the DB problem is a note.
        std::cerr << "lint: run not recorded in DB: " << e.what() << "\n";
      } catch (const std::exception &e) {
        std::cerr << "lint: run not recorded in DB: " << e.what() << "\n";
      }
    }
    if (fail_on_new && n_new > 0) return 4;
    return std::cout ? 0 : 4;
  }
  if (argc >= 2 && (std::string(argv[1]) == "metrics" || std::string(argv[1]) == "dup")) {
    const bool duplication = std::string(argv[1]) == "dup";
    bool forced = false, tokens_seen = false;
    lmok::Grammar forced_grammar = lmok::Grammar::Json;
    int64_t min_tokens = 20;
    std::vector<std::string> inputs;
    for (int i = 2; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--lang" && !forced && i + 1 < argc) {
        if (!parse_lang(argv[++i], forced_grammar)) { std::cerr << usage; return 2; }
        forced = true;
      } else if (arg == "--tokens" && duplication && !tokens_seen && i + 1 < argc) {
        const std::string value = argv[++i];
        size_t used = 0;
        try { min_tokens = std::stoll(value, &used); }
        catch (const std::exception &) { std::cerr << usage; return 2; }
        if (used != value.size() || min_tokens <= 0) { std::cerr << "--tokens must be a positive integer\n"; return 2; }
        tokens_seen = true;
      } else if (arg.rfind("--", 0) == 0) { std::cerr << usage; return 2; }
      else inputs.push_back(arg);
    }
    if (inputs.empty()) { std::cerr << usage; return 2; }
    std::vector<Target> targets;
    if (const int rc = expand_targets(inputs, 0, targets, forced ? &forced_grammar : nullptr)) return rc;
    for (const auto &target : targets) {
      lmok::Grammar grammar = forced_grammar;
      if ((!forced || target.from_dir) && !grammar_for_extension(std::filesystem::u8path(target.path).extension().u8string(), grammar)) {
        std::cerr << "cannot infer the language of " << target.path << " from its extension (use --lang)\n"; return 2;
      }
      try {
        const auto raw = read_file(target.path);
        const auto tree = lmok::parse(grammar, raw);
        if (duplication) {
          for (const auto &duplicate : lmok::duplicate_windows(tree, min_tokens))
            std::cout << lmok::duplicate_json(target.path, duplicate) << "\n";
        } else std::cout << lmok::metrics_json(target.path, lmok::measure_file(tree, raw)) << "\n";
      } catch (const std::exception &error) {
        std::cerr << argv[1] << " " << target.path << ": " << error.what() << "\n"; return 3;
      }
    }
    return std::cout ? 0 : 4;
  }
  // Definitions always come from live facts. The optional DB is ranking only.
  if (argc >= 2 && std::string(argv[1]) == "outline") {
    std::optional<std::string> dbpath;
    size_t budget = std::numeric_limits<size_t>::max();
    std::vector<std::string> paths;
    bool positional = false;
    for (int i = 2; i < argc; ++i) {
      const std::string arg = argv[i];
      if (!positional && arg == "--") { positional = true; continue; }
      if (!positional && (arg == "--db" || arg == "--budget")) {
        if (++i == argc) { std::cerr << arg << " needs a value\n"; return 2; }
        if (arg == "--db") dbpath = argv[i];
        else {
          const std::string value = argv[i];
          const auto parsed = std::from_chars(value.data(), value.data() + value.size(), budget);
          if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || budget == 0) {
            std::cerr << "--budget needs a positive integer\n"; return 2;
          }
        }
      } else if (!positional && arg.rfind("--", 0) == 0) {
        std::cerr << "unknown outline option: " << arg << "\n"; return 2;
      } else paths.push_back(arg);
    }
    if (paths.empty()) { std::cerr << usage; return 2; }
    std::vector<Target> targets;
    if (const int rc = expand_targets(paths, 0, targets)) return rc;
    struct Block { std::string path; int64_t refs = 0; std::vector<std::string> lines; };
    std::vector<Block> blocks;
    std::string current;
    try {
      const auto resolved = lmok::resolve_db_path(dbpath, false);
      const bool ranked = dbpath.has_value() || std::filesystem::exists(resolved);
      std::map<std::string, int64_t> counts;
      if (ranked) {
        lmok::Database db(resolved);
        auto query = db.prepare("SELECT name, count(*) FROM refs GROUP BY name");
        while (query.step()) counts.emplace(query.text(0), query.integer(1));
      }
      std::set<std::string> seen;
      for (const auto &target : targets) {
        current = target.path;
        const auto path = std::filesystem::u8path(current).lexically_normal().generic_u8string();
        if (!seen.insert(path).second) continue;
        lmok::Grammar grammar;
        if (!grammar_for_extension(std::filesystem::u8path(path).extension().u8string(), grammar)) {
          std::cerr << "cannot infer the language of " << current << " from its extension\n"; return 2;
        }
        const auto raw = read_file(current);
        const auto tree = lmok::parse(grammar, raw);
        Block block{path, 0, {}};
        std::set<std::string> names;
        for (const auto &fact : lmok::extract_facts(tree, grammar)) {
          if (fact.kind.rfind("def.", 0) != 0) continue;
          names.insert(fact.name);
          const size_t begin = static_cast<size_t>(fact.start_byte);
          const size_t end = std::min(static_cast<size_t>(fact.end_byte), raw.find_first_of("\r\n", begin));
          std::string signature = raw.substr(begin, end - begin);
          while (!signature.empty() && (signature.back() == ' ' || signature.back() == '\t')) signature.pop_back();
          std::string line = "  " + json_escape(fact.name) + "  " + fact.kind.substr(4) + "  " +
              std::to_string(fact.row) + ":" + std::to_string(fact.col);
          if (!fact.scope.empty()) line += "  scope: " + json_escape(fact.scope);
          line += "  signature: " + json_escape(signature);
          block.lines.push_back(std::move(line));
        }
        if (block.lines.empty()) continue;
        for (const auto &name : names) {
          const auto found = counts.find(name);
          if (found != counts.end()) block.refs += found->second;
        }
        block.lines.insert(block.lines.begin(), json_escape(path) +
            (ranked ? " (refs: " + std::to_string(block.refs) + ")" : ""));
        blocks.push_back(std::move(block));
      }
      std::sort(blocks.begin(), blocks.end(), [](const Block &a, const Block &b) {
        return a.refs != b.refs ? a.refs > b.refs : a.path < b.path;
      });
      size_t total = 0;
      for (const auto &block : blocks) total += block.lines.size();
      const bool truncated = total > budget;
      size_t remaining = truncated ? budget - 1 : budget;
      size_t printed = 0;
      for (const auto &block : blocks) {
        if (block.lines.size() > remaining) break;
        for (const auto &line : block.lines) std::cout << line << '\n';
        remaining -= block.lines.size();
        ++printed;
      }
      if (truncated) std::cout << "... (" << blocks.size() - printed << " more files)\n";
    } catch (const lmok::DbError &error) {
      std::cerr << "outline: " << error.what() << "\n"; return error.exit_code;
    } catch (const std::exception &error) {
      std::cerr << "outline " << current << ": " << error.what() << "\n"; return 3;
    }
    return std::cout ? 0 : 4;
  }
  // E6: per-file facts as JSON Lines (one record per definition / reference /
  // edge; see cpp/facts.hpp and docs/tier3/deep-analysis.md §4). Shares
  // lint's target expansion. `--lang` forces the grammar of explicitly named
  // files and filters directory expansion to that language; without it the
  // extension decides, and an explicit file with an unknown extension is a
  // usage error.
  if (argc >= 2 && std::string(argv[1]) == "facts") {
    int argi = 2;
    bool forced = false;
    lmok::Grammar forced_grammar = lmok::Grammar::Json;
    if (argi < argc && std::string(argv[argi]) == "--lang") {
      if (argi + 1 >= argc) { std::cerr << "--lang needs a value (json|c|python|bash|java|go|javascript|typescript|tsx|lmscript|lmcode)\n" << usage; return 2; }
      if (!parse_lang(argv[argi + 1], forced_grammar)) {
        std::cerr << "unknown language '" << argv[argi + 1] << "' (known: json, c, python, bash, java, go, javascript, typescript, tsx, lmscript, lmcode)\n" << usage;
        return 2;
      }
      forced = true;
      argi += 2;
    }
    if (argi >= argc) { std::cerr << usage; return 2; }
    std::vector<Target> targets;
    if (const int rc = expand_targets(std::vector<std::string>(argv + argi, argv + argc), 0, targets,
                                      forced ? &forced_grammar : nullptr)) return rc;
    for (const auto &target : targets) {
      lmok::Grammar grammar = forced_grammar;
      if (!forced || target.from_dir) {
        const std::string ext = std::filesystem::u8path(target.path).extension().u8string();
        if (!grammar_for_extension(ext, grammar)) {
          std::cerr << "cannot infer the language of " << target.path << " from its extension (use --lang)\n";
          return 2;
        }
      }
      std::string raw;
      try { raw = read_file(target.path); }
      catch (const dslvm::Error &e) { std::cerr << e.what() << "\n"; return 3; }
      std::vector<lmok::Fact> facts;
      try {
        const lmok::Tree tree = lmok::parse(grammar, raw);
        facts = lmok::extract_facts(tree, grammar);
      } catch (const lmok::QueryError &e) {
        std::cerr << "facts " << target.path << ": " << e.what() << "\n";
        return 3;
      } catch (const std::exception &e) { // oversized input, allocation
        std::cerr << "facts " << target.path << ": " << e.what() << "\n";
        return 3;
      }
      const std::string lang = lmok::grammar_name(grammar);
      for (const auto &f : facts) std::cout << lmok::fact_json(f, target.path, lang, json_escape) << "\n";
    }
    return std::cout ? 0 : 4;
  }
  if (argc >= 2 && std::string(argv[1]) == "check") {
    try {
    std::vector<char *> check_argv;
    for(int i=0;i<argc;++i){if(i>=2 && std::string(argv[i])=="--no-suppress")host.no_suppress=true;else check_argv.push_back(argv[i]);}
    argc=static_cast<int>(check_argv.size());argv=check_argv.data();
    int argi = 2;
    lmok::Grammar grammar = lmok::Grammar::Json;
    if (argi + 1 < argc && std::string(argv[argi]) == "--lang") {
      const std::string lang = argv[argi + 1];
      if (!parse_lang(lang, grammar)) return dslvm::failure_frame("cli",dslvm::Error("CLI_USAGE","unknown check language: "+lang));
      argi += 2;
    }
    if (argi + 1 >= argc) return dslvm::failure_frame("cli",dslvm::Error("CLI_USAGE",usage));
    // --sarif FILE may appear once among the positionals; the rest are the
    // bytecode then its input files/dirs.
    std::optional<std::string> sarif_path;
    std::vector<std::string> check_inputs;
    for (int i = argi; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--sarif" && !sarif_path && i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
        sarif_path = argv[++i];
        continue;
      }
      if (arg.empty() || arg.rfind("--", 0) == 0) return dslvm::failure_frame("cli",dslvm::Error("CLI_USAGE",usage));
      check_inputs.push_back(arg);
    }
    if (check_inputs.size() < 2) return dslvm::failure_frame("cli",dslvm::Error("CLI_USAGE",usage));
    const std::string schema(reinterpret_cast<const char *>(lmok::embedded_schema), lmok::embedded_schema_size);
    auto results=dslvm::InvocationResults::from_command({check_inputs[0]});
    const auto fail_check=[&](const std::string& phase,const dslvm::Error& error){results.fail(0,phase,error);return results.write_frame(std::cout);};
    dslvm::Program program;
    try { program = dslvm::Program::load(read_bytecode(check_inputs[0]), schema, reg); }
    catch (const dslvm::Error &error) { return fail_check("load", error); }
    std::vector<Target> check_targets;
    if(const int rc=expand_targets(std::vector<std::string>(check_inputs.begin()+1,check_inputs.end()),0,check_targets,&grammar)){(void)rc;results.fail(std::nullopt,"run",dslvm::Error("RUN_HOST","cannot expand check inputs"));return results.write_frame(std::cout);}
    std::map<std::string, std::vector<size_t>> by_kind;
    for (size_t i = 0; i < program.entries().size(); ++i) {
      const auto &entry = program.entries()[i];
      if (entry.iface != "interface Rule(Node) -> void") {
        return fail_check("load", dslvm::Error("LOAD_BAD_BYTECODE", "check entry '" + entry.name + "' must implement Rule(Node) -> void"));
      }
      const std::string problem = lmok::annotation_problem(grammar, entry.annotation);
      if (!problem.empty()) {
        return fail_check("load", dslvm::Error("LOAD_UNBOUND", "check entry '" + entry.name + "': annotation '@" + entry.annotation + "' " + problem));
      }
      by_kind[entry.annotation].push_back(i); // duplicate kinds retain source order
    }
    dslvm::Context context;
    context.set("ts", &host);
    // One output group per findings sink: the JSON array, then `N findings in WHERE` (the same line `lint`
    // prints, WHERE = the input path as given, or `body`), then the success record. Body findings are their own
    // group, printed only when the body reported something (WP14 F2). When an entry fails mid-walk the group
    // collected so far is printed before the located error record and the run stops with exit 3 (WP14 F5/F6).
    const auto print_group = [&](const std::string &where) {
      host.suppression_warnings(where);
      std::cout << findings_json(host.findings,host.include_metadata) << "\n" << host.findings.size() << " findings in " << where << lmok::suppressed_suffix(host.suppressed_count()) << "\n";
    };
    const char *where = "body";
    std::vector<lmok::SarifResult> sarif_results;
    const auto collect_sarif = [&sarif_path, &sarif_results, &host](const std::string &uri, std::shared_ptr<const std::string> source = {}) {
      if (!sarif_path) return;
      for (const auto &f : host.findings)
        sarif_results.push_back({f.rule, f.severity, f.message, uri, f.row, f.col, f.end_row, f.end_col, f.metadata, source});
    };
    try {
      results.select(0,reg,dslvm::ResultContract::from_schema(schema)); // body + entries across inputs
      program.run(context, reg); // configuration/effects once, not once per node or file
      if (!host.findings.empty() || host.suppressed_count()) { collect_sarif(""); print_group("body"); }
      for (const auto &target : check_targets) {
        host.reset(); // transient body/file objects and findings do not escape an input
        where = target.path.c_str();
        const lmok::Tree tree = lmok::parse(grammar, read_file(target.path));
        host.suppressions.try_emplace(&tree, tree.source());
        lmok::walk_nodes(tree, [&](const lmok::Node &node, const std::vector<lmok::Node> &ancestors) {
          const auto found = by_kind.find(lmok::node_kind(node));
          if (found == by_kind.end()) return;
          // Record the ancestor chain up to the first already-known link: every ancestor is stored once per file.
          const lmok::Node *child = &node;
          for (size_t k = ancestors.size(); k-- > 0; child = &ancestors[k]) if (!host.remember_parent(*child, ancestors[k])) break;
          const Value handle = host.hold_node(node); // per-file arena; Tree stays alive
          for (size_t entry : found->second) program.call(entry, context, reg, {handle});
        });
        collect_sarif(target.path, sarif_path ? std::make_shared<const std::string>(tree.source()) : nullptr);
        print_group(where);
        std::cout << "{\"host_summary\":{\"input\":" << dslvm::json_escape(where) << "}}\n";
        if (!std::cout) return 4;
      }
    } catch (const dslvm::Error &error) {
      if (where != std::string("body") || !host.findings.empty() || host.suppressed_count()) print_group(where);
      return fail_check("run", error);
    } catch (const std::exception &error) {
      if (where != std::string("body") || !host.findings.empty() || host.suppressed_count()) print_group(where);
      return fail_check("run", dslvm::Error("RUN_HOST", error.what()));
    }
    if (sarif_path) {
      // driver.rules: distinct rule ids in first-seen order, derived from the
      // results — lmcode rules carry no pack, and the example script derives
      // its catalog the same way.
      std::vector<lmok::SarifRuleMeta> sarif_rules;
      std::map<std::string,size_t> seen_ids;
      for (const auto &f : sarif_results) if(!f.rule.empty()) {
        const auto inserted=seen_ids.emplace(f.rule,sarif_rules.size());
        if(inserted.second)sarif_rules.push_back({f.rule,"rule "+f.rule,"",f.metadata});
        else if(sarif_rules[inserted.first->second].metadata.empty())
          sarif_rules[inserted.first->second].metadata=f.metadata; // first declared metadata; each result retains its own
      }
      if (const int rc = write_sarif_file(*sarif_path,
                                          lmok::sarif_log(LMOK_VERSION, sarif_rules, sarif_results, json_escape)))
        {results.fail(std::nullopt,"app",dslvm::Error("APP_FAIL","cannot write SARIF"));return results.write_frame(std::cout);}
    }
    try{results.complete(0);}catch(const dslvm::Error& e){results.fail(0,"run",e);}
    return results.write_frame(std::cout);
    }catch(const dslvm::Error& error){return dslvm::failure_frame("cli",error);}
  }
  if (argc >= 2 && (std::string(argv[1]) == "--lmcode"||std::string(argv[1])=="--first"||std::string(argv[1])=="--last")) {
    std::vector<std::string> files;std::vector<int> priorities;
    for(int i=1;i<argc;i+=2){std::string flag=argv[i];if(i+1>=argc||(flag!="--lmcode"&&flag!="--first"&&flag!="--last"))return dslvm::failure_frame("cli",dslvm::Error("CLI_USAGE",usage));files.emplace_back(argv[i+1]);priorities.push_back(flag=="--first"?0:flag=="--last"?2:1);}
    try{
      auto results=dslvm::InvocationResults::from_command(files);
      const std::string schema(reinterpret_cast<const char*>(lmok::embedded_schema),lmok::embedded_schema_size);
      const auto contract=dslvm::ResultContract::from_schema(schema);
      dslvm::Context context;context.set("ts",&host);
      std::vector<size_t> order;for(size_t i=0;i<files.size();++i)order.push_back(i);
      std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b){return priorities[a]<priorities[b];});
      for(size_t position:order){const auto& part=results.registrations()[position];std::string phase="load";
        try{auto program=dslvm::Program::load(read_bytecode(part.file),schema,reg);
          results.select(part.index,reg,contract);phase="run";program.run(context,reg);
          results.complete(part.index);
          std::cout<<"{\"host_summary\":{\"part\":"<<part.index<<"}}\n";
        }catch(const dslvm::Error& error){results.fail(part.index,phase,error);break;}
        catch(const std::exception& error){results.fail(part.index,phase,dslvm::Error("RUN_HOST",error.what()));break;}
      }
      return results.write_frame(std::cout);
    }catch(const dslvm::Error& error){return dslvm::failure_frame("cli",error);}
  }

  std::cerr << usage;
  return 2;
}

#ifdef _WIN32
// The narrow CRT argv uses the Windows ANSI code page and loses Unicode paths.
// Keep the CLI/DSL boundary UTF-8, converting only at the native entry point.
int wmain(int argc, wchar_t **wide) {
  std::vector<std::string> text;
  text.reserve(argc);
  for (int i = 0; i < argc; ++i) {
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, nullptr, 0, nullptr, nullptr);
    if (size == 0) { std::cerr << "invalid Unicode command argument\n"; return 2; }
    std::string value(static_cast<size_t>(size), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, value.data(), size, nullptr, nullptr)) {
      std::cerr << "invalid Unicode command argument\n"; return 2;
    }
    value.pop_back();
    text.push_back(std::move(value));
  }
  std::vector<char *> argv;
  argv.reserve(static_cast<size_t>(argc) + 1);
  for (auto &value : text) argv.push_back(value.data());
  argv.push_back(nullptr);
  return run_cli(argc, argv.data());
}
#else
int main(int argc, char **argv) { return run_cli(argc, argv); }
#endif
