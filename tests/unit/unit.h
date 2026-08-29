/* unit.h -- the unit test harness.
 *
 * Deliberately small and dependency-free. Suites register in an explicit table
 * in main.c rather than through constructor attributes, which keeps the harness
 * portable to MSVC when Windows support lands in v0.5.
 *
 * A failing check records and continues, so one test reports every problem it
 * finds rather than only the first.
 */
#ifndef DOOT_UNIT_H
#define DOOT_UNIT_H

#include <stdbool.h>
#include <stddef.h>

#include "../../src/base/plat.h"
#include "../../src/base/slice.h"

typedef struct {
  const char *suite;
  const char *name;
  size_t checks;
  size_t failures;
} unit;

typedef void (*unit_fn)(unit *t);

typedef struct {
  const char *name;
  unit_fn fn;
} unit_case;

typedef struct {
  const char *name;
  const unit_case *cases;
  size_t count;
} unit_suite;

#define UNIT_SUITE(sym, name, cases_array)                                                         \
  const unit_suite sym = {name, cases_array, sizeof(cases_array) / sizeof((cases_array)[0])}

void unit_failf(unit *t, const char *file, int line, const char *fmt, ...) DOOT_PRINTF(4, 5);
int unit_run(const unit_suite *const *suites, size_t suite_count, const char *filter);

#define UNIT_TRUE(t, cond)                                                                         \
  do {                                                                                             \
    (t)->checks++;                                                                                 \
    if (!(cond)) {                                                                                 \
      unit_failf((t), __FILE__, __LINE__, "expected true: %s", #cond);                             \
    }                                                                                              \
  } while (0)

#define UNIT_FALSE(t, cond)                                                                        \
  do {                                                                                             \
    (t)->checks++;                                                                                 \
    if (cond) {                                                                                    \
      unit_failf((t), __FILE__, __LINE__, "expected false: %s", #cond);                            \
    }                                                                                              \
  } while (0)

#define UNIT_EQ_INT(t, got, want)                                                                  \
  do {                                                                                             \
    long long g_ = (long long)(got);                                                               \
    long long w_ = (long long)(want);                                                              \
    (t)->checks++;                                                                                 \
    if (g_ != w_) {                                                                                \
      unit_failf((t), __FILE__, __LINE__, "%s: expected %lld, got %lld", #got, w_, g_);            \
    }                                                                                              \
  } while (0)

#define UNIT_EQ_PTR(t, got, want)                                                                  \
  do {                                                                                             \
    const void *g_ = (const void *)(got);                                                          \
    const void *w_ = (const void *)(want);                                                         \
    (t)->checks++;                                                                                 \
    if (g_ != w_) {                                                                                \
      unit_failf((t), __FILE__, __LINE__, "%s: expected %p, got %p", #got, w_, g_);                \
    }                                                                                              \
  } while (0)

#define UNIT_NOT_NULL(t, p)                                                                        \
  do {                                                                                             \
    (t)->checks++;                                                                                 \
    if ((p) == NULL) {                                                                             \
      unit_failf((t), __FILE__, __LINE__, "%s: expected non-NULL", #p);                            \
    }                                                                                              \
  } while (0)

#define UNIT_NULL(t, p)                                                                            \
  do {                                                                                             \
    (t)->checks++;                                                                                 \
    if ((p) != NULL) {                                                                             \
      unit_failf((t), __FILE__, __LINE__, "%s: expected NULL", #p);                                \
    }                                                                                              \
  } while (0)

/* Compares a slice against a C string literal, reporting both on mismatch. */
#define UNIT_EQ_SLICE(t, got, want)                                                                \
  do {                                                                                             \
    slice g_ = (got);                                                                              \
    const char *w_ = (want);                                                                       \
    (t)->checks++;                                                                                 \
    if (!slice_eq_cstr(g_, w_)) {                                                                  \
      unit_failf((t), __FILE__, __LINE__, "%s:\n    expected: \"%s\"\n         got: \"%.*s\"",     \
                 #got, w_, (int)g_.n, g_.p);                                                       \
    }                                                                                              \
  } while (0)

#define UNIT_EQ_STR(t, got, want) UNIT_EQ_SLICE(t, slice_from_cstr(got), want)

#endif /* DOOT_UNIT_H */
