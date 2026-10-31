#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

namespace lmok {

struct DbError : std::runtime_error {
  int exit_code;
  DbError(int code, const std::string &message) : std::runtime_error(message), exit_code(code) {}
};

// Explicit/env paths require existing parents. Only automatically selected
// project/cache directories may be created, and only for a write operation.
std::filesystem::path resolve_db_path(const std::optional<std::string> &explicit_path = {},
                                      bool write = false);

class Statement {
  sqlite3_stmt *stmt_ = nullptr;
public:
  Statement(sqlite3 *db, const std::string &sql);
  ~Statement();
  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;
  Statement(Statement &&other) noexcept;
  Statement &operator=(Statement &&other) noexcept;
  void bind(int index, const std::string &value);
  void bind(int index, int64_t value);
  void bind_null(int index);
  bool step(); // true = row, false = done; errors throw
  void reset(); // reset execution and clear bindings for reuse
  int64_t integer(int column) const;
  std::string text(int column) const;
  bool is_null(int column) const;
};

class Database {
  sqlite3 *db_ = nullptr;
public:
  enum class Mode { ReadOnly, Create };
  explicit Database(const std::filesystem::path &path, Mode mode = Mode::ReadOnly);
  ~Database();
  Database(const Database &) = delete;
  Database &operator=(const Database &) = delete;
  void exec(const std::string &sql);
  Statement prepare(const std::string &sql) { return Statement(db_, sql); }
  // Statements/transactions should remain within the Database's lifetime.
  sqlite3 *handle() const noexcept { return db_; }
};

class Transaction {
  Database &db_;
  bool active_ = true;
public:
  explicit Transaction(Database &db, bool write = true);
  ~Transaction(); // rollback unless committed; never throws
  Transaction(const Transaction &) = delete;
  Transaction &operator=(const Transaction &) = delete;
  void commit();
};

// Shared with the query-plan regression. FTS path is UNINDEXED: materialize
// its membership set once instead of correlating a full scan for every file.
inline constexpr const char *index_files_sql =
    "SELECT f.id, f.path, f.sha256, f.mtime, f.size,"
    " f.path IN (SELECT path FROM file_text) FROM files f";

// Stable text report from one read transaction (no torn row-count snapshots).
std::string db_status(Database &db, const std::filesystem::path &path);

// True when `table` has a column named `column` (schema additions within one
// schema_version are optional: readers must tolerate their absence).
bool db_has_column(Database &db, const std::string &table, const std::string &column);

} // namespace lmok
