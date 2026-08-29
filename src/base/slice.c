#include "slice.h"

#include <string.h>

#include "assert.h"

slice slice_from_cstr(const char *s) {
  return s == NULL ? SLICE_EMPTY : slice_make(s, strlen(s));
}

bool slice_is_empty(slice s) {
  return s.n == 0;
}

bool slice_eq(slice a, slice b) {
  if (a.n != b.n) {
    return false;
  }
  return a.n == 0 || memcmp(a.p, b.p, a.n) == 0;
}

bool slice_eq_cstr(slice a, const char *b) {
  return slice_eq(a, slice_from_cstr(b));
}

int slice_cmp(slice a, slice b) {
  size_t min = a.n < b.n ? a.n : b.n;
  int r = min == 0 ? 0 : memcmp(a.p, b.p, min);
  if (r != 0) {
    return r;
  }
  if (a.n == b.n) {
    return 0;
  }
  return a.n < b.n ? -1 : 1;
}

uint64_t slice_hash(slice s) {
  uint64_t h = 0xcbf29ce484222325ull;
  size_t i;

  for (i = 0; i < s.n; i++) {
    h ^= (uint64_t)(unsigned char)s.p[i];
    h *= 0x100000001b3ull;
  }
  return h;
}

bool slice_starts_with(slice s, slice prefix) {
  if (prefix.n > s.n) {
    return false;
  }
  return prefix.n == 0 || memcmp(s.p, prefix.p, prefix.n) == 0;
}

bool slice_ends_with(slice s, slice suffix) {
  if (suffix.n > s.n) {
    return false;
  }
  return suffix.n == 0 || memcmp(s.p + (s.n - suffix.n), suffix.p, suffix.n) == 0;
}

bool slice_contains_byte(slice s, char c) {
  return s.n != 0 && memchr(s.p, (unsigned char)c, s.n) != NULL;
}

slice slice_sub(slice s, size_t start, size_t n) {
  if (start >= s.n) {
    return SLICE_EMPTY;
  }
  if (n > s.n - start) {
    n = s.n - start;
  }
  return slice_make(s.p + start, n);
}

static bool is_ascii_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

slice slice_trim_ascii(slice s) {
  while (s.n != 0 && is_ascii_space(s.p[0])) {
    s.p++;
    s.n--;
  }
  while (s.n != 0 && is_ascii_space(s.p[s.n - 1u])) {
    s.n--;
  }
  return s;
}

bool slice_cut(slice s, char sep, slice *before, slice *after) {
  const char *hit;

  DOOT_ASSERT(before != NULL && after != NULL);
  hit = s.n == 0 ? NULL : (const char *)memchr(s.p, (unsigned char)sep, s.n);
  if (hit == NULL) {
    *before = s;
    *after = SLICE_EMPTY;
    return false;
  }
  *before = slice_make(s.p, (size_t)(hit - s.p));
  *after = slice_make(hit + 1, s.n - (size_t)(hit - s.p) - 1u);
  return true;
}

const char *slice_cstr(arena *a, slice s) {
  return arena_dup(a, s.p, s.n);
}
