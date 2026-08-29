/* slice.h -- a non-owning view over bytes.
 *
 * The compiler's string type. Slices point into a source buffer or into an
 * arena; they never own storage and are never freed. Not NUL-terminated in
 * general, so never pass slice.p to a C library function expecting a string.
 */
#ifndef DOOT_SLICE_H
#define DOOT_SLICE_H

#include <stdbool.h>

#include "arena.h"
#include "plat.h"

typedef struct {
  const char *p;
  size_t n;
} slice;

/* String literals only: uses sizeof, so it does not scan. */
#define SLICE_LIT(s) slice_make((s), sizeof(s) - 1u)
#define SLICE_EMPTY slice_make("", 0)

DOOT_INLINE slice slice_make(const char *p, size_t n) {
  slice s;
  s.p = p;
  s.n = n;
  return s;
}

slice slice_from_cstr(const char *s);
bool slice_is_empty(slice s);

bool slice_eq(slice a, slice b);
bool slice_eq_cstr(slice a, const char *b);
int slice_cmp(slice a, slice b);
uint64_t slice_hash(slice s); /* FNV-1a, 64-bit */

bool slice_starts_with(slice s, slice prefix);
bool slice_ends_with(slice s, slice suffix);
bool slice_contains_byte(slice s, char c);

/* Clamped to the bounds of s, so an out-of-range request yields an empty or
 * truncated slice rather than a fault. */
slice slice_sub(slice s, size_t start, size_t n);
slice slice_trim_ascii(slice s);

/* Splits at the first occurrence of sep. Returns false when sep is absent, in
 * which case *before is s and *after is empty. */
bool slice_cut(slice s, char sep, slice *before, slice *after);

/* A NUL-terminated copy in the arena, for handing to the C library. */
const char *slice_cstr(arena *a, slice s);

#endif /* DOOT_SLICE_H */
