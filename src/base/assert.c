#include "assert.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

DOOT_NORETURN void doot_panic(const char *file, int line, const char *func, const char *fmt, ...) {
  va_list args;

  (void)fflush(stdout);
  (void)fprintf(stderr, "\ndoot: internal error at %s:%d in %s()\n  ", file, line, func);

  va_start(args, fmt);
  (void)vfprintf(stderr, fmt, args);
  va_end(args);

  (void)fprintf(stderr, "\n\nThis is a bug in doot, not in your program.\n"
                        "Please report it at https://github.com/teotiarah/doot/issues\n");
  (void)fflush(stderr);
  abort();
}
