#include "db.hpp"
#include <iostream>
#include <stdexcept>
#include <utility>

static void require(bool value) { if (!value) throw std::runtime_error("DB wrapper assertion failed"); }
int main(int argc, char **argv) {
  try {
    require(argc == 2 || (argc == 3 && std::string(argv[2]) == "--index-plan"));
    lmok::Database db(std::filesystem::u8path(argv[1]), lmok::Database::Mode::Create);
    if (argc == 3) {
      auto materialized_once = [&](const std::string &sql) {
        auto plan = db.prepare("EXPLAIN QUERY PLAN " + sql);
        bool list = false;
        int fts_scans = 0;
        while (plan.step()) {
          const auto detail = plan.text(3);
          if (detail.find("CORRELATED") != std::string::npos) return false;
          if (detail.find("LIST SUBQUERY") != std::string::npos) list = true;
          if (detail.find("VIRTUAL TABLE INDEX") != std::string::npos) ++fts_scans;
        }
        return list && fts_scans == 1;
      };
      require(materialized_once(lmok::index_files_sql));
      // Negative control: the former quadratic query must fail the plan oracle.
      require(!materialized_once("SELECT f.id, EXISTS(SELECT 1 FROM file_text t WHERE t.path=f.path) FROM files f"));
      db.exec("INSERT INTO files VALUES(1,'empty.c','c','hash',0,0,1),"
              "(2,'placeholder.c','c','hash',0,0,1),"
              "(3,'duplicate.c','c','hash',0,0,1)");
      db.exec("INSERT INTO file_text(path,lang,text) VALUES"
              "('empty.c','c',''),('duplicate.c','c','x'),('duplicate.c','c','y'),(NULL,'c','orphan')");
      auto rows = db.prepare(lmok::index_files_sql);
      int count = 0;
      while (rows.step()) {
        const auto id = rows.integer(0);
        require(id >= 1 && id <= 3);
        require((rows.integer(5) != 0) == (id != 2));
        ++count;
      }
      require(count == 3); // duplicates in FTS must not duplicate files rows
      std::cout << "index lookup: one uncorrelated FTS scan, placeholder/empty/duplicate controls\n";
      return 0;
    }
    try {
      lmok::Transaction rollback(db);
      db.exec("INSERT INTO runs VALUES(1,1,'rolled back')");
      // Simulate a failed per-file import; destructor must roll back all writes.
      db.exec("INSERT INTO no_such_table VALUES(1)");
    } catch (const lmok::DbError &) {}
    auto count = db.prepare("SELECT count(*) FROM runs");
    require(count.step() && count.integer(0) == 0 && !count.step());
    {
      lmok::Transaction commit(db);
      auto insert = db.prepare("INSERT INTO runs VALUES(?,?,?)");
      insert.bind(1, int64_t(1)); insert.bind(2, int64_t(10)); insert.bind(3, std::string("a\0b", 3));
      require(!insert.step());
      insert.reset();
      insert.bind(1, int64_t(2)); insert.bind(2, int64_t(11)); insert.bind(3, std::string("second"));
      require(!insert.step());
      commit.commit();
    }
    auto query = db.prepare("SELECT command FROM runs ORDER BY id");
    auto moved = std::move(query);
    require(moved.step() && moved.text(0) == std::string("a\0b", 3));
    require(moved.step() && moved.text(0) == "second" && !moved.step());
    auto null = db.prepare("SELECT ?"); null.bind_null(1);
    require(null.step() && null.is_null(0));
    auto options = db.prepare("SELECT sqlite_version(), sqlite_compileoption_used('ENABLE_FTS5'), sqlite_compileoption_used('THREADSAFE=1')");
    require(options.step() && options.text(0) == "3.54.1" && options.integer(1) == 1 && options.integer(2) == 1);
    auto fk = db.prepare("PRAGMA foreign_keys"); require(fk.step() && fk.integer(0) == 1);
    bool rejected = false;
    try { db.exec("INSERT INTO symbols VALUES(1,999,'x','function',0,0,0,1,'','exact')"); }
    catch (const lmok::DbError &) { rejected = true; }
    require(rejected);
    std::cout << "DB wrapper: rollback, commit, bindings/reset/move/null, foreign keys\n";
    return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
