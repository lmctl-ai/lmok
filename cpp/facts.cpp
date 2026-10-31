// E6 facts extraction; see facts.hpp for the contract.
#include "facts.hpp"
#include "facts_c_embedded.hpp"
#include "facts_python_embedded.hpp"
#include "facts_bash_embedded.hpp"
#include "facts_json_embedded.hpp"
#include "facts_java_embedded.hpp"
#include "facts_go_embedded.hpp"
#include "facts_javascript_embedded.hpp"
#include "facts_typescript_embedded.hpp"
#include "facts_tsx_embedded.hpp"
#include "facts_lmscript_embedded.hpp"
#include "facts_lmcode_embedded.hpp"

#include <algorithm>
#include <sstream>
#include <unordered_map>

namespace lmok {

const std::string &facts_query(Grammar grammar) {
  static const std::string c(reinterpret_cast<const char *>(embedded_facts_c), embedded_facts_c_size);
  static const std::string python(reinterpret_cast<const char *>(embedded_facts_python), embedded_facts_python_size);
  static const std::string bash(reinterpret_cast<const char *>(embedded_facts_bash), embedded_facts_bash_size);
  static const std::string json(reinterpret_cast<const char *>(embedded_facts_json), embedded_facts_json_size);
  static const std::string java(reinterpret_cast<const char *>(embedded_facts_java), embedded_facts_java_size);
  static const std::string go(reinterpret_cast<const char *>(embedded_facts_go), embedded_facts_go_size);
  static const std::string javascript(reinterpret_cast<const char *>(embedded_facts_javascript), embedded_facts_javascript_size);
  static const std::string typescript(reinterpret_cast<const char *>(embedded_facts_typescript), embedded_facts_typescript_size);
  static const std::string tsx(reinterpret_cast<const char *>(embedded_facts_tsx), embedded_facts_tsx_size);
  static const std::string lmscript(reinterpret_cast<const char *>(embedded_facts_lmscript), embedded_facts_lmscript_size);
  static const std::string lmcode(reinterpret_cast<const char *>(embedded_facts_lmcode), embedded_facts_lmcode_size);
  switch (grammar) {
  case Grammar::C: return c;
  case Grammar::Python: return python;
  case Grammar::Bash: return bash;
  case Grammar::Json: return json;
  case Grammar::Java: return java;
  case Grammar::Go: return go;
  case Grammar::Javascript: return javascript;
  case Grammar::Typescript: return typescript;
  case Grammar::Tsx: return tsx;
  case Grammar::Lmscript: return lmscript;
  case Grammar::Lmcode: return lmcode;
  }
  return json;
}

namespace {

bool starts_with(const std::string &s, const char *prefix) { return s.rfind(prefix, 0) == 0; }

// Definitions that open a scope for the facts nested inside them.
bool is_scope_kind(const std::string &kind) {
  return kind == "def.function" || kind == "def.method" || kind == "def.prototype" ||
         kind == "def.class" || kind == "def.interface" || kind == "def.struct" || kind == "def.union" || kind == "def.enum" ||
         kind == "def.key";
}

std::string default_confidence(const std::string &kind) {
  if (starts_with(kind, "def.")) return "exact";
  if (kind == "ref.call" || kind == "ref.read") return "candidate";
  return "exact"; // edges with a literal target
}

// Strip one layer of matching quotes / angle brackets.
std::string unwrap(const std::string &text, char open, char close) {
  if (text.size() >= 2 && text.front() == open && text.back() == close)
    return text.substr(1, text.size() - 2);
  return text;
}

struct Raw {
  Fact fact;
  Node anchor;
  bool is_def = false;
};

} // namespace

std::vector<Fact> extract_facts(const Tree &tree, Grammar grammar) {
  const Node root = node_root(tree);
  const QueryMatches qm = query_matches(root, facts_query(grammar), grammar);

  std::vector<Raw> raws;
  for (const QueryMatch &match : qm.matches) {
    const Node *name = nullptr, *anchor = nullptr;
    std::string kind;
    for (const QueryCapture &cap : match.captures) {
      if (cap.name == "name") name = &cap.node;
      else if (starts_with(cap.name, "def.") || starts_with(cap.name, "ref.")) { kind = cap.name; anchor = &cap.node; }
      // other captures (`@_cmd`) are predicate helpers
    }
    if (!name || !anchor) continue;
    Raw raw;
    raw.anchor = *anchor;
    raw.is_def = starts_with(kind, "def.");
    Fact &f = raw.fact;
    f.kind = kind;
    f.name = node_text(*name);
    const auto &props = qm.properties[match.pattern];
    const auto conf = props.find("confidence");
    f.confidence = conf != props.end() ? conf->second : default_confidence(kind);
    // Edge target normalisation (host logic the query cannot express).
    if (kind == "ref.include") f.name = unwrap(f.name, '<', '>');
    if (grammar == Grammar::Go && kind == "ref.import")
      f.name = unwrap(unwrap(f.name, '"', '"'), '`', '`');
    if (grammar == Grammar::Javascript && kind == "ref.import")
      f.name = unwrap(unwrap(f.name, '"', '"'), '\'', '\'');
    if (grammar == Grammar::Typescript && kind == "ref.import")
      f.name = unwrap(unwrap(f.name, '"', '"'), '\'', '\'');
    if (grammar == Grammar::Tsx && kind == "ref.import")
      f.name = unwrap(unwrap(f.name, '"', '"'), '\'', '\'');
    if (kind == "ref.source") {
      const std::string raw_text = f.name;
      f.name = unwrap(unwrap(f.name, '"', '"'), '\'', '\'');
      if (raw_text.find('$') != std::string::npos || raw_text.find('`') != std::string::npos)
        f.confidence = "unresolved";
    }
    if (grammar == Grammar::Lmscript && (kind == "ref.import" || kind == "ref.schema"))
      f.name = unwrap(unwrap(f.name, '"', '"'), '\'', '\'');
    f.row = node_start_row(*anchor);
    f.col = node_start_col(*anchor);
    f.end_row = node_end_row(*anchor);
    f.end_col = node_end_col(*anchor);
    f.start_byte = node_start_byte(*anchor);
    f.end_byte = node_end_byte(*anchor);
    f.name_row = node_start_row(*name);
    f.name_col = node_start_col(*name);
    raws.push_back(std::move(raw));
  }

  // Scope-defining anchors by node identity.
  std::unordered_map<const void *, size_t> scopes;
  for (size_t i = 0; i < raws.size(); ++i)
    if (raws[i].is_def && is_scope_kind(raws[i].fact.kind)) scopes.emplace(node_id(raws[i].anchor), i);

  // Scope chain: enclosing scope definitions, outermost first. A definition's
  // own anchor is excluded (we start from its parent).
  for (Raw &raw : raws) {
    std::vector<std::string> chain;
    size_t nearest = static_cast<size_t>(-1);
    Node cur = raw.anchor;
    while (node_has_parent(cur)) {
      cur = node_parent(cur);
      const auto it = scopes.find(node_id(cur));
      if (it == scopes.end()) continue;
      if (nearest == static_cast<size_t>(-1)) nearest = it->second;
      chain.push_back(raws[it->second].fact.name);
    }
    std::string scope;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      if (!scope.empty()) scope += '.';
      scope += *it;
    }
    raw.fact.scope = scope;
    if (grammar == Grammar::Python && raw.fact.kind == "def.function" &&
        nearest != static_cast<size_t>(-1) && raws[nearest].fact.kind == "def.class")
      raw.fact.kind = "def.method";
  }

