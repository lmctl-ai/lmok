// lmok wrapper over tree-sitter. The only translation unit that includes
// tree-sitter headers; see host.hpp for the interface contract.
#include "host.hpp"
#include <array>
#include <cmath>
#include "bounded_regex.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <regex>
#include <tree_sitter/api.h>

extern "C" const TSLanguage *tree_sitter_json(void);
extern "C" const TSLanguage *tree_sitter_c(void);
extern "C" const TSLanguage *tree_sitter_python(void);
extern "C" const TSLanguage *tree_sitter_bash(void);
extern "C" const TSLanguage *tree_sitter_java(void);
extern "C" const TSLanguage *tree_sitter_go(void);
extern "C" const TSLanguage *tree_sitter_javascript(void);
extern "C" const TSLanguage *tree_sitter_typescript(void);
extern "C" const TSLanguage *tree_sitter_tsx(void);

extern "C" const TSLanguage *tree_sitter_lmscript(void);
extern "C" const TSLanguage *tree_sitter_lmcode(void);

namespace lmok {

static const TSLanguage *language_of(Grammar grammar) {
  switch (grammar) {
  case Grammar::Json: return tree_sitter_json();
  case Grammar::C: return tree_sitter_c();
  case Grammar::Python: return tree_sitter_python();
  case Grammar::Bash: return tree_sitter_bash();
  case Grammar::Java: return tree_sitter_java();
  case Grammar::Go: return tree_sitter_go();
  case Grammar::Javascript: return tree_sitter_javascript();
  case Grammar::Typescript: return tree_sitter_typescript();
  case Grammar::Tsx: return tree_sitter_tsx();
  case Grammar::Lmscript: return tree_sitter_lmscript();
  case Grammar::Lmcode: return tree_sitter_lmcode();
  }
  return tree_sitter_json();
}

static_assert(sizeof(TSNode) <= sizeof(Node::bytes) && alignof(TSNode) <= alignof(void *),
              "Node::bytes must hold a TSNode");

static const TSNode &ts(const Node &node) { return *reinterpret_cast<const TSNode *>(node.bytes); }

const void *tree_handle(const Tree &tree) { return tree.tree_; }

Tree::~Tree() {
  if (tree_) ts_tree_delete(static_cast<TSTree *>(tree_));
}

Tree::Tree(Tree &&other) noexcept
    : tree_(other.tree_), source_(std::move(other.source_)),
      has_pending_edit_(other.has_pending_edit_), edit_start_(other.edit_start_),
      edit_old_end_(other.edit_old_end_), edit_new_end_(other.edit_new_end_) {
  other.tree_ = nullptr;
  other.has_pending_edit_ = false;
}

Tree &Tree::operator=(Tree &&other) noexcept {
  if (this != &other) {
    if (tree_) ts_tree_delete(static_cast<TSTree *>(tree_));
    tree_ = other.tree_;
    source_ = std::move(other.source_);
    has_pending_edit_ = other.has_pending_edit_;
    edit_start_ = other.edit_start_;
    edit_old_end_ = other.edit_old_end_;
    edit_new_end_ = other.edit_new_end_;
    other.tree_ = nullptr;
    other.has_pending_edit_ = false;
  }
  return *this;
}

static void check_text_size(const char *who, const std::string &text) {
  if (text.size() > static_cast<size_t>(UINT32_MAX))
    throw std::length_error(std::string(who) + ": input is " + std::to_string(text.size()) +
                            " bytes; tree-sitter addresses at most 4294967295");
}

const char *grammar_name(Grammar grammar) {
  switch (grammar) {
  case Grammar::Json: return "json";
  case Grammar::C: return "c";
  case Grammar::Python: return "python";
  case Grammar::Bash: return "bash";
  case Grammar::Java: return "java";
  case Grammar::Go: return "go";
  case Grammar::Javascript: return "javascript";
  case Grammar::Typescript: return "typescript";
  case Grammar::Tsx: return "tsx";
  case Grammar::Lmscript: return "lmscript";
  case Grammar::Lmcode: return "lmcode";
  }
  return "json";
}

std::string annotation_problem(Grammar grammar, const std::string &kind) {
  const TSLanguage *language = language_of(grammar);
  const std::string suffix = std::string(" of the ") + grammar_name(grammar) + " grammar";
  bool visible = false, supertype = false, hidden = false;
  for (bool named : {true, false}) {
    const TSSymbol symbol = ts_language_symbol_for_name(language, kind.data(), uint32_t(kind.size()), named);
    if (symbol == 0) continue;
    switch (ts_language_symbol_type(language, symbol)) {
    case TSSymbolTypeRegular: case TSSymbolTypeAnonymous: visible = true; break;
    case TSSymbolTypeSupertype: supertype = true; break;
    case TSSymbolTypeAuxiliary: hidden = true; break;
    }
  }
  if (visible) return "";
  if (supertype) return "is a supertype" + suffix + " (abstract: annotate its concrete kinds, see `lmok parse`)";
  if (hidden) return "is a hidden symbol" + suffix + " and never appears as a node";
  return "is not a node kind" + suffix + " (see `lmok parse` for the kinds in a tree)";
}

Tree parse(Grammar grammar, const std::string &text) {
  check_text_size("parse", text);
  TSParser *parser = ts_parser_new();
  ts_parser_set_language(parser, language_of(grammar));
  Tree result;
  result.tree_ = ts_parser_parse_string(parser, nullptr, text.data(),
                                        static_cast<uint32_t>(text.size()));
  result.source_ = text;
  ts_parser_delete(parser);
  return result;
}

Tree parse_range(Grammar grammar, const Node &body) {
  const TSNode &node=ts(body);
  const TSRange range{ts_node_start_point(node),ts_node_end_point(node),ts_node_start_byte(node),ts_node_end_byte(node)};
  TSParser *parser=ts_parser_new();
  ts_parser_set_language(parser,language_of(grammar));
  if(!ts_parser_set_included_ranges(parser,&range,1)){ts_parser_delete(parser);throw std::runtime_error("invalid injection range");}
  Tree result;
  result.source_=body.owner->source();
  result.tree_=ts_parser_parse_string(parser,nullptr,result.source_.data(),static_cast<uint32_t>(result.source_.size()));
  ts_parser_delete(parser);
  return result;
}

std::vector<Tree> lmcode_injections(const Tree &outer) {
  std::vector<Tree> trees;
  for(const auto &node:tree_nodes(outer).items)
    if(node_kind(node)=="lmcode_body")trees.push_back(parse_range(Grammar::Lmcode,node));
  return trees;
}

std::vector<Tree> analysis_trees(Grammar grammar,const std::string &text,bool lmscript_container) {
  if(grammar==Grammar::Lmcode && lmscript_container){const auto outer=parse(Grammar::Lmscript,text);return lmcode_injections(outer);}
  std::vector<Tree> trees;trees.push_back(parse(grammar,text));return trees;
}

static TSPoint point_for_byte(const std::string &source, uint32_t byte) {
  TSPoint point = {0, 0};
  const uint32_t limit = std::min<uint32_t>(byte, source.size());
  for (uint32_t i = 0; i < limit; ++i) {
    if (source[i] == '\n') { ++point.row; point.column = 0; }
    else ++point.column;
  }
  return point;
}

void tree_edit(Tree &tree, int64_t start_byte, int64_t old_end_byte, int64_t new_end_byte) {
  if (tree.has_pending_edit_)
    throw std::out_of_range("Tree.edit: one pending edit per reparse");
  const int64_t size = static_cast<int64_t>(tree.source_.size());
  if (start_byte < 0 || old_end_byte < start_byte || old_end_byte > size || new_end_byte < start_byte)
    throw std::out_of_range("Tree.edit: bad byte offsets (need 0 <= start <= old_end <= " +
                            std::to_string(size) + " and new_end >= start)");
  tree.has_pending_edit_ = true;
  tree.edit_start_ = start_byte;
  tree.edit_old_end_ = old_end_byte;
  tree.edit_new_end_ = new_end_byte;
}

Tree reparse(Grammar grammar, Tree &old_tree, const std::string &new_text) {
  check_text_size("reparse", new_text);
  if (!old_tree.has_pending_edit_) {
    // Without an edit tree-sitter reuses every old subtree and hands back the
    // old structure whatever the new text says. Identical text is the only
    // honest no-edit reparse.
    if (new_text != old_tree.source_)
      throw std::out_of_range("Tree.reparse: no pending edit for changed text (call Tree.edit first)");
  } else {
    const int64_t expected = static_cast<int64_t>(old_tree.source_.size()) -
                             (old_tree.edit_old_end_ - old_tree.edit_start_) +
                             (old_tree.edit_new_end_ - old_tree.edit_start_);
    if (static_cast<int64_t>(new_text.size()) != expected)
      throw std::out_of_range("Tree.reparse: new text is " + std::to_string(new_text.size()) +
                              " bytes but the pending edit implies " + std::to_string(expected) +
                              " (old " + std::to_string(old_tree.source_.size()) + " - (" +
                              std::to_string(old_tree.edit_old_end_) + " - " + std::to_string(old_tree.edit_start_) +
                              ") + (" + std::to_string(old_tree.edit_new_end_) + " - " +
                              std::to_string(old_tree.edit_start_) + "))");
  }
  if (old_tree.has_pending_edit_) {
    TSInputEdit edit;
    edit.start_byte = static_cast<uint32_t>(old_tree.edit_start_);
    edit.old_end_byte = static_cast<uint32_t>(old_tree.edit_old_end_);
    edit.new_end_byte = static_cast<uint32_t>(old_tree.edit_new_end_);
    edit.start_point = point_for_byte(old_tree.source_, edit.start_byte);
    edit.old_end_point = point_for_byte(old_tree.source_, edit.old_end_byte);
    edit.new_end_point = point_for_byte(new_text, edit.new_end_byte);
    ts_tree_edit(static_cast<TSTree *>(old_tree.tree_), &edit);
    old_tree.has_pending_edit_ = false;
  }
  TSParser *parser = ts_parser_new();
  ts_parser_set_language(parser, language_of(grammar));
  Tree result;
  result.tree_ = ts_parser_parse_string(parser, static_cast<TSTree *>(old_tree.tree_),
                                        new_text.data(), static_cast<uint32_t>(new_text.size()));
  result.source_ = new_text;
  ts_parser_delete(parser);
  return result;
}

std::string changed_ranges_json(const Tree &old_tree, const Tree &new_tree) {
  uint32_t count = 0;
  TSRange *ranges = ts_tree_get_changed_ranges(
      static_cast<const TSTree *>(old_tree.tree_), static_cast<const TSTree *>(new_tree.tree_), &count);
  std::string out = "[";
  for (uint32_t i = 0; i < count; ++i) {
    if (i) out += ",";
    out += "[" + std::to_string(ranges[i].start_byte) + "," + std::to_string(ranges[i].end_byte) + "]";
  }
  std::free(ranges);
  return out + "]";
}

static TSNode root(const Tree &tree) {
  return ts_tree_root_node(static_cast<TSTree *>(const_cast<void *>(tree_handle(tree))));
}

std::string tree_sexp(const Tree &tree) {
  char *text = ts_node_string(root(tree));
  std::string out = text ? text : "";
  std::free(text);
  return out;
}

bool tree_has_error(const Tree &tree) { return ts_node_has_error(root(tree)); }

int64_t tree_node_count(const Tree &tree) {
  return static_cast<int64_t>(ts_node_descendant_count(root(tree))) + 1;
}

Node node_root(const Tree &tree) {
  Node node;
  node.owner = &tree;
  new (node.bytes) TSNode(root(tree));
  return node;
}

NodeList tree_nodes(const Tree &tree) {
  NodeList list;
  std::vector<TSNode> stack;
  stack.push_back(root(tree));
  while (!stack.empty()) {
    const TSNode current = stack.back();
    stack.pop_back();
    Node node;
    node.owner = &tree;
    new (node.bytes) TSNode(current);
    list.items.push_back(node);
    const uint32_t count = ts_node_named_child_count(current);
    for (uint32_t i = count; i > 0; --i)
      stack.push_back(ts_node_named_child(current, i - 1));
  }
  return list;
}

void walk_nodes(const Tree &tree, const std::function<void(const Node &, const std::vector<Node> &)> &visit) {
  struct Cursor {
    TSTreeCursor value;
    ~Cursor() { ts_tree_cursor_delete(&value); }
  } cursor{ts_tree_cursor_new(root(tree))};
  std::vector<Node> ancestors; // root..parent of the current node, maintained by the cursor moves
  for (;;) {
    Node node;
    node.owner = &tree;
    new (node.bytes) TSNode(ts_tree_cursor_current_node(&cursor.value));
    visit(node, ancestors); // do not filter named nodes: @if denotes an anonymous token
    if (ts_tree_cursor_goto_first_child(&cursor.value)) { ancestors.push_back(node); continue; }
    while (!ts_tree_cursor_goto_next_sibling(&cursor.value)) {
      if (!ts_tree_cursor_goto_parent(&cursor.value)) return;
      ancestors.pop_back();
    }
  }
}

std::string node_kind(const Node &node) { return ts_node_type(ts(node)); }

std::string node_text(const Node &node) {
  const std::string &source = node.owner->source_;
  const uint32_t start = ts_node_start_byte(ts(node));
  const uint32_t end = ts_node_end_byte(ts(node));
  if (end <= start || start >= source.size()) return "";
  return source.substr(start, std::min<uint32_t>(end, source.size()) - start);
}

bool node_is_named(const Node &node) { return ts_node_is_named(ts(node)); }
bool node_is_missing(const Node &node) { return ts_node_is_missing(ts(node)); }
const void *node_id(const Node &node) { return ts(node).id; }

int64_t node_start_row(const Node &node) { return ts_node_start_point(ts(node)).row; }
int64_t node_start_col(const Node &node) { return ts_node_start_point(ts(node)).column; }
int64_t node_end_row(const Node &node) { return ts_node_end_point(ts(node)).row; }
int64_t node_end_col(const Node &node) { return ts_node_end_point(ts(node)).column; }
int64_t node_start_byte(const Node &node) { return ts_node_start_byte(ts(node)); }
int64_t node_end_byte(const Node &node) { return ts_node_end_byte(ts(node)); }

int64_t node_child_count(const Node &node) {
  return static_cast<int64_t>(ts_node_named_child_count(ts(node)));
}

Node node_child(const Node &node, int64_t index) {
  Node child;
  child.owner = node.owner;
  new (child.bytes) TSNode(ts_node_named_child(ts(node), static_cast<uint32_t>(index)));
  return child;
}

bool node_has_parent(const Node &node) {
  return !ts_node_is_null(ts_node_parent(ts(node)));
}

Node node_parent(const Node &node) {
  Node parent;
  parent.owner = node.owner;
  new (parent.bytes) TSNode(ts_node_parent(ts(node)));
  return parent;
}

static const char *query_error_word(TSQueryError error) {
  switch (error) {
  case TSQueryErrorSyntax: return "syntax";
  case TSQueryErrorNodeType: return "node-type";
  case TSQueryErrorField: return "field";
  case TSQueryErrorCapture: return "capture";
  case TSQueryErrorStructure: return "structure";
  case TSQueryErrorLanguage: return "language";
  default: return "unknown";
  }
}

// ---- query predicates (E1) -------------------------------------------------
// Compiled once per node_query call (one entry per pattern), evaluated per
// match following upstream tree-sitter (lib/binding_rust/lib.rs,
// satisfies_text_predicates, and lib/binding_web/src/query.ts for the
// every/some reading of plain vs any-* forms): eq/match/any-of text
// predicates, capture-vs-capture eq (every/some node of the first capture
// equal to some node of the second), regex UNANCHORED (regex::is_match). Property predicates (#is?/#is-not?) and
// directives (#set!, any `!`) are accepted and ignored; malformed known
// predicates and unknown `?` predicates are refused (upstream refuses the
// former at query compile time; for the latter upstream merely stores them,
// but a silently ignored predicate here would over-match).
bool regex_search_bounded(const std::string &text, const std::regex &re) {
  if (text.size() > kRegexTextCap) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      std::fprintf(stderr,
                   "lmok: regex not applied to a %zu-byte node text (limit %zu bytes: "
                   "std::regex recursion overflows the stack on long text); treated as no match\n",
                   text.size(), kRegexTextCap);
    }
    return false;
  }
  return bounded_regex_search(text, re); // work-budgeted; may throw RegexLimit
}

