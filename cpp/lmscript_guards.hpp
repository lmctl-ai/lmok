#pragma once
// Structural refinements for the migration pack. No execution, regex heuristics or rule-ID dispatch.
#include "host.hpp"
#include "stdlib/json.hpp"
#include <optional>
#include <set>
#include <cctype>
namespace lmok {
inline std::vector<Node> migration_children(const Node& n){std::vector<Node> out;for(int64_t i=0;i<node_child_count(n);++i){auto c=node_child(n,i);if(node_kind(c)!="comment")out.push_back(c);}return out;}
inline bool migration_same(const Node&a,const Node&b){return a.owner==b.owner&&node_start_byte(a)==node_start_byte(b)&&node_end_byte(a)==node_end_byte(b);}
inline bool migration_literal(const Node& n,std::string& value){
 if(node_kind(n)!="plain_string")return false;
 const auto text=node_text(n);
 try{dslvm::stdlib::json_detail::Parser p(text);auto v=p.val();if(v.kind!=dslvm::stdlib::JsonNode::Kind::String)return false;value=v.text;return true;}catch(const std::exception&){return false;}
}
inline bool migration_bound(const Node& use){
 const auto name=node_text(use);Node cur=use;bool function_body=false;
 while(node_has_parent(cur)){
  const Node parent=node_parent(cur);const auto kind=node_kind(parent);const auto children=migration_children(parent);
  if(kind=="parameter_list")return true; // this identifier declares a parameter, not a global read
  if((kind=="let_statement"||kind=="function_declaration"||kind=="for_statement")&&!children.empty()&&migration_same(children[0],use))return true;
  if(kind=="function_declaration"){
   function_body=true;
   for(const auto& c:children)if(node_kind(c)=="parameter_list")for(const auto& p:migration_children(c))if(node_text(p)==name)return true;
  }
  if(kind=="for_statement"){
   for(const auto& c:children)if(node_kind(c)=="block"&&node_start_byte(use)>=node_start_byte(c)&&!children.empty()&&node_text(children[0])==name)return true;
  }
  if(kind=="block"||kind=="source_file")for(const auto& c:children){
   const auto ck=node_kind(c);const auto parts=migration_children(c);if(parts.empty())continue;
   if(ck=="function_declaration"&&node_text(parts[0])==name)return true;
   if(ck=="let_statement"&&node_text(parts[0])==name&&(node_end_byte(c)<=node_start_byte(use)||(kind=="source_file"&&function_body)))return true;
   if(ck=="import_statement"&&parts.size()>1&&node_text(parts.back())==name&&node_end_byte(c)<=node_start_byte(use))return true;
  }
  cur=parent;
 }
 return false;
}
inline std::string migration_executable(std::string name){const auto slash=name.find_last_of("/\\");if(slash!=std::string::npos)name=name.substr(slash+1);for(auto suffix:{".exe",".cmd",".bat"})if(name.size()>=4&&name.substr(name.size()-4)==suffix){name.resize(name.size()-4);break;}return name;}
inline std::optional<std::vector<std::string>> migration_argv(const Node& call){
 auto kids=migration_children(call);std::vector<std::string> out;
 if(node_kind(call)=="system_call"){
  if(kids.empty()||node_kind(kids[0])!="list_literal")return std::nullopt;
  for(const auto& c:migration_children(kids[0])){std::string s;if(!migration_literal(c,s))return std::nullopt;out.push_back(s);}return out;
 }
 if(node_kind(call)!="command")return std::nullopt;
 // Commands already passed the command CST grammar. Tokenize literal argv only; fields remain unknown.
 const auto text=node_text(call);std::string word;char quote=0;bool started=false;std::vector<std::string> all;
 for(size_t i=0;i<text.size();++i){char c=text[i];if((c=='$'||c=='`')&&quote!='\'')return std::nullopt;
  if(c=='\\'&&quote!='\''){if(i+1==text.size())return std::nullopt;word+=text[++i];started=true;continue;}
  if(quote){if(c==quote)quote=0;else word+=c;started=true;continue;}
  if(c=='\''||c=='"'){quote=c;started=true;continue;}
  if(std::isspace(static_cast<unsigned char>(c))||c==';'){if(started){all.push_back(word);word.clear();started=false;}continue;}
  word+=c;started=true;
 }
 if(started)all.push_back(word);
 if(quote||all.empty()||all[0]!="system")return std::nullopt;
 all.erase(all.begin());return all;
}
inline std::optional<Node> migration_system(const Node& n){Node cur=n;for(;;){if(node_kind(cur)=="system_call"||node_kind(cur)=="command")return cur;if(!node_has_parent(cur))return std::nullopt;cur=node_parent(cur);}}
inline bool migration_guard(const std::string& guard,const Node& n){
 if(guard.empty())return true;
 if(guard=="unbound_identifier")return !migration_bound(n);
 if(guard=="computed_parallel"){const auto args=migration_children(n);return args.empty()||node_kind(args[0])!="list_literal";}
 const auto call=migration_system(n);if(!call)return false;const auto parsed=migration_argv(*call);if(!parsed||parsed->empty())return false;const auto& argv=*parsed;const auto exe=migration_executable(argv[0]);
 if(guard=="unchecked_lmctl_script"){
  if(exe!="lmctl"||argv.size()<2||argv[1]!="script")return false;
  for(size_t i=2;i<argv.size();++i){if(argv[i]=="--"||argv[i].empty()||argv[i][0]!='-')break;if(argv[i]=="--check")return false;}
  return true;
 }
 if(guard=="host_option"){
  if(exe!="lmctl"&&exe!="lmscript")return false;
  const auto kids=migration_children(*call);if(kids.empty())return false;const auto tokens=migration_children(kids[0]);size_t target=tokens.size();for(size_t i=0;i<tokens.size();++i)if(migration_same(tokens[i],n))target=i;
  size_t first=exe=="lmctl"&&argv.size()>1&&argv[1]=="script"?2:1;
  const std::set<std::string> no_value={"--check","--dry-run","--json","--without-std","--debug","--verbose","--no-color"};
  for(size_t i=first;i<argv.size();++i){if(argv[i]=="--"||argv[i].empty()||argv[i][0]!='-')return false;if(i==target)return true;if(!no_value.count(argv[i]))return false;}
 }
 return false;
}
} // namespace lmok
