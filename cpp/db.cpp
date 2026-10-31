#include "db.hpp"
#include "dslvm.hpp" // existing SHA-256/hex helpers; no tree-sitter dependency
#include "sqlite3.h"

#include <cstdlib>
#include <ctime>
#include <sstream>
#include <utility>

namespace lmok {
namespace {
constexpr const char *schema_sql = R"LMOKSQL(
CREATE TABLE meta (
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
-- rows: schema_version (=2), lmok_version, created_at

CREATE TABLE files (
  id         INTEGER PRIMARY KEY,
  path       TEXT NOT NULL UNIQUE,   -- repo-relative, forward slashes
  lang       TEXT NOT NULL,          -- json | c | python | bash
  sha256     TEXT NOT NULL,          -- content hash (invalidation key)
  mtime      INTEGER NOT NULL,
  size       INTEGER NOT NULL,
  indexed_at INTEGER NOT NULL
);

CREATE TABLE symbols (
  id         INTEGER PRIMARY KEY,
  file_id    INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
  name       TEXT NOT NULL,
  kind       TEXT NOT NULL,           -- function|class|variable|macro|struct|key
  start_row  INTEGER NOT NULL, start_col INTEGER NOT NULL,
  end_row    INTEGER NOT NULL, end_col INTEGER NOT NULL,
  scope      TEXT NOT NULL DEFAULT '', -- enclosing path, e.g. "Server.handle"; '' = top level
  confidence TEXT NOT NULL DEFAULT 'exact' -- exact|candidate|unresolved|external
);
CREATE INDEX idx_symbols_name ON symbols(name);
CREATE INDEX idx_symbols_file ON symbols(file_id);

CREATE TABLE refs (
  id         INTEGER PRIMARY KEY,
  file_id    INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
  name       TEXT NOT NULL,           -- identifier being referenced
  kind       TEXT NOT NULL,           -- call|read|write|include|import
  start_row  INTEGER NOT NULL, start_col INTEGER NOT NULL,
  end_row    INTEGER NOT NULL, end_col INTEGER NOT NULL,
  confidence TEXT NOT NULL DEFAULT 'exact'
);
CREATE INDEX idx_refs_name ON refs(name);

CREATE TABLE edges (                 -- import/include graph
  from_file     INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
  to_name       TEXT NOT NULL,       -- as written: "util.h", "os.path", "./lib.sh"
  kind          TEXT NOT NULL,       -- include|import|from_import|source
  resolved_file INTEGER REFERENCES files(id),  -- NULL = external/unresolved
  confidence    TEXT NOT NULL DEFAULT 'exact'
);

CREATE TABLE runs (
  id         INTEGER PRIMARY KEY,
  started_at INTEGER NOT NULL,
  command    TEXT NOT NULL           -- e.g. "lint rules/c-tier1.json src/"
);

-- Every file visited by a recorded lint run, including clean files. Scope is
-- the rule-pack path, so a scan with a different pack cannot reuse findings.
CREATE TABLE run_files (
  run_id  INTEGER NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
  file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
  scope   TEXT NOT NULL,
  PRIMARY KEY (run_id, file_id, scope)
);

CREATE INDEX idx_run_files_scope ON run_files(file_id, scope, run_id);

CREATE TABLE findings (
  id         INTEGER PRIMARY KEY,
  run_id     INTEGER NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
  file_id    INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
  rule       TEXT NOT NULL,
  severity   TEXT NOT NULL,
  message    TEXT NOT NULL,
  start_row  INTEGER NOT NULL, start_col INTEGER NOT NULL,
  end_row    INTEGER NOT NULL, end_col INTEGER NOT NULL,
  superseded INTEGER NOT NULL DEFAULT 0,
  snippet    TEXT                      -- whitespace-collapsed node text (baseline key); NULL in pre-baseline rows
);
CREATE INDEX idx_findings_file ON findings(file_id, superseded);
CREATE INDEX idx_findings_rule ON findings(rule, superseded);

-- the search half: full text mirror of every indexed file
CREATE VIRTUAL TABLE file_text USING fts5(
  path UNINDEXED, lang UNINDEXED, text, byte_scan UNINDEXED, tokenize='trigram'
);
)LMOKSQL";

[[noreturn]] void error(sqlite3 *db, const std::string &operation) {
  throw DbError(3, operation + ": " + sqlite3_errmsg(db));
}
void check(sqlite3 *db, int code, const std::string &operation) {
  if (code != SQLITE_OK) error(db, operation);
}
std::string env(const char *name) {
#ifdef _WIN32
  // Windows' narrow CRT environment is in the active ANSI code page, while
  // all CLI/DB paths are UTF-8. Read UTF-16 directly, just as wmain does for argv.
  std::wstring wide_name;
  for (const char *p = name; *p; ++p) wide_name.push_back(static_cast<wchar_t>(*p));
  const wchar_t *value = _wgetenv(wide_name.c_str());
  return value ? std::filesystem::path(value).u8string() : "";
#else
  const char *value = std::getenv(name);
  return value ? value : "";
#endif
}
[[noreturn]] void schema_error() {
  throw DbError(2, "DB schema mismatch (expected schema_version=2); delete the DB and re-index with `lmok index`");
}
} // namespace

std::filesystem::path resolve_db_path(const std::optional<std::string> &explicit_path, bool write) {
  namespace fs = std::filesystem;
  try {
    fs::path result;
    bool automatic = false;
    const std::string configured = env("LMOK_DB");
    if (explicit_path) {
      if (explicit_path->empty()) throw DbError(2, "--db needs a nonempty path");
      result = fs::u8path(*explicit_path);
    } else if (!configured.empty()) {
      result = fs::u8path(configured);
    } else {
      automatic = true;
      const fs::path cwd = fs::current_path();
      for (fs::path dir = cwd;; dir = dir.parent_path()) {
        // .git may be a file in worktrees/submodules, not only a directory.
        if (fs::exists(dir / ".git") || fs::is_directory(dir / ".lmok")) {
          result = dir / ".lmok" / "lmok.db";
          break;
        }
        if (dir == dir.parent_path()) break;
      }
      if (result.empty()) {
        std::string cache = env("XDG_CACHE_HOME");
        if (cache.empty()) {
          std::string home = env("HOME");
#ifdef _WIN32
          if (home.empty()) home = env("USERPROFILE");
#endif
          if (home.empty()) throw DbError(2, "set --db, LMOK_DB, XDG_CACHE_HOME or HOME to locate the DB");
          cache = (fs::u8path(home) / ".cache").u8string();
        }
        result = fs::u8path(cache) / "lmok" / (dslvm::hex(dslvm::sha256(cwd.generic_u8string())) + ".db");
      }
    }
    result = fs::absolute(result).lexically_normal();
    if (write && automatic) fs::create_directories(result.parent_path());
    return result;
  } catch (const fs::filesystem_error &e) {
    throw DbError(3, std::string("cannot resolve DB path: ") + e.what());
  }
}

Statement::Statement(sqlite3 *db, const std::string &sql) {
  const int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt_, nullptr);
  if (rc != SQLITE_OK) {
    sqlite3_finalize(stmt_);
    stmt_ = nullptr;
    error(db, "prepare SQL");
  }
  if (!stmt_) throw DbError(3, "prepare SQL: empty statement");
}
Statement::~Statement() { sqlite3_finalize(stmt_); }
Statement::Statement(Statement &&other) noexcept : stmt_(std::exchange(other.stmt_, nullptr)) {}
Statement &Statement::operator=(Statement &&other) noexcept {
  if (this != &other) {
    sqlite3_finalize(stmt_);
    stmt_ = std::exchange(other.stmt_, nullptr);
  }
  return *this;
}
void Statement::bind(int index, const std::string &value) {
  check(sqlite3_db_handle(stmt_), sqlite3_bind_text64(stmt_, index, value.data(), value.size(), SQLITE_TRANSIENT, SQLITE_UTF8), "bind text");
}
void Statement::bind(int index, int64_t value) {
  check(sqlite3_db_handle(stmt_), sqlite3_bind_int64(stmt_, index, value), "bind integer");
}
void Statement::bind_null(int index) {
  check(sqlite3_db_handle(stmt_), sqlite3_bind_null(stmt_, index), "bind null");
}
bool Statement::step() {
  const int rc = sqlite3_step(stmt_);
  if (rc == SQLITE_ROW) return true;
  if (rc == SQLITE_DONE) return false;
  error(sqlite3_db_handle(stmt_), "step SQL");
}
void Statement::reset() {
  check(sqlite3_db_handle(stmt_), sqlite3_reset(stmt_), "reset SQL");
  check(sqlite3_db_handle(stmt_), sqlite3_clear_bindings(stmt_), "clear bindings");
}
int64_t Statement::integer(int column) const { return sqlite3_column_int64(stmt_, column); }
std::string Statement::text(int column) const {
  const auto *value = sqlite3_column_text(stmt_, column);
  return value ? std::string(reinterpret_cast<const char *>(value), sqlite3_column_bytes(stmt_, column)) : "";
}
bool Statement::is_null(int column) const { return sqlite3_column_type(stmt_, column) == SQLITE_NULL; }

