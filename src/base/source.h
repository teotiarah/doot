/* source.h -- source text, byte spans, and line/column mapping.
 *
 * A source owns its text in an arena and is immutable once loaded. Byte offsets
 * are 32-bit throughout the compiler, which keeps a span at 8 bytes; the limit
 * is enforced at load time (DT0002).
 *
 * Loading validates the text: strict UTF-8, no NUL bytes, within the size cap.
 * A failed load reports a diagnostic and returns NULL, so the caller never sees
 * a partially valid source.
 */
#ifndef DOOT_SOURCE_H
#define DOOT_SOURCE_H

#include <stdbool.h>

#include "arena.h"
#include "plat.h"
#include "slice.h"

/* Half-open byte range [start, end). */
typedef struct {
  uint32_t start;
  uint32_t end;
} span;

#define SPAN_NONE_START UINT32_MAX

typedef struct {
  uint32_t line; /* 1-based */
  uint32_t col;  /* 1-based, counted in characters, not bytes */
} line_col;

typedef struct source source;
typedef struct diag_sink diag_sink;

DOOT_INLINE span span_make(uint32_t start, uint32_t end) {
  span s;
  s.start = start;
  s.end = end;
  return s;
}

DOOT_INLINE span span_none(void) {
  return span_make(SPAN_NONE_START, SPAN_NONE_START);
}
DOOT_INLINE bool span_is_none(span s) {
  return s.start == SPAN_NONE_START;
}
DOOT_INLINE span span_join(span a, span b) {
  if (span_is_none(a)) {
    return b;
  }
  if (span_is_none(b)) {
    return a;
  }
  return span_make(a.start < b.start ? a.start : b.start, a.end > b.end ? a.end : b.end);
}

/* `path` is copied. `text` is copied. Reports DT0001/DT0002/DT0003 into sink on
 * invalid input and returns NULL; sink may be NULL to validate silently. */
source *source_from_memory(arena *a, slice path, slice text, diag_sink *sink);

/* Reads the file at `path`. Reports DT1001 if it cannot be read. */
source *source_from_file(arena *a, slice path, diag_sink *sink);

slice source_path(const source *s);
slice source_text(const source *s);
uint32_t source_size(const source *s);
uint32_t source_line_count(const source *s);

/* Offsets past the end clamp to the last position, so a diagnostic at a
 * synthetic or stale offset still renders somewhere sensible. */
line_col source_line_col(const source *s, uint32_t offset);

/* 1-based line, excluding the newline terminator. Out-of-range yields empty. */
slice source_line(const source *s, uint32_t line);
uint32_t source_line_start(const source *s, uint32_t line);

#endif /* DOOT_SOURCE_H */
