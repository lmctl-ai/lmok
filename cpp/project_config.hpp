#pragma once

// Project configuration is normally read from ROOT/.lmok/config.json.  Paths
// in packs, ignore, and baseline are relative to ROOT, not this file's
// directory.  baseline is parsed here for forward compatibility; the main
// runner must reject a non-null baseline until item 3 handles it.

#include "input.hpp"
#include "stdlib/json.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lmok {

struct ProjectConfig {
  std::map<std::string, std::vector<std::string>> packs;
  std::vector<std::string> ignore;
  std::string severity_floor = "major";
  std::string fail_on = "findings";
  std::string header_lang = "c";
  std::optional<std::string> baseline;
};

inline int severity_rank(const std::string &severity) {
  if (severity == "info") return 0;
  if (severity == "minor") return 1;
  if (severity == "major") return 2;
  if (severity == "critical") return 3;
  return -1;
}

namespace project_config_detail {

using dslvm::stdlib::JsonNode;

inline const JsonNode *member(const JsonNode &object, const std::string &key) {
  if (object.kind != JsonNode::Kind::Object) return nullptr;
  for (const auto &entry : object.object)
    if (entry.key == key) return entry.value.get();
  return nullptr;
}

[[noreturn]] inline void fail(const std::string &path, const std::string &what) {
  throw dslvm::Error("project config " + path + ": " + what);
}

inline void require_string_array(const JsonNode &node, const std::string &path,
                                 std::vector<std::string> &out,
                                 bool require_nonempty) {
  if (node.kind != JsonNode::Kind::Array || (require_nonempty && node.array.empty()))
    fail(path, require_nonempty ? "must be a nonempty array of strings"
                                : "must be an array of strings");
  for (size_t i = 0; i < node.array.size(); ++i) {
    const JsonNode &entry = node.array[i];
    if (entry.kind != JsonNode::Kind::String || entry.text.empty())
      fail(path, "entry " + std::to_string(i) + " must be a nonempty string");
    out.push_back(entry.text);
  }
}

} // namespace project_config_detail

inline ProjectConfig load_project_config(const std::string &path) {
  using namespace project_config_detail;
  const std::string text = read_input_file(path);
  JsonNode root;
  try {
    dslvm::stdlib::json_detail::Parser parser(text);
    root = parser.val();
    parser.ws();
    if (parser.p != text.size()) throw dslvm::Error("trailing bytes");
  } catch (const dslvm::Error &e) {
    fail(path, "invalid JSON (" + std::string(e.what()) + ")");
  }
  if (root.kind != JsonNode::Kind::Object) fail(path, "top level must be an object");

  for (const auto &entry : root.object) {
    if (entry.key != "version" && entry.key != "packs" && entry.key != "ignore" &&
        entry.key != "severity_floor" && entry.key != "fail_on" &&
        entry.key != "header_lang" && entry.key != "baseline")
      fail(path, "unknown key '" + entry.key + "'");
  }

  ProjectConfig config;
  if (const JsonNode *version = member(root, "version")) {
    if (version->kind != JsonNode::Kind::Int || version->integer != 1)
      fail(path, "'version' must be integer 1");
  }

  const JsonNode *packs = member(root, "packs");
  if (!packs || packs->kind != JsonNode::Kind::Object || packs->object.empty())
    fail(path, "missing 'packs' nonempty object");
  for (const auto &entry : packs->object) {
    // The runner validates names through the host language registry. New
    // grammars must not require a second configuration allowlist.
    if (entry.key.empty()) fail(path, "pack language must not be empty");
    std::vector<std::string> files;
    require_string_array(*entry.value, "'packs." + entry.key + "'", files, true);
    config.packs.emplace(entry.key, std::move(files));
  }

  if (const JsonNode *ignore = member(root, "ignore"))
    require_string_array(*ignore, "'ignore'", config.ignore, false);

  if (const JsonNode *severity = member(root, "severity_floor")) {
    if (severity->kind != JsonNode::Kind::String || severity_rank(severity->text) < 0)
      fail(path, "'severity_floor' must be one of info, minor, major, critical");
    config.severity_floor = severity->text;
  }

  if (const JsonNode *fail_on = member(root, "fail_on")) {
    if (fail_on->kind != JsonNode::Kind::String ||
        (fail_on->text != "findings" && fail_on->text != "none"))
      fail(path, "'fail_on' must be one of findings, none");
    config.fail_on = fail_on->text;
  }

  if (const JsonNode *header_lang = member(root, "header_lang")) {
    if (header_lang->kind != JsonNode::Kind::String ||
        (header_lang->text != "c" && header_lang->text != "cpp"))
      fail(path, "'header_lang' must be one of c, cpp");
    config.header_lang = header_lang->text;
  }

  if (const JsonNode *baseline = member(root, "baseline")) {
    if (baseline->kind == JsonNode::Kind::Null) {
      config.baseline.reset();
    } else if (baseline->kind == JsonNode::Kind::String && !baseline->text.empty()) {
      config.baseline = baseline->text;
    } else {
      fail(path, "'baseline' must be null or a nonempty string");
    }
  }
  return config;
}

} // namespace lmok
