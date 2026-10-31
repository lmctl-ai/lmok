#pragma once
#include "db.hpp"
#include <cstddef>
#include <iosfwd>
#include <optional>
#include <string>

namespace lmok {
struct FindOptions {
  std::string pattern;
  std::optional<std::string> lang;
  size_t limit = 1000;
  bool fts = false;
};
bool search_needs_byte_scan(const std::string &text);
// Upgrade only the text index, atomically; facts/findings and schema version stay intact.
void ensure_trigram_index(Database &db);
int find_text(Database &db, const FindOptions &options, std::ostream &out, std::ostream &err);
}
