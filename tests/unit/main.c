/* main.c -- unit test entry point.
 *
 * Suites are registered explicitly here rather than through constructor
 * attributes, which keeps the harness portable to MSVC in v0.5 and keeps the
 * set of suites visible in one place.
 */
#include <stdio.h>
#include <string.h>

#include "unit.h"

extern const unit_suite suite_arena;
extern const unit_suite suite_buf;
extern const unit_suite suite_diag;
extern const unit_suite suite_lex;
extern const unit_suite suite_slice;
extern const unit_suite suite_source;

int main(int argc, char **argv) {
  /* Ordered by layer, so a base-layer failure is reported before the failures it
   * causes further up. */
  static const unit_suite *const suites[] = {
      &suite_arena, &suite_buf, &suite_slice, &suite_source, &suite_diag, &suite_lex,
  };
  const char *filter = NULL;

  if (argc > 1) {
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
      (void)printf("usage: doot_test [filter]\n"
                   "\n"
                   "Runs suites and tests whose name contains <filter>.\n");
      return 0;
    }
    filter = argv[1];
  }

  return unit_run(suites, sizeof(suites) / sizeof(suites[0]), filter);
}
