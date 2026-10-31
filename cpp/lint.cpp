// Rule-pack loading and the lint dispatcher; see lint.hpp for the contract.
#include "lint.hpp"
#include "bounded_regex.hpp"
#include "db.hpp"
#include "input.hpp"
#include "dslvm.hpp"
#include "stdlib/json.hpp"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <utility>

namespace lmok {
namespace {

using dslvm::stdlib::JsonNode;

const JsonNode *member(const JsonNode &obj, const char *key) {
  if (obj.kind != JsonNode::Kind::Object) return nullptr;
  for (const auto &m : obj.object)
    if (m.key == key) return m.value.get();
  return nullptr;
}

std::string pack_error(const std::string &path, const std::string &what) {
  return "rules pack " + path + ": " + what;
}

bool grammar_of(const std::string &name, Grammar &grammar) {
  if (name == "json") { grammar = Grammar::Json; return true; }
  if (name == "c") { grammar = Grammar::C; return true; }
  if (name == "python") { grammar = Grammar::Python; return true; }
  if (name == "java") { grammar = Grammar::Java; return true; }
  if (name == "bash") { grammar = Grammar::Bash; return true; }
  if (name == "go") { grammar = Grammar::Go; return true; }
  if (name == "javascript") { grammar = Grammar::Javascript; return true; }
  if (name == "typescript") { grammar = Grammar::Typescript; return true; }
  if (name == "tsx") { grammar = Grammar::Tsx; return true; }
  if (name == "lmscript") { grammar = Grammar::Lmscript; return true; }
  if (name == "lmcode") { grammar = Grammar::Lmcode; return true; }
  return false;
}

std::string expand_message(const std::string &message, const std::string &text) {
  std::string out;
  size_t at = 0;
  for (;;) {
    const size_t mark = message.find("{text}", at);
    if (mark == std::string::npos) return out + message.substr(at);
    out += message.substr(at, mark - at) + text;
    at = mark + 6;
  }
}

void maybe_emit(const LintRule &rule, const Node &node, std::vector<LintFinding> &findings, Suppression *suppression, bool no_suppress) {
  const std::string text = node_text(node);
  try {
    if (!rule.text_matches.empty() && !regex_search_bounded(text, rule.text_regex)) return;
  } catch (const RegexLimit &e) {
    throw QueryError("rule " + rule.id + ": " + e.what());
  }
  if(suppression && suppression->drop(static_cast<size_t>(node_start_row(node)),rule.id,no_suppress))return;
  std::string display = text;
  if (rule.redact) {
    // Only content prefix leaves this boundary; the raw candidate is never persisted.
    const size_t first = display.find_first_not_of("\"'");
    display = first == std::string::npos ? "" : display.substr(first, 4);
    display += "[REDACTED]";
  }
  findings.push_back({node_start_row(node), node_start_col(node),
                      node_end_row(node), node_end_col(node),
                      rule.id, rule.severity, expand_message(rule.message, display), rule.metadata,
                      rule.fix ? std::optional<FixEdit>(FixEdit{node_start_byte(node), node_end_byte(node), *rule.fix, rule.grammar}) : std::nullopt,
                      // Preserve baseline identity without retaining a secret.
                      // Hash the normalized value, since the four-byte display
                      // prefix cannot distinguish credential rotations.
                      rule.redact ? "sha256:" + dslvm::hex(dslvm::sha256(normalize_snippet(text))) : normalize_snippet(text), rule.redact});
}

} // namespace

std::vector<LintRule> load_rule_pack(const std::string &path) {
  const std::string text = read_input_file(path);

  JsonNode root;
  try {
    dslvm::stdlib::json_detail::Parser parser(text);
    root = parser.val();
    parser.ws();
    if (parser.p != text.size()) throw dslvm::Error("trailing bytes");
  } catch (const dslvm::Error &e) {
    throw dslvm::Error(pack_error(path, "invalid JSON (" + std::string(e.what()) + ")"));
  }
  if (root.kind != JsonNode::Kind::Object)
    throw dslvm::Error(pack_error(path, "top level must be an object"));

  Grammar pack_grammar = Grammar::Json;
  bool pack_has_lang = false;
  if (const JsonNode *lang = member(root, "lang")) {
    if (lang->kind != JsonNode::Kind::String || !grammar_of(lang->text, pack_grammar))
      throw dslvm::Error(pack_error(path, "unknown lang '" +
        (lang->kind == JsonNode::Kind::String ? lang->text : "?") + "' (known: json, c, python, bash, java, go, javascript, typescript, tsx, lmscript, lmcode)"));
    pack_has_lang = true;
  }
  const JsonNode *rules_node = member(root, "rules");
  if (!rules_node || rules_node->kind != JsonNode::Kind::Array)
    throw dslvm::Error(pack_error(path, "missing 'rules' array"));

  std::vector<LintRule> rules;
  std::set<std::string> seen_ids;
  size_t index = 0;
  for (const auto &entry : rules_node->array) {
    ++index;
    const std::string who = "rule " + [&] {
      if (const JsonNode *id = member(entry, "id"))
        if (id->kind == JsonNode::Kind::String && !id->text.empty()) return id->text;
      return "#" + std::to_string(index);
    }();
    const auto fail = [&](const std::string &what) {
      throw dslvm::Error(pack_error(path, who + ": " + what));
    };
    LintRule rule;
    const JsonNode *id = member(entry, "id");
    if (!id || id->kind != JsonNode::Kind::String || id->text.empty())
      fail("missing 'id'");
    rule.id = id->text;
    try { rule.metadata = read_rule_metadata(entry); } catch(const dslvm::Error &e) { fail(e.what()); }
    if (!seen_ids.insert(rule.id).second) fail("duplicate rule id (ids must be unique within a pack)");
    const JsonNode *on = member(entry, "on");
    if (on) {
      if (on->kind != JsonNode::Kind::String || on->text.empty()) fail("'on' must be a node-kind string");
      rule.on = on->text;
    }
    const JsonNode *query = member(entry, "query");
    if (query) {
      if (query->kind != JsonNode::Kind::String || query->text.empty()) fail("'query' must be a pattern string");
      rule.query = query->text;
    }
    const JsonNode *pattern = member(entry, "pattern");
    if (pattern) {
      if (pattern->kind != JsonNode::Kind::String || pattern->text.empty()) fail("'pattern' must be a code-shaped pattern string");
      rule.pattern = pattern->text;
    }
    const int selectors = (on ? 1 : 0) + (query ? 1 : 0) + (pattern ? 1 : 0);
    if (selectors == 0) fail("needs 'on', 'query' or 'pattern'");
    if (selectors > 1) {
      std::string present;
      for (const char *key : {"on", "query", "pattern"})
        if (member(entry, key)) present += (present.empty() ? "'" : (present.find(' ') == std::string::npos && selectors == 2 ? "' and '" : "', '")) + std::string(key);
      fail(present + "' are mutually exclusive (pick one selector)");
    }
    for (const char *key : {"capture", "pattern-inside", "pattern-not-inside"}) {
      const JsonNode *value = member(entry, key);
      if (!value) continue;
      if (!pattern && !(query && std::string(key) == "capture")) fail(std::string("'") + key + "' needs 'pattern'");
      if (value->kind != JsonNode::Kind::String || value->text.empty()) fail(std::string("'") + key + "' must be a non-empty string");
      std::string &target = std::string(key) == "capture" ? rule.capture : std::string(key) == "pattern-inside" ? rule.pattern_inside : rule.pattern_not_inside;
      target = value->text;
    }
    const JsonNode *message = member(entry, "message");
    if (!message || message->kind != JsonNode::Kind::String)
      fail("missing 'message'");
    rule.message = message->text;
    if (const JsonNode *redact = member(entry, "redact")) {
      if (redact->kind != JsonNode::Kind::Bool) fail("'redact' must be a boolean");
      rule.redact = redact->boolean;
    }
    if (const JsonNode *fix = member(entry, "fix")) {
      if (fix->kind != JsonNode::Kind::String) fail("'fix' must be a plain replacement string");
      // Replacements are public JSON in normal lint output. Redacted rules
      // cannot safely certify arbitrary replacement text, so refuse it before
      // any finding, error, preview or DB sink can expose the value.
      if (rule.redact) fail("'fix' is not supported for redacted rules");
      rule.fix = fix->text;
    }
    if (const JsonNode *severity = member(entry, "severity")) {
      if (severity->kind != JsonNode::Kind::String || severity->text.empty())
        fail("'severity' must be a string");
      rule.severity = severity->text;
    }
    if (const JsonNode *lang = member(entry, "lang")) {
      if (lang->kind != JsonNode::Kind::String || !grammar_of(lang->text, rule.grammar))
        fail("unknown lang '" + (lang->kind == JsonNode::Kind::String ? lang->text : "?") +
             "' (known: json, c, python, bash, java, go, javascript, typescript, tsx, lmscript, lmcode)");
    } else {
      if (!pack_has_lang) fail("no language (set 'lang' at pack or rule level)");
      rule.grammar = pack_grammar;
    }
    if (const JsonNode *tm = member(entry, "text_matches")) {
      if (tm->kind != JsonNode::Kind::String || tm->text.empty())
        fail("'text_matches' must be a regex string");
      rule.text_matches = tm->text;
      try {
        rule.text_regex = bounded_regex(rule.text_matches); // ECMAScript
      } catch (const std::length_error &e) {
        fail(e.what());
      } catch (const std::regex_error &e) {
        fail("bad text_matches regex '" + rule.text_matches + "'");
      }
    }
    if (!rule.pattern.empty()) {
      // Compile once at pack load, against the rule's grammar; a bad pattern is a pack error (exit 2) naming it.
      try {
        rule.compiled = compile_pattern(rule.grammar, rule.pattern);
        if (!rule.pattern_inside.empty()) rule.compiled_inside = compile_pattern(rule.grammar, rule.pattern_inside);
        if (!rule.pattern_not_inside.empty()) rule.compiled_not_inside = compile_pattern(rule.grammar, rule.pattern_not_inside);
      } catch (const PatternError &e) {
        fail(e.what());
      }
      if (!rule.capture.empty() && !rule.compiled.first_capture.count(rule.capture)) {
        std::string names;
        for (const auto &m : rule.compiled.metavariables) names += (names.empty() ? "" : ", ") + m;
        fail("'capture' names no metavariable of the pattern (metavariables: " + (names.empty() ? std::string("none") : names) + ")");
      }
    }
    rules.push_back(std::move(rule));
  }
  return rules;
}

std::vector<LintFinding> lint_text(const std::vector<LintRule> &rules, const std::string &text, Suppression *suppression, bool no_suppress, const std::map<Grammar, Tree> *parsed, bool lmscript_container) {
  std::vector<LintFinding> findings;
  for (const Grammar grammar : {Grammar::Json, Grammar::C, Grammar::Python, Grammar::Bash, Grammar::Java, Grammar::Go, Grammar::Javascript, Grammar::Typescript, Grammar::Tsx, Grammar::Lmscript, Grammar::Lmcode}) {
    std::vector<const LintRule *> here;
    for (const auto &rule : rules)
      if (rule.grammar == grammar) here.push_back(&rule);
    if (here.empty()) continue;
    std::vector<Tree> owned;
    std::vector<const Tree *> inputs;
    // A verified whole-file lmcode parse is not an injection forest.
    if(parsed && !(grammar==Grammar::Lmcode && lmscript_container)){
      const auto found=parsed->find(grammar);if(found!=parsed->end())inputs.push_back(&found->second);
    }
    if(inputs.empty()){owned=analysis_trees(grammar,text,lmscript_container);for(const auto &tree:owned)inputs.push_back(&tree);}
    for(const Tree *input:inputs){
    const Tree &tree=*input;
    const Node root = node_root(tree);

    // Lint retains its named-node kind index; check uses the corresponding
    // all-node cursor policy for v6 annotated entries.
    std::map<std::string, std::vector<const LintRule *>> by_kind;
    for (const LintRule *rule : here)
      if (!rule->on.empty()) by_kind[rule->on].push_back(rule);
    if (!by_kind.empty()) {
      const NodeList all = tree_nodes(tree); // named nodes only
      for (const Node &node : all.items) {
        const auto it = by_kind.find(node_kind(node));
        if (it == by_kind.end()) continue;
        for (const LintRule *rule : it->second) maybe_emit(*rule, node, findings, suppression, no_suppress);
      }
    }

    // Pattern rules: the compiled query, then the ancestor filters over one parent map per file.
    std::unordered_map<const void *, Node> parent_of; // built on first use: node id -> parent node
    bool parents_built = false;
    const auto ancestor_matches = [&](const Node &node, const CompiledPattern &inside) {
      if (!parents_built) {
        walk_nodes(tree, [&](const Node &n, const std::vector<Node> &ancestors) {
          if (!ancestors.empty()) parent_of.emplace(node_id(n), ancestors.back());
        });
        parents_built = true;
      }
      std::unordered_set<const void *> roots;
      for (const Node &r : pattern_matches(root, inside, grammar).items) roots.insert(node_id(r));
      const Node *cur = &node;
      for (;;) {
        const auto it = parent_of.find(node_id(*cur));
        if (it == parent_of.end()) return false;
        cur = &it->second;
        if (roots.count(node_id(*cur))) return true;
      }
    };
    for (const LintRule *rule : here) {
      if (rule->pattern.empty()) continue;
      NodeList nodes;
      try {
        nodes = pattern_matches(root, rule->compiled, grammar, rule->capture);
      } catch (const QueryError &e) {
        throw QueryError("rule " + rule->id + ": " + e.what());
      }
      for (const Node &node : nodes.items) {
        if (!rule->pattern_inside.empty() && !ancestor_matches(node, rule->compiled_inside)) continue;
        if (!rule->pattern_not_inside.empty() && ancestor_matches(node, rule->compiled_not_inside)) continue;
        maybe_emit(*rule, node, findings, suppression, no_suppress);
      }
    }

    // One query cursor per query rule (later optimization: fold into the walk).
    for (const LintRule *rule : here) {
      if (rule->query.empty()) continue;
      NodeList captures;
      try {
        if (rule->capture.empty()) captures = node_query(root, rule->query, grammar);
        else {
          const auto matches = query_matches(root, rule->query, grammar);
          if (std::find(matches.capture_names.begin(), matches.capture_names.end(), rule->capture) == matches.capture_names.end())
            throw QueryError("capture '" + rule->capture + "' is not declared by query");
          for (const auto &match : matches.matches)
            for (const auto &capture : match.captures)
              if (capture.name == rule->capture) captures.items.push_back(capture.node);
        }
      } catch (const QueryError &e) {
        throw QueryError("rule " + rule->id + ": " + e.what());
      }
      for (const Node &node : captures.items) maybe_emit(*rule, node, findings, suppression, no_suppress);
    }
  }
    } // one independent tree per injected body
  std::stable_sort(findings.begin(), findings.end(), [](const LintFinding &a, const LintFinding &b) {
    return a.row < b.row || (a.row == b.row && a.col < b.col);
  });
  return findings;
}

std::string normalize_snippet(const std::string &text) {
  std::string out;
  bool pending_space = false;
  for (const char c : text) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
      pending_space = !out.empty();
    } else {
      if (pending_space) out += ' ';
      pending_space = false;
      out += c;
    }
  }
  return out;
}