  std::vector<Fact> facts;
  facts.reserve(raws.size());
  for (Raw &raw : raws) facts.push_back(std::move(raw.fact));
  if(grammar==Grammar::Lmscript){
    for(const auto &inner:lmcode_injections(tree)){
      auto injected=extract_facts(inner,Grammar::Lmcode);
      for(auto &fact:injected){fact.language="lmcode";facts.push_back(std::move(fact));}
    }
  }
  std::stable_sort(facts.begin(), facts.end(), [](const Fact &a, const Fact &b) {
    if (a.start_byte != b.start_byte) return a.start_byte < b.start_byte;
    if (a.kind != b.kind) return a.kind < b.kind;
    if (a.name != b.name) return a.name < b.name;
    return a.end_byte < b.end_byte;
  });
  facts.erase(std::unique(facts.begin(), facts.end(), [](const Fact &a, const Fact &b) {
                return a.kind == b.kind && a.name == b.name && a.start_byte == b.start_byte &&
                       a.end_byte == b.end_byte && a.scope == b.scope;
              }),
              facts.end());
  return facts;
}

std::string fact_json(const Fact &f, const std::string &file, const std::string &lang,
                      const std::function<std::string(const std::string &)> &escape) {
  std::ostringstream out;
  out << "{\"file\":\"" << escape(file) << "\",\"lang\":\"" << (f.language.empty()?lang:f.language)
      << "\",\"kind\":\"" << f.kind << "\",\"name\":\"" << escape(f.name)
      << "\",\"scope\":\"" << escape(f.scope) << "\",\"confidence\":\"" << f.confidence
      << "\",\"row\":" << f.row << ",\"col\":" << f.col
      << ",\"end_row\":" << f.end_row << ",\"end_col\":" << f.end_col
      << ",\"start_byte\":" << f.start_byte << ",\"end_byte\":" << f.end_byte
      << ",\"name_row\":" << f.name_row << ",\"name_col\":" << f.name_col << "}";
  return out.str();
}

} // namespace lmok
