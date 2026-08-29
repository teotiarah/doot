#include "unit.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void unit_failf(unit *t, const char *file, int line, const char *fmt, ...) {
  va_list ap;

  if (t->failures == 0) {
    (void)printf("\n  FAIL %s / %s\n", t->suite, t->name);
  }
  t->failures++;

  (void)printf("    %s:%d: ", file, line);
  va_start(ap, fmt);
  (void)vprintf(fmt, ap);
  va_end(ap);
  (void)printf("\n");
}

static bool matches(const char *filter, const char *suite, const char *name) {
  if (filter == NULL) {
    return true;
  }
  return strstr(suite, filter) != NULL || strstr(name, filter) != NULL;
}

int unit_run(const unit_suite *const *suites, size_t suite_count, const char *filter) {
  size_t total_cases = 0;
  size_t total_checks = 0;
  size_t failed_cases = 0;
  size_t si;

  for (si = 0; si < suite_count; si++) {
    const unit_suite *s = suites[si];
    size_t ci;
    size_t ran = 0;
    size_t failed = 0;

    for (ci = 0; ci < s->count; ci++) {
      unit t;

      if (!matches(filter, s->name, s->cases[ci].name)) {
        continue;
      }
      t.suite = s->name;
      t.name = s->cases[ci].name;
      t.checks = 0;
      t.failures = 0;

      s->cases[ci].fn(&t);

      ran++;
      total_checks += t.checks;
      if (t.failures != 0) {
        failed++;
        failed_cases++;
      }
    }

    total_cases += ran;
    if (ran != 0) {
      (void)printf("%-4s %-12s %lu tests\n", failed == 0 ? "ok" : "FAIL", s->name,
                   (unsigned long)ran);
    }
  }

  if (total_cases == 0) {
    (void)printf("no tests matched%s%s\n", filter == NULL ? "" : " filter ",
                 filter == NULL ? "" : filter);
    return 1;
  }

  (void)printf("\n%lu tests, %lu checks, %lu failed\n", (unsigned long)total_cases,
               (unsigned long)total_checks, (unsigned long)failed_cases);
  return failed_cases == 0 ? 0 : 1;
}
