#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#define LEGACY_LIMIT 128 // lmok-expect: CPP009

using namespace std; // lmok-expect: CPP004

void demo(char *dst, const char *src, int *ptr, const int *cptr, long addr) {
  std::strcpy(dst, src); // lmok-expect: CPP001
  std::system(src); // lmok-expect: CPP002
  try {
    throw std::runtime_error("err");
  } catch (const std::exception &e) { } // lmok-expect: CPP003
  int *p1 = reinterpret_cast<int *>(addr); // lmok-expect: CPP005
  int *p2 = const_cast<int *>(cptr); // lmok-expect: CPP005
  int cond = 0;
  if (cond = 1) { // lmok-expect: CPP006
    p1 = NULL; // lmok-expect: CPP007
  }
  delete ptr; // lmok-expect: CPP008
}
