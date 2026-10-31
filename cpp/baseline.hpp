#pragma once
// Baseline / new-code-only support for `lmok lint` (roadmap wave 1).
//
// A finding is NEW when its key has no unconsumed baseline entry. The key is
// (rule, file, message, normalized snippet) — positions are deliberately NOT
// part of it, so edits above a finding do not resurrect it as "new". The
// snippet is the finding node's source text with whitespace collapsed
// (lmok::normalize_snippet, lint.hpp). Baselines can come from the findings
// DB (--new-since RUN) or from a previous lint JSON-lines output file
// (--baseline FILE); both are multisets: N baseline occurrences of a key
// absorb the first N current occurrences, so a duplicated violation still
// shows its extra copies as new.
//
// Fallback: entries recorded before the findings.snippet column existed
// (NULL) or read from an old lint output file (no snippet field) match on
// (rule, file, message) alone. A snippet-bearing baseline entry only matches
// a current finding with the same snippet.

#include "db.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace lmok {

// Thrown for every baseline/change-list usage error; maps to exit 2 in main
// (unknown run selector, unreadable/missing DB, not a git repository, bad
// revision). An unreadable --baseline FILE is exit 3 (an input, like a lint
// target) and is raised as dslvm::Error by the reader instead.
struct BaselineError : std::runtime_error {
  explicit BaselineError(const std::string &message) : std::runtime_error(message) {}
};

class Baseline {
  struct Entry { std::string snippet; bool has_snippet; bool consumed = false; };
  std::map<std::tuple<std::string, std::string, std::string>, std::vector<Entry>> by_key_;
public:
  void add(const std::string &rule, const std::string &file, const std::string &message,
           const std::string &snippet, bool has_snippet);
  // True (and consumes one baseline occurrence) when this finding existed in
  // the baseline: exact snippet match preferred, snippet-less entry as the
  // documented fallback. False = the finding is new.
  bool match(const std::string &rule, const std::string &file, const std::string &message,
             const std::string &snippet);
};

// The run selector for --new-since: "last" (latest started_at, then id) or a
// runs.id. Throws BaselineError when the selector names no recorded run.
int64_t resolve_baseline_run(Database &db, const std::string &selector);

// Findings from the latest recorded observation for each file and rule-pack
// scope at or before run_id, keyed by (rule, path-as-recorded, message,
// snippet). Clean observations contribute no baseline entries. Callers must
// pass has_snippet = db_has_column(db, "findings", "snippet"): a DB recorded
// before the column existed falls back to (rule, file, message) for its rows.
Baseline load_baseline_db(Database &db, int64_t run_id, bool has_snippet,
                          const std::string &scope = "");

// A previous lint JSON-lines output: finding lines carry no path, so they
// attach to the file named by the next `N findings in PATH` line (and to ""
// before the first one, which never matches a real file). Unparseable lines
// are ignored. Throws dslvm::Error when the file cannot be read.
Baseline load_baseline_file(const std::string &path);

// Absolute, forward-slash paths of the files `git diff --name-only REV`
// reports for the repository containing the current working directory
// (rev defaults to HEAD at the call site: working tree vs HEAD). Throws
// BaselineError outside a git repository, when git is missing, or when the
// revision is unknown, is empty, starts with '-' (git would read it as an
// option) or contains characters outside [A-Za-z0-9._/~^-] (the whitelist
// keeps the popen'd command line injection-free on both sh and cmd.exe).
std::set<std::string> git_changed_files(const std::string &rev);

} // namespace lmok
