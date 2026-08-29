#include "buf.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "assert.h"

#define BUF_DEFAULT_CAP 256u

void buf_init(buf *b, arena *a, size_t initial_cap) {
  DOOT_ASSERT(b != NULL && a != NULL);
  b->a = a;
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
  b->failed = false;
  if (initial_cap == 0) {
    initial_cap = BUF_DEFAULT_CAP;
  }
  b->data = (char *)arena_alloc(a, initial_cap, 1u);
  if (b->data == NULL) {
    b->failed = true;
    return;
  }
  b->cap = initial_cap;
}

/* Reserves room for n more bytes, plus one spare so buf_cstr never has to grow. */
static bool buf_reserve(buf *b, size_t n) {
  size_t want;
  size_t cap;
  char *grown;

  if (b->failed) {
    return false;
  }
  if (n < b->cap - b->len) {
    return true;
  }

  if (b->len > SIZE_MAX - n - 1u) {
    b->failed = true;
    return false;
  }
  want = b->len + n + 1u;
  cap = b->cap == 0 ? BUF_DEFAULT_CAP : b->cap;
  while (cap < want) {
    if (cap > SIZE_MAX / 2u) {
      cap = want;
      break;
    }
    cap *= 2u;
  }

  grown = (char *)arena_extend(b->a, b->data, b->cap, cap, 1u);
  if (grown == NULL) {
    b->failed = true;
    return false;
  }
  b->data = grown;
  b->cap = cap;
  return true;
}

bool buf_append(buf *b, const void *p, size_t n) {
  DOOT_ASSERT(b != NULL);
  if (n == 0) {
    return !b->failed;
  }
  if (!buf_reserve(b, n)) {
    return false;
  }
  memcpy(b->data + b->len, p, n);
  b->len += n;
  return true;
}

bool buf_append_slice(buf *b, slice s) {
  return buf_append(b, s.p, s.n);
}

bool buf_append_cstr(buf *b, const char *s) {
  return buf_append_slice(b, slice_from_cstr(s));
}

bool buf_append_byte(buf *b, char c) {
  DOOT_ASSERT(b != NULL);
  if (!buf_reserve(b, 1u)) {
    return false;
  }
  b->data[b->len] = c;
  b->len++;
  return true;
}

bool buf_append_repeat(buf *b, char c, size_t count) {
  DOOT_ASSERT(b != NULL);
  if (count == 0) {
    return !b->failed;
  }
  if (!buf_reserve(b, count)) {
    return false;
  }
  memset(b->data + b->len, (unsigned char)c, count);
  b->len += count;
  return true;
}

bool buf_printf(buf *b, const char *fmt, ...) {
  va_list ap;
  va_list probe;
  int n;

  DOOT_ASSERT(b != NULL);
  if (b->failed) {
    return false;
  }

  /* One va_start, with va_copy for the sizing pass: both passes then provably
   * see identical arguments, which two separate va_start calls do not guarantee
   * as clearly to a reader. */
  va_start(ap, fmt);
  va_copy(probe, ap);
  n = vsnprintf(NULL, 0, fmt, probe);
  va_end(probe);

  if (n < 0) {
    va_end(ap);
    DOOT_FATAL("vsnprintf failed formatting \"%s\"", fmt);
  }
  if (!buf_reserve(b, (size_t)n)) {
    va_end(ap);
    return false;
  }

  (void)vsnprintf(b->data + b->len, (size_t)n + 1u, fmt, ap);
  va_end(ap);
  b->len += (size_t)n;
  return true;
}

bool buf_append_json_escaped(buf *b, slice s) {
  size_t i;

  DOOT_ASSERT(b != NULL);
  for (i = 0; i < s.n; i++) {
    unsigned char c = (unsigned char)s.p[i];
    bool ok;

    switch (c) {
    case '"':
      ok = buf_append_cstr(b, "\\\"");
      break;
    case '\\':
      ok = buf_append_cstr(b, "\\\\");
      break;
    case '\n':
      ok = buf_append_cstr(b, "\\n");
      break;
    case '\r':
      ok = buf_append_cstr(b, "\\r");
      break;
    case '\t':
      ok = buf_append_cstr(b, "\\t");
      break;
    case '\b':
      ok = buf_append_cstr(b, "\\b");
      break;
    case '\f':
      ok = buf_append_cstr(b, "\\f");
      break;
    default:
      if (c < 0x20u) {
        ok = buf_printf(b, "\\u%04x", (unsigned)c);
      } else {
        ok = buf_append_byte(b, (char)c);
      }
      break;
    }
    if (!ok) {
      return false;
    }
  }
  return true;
}

slice buf_slice(const buf *b) {
  DOOT_ASSERT(b != NULL);
  return slice_make(b->data == NULL ? "" : b->data, b->len);
}

const char *buf_cstr(buf *b) {
  DOOT_ASSERT(b != NULL);
  if (!buf_reserve(b, 1u)) {
    return "";
  }
  b->data[b->len] = '\0';
  return b->data;
}

void buf_clear(buf *b) {
  DOOT_ASSERT(b != NULL);
  b->len = 0;
}

bool buf_failed(const buf *b) {
  DOOT_ASSERT(b != NULL);
  return b->failed;
}
