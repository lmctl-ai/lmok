// Baseline / new-code-only support; see baseline.hpp for the contract.
#include "baseline.hpp"
#include "input.hpp"
#include "dslvm.hpp"
#include "stdlib/json.hpp"

#include <cstdio>
#include <filesystem>
#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace lmok {
namespace {

using dslvm::stdlib::JsonNode;

const JsonNode *member(const JsonNode &obj, const char *key) {
  if (obj.kind != JsonNode::Kind::Object) return nullptr;
  for (const auto &m : obj.object)
    if (m.key == key) return m.value.get();
  return nullptr;
}

std::string member_string(const JsonNode &obj, const char *key) {
  const JsonNode *value = member(obj, key);
  return value && value->kind == JsonNode::Kind::String ? value->text : "";
}

// Run `command` through the platform shell, capturing stdout. The exit code
// is returned. Git -z uses NUL separators; preserve CR bytes in filenames.
int shell_capture(const std::string &command, std::vector<std::string> &lines, char separator = '\n') {
#ifdef _WIN32
  FILE *pipe = _popen(command.c_str(), "rb");
#else
  FILE *pipe = popen(command.c_str(), "r");
#endif
  if (!pipe) return -1;
  std::string current;
  char buffer[4096];
  size_t n;
  while ((n = std::fread(buffer, 1, sizeof buffer, pipe)) > 0) {
    for (size_t i = 0; i < n; ++i) {
      if (buffer[i] == separator) {
        if (separator == '\n' && !current.empty() && current.back() == '\r') current.pop_back();
        lines.push_back(current);
        current.clear();
      } else current += buffer[i];
    }
  }
  if (!current.empty()) {
    if (separator == '\n' && current.back() == '\r') current.pop_back();
    lines.push_back(current);
  }
#ifdef _WIN32
  return _pclose(pipe); // _pclose hands back the exit code directly
#else
  const int status = pclose(pipe);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

} // namespace

void Baseline::add(const std::string &rule, const std::string &file, const std::string &message,
                   const std::string &snippet, bool has_snippet) {
  by_key_[std::make_tuple(rule, file, message)].push_back({snippet, has_snippet, false});
}

bool Baseline::match(const std::string &rule, const std::string &file, const std::string &message,
                     const std::string &snippet) {
  const auto found = by_key_.find(std::make_tuple(rule, file, message));
  if (found == by_key_.end()) return false;
  size_t fallback = found->second.size(); // first unconsumed snippet-less entry
  for (size_t i = 0; i < found->second.size(); ++i) {
    Entry &entry = found->second[i];
    if (entry.consumed) continue;
    if (entry.has_snippet) {
      if (entry.snippet == snippet) { entry.consumed = true; return true; }
    } else if (snippet.rfind("sha256:", 0) != 0 && fallback == found->second.size()) {
      // Legacy baselines have no safe identity for credential findings. Treat
      // a redacted current snippet as new rather than allowing a plaintext-
      // unaware fallback to hide rotation or reintroduction.
      fallback = i;
    }
  }
  if (fallback < found->second.size()) {
    found->second[fallback].consumed = true;
    return true;
  }
  return false;
}

int64_t resolve_baseline_run(Database &db, const std::string &selector) {
  if (selector == "last") {
    auto q = db.prepare("SELECT id FROM runs ORDER BY started_at DESC, id DESC LIMIT 1");
    if (q.step()) return q.integer(0);
    throw BaselineError("--new-since last: the DB has no recorded runs (record one with `lmok lint ... --db`)");
  }
  int64_t id = -1;
  try {
    size_t used = 0;
    id = std::stoll(selector, &used);
    if (used != selector.size()) id = -1;
  } catch (const std::exception &) { id = -1; }
  if (id < 0)
    throw BaselineError("bad --new-since value '" + selector + "' (want a run id or 'last')");
  auto q = db.prepare("SELECT id FROM runs WHERE id = ?");
  q.bind(1, id);
  if (q.step()) return id;
  throw BaselineError("--new-since " + selector + ": no recorded run with id " + selector +
                      " (see `lmok status` for the last run)");
}

Baseline load_baseline_db(Database &db, int64_t run_id, bool has_snippet, const std::string &scope) {
  Baseline baseline;
  auto table_exists = db.prepare("SELECT count(*) FROM sqlite_master WHERE type='table' AND name='run_files'");
  table_exists.step();
  const bool has_observations = table_exists.integer(0) != 0;
  // A run is a snapshot for each file. Repeated scans supersede the prior
  // observation, but the superseded flag is mutable and cannot answer an
  // historical selector after later scans. Select the latest run for each
  // file at or before the requested snapshot instead.
  // Old databases did not record clean scans or scan scope. Their history
  // cannot prove an occurrence still belonged to the selected snapshot;
  // conservatively report new findings until a fresh scan is recorded.
  if (!has_observations) return baseline;
  const std::string sql = std::string("SELECT f.rule, files.path, f.message, ") +
      (has_snippet ? "f.snippet" : "NULL") +
      " FROM findings f JOIN files ON files.id = f.file_id JOIN run_files rf"
      " ON rf.run_id=f.run_id AND rf.file_id=f.file_id"
      " WHERE rf.scope = ? AND rf.run_id = (SELECT MAX(rf2.run_id) FROM run_files rf2"
      " WHERE rf2.file_id = f.file_id AND rf2.scope = rf.scope AND rf2.run_id <= ?)";
  auto q = db.prepare(sql);
  q.bind(1, scope); q.bind(2, run_id);
  while (q.step())
    baseline.add(q.text(0), q.text(1), q.text(2), q.text(3), has_snippet && !q.is_null(3));
  return baseline;
}

Baseline load_baseline_file(const std::string &path) {
  const std::string text = read_input_file(path); // dslvm::Error when unreadable (exit 3)
  Baseline baseline;
  // Finding lines PRECEDE their `N findings in PATH` line, so buffer them
  // and attribute the buffer when the count line arrives. Leftovers at EOF
  // (truncated file) get the empty path and never match a real file.
  struct Pending { std::string rule, message, snippet; bool has_snippet; };
  std::vector<Pending> pending;
  size_t at = 0;
  while (at <= text.size()) {
    const size_t newline = text.find('\n', at);
    std::string line = text.substr(at, newline == std::string::npos ? std::string::npos : newline - at);
    at = newline == std::string::npos ? text.size() + 1 : newline + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    if (line.front() == '{') {
      JsonNode node;
      try {
        dslvm::stdlib::json_detail::Parser parser(line);
        node = parser.val();
        parser.ws();
        if (parser.p != line.size()) continue; // trailing bytes: not a finding line
      } catch (const dslvm::Error &) { continue; } // not JSON: not a finding line
      if (node.kind != JsonNode::Kind::Object) continue;
      // Entries missing the key fields (old/hand-made baselines) simply
      // never match: their findings are reported as new, which is the safe
      // direction for a gate.
      const JsonNode *snippet = member(node, "snippet");
      const bool has_snippet = snippet && snippet->kind == JsonNode::Kind::String;
      // Explicit file keys are escaped JSON, independent of suppression
      // counters and newlines in the human-readable footer.
      const JsonNode *file = member(node, "file");
      if (file && file->kind == JsonNode::Kind::String) {
        baseline.add(member_string(node, "rule"), file->text, member_string(node, "message"),
                     has_snippet ? snippet->text : "", has_snippet);
      } else {
        pending.push_back({member_string(node, "rule"), member_string(node, "message"),
                           has_snippet ? snippet->text : "", has_snippet});
      }
      continue;
    }
    // `N findings in PATH` (the line lint prints after each file's findings).
    const std::string marker = " findings in ";
    const size_t space = line.find(' ');
    if (space == std::string::npos) continue;
    bool digits = space > 0;
    for (size_t i = 0; i < space; ++i)
      if (line[i] < '0' || line[i] > '9') digits = false;
    if (digits && line.compare(space, marker.size(), marker) == 0) {
      std::string file = line.substr(space + marker.size());
      // Suppression counts are human-readable output metadata, not part of
      // the durable path key. Strip only the exact suffix emitted by lmok.
      const std::string suffix_start = " (";
      const size_t suffix = file.rfind(suffix_start);
      const size_t tail = file.size() >= 12 ? file.size() - 12 : std::string::npos;
      if (suffix != std::string::npos && tail > suffix + 2 &&
          file.compare(tail, 12, " suppressed)") == 0) {
        size_t digits = suffix + 2;
        while (digits < tail && file[digits] >= '0' && file[digits] <= '9') ++digits;
        if (digits == tail) file.resize(suffix);
      }
      for (const auto &p : pending) baseline.add(p.rule, file, p.message, p.snippet, p.has_snippet);
      pending.clear();
    }
  }
  for (const auto &p : pending) baseline.add(p.rule, "", p.message, p.snippet, p.has_snippet);
  return baseline;
}

std::set<std::string> git_changed_files(const std::string &rev) {
  // A leading '-' would be parsed by git as an OPTION (`--quiet` lints nothing
  // and exits 0; `-O<file>` reorders the working-tree diff): refuse it.
  if (rev.empty() || rev.front() == '-')
    throw BaselineError("bad --changed revision '" + rev + "' (must not start with '-')");
  for (const char c : rev) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == '/' || c == '~' || c == '^' || c == '-';
    if (!ok) throw BaselineError("bad --changed revision '" + rev + "' (allowed: A-Z a-z 0-9 . _ / ~ ^ -)");
  }
  std::vector<std::string> lines;
  if (shell_capture("git -c core.quotepath=false rev-parse --show-toplevel", lines) != 0 || lines.empty())
    throw BaselineError("--changed needs a git repository (git rev-parse --show-toplevel failed here)");
  namespace fs = std::filesystem;
  const fs::path root = fs::u8path(lines.front());
  std::vector<std::string> names;
  if (shell_capture("git -c core.quotepath=false diff --name-only -z \"" + rev + "\"", names, '\0') != 0)
    throw BaselineError("git diff --name-only " + rev + " failed (unknown revision?)");
  std::set<std::string> changed;
  for (const auto &name : names) {
    if (name.empty()) continue;
    changed.insert(fs::absolute(root / fs::u8path(name)).lexically_normal().generic_u8string());
  }
  return changed;
}

} // namespace lmok
