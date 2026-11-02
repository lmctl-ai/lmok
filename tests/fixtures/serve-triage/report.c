#include "names.h"

void report_header(char *out) {
  copy_name(out, "report");
  copy_title(out + 8, 8, "title");
}
