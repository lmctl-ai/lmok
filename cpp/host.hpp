#pragma once
// lmok wrapper over the tree-sitter incremental parsing engine.
//
// This is the only lmok-authored interface to tree-sitter: all
// tree_sitter/api.h fallout stays in host.cpp; the CLI host (main.cpp) talks
// to this plain interface only. Parsed trees and nodes are held as opaque
// values so no tree-sitter header leaks into this file.

#include <cstdint>
#include <functional>
#include <map>
#include <regex>
#include <string>
#include <stdexcept>
#include <vector>

namespace lmok {

struct Node;

// The vendored grammars. The DSL surface is explicit per grammar
// (ts.parse / ts.parse_c / ts.parse_python / ts.parse_bash, same for query and
// reparse). CLI grammar selection is language-parameterized separately.
enum class Grammar { Json, C, Python, Bash, Java, Go, Javascript, Typescript, Tsx, Lmscript, Lmcode };
const char *grammar_name(Grammar grammar); // stable lower-case CLI language name
// Empty when `kind` is a kind the walk can visit in this grammar (a named node, an anonymous token or ERROR);
// otherwise the reason it can never match: unknown spelling, a supertype (abstract; annotate its concrete
// kinds) or a hidden/auxiliary symbol. Used by `check` to refuse annotations at load instead of silently
// never firing them (WP14 F3).
std::string annotation_problem(Grammar grammar, const std::string &kind);

// Owning handle for one parsed tree plus a copy of its source text (kept
// alive so Node text() stays valid). Never throws: the tree-sitter parser
// always produces a tree, even for malformed input; use tree_has_error to
// detect error nodes.
class Tree {
public:
  Tree() = default;
  const std::string &source() const { return source_; }
  ~Tree();
  Tree(const Tree &) = delete;
  Tree &operator=(const Tree &) = delete;
  Tree(Tree &&other) noexcept;
  Tree &operator=(Tree &&other) noexcept;

private:
  friend const void *tree_handle(const Tree &tree);
  friend std::string node_text(const Node &node);
  friend Tree parse(Grammar grammar, const std::string &text);
  friend Tree parse_range(Grammar grammar, const Node &body);
  friend void tree_edit(Tree &tree, int64_t start_byte, int64_t old_end_byte, int64_t new_end_byte);
  friend Tree reparse(Grammar grammar, Tree &old_tree, const std::string &new_text);
  friend std::string changed_ranges_json(const Tree &old_tree, const Tree &new_tree);
  friend std::string tree_sexp(const Tree &tree);
  friend bool tree_has_error(const Tree &tree);
  friend int64_t tree_node_count(const Tree &tree);
  friend struct Node;
  void *tree_ = nullptr; // TSTree *
  std::string source_;
  // Pending edit recorded by tree_edit (byte offsets only), applied at
  // reparse time when the new text (and thus the new-end point) is known.
  // Only ONE pending edit per reparse — a second edit() first is a contract
  // violation reported by the DSL registry.
  bool has_pending_edit_ = false;
  int64_t edit_start_ = 0, edit_old_end_ = 0, edit_new_end_ = 0;
};

// Borrowed view of one CST node. The owning Tree must outlive the handle (it
// does: trees live in the host arena for the whole run). `bytes` holds a
// TSNode by value; host.cpp static-asserts the fit.
struct Node {
  const Tree *owner = nullptr;
  alignas(void *) unsigned char bytes[32];
};

// Parse text with the given vendored grammar (Grammar::Json is the default
// `parse` surface). Throws std::length_error for text longer than
// UINT32_MAX bytes (tree-sitter's own byte-offset limit) instead of silently
// truncating.
Tree parse(Grammar grammar, const std::string &text);

// Parse the exact body range while retaining absolute points/bytes and full source ownership.
Tree parse_range(Grammar grammar, const Node &body);
// A standalone input yields one tree; lmcode selected on a lmscript container
// yields one tree per named lmcode_body, in source order (none for empty bodies).
std::vector<Tree> analysis_trees(Grammar grammar, const std::string &text, bool lmscript_container = false);
std::vector<Tree> lmcode_injections(const Tree &outer);

// Record an edit on the tree (byte offsets; insertion is old_end == start,
// deletion is new_end == start). The edit is APPLIED at reparse time, when
// the new text — and thus the new-end point — is known. One pending edit per
// reparse.
void tree_edit(Tree &tree, int64_t start_byte, int64_t old_end_byte, int64_t new_end_byte);

// Re-parse new_text incrementally against old_tree (applying its pending
// edit first). Returns a NEW tree. The old tree stays alive and valid for
// changed_ranges_json; note its stored source is the PRE-edit text, so
// node_text() on old-tree nodes past the edit point is stale — use the new
// tree for reads. Keeping both alive costs two trees' memory (documented).
// Contract checks (std::out_of_range): a pending edit is REQUIRED unless
// new_text equals the old source byte-for-byte (tree-sitter would otherwise
// reuse the whole old tree and silently return it for different text), and
// the edit must be consistent with the new text:
//   new_text.size() == old.size() - (old_end - start) + (new_end - start).
// Throws std::length_error like parse() for oversized text.
Tree reparse(Grammar grammar, Tree &old_tree, const std::string &new_text);

// The syntactically changed ranges between two trees as a compact JSON array
// string `[[start_byte,end_byte],...]` (script can json_parse it). Ranges may
// be slightly larger than the exact change (tree-sitter semantics).
std::string changed_ranges_json(const Tree &old_tree, const Tree &new_tree);

// The tree as a single-line s-expression, like `tree-sitter parse` prints it.
std::string tree_sexp(const Tree &tree);

// True when the tree contains an ERROR or MISSING node (malformed input).
bool tree_has_error(const Tree &tree);

// Total number of nodes in the tree, including the root.
int64_t tree_node_count(const Tree &tree);

// Root node of the tree.
Node node_root(const Tree &tree);

// Flat pre-order list of a tree's NAMED nodes, root first. Built fresh on
// every Tree.nodes() call: O(tree) time and memory, fine at this scale.
struct NodeList {
  std::vector<Node> items;
};

// Pre-order traversal of the named nodes of tree (root first).
NodeList tree_nodes(const Tree &tree);

// Single pre-order cursor walk including anonymous tokens. The callback may
// inspect/query nodes; it must not edit the tree while its cursor is live.
// `ancestors` is the path root..parent of the visited node (empty for the root); a host that caches it makes
// Node.parent() O(1) instead of tree-sitter's O(depth) ts_node_parent walk (WP14 F4).
void walk_nodes(const Tree &tree, const std::function<void(const Node &, const std::vector<Node> &ancestors)> &visit);

// Thrown by node_query for an invalid pattern: what() is
// "query error at byte offset N: <syntax|node-type|field|capture|structure|language>".
struct QueryError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// All captured nodes of all matches of a tree-sitter S-expression query
// pattern (e.g. `(pair key: (string (string_content) @key))`), executed over
// the subtree at root, in cursor order (matches in byte order, captures in
// pattern order within a match). Duplicate nodes across captures/matches are
// kept. A pattern with zero captures yields an empty list. Query predicates
// are evaluated per match with upstream tree-sitter's text-predicate
// semantics (lib/binding_rust satisfies_text_predicates): #eq?/#not-eq?/
// #any-eq?/#any-not-eq? (capture text vs literal or another capture),
// #match?/#not-match?/#any-match?/#any-not-match? (ECMAScript regex,
// UNANCHORED like upstream — authors anchor with ^...$), and
// #any-of?/#not-any-of?, plus #entropy-gt? @capture "bits" (strict byte Shannon
// entropy threshold, finite 0..8, non-empty captures <=16 KiB). The plain forms must hold for EVERY node of a
// quantified capture, the any-* forms for SOME node (the web binding's
// every/some semantics; the Rust binding lets any-* pass vacuously, which is
// not copied). Property predicates
// (#is?, #is-not?) and directives (#set! and anything ending in `!`) are
// accepted and ignored. A malformed known predicate (wrong arity, literal
// where a capture is required, capture as #match? regex) or an UNKNOWN
// `?`-predicate throws QueryError — silently ignoring one would over-match.
NodeList node_query(const Node &root, const std::string &pattern, Grammar grammar);

// A query match with its capture NAMES preserved (node_query drops them).
// `properties` is indexed by pattern and holds the `(#set! key "value")`
// directives of that pattern — the tree-sitter convention for attaching data
// to a pattern, used by facts.scm for per-pattern confidence. Predicates are
// evaluated exactly as in node_query; non-matching matches are dropped.
struct QueryCapture {
  std::string name; // capture name without the '@'
  Node node;
};
struct QueryMatch {
  uint32_t pattern = 0;
  std::vector<QueryCapture> captures; // in pattern order
};
struct QueryMatches {
  std::vector<std::string> capture_names; // declarations, including queries with no matches
  std::vector<QueryMatch> matches;                            // cursor order
  std::vector<std::map<std::string, std::string>> properties; // per pattern
};
QueryMatches query_matches(const Node &root, const std::string &pattern, Grammar grammar);

// Regex matching over node text is bounded: std::regex (libstdc++ and libc++)
// backtracks recursively, so a long node text (a 40 KB comment with the
// shipped LBC010 rule, a few KB on a 1 MB Windows stack) overflows the stack
// and kills the process. Texts longer than kRegexTextCap bytes are NOT
// searched (treated as "no match"); the first skip prints one note on
// stderr. Shared by lint text_matches and query #match?.
constexpr size_t kRegexTextCap = 16384;
bool regex_search_bounded(const std::string &text, const std::regex &re);

// Node kind (ts_node_type), e.g. "document", "object", "pair", "string".
std::string node_kind(const Node &node);

// Source slice the node spans (includes quotes for string nodes).
std::string node_text(const Node &node);

// True for named nodes (false for anonymous tokens like punctuation).
bool node_is_named(const Node &node);
// True for a zero-width MISSING placeholder tree-sitter inserted for error recovery; it carries the
// expected kind (e.g. `number`) and empty text, so rules dispatched on that kind can skip it.
bool node_is_missing(const Node &node);
// Stable identity of the node within its tree (tree-sitter's subtree pointer): equal for every handle
// of the same node, so a host can hand out one handle per node instead of one per call.
const void *node_id(const Node &node);

// Extent of the node. Rows and columns are 0-based (tree-sitter convention).
int64_t node_start_row(const Node &node);
int64_t node_start_col(const Node &node);
int64_t node_end_row(const Node &node);
int64_t node_end_col(const Node &node);
// Byte extent of the node (0-based, end exclusive).
int64_t node_start_byte(const Node &node);
int64_t node_end_byte(const Node &node);

// Number of NAMED children (anonymous tokens are not counted).
int64_t node_child_count(const Node &node);

// The i-th NAMED child. The caller must bounds-check against
// node_child_count first; passing an out-of-range index is undefined
// (the DSL registry in main.cpp performs the check and throws).
Node node_child(const Node &node, int64_t index);

// False for the root node (ts_node_parent of the root is a null node).
bool node_has_parent(const Node &node);

// The parent node. Root is a contract violation: the DSL registry checks
// node_has_parent first and throws (exit 3); C++ callers must do the same.
Node node_parent(const Node &node);

// Opaque access to the wrapped tree (implementation detail; used only by
// host.cpp helpers). Returns nullptr for a moved-from handle.
const void *tree_handle(const Tree &tree);

} // namespace lmok
