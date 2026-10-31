#pragma once

// Project-level CI runner.  This header deliberately keeps filesystem
// expansion in main.cpp: the existing target walker owns .lmokignore,
// hidden-directory and platform-path semantics.  The template callback lets
// `ci` reuse that walker without introducing a second scanner.

#include "lint.hpp"
#include "project_config.hpp"
#include "input.hpp"
#include "sarif.hpp"

#include <cstddef>
#include <functional>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace lmok {
// Optional side channel for `ci --sarif FILE` (item 4): when set, the runner
// appends one SarifResult per printed finding and fills `rules` from the
// loaded packs; the caller serializes with sarif_log after the run.
struct SarifSink {
  std::vector<SarifRuleMeta> *rules = nullptr;
  std::vector<SarifResult> *results = nullptr;
};

// Target must expose `path` and `from_dir`.  Expand must have the shape:
//   int expand(vector<string> roots, vector<Target>& out, vector<string> ignore)
// and return the normal lmok CLI status (0, 2, or 3).  GrammarForExtension
// has the shape bool(string extension, Grammar& out).  ReadFile returns the
// source bytes and may throw dslvm::Error.
template <class Target, class Expand, class GrammarForExtension, class ReadFile, class Escape, class ParseLanguage>
int run_project_ci(const ProjectConfig &config, const std::string &root,
                   bool no_suppress, Expand expand,
                   GrammarForExtension grammar_for_extension, ReadFile read_file, Escape json_escape,
                   ParseLanguage parse_language, SarifSink *sarif = nullptr, bool include_metadata = false) {
  if (config.baseline) {
    // Item 3 owns baseline comparison.  Keep the field in the versioned
    // format, but refuse it here so CI cannot silently claim a baseline was
    // applied when it was not.
    std::cerr << "project config baseline is reserved for baseline support (item 3)\n";
    return 2;
  }

  std::vector<LintRule> rules;
  std::set<std::string> ids;
  for (const auto &language_packs : config.packs) {
    Grammar configured;
    if (!parse_language(language_packs.first, configured)) {
      std::cerr << "project config unknown pack language '" << language_packs.first << "'\n";
      return 2;
    }
    for (const std::string &pack : language_packs.second) {
      const std::string path = (std::filesystem::u8path(root) / std::filesystem::u8path(pack)).generic_u8string();
      std::vector<LintRule> loaded;
      try { loaded = load_rule_pack(path); }
      catch (const dslvm::Error &e) { std::cerr << e.what() << "\n"; return 2; }
      for (auto &rule : loaded) {
        if (rule.grammar != configured) {
          std::cerr << "project config pack '" << path << "' language does not match '"
                    << language_packs.first << "'\n";
          return 2;
        }
        if (!ids.insert(rule.id).second) {
          std::cerr << "project config duplicate rule id '" << rule.id << "'\n";
          return 2;
        }
        if (severity_rank(rule.severity) < 0) { std::cerr << "project config invalid rule severity: " << rule.severity << "\n"; return 2; }
        rules.push_back(std::move(rule));
      }
    }
  }

  std::vector<Target> targets;
  const int walk_status = expand(std::vector<std::string>{root}, targets, config.ignore);
  if (walk_status != 0) return walk_status;

  if (sarif && sarif->rules)
    for (const auto &rule : rules) {
      const auto url = rule.metadata.strings.find("url");
      sarif->rules->push_back({rule.id, rule.message,
                               url == rule.metadata.strings.end() ? "" : url->second, rule.metadata});
    }

  size_t findings_count = 0;
  size_t suppressed_count = 0;
  bool threshold_hit = false;
  const int floor = severity_rank(config.severity_floor);
  for (const Target &target : targets) {
    Grammar file_grammar;
    const std::string path = target.path;
    const std::string ext = std::filesystem::u8path(path).extension().u8string();
    if (!grammar_for_extension(ext, file_grammar)) continue;
    std::vector<LintRule> file_rules;
    for (const auto &rule : rules)
      if (rule.grammar == file_grammar || (file_grammar==Grammar::Lmscript && rule.grammar==Grammar::Lmcode)) file_rules.push_back(rule);
    if (file_rules.empty()) continue;
    std::string source;
    try { source = read_file(path); }
    catch (const dslvm::Error &e) { std::cerr << e.what() << "\n"; return 3; }
    const auto sarif_source = (sarif && sarif->results)
        ? std::make_shared<const std::string>(source)
        : std::shared_ptr<const std::string>{};
    Suppression suppression(source);
    std::vector<LintFinding> findings;
    try { findings = lint_text(file_rules, source, &suppression, no_suppress, nullptr, file_grammar==Grammar::Lmscript); }
    catch (const std::exception &e) { std::cerr << e.what() << "\n"; return 3; }
    suppression.warn(path, ids);
    suppressed_count += suppression.suppressed;
    findings_count += findings.size();
    for (const auto &finding : findings) {
      std::cout << "{\"path\":\"" << json_escape(path)
                << "\",\"row\":" << finding.row << ",\"col\":" << finding.col
                << ",\"end_row\":" << finding.end_row << ",\"end_col\":" << finding.end_col
                << ",\"rule\":\"" << json_escape(finding.rule)
                << "\",\"severity\":\"" << json_escape(finding.severity)
                << "\",\"message\":\"" << json_escape(finding.message) << "\"" << metadata_suffix(finding.metadata,include_metadata) << "}\n";
      if (sarif && sarif->results)
        sarif->results->push_back({finding.rule, finding.severity, finding.message, path,
                                   finding.row, finding.col, finding.end_row, finding.end_col,
                                   finding.metadata, sarif_source});
      if (config.fail_on == "findings" && severity_rank(finding.severity) >= floor)
        threshold_hit = true;
    }
  }
  std::cout << "{\"summary\":{\"findings\":" << findings_count
            << ",\"suppressed\":" << suppressed_count
            << ",\"severity_floor\":\"" << json_escape(config.severity_floor)
            << "\",\"fail_on\":\"" << json_escape(config.fail_on)
            << "\"}}\n";
  if (!std::cout) return 4;
  return threshold_hit ? 1 : 0;
}

} // namespace lmok
