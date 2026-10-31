#pragma once
// lmok test is a pack-author oracle: exact (one-based line, rule id) sets.
// Only real CST comment tokens count; a marker inside a string is never an annotation.
#include "lint.hpp"
#include <set>
#include <sstream>
namespace lmok {
struct RuleTestDiff {std::string file,rule,kind;int64_t line;};
struct RuleTestResult {size_t expected=0,actual=0;std::vector<RuleTestDiff> diff;};
inline RuleTestResult test_rule_file(const std::vector<LintRule> &rules,Grammar grammar,const std::string &path,const std::string &source){
  std::set<std::pair<int64_t,std::string>> expected,actual;
  const Tree tree=parse(grammar,source);
  const auto comment = [&](const Node &node,int64_t row_offset,int64_t byte_offset){
    const auto end=static_cast<size_t>(node_end_byte(node)+byte_offset);
    const auto eol=source.find('\n',end);
    if(source.substr(end,eol==std::string::npos?eol:eol-end).find_first_not_of(" \t\r")!=std::string::npos)return;
    auto text=node_text(node);const std::string token="lmok-expect:";const auto mark=text.find(token);
    if(mark==std::string::npos)return;
    if(mark&&std::string("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-").find(text[mark-1])!=std::string::npos)return;
    const int64_t row=row_offset+node_start_row(node)+std::count(text.begin(),text.begin()+mark,'\n')+1;
    auto ids=text.substr(mark+token.size());
    if(ids.size()>=2&&ids.substr(ids.size()-2)=="*/")ids.resize(ids.size()-2);
    std::stringstream list(ids);std::string id;bool any=false;
    while(std::getline(list,id,',')){
      const auto first=id.find_first_not_of(" \t\r");const auto last=id.find_last_not_of(" \t\r");
      id=first==std::string::npos?"":id.substr(first,last-first+1);
      // Pack IDs are strings: retain UTF-8 and namespaces; only whitespace/comma delimit tokens.
      if(id.empty()||std::any_of(id.begin(),id.end(),[](unsigned char c){return c<=32;}))
        throw dslvm::Error(path+":"+std::to_string(row)+": invalid lmok-expect rule list");
      expected.insert({row,id});any=true;
    }
    if(!any||(!ids.empty()&&ids.back()==','))throw dslvm::Error(path+":"+std::to_string(row)+": empty lmok-expect rule");
  };
  const auto injected=grammar==Grammar::Lmscript?lmcode_injections(tree):std::vector<Tree>{};
  std::vector<const Tree *> inputs{&tree};for(const auto &inner:injected)inputs.push_back(&inner);
  for(const Tree *input:inputs)for(const auto &node:tree_nodes(*input).items){
    const auto kind=node_kind(node);
    if(kind.find("comment")!=std::string::npos)comment(node,0,0);
    // C's preprocessor grammar makes the value one opaque token, including
    // trailing comments. Reparse that token so strings still cannot forge an
    // expectation, while #define ... // lmok-expect remains a real comment.
    if(grammar==Grammar::C&&kind=="preproc_arg"){
      const auto value=parse(grammar,node_text(node));
      for(const auto &part:tree_nodes(value).items)if(node_kind(part).find("comment")!=std::string::npos)
        comment(part,node_start_row(node),node_start_byte(node));
    }
  }
  // No ignores/suppression/baseline: they must not make an oracle silently clean.
  for(const auto &finding:lint_text(rules,source,nullptr,false,nullptr,grammar==Grammar::Lmscript))actual.insert({finding.row+1,finding.rule});
  RuleTestResult result;result.expected=expected.size();result.actual=actual.size();
  for(const auto &item:expected)if(!actual.count(item))result.diff.push_back({path,item.second,"missing",item.first});
  for(const auto &item:actual)if(!expected.count(item))result.diff.push_back({path,item.second,"unexpected",item.first});
  return result;
}
inline std::string rule_test_json(const RuleTestResult &result){
  std::string out="{\"ok\":"+std::string(result.diff.empty()?"true":"false")+",\"expected\":"+std::to_string(result.expected)+",\"actual\":"+std::to_string(result.actual)+",\"diff\":[";
  bool first=true;for(const auto &d:result.diff){if(!first)out+=',';first=false;out+="{\"file\":"+meta_quote(d.file)+",\"line\":"+std::to_string(d.line)+",\"rule\":"+meta_quote(d.rule)+",\"kind\":"+meta_quote(d.kind)+"}";}return out+"]}";
}
}