namespace {

struct RaiiQuery {
  TSQuery *q;
  ~RaiiQuery() { if (q) ts_query_delete(q); }
};
struct RaiiCursor {
  TSQueryCursor *c;
  ~RaiiCursor() { if (c) ts_query_cursor_delete(c); }
};

struct Predicate {
  enum Kind { Eq, Match, AnyOf, Entropy } kind = Eq;
  bool positive = true;
  bool match_all = true;
  uint32_t capture = 0;          // first argument (always a capture)
  bool other_is_capture = false; // Eq: second argument kind
  uint32_t other_capture = 0;
  std::string operand;           // Eq literal
  std::vector<std::string> options; // AnyOf literals
  std::regex re;                 // Match
  double threshold = 0;         // Entropy, bits per byte
};

std::string string_value(const TSQuery *query, uint32_t id) {
  uint32_t len = 0;
  const char *p = ts_query_string_value_for_id(query, id, &len);
  return std::string(p, len);
}

std::string capture_name(const TSQuery *query, uint32_t id) {
  uint32_t len = 0;
  const char *p = ts_query_capture_name_for_id(query, id, &len);
  return std::string(p, len);
}

using PatternProperties = std::vector<std::map<std::string, std::string>>;

std::vector<std::vector<Predicate>> compile_predicates(const TSQuery *query, PatternProperties *properties = nullptr) {
  const uint32_t patterns = ts_query_pattern_count(query);
  std::vector<std::vector<Predicate>> out(patterns);
  if (properties) properties->assign(patterns, {});
  for (uint32_t pattern = 0; pattern < patterns; ++pattern) {
    uint32_t step_count = 0;
    const TSQueryPredicateStep *steps = ts_query_predicates_for_pattern(query, pattern, &step_count);
    uint32_t i = 0;
    while (i < step_count) {
      // Each predicate is: String(operator) args... Done. The C API carries
      // no operator field; the first string step is the operator name.
      if (steps[i].type != TSQueryPredicateStepTypeString) {
        while (i < step_count && steps[i].type != TSQueryPredicateStepTypeDone) ++i;
        ++i;
        continue;
      }
      const std::string op = string_value(query, steps[i].value_id);
      ++i;
      std::vector<const TSQueryPredicateStep *> args;
      while (i < step_count && steps[i].type != TSQueryPredicateStepTypeDone) args.push_back(&steps[i++]);
      ++i; // past Done
      const auto fail = [&](const std::string &what) {
        throw QueryError("query predicate error in pattern " + std::to_string(pattern) + ": #" + op + " " + what);
      };
      const auto is_capture = [](const TSQueryPredicateStep *s) { return s->type == TSQueryPredicateStepTypeCapture; };
      if (op.empty()) fail("has no operator name");
      if (op == "set!" && properties && args.size() == 2 && !is_capture(args[0]) && !is_capture(args[1])) {
        (*properties)[pattern][string_value(query, args[0]->value_id)] = string_value(query, args[1]->value_id);
        continue;
      }
      if (op.back() == '!') continue; // other directives: accepted, ignored
      if (op == "is?" || op == "is-not?") continue; // property predicates: not evaluated here (nor by the tree-sitter CLI)
      Predicate p;
      if (op == "eq?" || op == "not-eq?" || op == "any-eq?" || op == "any-not-eq?") {
        if (args.size() != 2) fail("takes 2 arguments, got " + std::to_string(args.size()));
        if (!is_capture(args[0])) fail("first argument must be a capture, got literal \"" + string_value(query, args[0]->value_id) + "\"");
        p.kind = Predicate::Eq;
        p.positive = op == "eq?" || op == "any-eq?";
        p.match_all = op == "eq?" || op == "not-eq?";
        p.capture = args[0]->value_id;
        p.other_is_capture = is_capture(args[1]);
        if (p.other_is_capture) p.other_capture = args[1]->value_id;
        else p.operand = string_value(query, args[1]->value_id);
      } else if (op == "match?" || op == "not-match?" || op == "any-match?" || op == "any-not-match?") {
        if (args.size() != 2) fail("takes 2 arguments, got " + std::to_string(args.size()));
        if (!is_capture(args[0])) fail("first argument must be a capture, got literal \"" + string_value(query, args[0]->value_id) + "\"");
        if (is_capture(args[1])) fail("second argument must be a regex literal, got capture @" + capture_name(query, args[1]->value_id));
        p.kind = Predicate::Match;
        p.positive = op == "match?" || op == "any-match?";
        p.match_all = op == "match?" || op == "not-match?";
        p.capture = args[0]->value_id;
        const std::string source = string_value(query, args[1]->value_id);
        try { p.re = bounded_regex(source); }
        catch (const std::length_error &e) { fail(e.what()); }
        catch (const std::regex_error &) { fail("has an invalid regex '" + source + "'"); }
      } else if (op == "entropy-gt?") {
        if (args.size() != 2 || !is_capture(args[0]) || is_capture(args[1]))
          fail("takes a capture and a numeric literal threshold (0..8)");
        p.kind = Predicate::Entropy;
        p.capture = args[0]->value_id;
        const std::string value = string_value(query, args[1]->value_id);
        size_t used = 0;
        try { p.threshold = std::stod(value, &used); }
        catch (const std::exception &) { fail("threshold must be finite and within 0..8"); }
        if (used != value.size() || !std::isfinite(p.threshold) || p.threshold < 0 || p.threshold > 8)
          fail("threshold must be finite and within 0..8");
      } else if (op == "any-of?" || op == "not-any-of?") {
        if (args.size() < 2) fail("takes a capture and at least one literal");
        if (!is_capture(args[0])) fail("first argument must be a capture, got literal \"" + string_value(query, args[0]->value_id) + "\"");
        p.kind = Predicate::AnyOf;
        p.positive = op == "any-of?";
        p.capture = args[0]->value_id;
        for (size_t a = 1; a < args.size(); ++a) {
          if (is_capture(args[a])) fail("arguments must be literals, got capture @" + capture_name(query, args[a]->value_id));
          p.options.push_back(string_value(query, args[a]->value_id));
        }
      } else if (op.back() == '?') {
        fail("is not a supported predicate (known: eq?, not-eq?, any-eq?, any-not-eq?, match?, not-match?, any-match?, any-not-match?, any-of?, not-any-of?, entropy-gt?, is?, is-not?)");
      } else {
        continue; // neither predicate nor directive syntax: nothing to evaluate
      }
      out[pattern].push_back(std::move(p));
    }
  }
  return out;
}

Node wrap_node(const Tree *owner, TSNode raw) {
  Node node;
  node.owner = owner;
  new (node.bytes) TSNode(raw);
  return node;
}

std::vector<std::string> capture_texts(const TSQueryMatch &match, uint32_t capture_index, const Tree *owner) {
  std::vector<std::string> texts;
  for (uint16_t c = 0; c < match.capture_count; ++c)
    if (match.captures[c].index == capture_index)
      texts.push_back(node_text(wrap_node(owner, match.captures[c].node)));
  return texts;
}

bool satisfies(const Predicate &p, const TSQueryMatch &match, const Tree *owner) {
  // every/some semantics of upstream's web binding (lib/binding_web/src/
  // query.ts): the plain forms hold for EVERY node of the capture, the any-*
  // forms for SOME node. (The Rust binding's loop ends in a bare `true`, so
  // its any-* forms pass vacuously when no node satisfies; the web binding
  // and the documented meaning do not, and neither do we.)
  const std::vector<std::string> texts = capture_texts(match, p.capture, owner);
  const auto every_or_some = [&](const auto &test) {
    if (p.match_all) {
      for (const auto &t : texts) if (!test(t)) return false;
      return true;
    }
    for (const auto &t : texts) if (test(t)) return true;
    return false;
  };
  switch (p.kind) {
  case Predicate::Eq:
    if (p.other_is_capture) {
      const std::vector<std::string> others = capture_texts(match, p.other_capture, owner);
      return every_or_some([&](const std::string &t) {
        for (const auto &o : others) if ((t == o) == p.positive) return true;
        return false;
      });
    }
    return every_or_some([&](const std::string &t) { return (t == p.operand) == p.positive; });
  case Predicate::Match:
    if (texts.empty()) return !p.positive;
    return every_or_some([&](const std::string &t) { return regex_search_bounded(t, p.re) == p.positive; });
  case Predicate::Entropy:
    if (texts.empty()) return false;
    return every_or_some([&](const std::string &t) {
      // Byte histogram, O(n + 256), bounded independently of regex matching.
      if (t.empty() || t.size() > kRegexTextCap) return false;
      std::array<size_t, 256> counts{};
      for (unsigned char c : t) ++counts[c];
      double entropy = 0;
      for (size_t count : counts) if (count) {
        const double probability = static_cast<double>(count) / t.size();
        entropy -= probability * std::log2(probability);
      }
      return entropy > p.threshold;
    });
  case Predicate::AnyOf:
    for (const auto &t : texts) {
      const bool hit = std::find(p.options.begin(), p.options.end(), t) != p.options.end();
      if (hit != p.positive) return false;
    }
    return true;
  }
  return true;
}

} // namespace