Database::Database(const std::filesystem::path &path, Mode mode) {
  if (mode == Mode::ReadOnly) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) throw DbError(3, "cannot access DB " + path.generic_u8string() + ": " + ec.message());
    if (!exists) throw DbError(2, "DB not found: " + path.generic_u8string() + "; run `lmok index` first");
  }
  const int flags = mode == Mode::ReadOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
  const int rc = sqlite3_open_v2(path.u8string().c_str(), &db_, flags, nullptr);
  try {
    check(db_, rc, "open DB " + path.generic_u8string());
    check(db_, sqlite3_busy_timeout(db_, 5000), "set DB busy timeout");
    exec("PRAGMA foreign_keys=ON");
    if (mode == Mode::Create) {
      Transaction transaction(*this);
      auto tables = prepare("SELECT count(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'");
      tables.step();
      const bool empty = tables.integer(0) == 0;
      // Finish the read before DDL/commit.
      tables.step();
      if (empty) {
        exec(schema_sql);
        auto insert = prepare("INSERT INTO meta(key,value) VALUES (?,?)");
        for (const auto &entry : {std::pair<std::string, std::string>{"schema_version", "2"},
                                  {"lmok_version", LMOK_VERSION},
                                  {"created_at", std::to_string(std::time(nullptr))}}) {
          insert.bind(1, entry.first); insert.bind(2, entry.second); insert.step(); insert.reset();
        }
      }
      // Validate below before committing an existing database.
      auto meta = prepare("SELECT count(*) FROM sqlite_master WHERE type='table' AND name='meta'");
      meta.step();
      if (meta.integer(0) != 1) schema_error();
      meta.step();
      auto version = prepare("SELECT value FROM meta WHERE key='schema_version'");
      if (!version.step() || version.text(0) != "2") schema_error();
      version.step();
      // Additive in-version migration: findings.snippet (baseline key part,
      // lmok 0.7). Nullable, so old DBs keep working and old rows stay NULL
      // (the baseline falls back to keying on rule/file/message for them).
      if (!db_has_column(*this, "findings", "snippet"))
        exec("ALTER TABLE findings ADD COLUMN snippet TEXT");
      exec("CREATE TABLE IF NOT EXISTS run_files (run_id INTEGER NOT NULL REFERENCES runs(id) ON DELETE CASCADE, file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE, scope TEXT NOT NULL, PRIMARY KEY (run_id,file_id,scope))");
      exec("CREATE INDEX IF NOT EXISTS idx_run_files_scope ON run_files(file_id, scope, run_id)");
      transaction.commit();
      exec("PRAGMA journal_mode=WAL");
    } else {
      auto meta = prepare("SELECT count(*) FROM sqlite_master WHERE type='table' AND name='meta'");
      meta.step();
      if (meta.integer(0) != 1) schema_error();
      auto version = prepare("SELECT value FROM meta WHERE key='schema_version'");
      if (!version.step() || version.text(0) != "2") schema_error();
    }
  } catch (...) {
    sqlite3_close_v2(db_);
    db_ = nullptr;
    throw;
  }
}
Database::~Database() { sqlite3_close_v2(db_); }
void Database::exec(const std::string &sql) {
  check(db_, sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr), "execute SQL");
}
Transaction::Transaction(Database &db, bool write) : db_(db) {
  db_.exec(write ? "BEGIN IMMEDIATE" : "BEGIN");
}
Transaction::~Transaction() {
  if (active_) sqlite3_exec(db_.handle(), "ROLLBACK", nullptr, nullptr, nullptr);
}
void Transaction::commit() { db_.exec("COMMIT"); active_ = false; }

bool db_has_column(Database &db, const std::string &table, const std::string &column) {
  auto info = db.prepare("PRAGMA table_info(" + table + ")");
  while (info.step())
    if (info.text(1) == column) return true;
  return false;
}

std::string db_status(Database &db, const std::filesystem::path &path) {
  Transaction snapshot(db, false);
  std::ostringstream out;
  auto version = db.prepare("SELECT value FROM meta WHERE key='schema_version'");
  version.step();
  out << "db: " << path.generic_u8string() << "\nschema_version: " << version.text(0) << "\n";
  for (const char *table : {"files", "symbols", "refs", "edges", "runs", "findings"}) {
    auto count = db.prepare(std::string("SELECT count(*) FROM ") + table);
    count.step();
    out << table << ": " << count.integer(0) << "\n";
  }
  auto last = db.prepare("SELECT id,started_at,command FROM runs ORDER BY started_at DESC,id DESC LIMIT 1");
  if (last.step()) {
    out << "last_run: id=" << last.integer(0) << " started_at=" << last.integer(1)
        << " command=" << dslvm::json_escape(last.text(2)) << "\n";
    last.step();
  } else out << "last_run: none\n";
  snapshot.commit();
  return out.str();
}
} // namespace lmok
