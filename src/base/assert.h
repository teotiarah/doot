/* assert.h -- always-on invariant checks (D048).
 *
 * These are never compiled out. A failure here is a bug in doot itself, not a
 * user error (which is a diagnostic) and not a language-level fault (which
 * terminates one task). Continuing past a violated invariant risks corrupting a
 * database or serving wrong data, so we abort loudly instead.
 */
#ifndef DOOT_ASSERT_H
#define DOOT_ASSERT_H

#include "plat.h"

DOOT_NORETURN void doot_panic(const char *file, int line, const char *func, const char *fmt, ...)
    DOOT_PRINTF(4, 5);

#define DOOT_ASSERT(cond)                                                                          \
  do {                                                                                             \
    if (DOOT_UNLIKELY(!(cond))) {                                                                  \
      doot_panic(__FILE__, __LINE__, __func__, "assertion failed: %s", #cond);                     \
    }                                                                                              \
  } while (0)

#define DOOT_ASSERTF(cond, ...)                                                                    \
  do {                                                                                             \
    if (DOOT_UNLIKELY(!(cond))) {                                                                  \
      doot_panic(__FILE__, __LINE__, __func__, __VA_ARGS__);                                       \
    }                                                                                              \
  } while (0)

#define DOOT_UNREACHABLE() doot_panic(__FILE__, __LINE__, __func__, "unreachable code reached")

#define DOOT_FATAL(...) doot_panic(__FILE__, __LINE__, __func__, __VA_ARGS__)

#endif /* DOOT_ASSERT_H */
