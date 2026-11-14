#pragma once
// Global structural replacement and scope-aware refactoring (`lmok refactor`).
// Dry-run is the default: plans and verifies every file in memory, printing real
// unified diffs (git-apply compatible), site listings, or structured JSON.
// `--apply` executes a preflighted two-phase multi-file write with rollback.

#include "host.hpp"

#include <cstdint>
#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lmok {

enum class RefactorMode { Pattern, Rename, Import, Rules };
enum class RefactorFormat { Diff, Sites, Json };

struct RefactorTarget {
  std::string path;
  Grammar grammar = Grammar::Json;
  bool from_dir = false;
};

struct RefactorError : std::runtime_error {
  int exit_code = 3;
  RefactorError(int code, const std::string &message) : std::runtime_error(message), exit_code(code) {}
};

struct RewriteCapture {
  std::string name;
  bool variadic = false;
  int64_t start_byte = 0;
  int64_t end_byte = 0;
  std::string text;
  bool empty = false;
  bool is_expression = false;
};

struct RewriteMatch {
  int64_t start_byte = 0;
  int64_t end_byte = 0;
  int64_t row = 0, col = 0, end_row = 0, end_col = 0;
  std::string text;
  bool has_unowned_comment = false;
  bool is_statement = false;
  bool is_expression = false;
  std::vector<RewriteCapture> bindings;
};

struct RewritePattern {
  Grammar grammar = Grammar::Json;
  std::string raw;
  std::shared_ptr<Tree> parsed;
  // Path of named-child indices from node_root(*parsed) to the unwrapped pattern root.
  std::vector<int64_t> root_path;
  std::vector<std::string> scalars;
  std::vector<std::string> variadics;
  bool is_statement = false;
  bool is_expression = false;
  std::string head_identifier; // callee or primary identifier for near-miss tracking
};

struct TemplatePart {
  enum class Kind { Literal, Meta, Variadic };
  Kind kind = Kind::Literal;
  std::string text;
  bool parenthesized = false; // true when template explicitly wrote ($X)
};

struct CompiledTemplate {
  std::string raw;
  bool is_delete = false;
  std::vector<TemplatePart> parts;
  std::vector<std::string> drops;
  std::vector<std::string> duplicates;
};

struct RewriteSearchResult {
  std::vector<RewriteMatch> matches;
  size_t near_misses = 0;
};

struct PlannedEdit {
  int64_t start_byte = 0;
  int64_t end_byte = 0;
  int64_t row = 0, col = 0, end_row = 0, end_col = 0;
  std::string before;
  std::string after;
  std::string confidence = "exact"; // "exact" | "candidate"
  std::string status = "planned";   // "planned" | "skipped"
  std::string reason;
  bool parenthesized = false;
  bool is_expression = false;
  std::vector<std::string> drops;
  std::vector<std::string> duplicates;
  std::map<std::string, std::string> bindings;
  // Byte spans relative to replacement string start for substituted expression metavariables.
  struct ExprSpan { int64_t rel_start = 0, rel_end = 0; bool wrap_parens = false; std::string meta_name; };
  std::vector<ExprSpan> expr_spans;
  bool force_outer_parens = false;
};

struct RefactorOptions {
  RefactorMode mode = RefactorMode::Pattern;
  std::string from;
  std::string to;
  std::optional<Grammar> forced_lang;
  bool apply = false;
  bool check = false;
  RefactorFormat format = RefactorFormat::Diff;
  std::string inside;
  std::string not_inside;
  std::vector<std::pair<std::string, std::string>> where_clauses;
  std::string rename_at_file;
  int64_t rename_at_line = 0; // 1-based cursor line; 0 when unset
  int64_t rename_at_col = 0;  // 1-based cursor col; 0 when unset
  std::string rename_kind;
  std::string rename_scope;
  bool allow_candidates = false;
  bool exact_only = false;
  bool drop_comments = false;
  bool skip_unsafe = false;
  bool strict_parse = false;
  bool git_clean = false;
  std::optional<std::string> expect_digest;
  std::optional<size_t> expect_sites;
  size_t context_lines = 3;
};

// Direct CST unifier and template APIs (cpp/refactor_pattern.cpp).
bool refactor_pattern_supports(Grammar grammar);
RewritePattern compile_rewrite_pattern(Grammar grammar, const std::string &pattern);
CompiledTemplate compile_rewrite_template(const RewritePattern &pat, const std::string &tmpl);
RewriteSearchResult find_rewrite_matches(
    const Tree &tree,
    const RewritePattern &pat,
    const RewritePattern *inside = nullptr,
    const RewritePattern *not_inside = nullptr,
    const std::vector<std::pair<std::string, std::regex>> &where_filters = {});

// Renders bottom-up edits for one file, verifies AST frame / precedence / error budget
// (applying verified conditional auto-parenthesization when safe), and produces new_text.
struct FileRewritePlan {
  std::string new_text;
  std::vector<PlannedEdit> planned;
  std::vector<PlannedEdit> skipped;
  std::vector<std::string> warnings;
  size_t near_misses = 0;
};

FileRewritePlan apply_pattern_rewrites(
    const Tree &old_tree,
    const RewritePattern &pat,
    const CompiledTemplate &tmpl,
    const RewriteSearchResult &search,
    bool drop_comments,
    bool skip_unsafe,
    bool strict_parse);

// Shared tree safety verifier for non-pattern refactors (--rename, --import, --rules).
void verify_refactor_tree(Grammar grammar, const Tree &old_tree, const std::string &new_text,
                          const std::vector<PlannedEdit> &applied_edits,
                          bool strict_parse);

// Helper used by lint `--diff`/`--fix` when a JSON rule uses `pattern` + `fix` with `$NAME`.
std::optional<std::string> expand_pattern_fix(Grammar grammar, const Tree &tree,
                                              const Node &match_root,
                                              const std::string &pattern,
                                              const std::string &fix_template);

// Execute `lmok refactor` over the target list. Returns CLI exit code (0, 1, 2, 3, or 4).
int refactor_run(const RefactorOptions &opts, const std::vector<RefactorTarget> &targets,
                 std::ostream &out, std::ostream &err);

} // namespace lmok
