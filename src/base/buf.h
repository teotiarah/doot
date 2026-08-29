/* buf.h -- arena-backed growable byte buffer.
 *
 * Appending grows in place while the buffer is the arena's most recent
 * allocation, which is the normal case for a response being rendered. That is
 * what makes markup output a sequence of memcpy calls with no reallocation
 * (docs/05-runtime.md, "Markup compilation").
 *
 * Every append returns false if the arena is exhausted, and a buffer that has
 * seen a failed append reports buf_failed() forever after, so a caller may
 * append freely and check once at the end.
 */
#ifndef DOOT_BUF_H
#define DOOT_BUF_H

#include <stdbool.h>

#include "arena.h"
#include "plat.h"
#include "slice.h"

typedef struct {
  arena *a;
  char *data;
  size_t len;
  size_t cap;
  bool failed;
} buf;

void buf_init(buf *b, arena *a, size_t initial_cap);

bool buf_append(buf *b, const void *p, size_t n);
bool buf_append_slice(buf *b, slice s);
bool buf_append_cstr(buf *b, const char *s);
bool buf_append_byte(buf *b, char c);
bool buf_append_repeat(buf *b, char c, size_t count);
bool buf_printf(buf *b, const char *fmt, ...) DOOT_PRINTF(2, 3);

/* Appends s with JSON string escaping applied. Does not add quotes. */
bool buf_append_json_escaped(buf *b, slice s);

slice buf_slice(const buf *b);
const char *buf_cstr(buf *b); /* NUL-terminates without counting the NUL in len */
void buf_clear(buf *b);       /* keeps capacity */
bool buf_failed(const buf *b);

#endif /* DOOT_BUF_H */