void record_lint_run(Database &db, int64_t started_at, const std::string &command,
                     const std::vector<LintFileRecord> &files, const std::string &requested_scope) {
  Transaction transaction(db); // rolls back on any DbError: no partial runs
  auto run = db.prepare("INSERT INTO runs(started_at,command) VALUES (?,?)");
  run.bind(1, started_at);
  run.bind(2, command);
  run.step();
  auto run_id_query = db.prepare("SELECT last_insert_rowid()");
  run_id_query.step();
  const int64_t run_id = run_id_query.integer(0);
  const int64_t now = std::time(nullptr);
  // The caller supplies the pack path explicitly. Never parse the flattened
  // command string: paths may contain spaces or quoting characters.
  const std::string &scope = requested_scope;

  auto file_id_query = db.prepare("SELECT id FROM files WHERE path = ?");
  auto file_insert = db.prepare("INSERT OR IGNORE INTO files(path,lang,sha256,mtime,size,indexed_at)"
                                " VALUES (?,?,?,?,?,?)");
  auto supersede = db.prepare("UPDATE findings SET superseded = 1 WHERE file_id = ? AND superseded = 0");
  auto observe = db.prepare("INSERT OR REPLACE INTO run_files(run_id,file_id,scope) VALUES (?,?,?)");
  auto finding_insert = db.prepare(
      "INSERT INTO findings(run_id,file_id,rule,severity,message,start_row,start_col,end_row,end_col,superseded,snippet)"
      " VALUES (?,?,?,?,?,?,?,?,?,0,?)");
  for (const auto &file : files) {
    file_insert.bind(1, file.path);
    file_insert.bind(2, file.lang);
    file_insert.bind(3, file.sha256);
    file_insert.bind(4, file.mtime);
    file_insert.bind(5, file.size);
    file_insert.bind(6, now);
    file_insert.step();
    file_insert.reset();
    file_id_query.bind(1, file.path);
    if (!file_id_query.step()) throw DbError(3, "record lint run: no files row for " + file.path);
    const int64_t file_id = file_id_query.integer(0);
    file_id_query.reset();
    observe.bind(1, run_id); observe.bind(2, file_id); observe.bind(3, scope);
    observe.step(); observe.reset();
    supersede.bind(1, file_id);
    supersede.step();
    supersede.reset();
    for (const auto &f : file.findings) {
      finding_insert.bind(1, run_id);
      finding_insert.bind(2, file_id);
      finding_insert.bind(3, f.rule);
      finding_insert.bind(4, f.severity);
      finding_insert.bind(5, f.message);
      finding_insert.bind(6, f.row);
      finding_insert.bind(7, f.col);
      finding_insert.bind(8, f.end_row);
      finding_insert.bind(9, f.end_col);
      finding_insert.bind(10, f.snippet);
      finding_insert.step();
      finding_insert.reset();
    }
  }
  transaction.commit();
}

} // namespace lmok
