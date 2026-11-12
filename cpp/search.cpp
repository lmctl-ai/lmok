#include "search.hpp"
#include "sqlite3.h"
#include <iterator>
#include <memory>
#include <limits>
#include <set>
#include <ostream>
#include <sstream>
#include <vector>

namespace lmok {
namespace {
bool trigram_index(Database &db) {
  auto q = db.prepare("SELECT sql FROM sqlite_master WHERE name='file_text'");
  return q.step() && q.text(0).find("tokenize='trigram'") != std::string::npos;
}

// A byte search is narrowed only by a valid Unicode phrase of at least three
// characters. Short or malformed UTF-8 patterns must not silently lose hits.
std::optional<size_t> unicode_length(const std::string &s) {
  size_t count = 0;
  for (size_t i = 0; i < s.size(); ++count) {
    const auto first = static_cast<unsigned char>(s[i++]);
    if (first < 0x80) continue;
    int extra; unsigned value, minimum;
    if (first >= 0xc2 && first <= 0xdf) { extra = 1; value = first & 31; minimum = 0x80; }
    else if (first >= 0xe0 && first <= 0xef) { extra = 2; value = first & 15; minimum = 0x800; }
    else if (first >= 0xf0 && first <= 0xf4) { extra = 3; value = first & 7; minimum = 0x10000; }
    else return {};
    while (extra--) {
      if (i == s.size()) return {};
      const auto next = static_cast<unsigned char>(s[i++]);
      if ((next & 0xc0) != 0x80) return {};
      value = (value << 6) | (next & 63);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return {};
  }
  return count;
}
std::string quote_phrase(const std::string &s) {
  std::string result = "\"";
  for (char ch : s) { result += ch; if (ch == '"') result += ch; }
  return result + '"';
}
bool is_fts_space(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}
bool is_fts_bareword(char ch) {
  const auto u = static_cast<unsigned char>(ch);
  return u >= 0x80 || (u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') ||
         (u >= 'a' && u <= 'z') || u == '_' || u == 0x1a;
}
bool is_all_bareword(const std::string &s, size_t begin, size_t end) {
  if (begin >= end) return false;
  for (size_t i = begin; i < end; ++i)
    if (!is_fts_bareword(s[i])) return false;
  return true;
}
bool is_fts_keyword(const std::string &tok) {
  return tok == "AND" || tok == "OR" || tok == "NOT";
}
size_t scan_quoted(const std::string &s, size_t start, bool &closed) {
  closed = false;
  size_t j = start + 1;
  while (j < s.size()) {
    if (s[j] == '"') {
      ++j;
      if (j < s.size() && s[j] == '"') { ++j; continue; }
      closed = true;
      return j;
    }
    ++j;
  }
  return j;
}
bool is_single_colon(const std::string &s, size_t pos) {
  if (pos >= s.size() || s[pos] != ':') return false;
  if (pos > 0 && s[pos - 1] == ':') return false;
  if (pos + 1 < s.size() && s[pos + 1] == ':') return false;
  if (pos + 2 < s.size() && s[pos + 1] == '/' && s[pos + 2] == '/') return false;
  return true;
}
bool is_fts_col_minus(const std::string &s, size_t pos) {
  size_t j = pos + 1;
  while (j < s.size() && is_fts_space(s[j])) ++j;
  if (j >= s.size()) return false;
  if (s[j] == '{') return true;
  if (s[j] == '"') {
    bool closed = false;
    j = scan_quoted(s, j, closed);
    if (!closed) return false;
  } else if (is_fts_bareword(s[j])) {
    const size_t start = j;
    while (j < s.size() && is_fts_bareword(s[j])) ++j;
    if (is_fts_keyword(s.substr(start, j - start))) return false;
  } else {
    return false;
  }
  while (j < s.size() && is_fts_space(s[j])) ++j;
  return is_single_colon(s, j);
}
bool is_fts_plus(const std::string &s, size_t pos, bool after_phrase) {
  if (!after_phrase) return false;
  if (pos > 0 && s[pos - 1] == '+') return false;
  if (pos + 1 < s.size() && s[pos + 1] == '+') return false;
  size_t j = pos + 1;
  while (j < s.size() && is_fts_space(s[j])) ++j;
  if (j >= s.size()) return false;
  return is_fts_bareword(s[j]) || s[j] == '"' || s[j] == '@';
}
bool is_fts_caret(const std::string &s, size_t pos) {
  if (pos + 1 >= s.size()) return false;
  const char next = s[pos + 1];
  return is_fts_bareword(next) || next == '"' || next == '@';
}
// SQLite FTS5 only accepts [A-Za-z0-9_\x1a] and non-ASCII bytes in unquoted
// barewords, rejecting common code tokens like `@Unauthenticated`, `pkg.Type`,
// `tree-sitter`, `#include`, `$var`, or `std::vector` as query syntax errors.
// Quote unquoted literal code tokens while preserving FTS5 query operators.
std::string normalize_fts_query(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  bool can_precede_colon = false;
  bool after_phrase = false;
  for (size_t i = 0; i < s.size();) {
    if (is_fts_space(s[i])) {
      out += s[i++];
      continue;
    }
    if (s[i] == '"') {
      bool closed = false;
      const size_t j = scan_quoted(s, i, closed);
      out.append(s, i, j - i);
      i = j;
      if (closed && i < s.size() && s[i] == '*' &&
          (i + 1 == s.size() || is_fts_space(s[i + 1]) || s[i + 1] == ')' || s[i + 1] == ',')) {
        out += s[i++];
        can_precede_colon = false;
        after_phrase = true;
      } else {
        can_precede_colon = closed;
        after_phrase = closed;
      }
      continue;
    }
    if (s[i] == '(' || s[i] == ')' || s[i] == '{' || s[i] == '}' || s[i] == ',') {
      const char ch = s[i++];
      out += ch;
      can_precede_colon = (ch == '}');
      after_phrase = false;
      continue;
    }
    if (s[i] == ':' && can_precede_colon && is_single_colon(s, i)) {
      out += s[i++];
      can_precede_colon = false;
      after_phrase = false;
      continue;
    }
    if (s[i] == '-' && is_fts_col_minus(s, i)) {
      out += s[i++];
      can_precede_colon = false;
      after_phrase = false;
      continue;
    }
    if (s[i] == '^' && is_fts_caret(s, i)) {
      out += s[i++];
      can_precede_colon = false;
      after_phrase = false;
      continue;
    }
    if (s[i] == '+' && is_fts_plus(s, i, after_phrase)) {
      out += s[i++];
      can_precede_colon = false;
      after_phrase = false;
      continue;
    }
    std::string tok;
    size_t j = i;
    while (j < s.size()) {
      const char ch = s[j];
      if (is_fts_space(ch) || ch == '(' || ch == ')' || ch == '{' || ch == '}' || ch == ',') break;
      if (ch == '"') {
        bool closed = false;
        const size_t end_q = scan_quoted(s, j, closed);
        if (!closed) break;
        tok.append(s, j, end_q - j);
        j = end_q;
        continue;
      }
      if (ch == ':' && j > i && j - i == tok.size() &&
          is_all_bareword(tok, 0, tok.size()) && !is_fts_keyword(tok) &&
          is_single_colon(s, j)) {
        break;
      }
      if (ch == '+' && j > i && j - i == tok.size() &&
          is_all_bareword(tok, 0, tok.size()) && !is_fts_keyword(tok) &&
          is_fts_plus(s, j, true)) {
        break;
      }
      tok += ch;
      ++j;
    }
    i = j;
    bool prefix_star = false;
    if (tok.size() >= 2 && tok.back() == '*' && is_fts_bareword(tok[tok.size() - 2])) {
      prefix_star = true;
      tok.pop_back();
    }
    const bool pure_bare = is_all_bareword(tok, 0, tok.size());
    const bool kw = !prefix_star && pure_bare && is_fts_keyword(tok);
    if (pure_bare && !(prefix_star && is_fts_keyword(tok))) {
      out += tok;
    } else {
      out += quote_phrase(tok);
    }
    if (prefix_star) out += '*';
    can_precede_colon = pure_bare && !kw && !prefix_star;
    after_phrase = !kw && !(pure_bare && !prefix_star && tok == "NEAR");
  }
  return out;
}

// FTS5 has no offsets() function. Its extension API supplies phrase-instance
// token positions; tokenize the stored column to map those to original bytes.
// Unlike highlight(), this preserves adjacent and overlapping occurrences.
struct TokenStarts {
  const std::set<int> &wanted;
  std::vector<int> bytes;
  int token = -1;
};
int token_start(void *context, int flags, const char *, int, int start, int) noexcept {
  try {
    auto &state = *static_cast<TokenStarts *>(context);
    if (!(flags & FTS5_TOKEN_COLOCATED)) {
      ++state.token;
      if (state.wanted.count(state.token)) state.bytes.push_back(start);
      if (state.token == *state.wanted.rbegin()) return SQLITE_DONE;
    }
    return SQLITE_OK;
  } catch (...) { return SQLITE_NOMEM; }
}
void offsets(const Fts5ExtensionApi *api, Fts5Context *fts, sqlite3_context *context,
             int argc, sqlite3_value **argv) noexcept {
  try {
    if (argc != 1) { sqlite3_result_error(context, "expected hit limit", -1); return; }
    const auto limit = sqlite3_value_int64(argv[0]);
    if (limit <= 0) { sqlite3_result_error(context, "invalid hit limit", -1); return; }
    const char *text = nullptr; int bytes = 0, instances = 0;
    int rc = api->xColumnText(fts, 2, &text, &bytes);
    if (rc == SQLITE_OK) rc = api->xInstCount(fts, &instances);
    std::set<int> wanted;
    for (int i = 0; rc == SQLITE_OK && i < instances; ++i) {
      int phrase, column, token;
      rc = api->xInst(fts, i, &phrase, &column, &token);
      if (rc == SQLITE_OK && column == 2) {
        if (token < 0) { rc = SQLITE_CORRUPT; break; }
        wanted.insert(token);
        if (wanted.size() > static_cast<uint64_t>(limit)) wanted.erase(std::prev(wanted.end()));
      }
    }
    TokenStarts starts{wanted, {}, -1};
    if (rc == SQLITE_OK && !wanted.empty()) {
      rc = api->xTokenize(fts, text, bytes, &starts, token_start);
      if (rc == SQLITE_DONE) rc = SQLITE_OK;
      if (rc == SQLITE_OK && starts.bytes.size() != wanted.size()) rc = SQLITE_CORRUPT;
    }
    if (rc != SQLITE_OK) { sqlite3_result_error_code(context, rc); return; }
    std::string result;
    for (int position : starts.bytes) result += std::to_string(position) + " ";
    sqlite3_result_text(context, result.data(), static_cast<int>(result.size()), SQLITE_TRANSIENT);
  } catch (...) { sqlite3_result_error_nomem(context); }
}
void register_offsets(Database &db) {
  sqlite3_stmt *raw = nullptr;
  int rc = sqlite3_prepare_v2(db.handle(), "SELECT fts5(?1)", -1, &raw, nullptr);
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw, sqlite3_finalize);
  fts5_api *api = nullptr;
  if (rc == SQLITE_OK) rc = sqlite3_bind_pointer(raw, 1, &api, "fts5_api_ptr", nullptr);
  if (rc == SQLITE_OK && sqlite3_step(raw) != SQLITE_ROW) rc = sqlite3_errcode(db.handle());
  if (rc != SQLITE_OK || !api || api->xCreateFunction(api, "lmok_find_offsets", nullptr, offsets, nullptr) != SQLITE_OK)
    throw DbError(3, "cannot register FTS5 search offsets");
}
}

bool search_needs_byte_scan(const std::string &text) {
  return text.find('\0') != std::string::npos || !unicode_length(text);
}

void ensure_trigram_index(Database &db) {
  Transaction transaction(db);
  if (!trigram_index(db) || !db_has_column(db, "file_text", "byte_scan")) {
    db.exec("CREATE VIRTUAL TABLE file_text_trigram USING fts5(path UNINDEXED, lang UNINDEXED, text, byte_scan UNINDEXED, tokenize='trigram')");
    {
      auto old = db.prepare("SELECT rowid,path,lang,text FROM file_text");
      auto insert = db.prepare("INSERT INTO file_text_trigram(rowid,path,lang,text,byte_scan) VALUES (?,?,?,?,?)");
      while (old.step()) {
        const std::string text = old.text(3);
        insert.bind(1, old.integer(0)); insert.bind(2, old.text(1)); insert.bind(3, old.text(2));
        insert.bind(4, text); insert.bind(5, static_cast<int64_t>(search_needs_byte_scan(text)));
        insert.step(); insert.reset();
      }
    }
    db.exec("DROP TABLE file_text");
    db.exec("ALTER TABLE file_text_trigram RENAME TO file_text");
  }
  transaction.commit();
}

int find_text(Database &db, const FindOptions &options, std::ostream &out, std::ostream &err) {
  Transaction snapshot(db, false);
  const bool trigram = trigram_index(db);
  const bool classified = db_has_column(db, "file_text", "byte_scan");
  const auto length = unicode_length(options.pattern);
  const bool match = options.fts || (trigram && classified && length && *length >= 3);
  if (!match)
    err << "find: scanning stored text (" << (!trigram || !classified ? "legacy text index; run `lmok index` to upgrade" : "literal shorter than 3 Unicode characters or invalid UTF-8") << ")\n";
  else if (options.fts && !trigram)
    err << "find: legacy word-token FTS5 semantics; run `lmok index` for trigram search\n";
  if (options.fts) register_offsets(db);
  const std::string pattern = options.fts ? normalize_fts_query(options.pattern) : quote_phrase(options.pattern);
  const std::string language = options.lang ? " AND lang=?2" : "";
  std::string sql;
  if (options.fts) {
    sql = "SELECT path,text,lmok_find_offsets(file_text,?3),rowid FROM file_text WHERE file_text MATCH ?1" + language;
  } else if (match) {
    // The Unicode index is not authoritative for NUL/malformed byte-text.
    // Classify once during indexing, not by re-reading all text on every find.
    sql = "SELECT path,text,byte_scan,rowid FROM file_text WHERE file_text MATCH ?1 AND byte_scan=0" + language +
        " UNION ALL SELECT path,text,byte_scan,rowid FROM file_text WHERE (byte_scan IS NULL OR byte_scan<>0)" + language;
  } else {
    sql = "SELECT path,text,1,rowid FROM file_text WHERE 1" + language;
  }
  sql += " ORDER BY path COLLATE BINARY, rowid";
  auto query = db.prepare(sql);
  if (match) query.bind(1, pattern);
  if (options.lang) query.bind(2, *options.lang);
  if (options.fts) {
    // Keep at most limit+1 distinct starts per file, including the truncation
    // sentinel; do not materialize millions of positions for a capped query.
    const auto max = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    const auto cap = options.limit >= max ? max : static_cast<uint64_t>(options.limit) + 1;
    query.bind(3, static_cast<int64_t>(cap));
  }
  size_t emitted = 0;
  bool warned_bytes = false;
  try {
    while (query.step()) {
      const std::string path = query.text(0), source = query.text(1);
      if (!options.fts && match && !warned_bytes && (query.is_null(2) || query.integer(2) != 0)) {
        err << "find: scanning stored byte-text outside the Unicode index\n";
        warned_bytes = true;
      }
      size_t row = 0, line_start = 0;
      auto emit = [&](size_t position) {
        if (emitted == options.limit) {
          err << "find: truncated after " << options.limit << " hits (use --limit N)\n";
          return false;
        }
        // Positions arrive in byte order, so traverse each stored file once.
        for (;;) {
          const size_t newline = source.find('\n', line_start);
          if (newline == std::string::npos || newline >= position) break;
          line_start = newline + 1; ++row;
        }
        size_t end = source.find('\n', line_start);
        if (end == std::string::npos) end = source.size();
        if (end > line_start && source[end - 1] == '\r') --end;
        out << path << ':' << row << ':' << position - line_start << ": ";
        out.write(source.data() + line_start, static_cast<std::streamsize>(end - line_start));
        out << '\n'; ++emitted;
        return true;
      };
      if (options.fts) {
        std::istringstream positions(query.text(2)); size_t position;
        while (positions >> position) {
          if (position >= source.size()) throw DbError(3, "invalid FTS5 byte offset");
          if (!emit(position)) { out.flush(); return out ? 0 : 4; }
        }
      } else {
        size_t position = source.find(options.pattern);
        while (position != std::string::npos) {
          if (!emit(position)) { out.flush(); return out ? 0 : 4; }
          position = source.find(options.pattern, position + 1); // overlapping hits count
        }
      }
      if (!out) return 4;
    }
  } catch (const DbError &error) {
    const std::string message = error.what();
    if (options.fts && (message.find("fts5:") != std::string::npos ||
        message.find("unterminated string") != std::string::npos || message.find("no such column:") != std::string::npos))
      throw DbError(2, "invalid FTS5 query: " + message);
    throw;
  }
  snapshot.commit();
  out.flush();
  return out ? 0 : 4;
}
}
