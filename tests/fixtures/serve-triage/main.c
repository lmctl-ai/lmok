#include <stdio.h>
#include "names.h"

int main(int argc, char **argv) {
  char user[16];
  if (argc < 2) return 1;
  copy_name(user, argv[1]);
  printf("hello %s\n", user);
  return 0;
}
