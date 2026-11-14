#ifndef LMOK_CPP_CLEAN_HPP
#define LMOK_CPP_CLEAN_HPP

#include <memory>
#include <stdexcept>
#include <string>

constexpr int kModernLimit = 128;

namespace demo {
using std::string;
using AliasPtr = std::unique_ptr<int>;

struct Obj {
  Obj() = default;
  Obj(const Obj &) = delete;
  void gets() {}
  void system() {}
};

inline void safe_fn(Obj &obj, int *ptr, double d) {
  using namespace demo;
  obj.gets();
  obj.system();
  int v = static_cast<int>(d);
  auto *clean_ptr = nullptr;
  if (auto *ok = ptr) {
    (void)ok;
  }
  int cond = 0;
  if ((cond = v) != 0) {
    (void)clean_ptr;
  }
  try {
    throw std::runtime_error("x");
  } catch (const std::exception &) {
    // intentionally ignored
  }
  const char *raw = R"(
    // lmok-expect: CPP001
    strcpy(dst, src);
  )";
  (void)raw;
}
} // namespace demo

#endif