// Shared query runner: nesting bound, compile, predicates; `on_match` sees
// every match that passed its pattern's predicates.
template <class OnMatch>
static void run_query(const Node &root_node, const std::string &pattern, Grammar grammar,
                      PatternProperties *properties, const OnMatch &on_match, std::vector<std::string> *capture_names = nullptr) {
  // tree-sitter's query compiler recurses on nested patterns. Bound nesting
  // before calling it, ignoring delimiters inside strings and comments.
  size_t depth = 0;
  bool quoted = false, escaped = false, comment = false;
  for (size_t i = 0; i < pattern.size(); ++i) {
    const char c = pattern[i];
    if (comment) { if (c == '\n') comment = false; continue; }
    if (quoted) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') quoted = false;
      continue;
    }
    if (c == ';') { comment = true; continue; }
    if (c == '"') { quoted = true; continue; }
    if (c == '(' || c == '[') {
      if (++depth > 256)
        throw QueryError("query error at byte offset " + std::to_string(i) +
                         ": nesting limit exceeded (256)");
    } else if ((c == ')' || c == ']') && depth) --depth;
  }
  uint32_t error_offset = 0;
  TSQueryError error_type = TSQueryErrorNone;
  RaiiQuery query{ts_query_new(language_of(grammar), pattern.data(),
                               static_cast<uint32_t>(pattern.size()), &error_offset, &error_type)};
  if (!query.q)
    throw QueryError("query error at byte offset " + std::to_string(error_offset) +
                     ": " + query_error_word(error_type));
  if (capture_names)
    for (uint32_t id = 0; id < ts_query_capture_count(query.q); ++id)
      capture_names->push_back(capture_name(query.q, id));
  const std::vector<std::vector<Predicate>> predicates = compile_predicates(query.q, properties);
  RaiiCursor cursor{ts_query_cursor_new()};
  ts_query_cursor_exec(cursor.c, query.q, ts(root_node));
  TSQueryMatch match;
  while (ts_query_cursor_next_match(cursor.c, &match)) {
    bool ok = true;
    try {
      for (const Predicate &p : predicates[match.pattern_index])
        if (!satisfies(p, match, root_node.owner)) { ok = false; break; }
    } catch (const RegexLimit &e) {
      throw QueryError(e.what()); // plain "regex work limit exceeded (65536)"
    }
    if (!ok) continue;
    on_match(query.q, match);
  }
}

NodeList node_query(const Node &root_node, const std::string &pattern, Grammar grammar) {
  NodeList list;
  run_query(root_node, pattern, grammar, nullptr, [&](const TSQuery *, const TSQueryMatch &match) {
    for (uint16_t i = 0; i < match.capture_count; ++i)
      list.items.push_back(wrap_node(root_node.owner, match.captures[i].node));
  });
  return list;
}

QueryMatches query_matches(const Node &root_node, const std::string &pattern, Grammar grammar) {
  QueryMatches out;
  run_query(root_node, pattern, grammar, &out.properties, [&](const TSQuery *query, const TSQueryMatch &match) {
    QueryMatch m;
    m.pattern = match.pattern_index;
    for (uint16_t i = 0; i < match.capture_count; ++i)
      m.captures.push_back({capture_name(query, match.captures[i].index),
                            wrap_node(root_node.owner, match.captures[i].node)});
    out.matches.push_back(std::move(m));
  }, &out.capture_names);
  return out;
}

} // namespace lmok
