#include <stdio.h>
#include <string.h>
#include "names.h"

void copy_name(char *dst, const char *src) {
  strcpy(dst, src);
}

void copy_title(char *dst, unsigned size, const char *src) {
  snprintf(dst, size, "%s", src);
}
