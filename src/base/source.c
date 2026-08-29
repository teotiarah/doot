#include "source.h"

#include <stdio.h>
#include <string.h>

#include "assert.h"
#include "buf.h"
#include "diag.h"

struct source {
  slice path;
  slice text;
  uint32_t *line_starts; /* line_starts[0] is line 1 */
  uint32_t line_count;
};

/* Strict UTF-8 scan. On success returns true; on failure sets *bad_at to the
 * offset of the first invalid byte. Rejects overlong encodings, surrogates,
 * anything above U+10FFFF, and NUL (reported separately by the caller). */
static bool utf8_validate(slice text, size_t *bad_at) {
  size_t i = 0;

  while (i < text.n) {
    unsigned char c = (unsigned char)text.p[i];
    size_t need;
    uint32_t cp;
    size_t k;

    if (c < 0x80u) {
      i++;
      continue;
    }
    if (c >= 0xc2u && c <= 0xdfu) {
      need = 1;
      cp = c & 0x1fu;
    } else if (c >= 0xe0u && c <= 0xefu) {
      need = 2;
      cp = c & 0x0fu;
    } else if (c >= 0xf0u && c <= 0xf4u) {
      need = 3;
      cp = c & 0x07u;
    } else {
      *bad_at = i; /* continuation byte in lead position, or 0xc0/0xc1/0xf5+ */
      return false;
    }

    if (text.n - i - 1u < need) {
      *bad_at = i;
      return false;
    }
    for (k = 1; k <= need; k++) {
      unsigned char cc = (unsigned char)text.p[i + k];
      if ((cc & 0xc0u) != 0x80u) {
        *bad_at = i + k;
        return false;
      }
      cp = (cp << 6) | (uint32_t)(cc & 0x3fu);
    }

    if (need == 2 && (cp < 0x800u || (cp >= 0xd800u && cp <= 0xdfffu))) {
      *bad_at = i;
      return false;
    }
    if (need == 3 && (cp < 0x10000u || cp > 0x10ffffu)) {
      *bad_at = i;
      return false;
    }
    i += need + 1u;
  }
  return true;
}

static bool build_line_index(source *s, arena *a) {
  uint32_t count = 1;
  size_t i;
  uint32_t *starts;
  uint32_t next = 1;

  for (i = 0; i < s->text.n; i++) {
    if (s->text.p[i] == '\n' && i + 1u < s->text.n) {
      count++;
    }
  }

  starts = ARENA_NEW_N(a, uint32_t, count);
  if (starts == NULL) {
    return false;
  }
  starts[0] = 0;
  for (i = 0; i < s->text.n; i++) {
    if (s->text.p[i] == '\n' && i + 1u < s->text.n) {
      DOOT_ASSERT(next < count);
      starts[next] = (uint32_t)(i + 1u);
      next++;
    }
  }
  s->line_starts = starts;
  s->line_count = count;
  return true;
}

source *source_from_memory(arena *a, slice path, slice text, diag_sink *sink) {
  source *s;
  size_t bad_at = 0;
  const char *nul;
  const char *cpath;
  char *text_copy;

  DOOT_ASSERT(a != NULL);

  cpath = slice_cstr(a, path);
  if (cpath == NULL) {
    return NULL;
  }

  if (text.n > DOOT_MAX_SOURCE_BYTES) {
    if (sink != NULL) {
      (void)diag_report(sink, DIAG_SOURCE_TOO_LARGE, NULL, span_none(),
                        "`%s` is %lu bytes; the maximum is %lu", cpath, (unsigned long)text.n,
                        (unsigned long)DOOT_MAX_SOURCE_BYTES);
    }
    return NULL;
  }

  nul = text.n == 0 ? NULL : (const char *)memchr(text.p, 0, text.n);
  if (nul != NULL) {
    if (sink != NULL) {
      (void)diag_report(sink, DIAG_SOURCE_NUL_BYTE, NULL, span_none(),
                        "`%s` contains a NUL byte at offset %lu", cpath,
                        (unsigned long)(nul - text.p));
    }
    return NULL;
  }

  if (!utf8_validate(text, &bad_at)) {
    if (sink != NULL) {
      (void)diag_report(sink, DIAG_INVALID_UTF8, NULL, span_none(),
                        "`%s` is not valid UTF-8; the first invalid byte is 0x%02x at offset %lu",
                        cpath, (unsigned)(unsigned char)text.p[bad_at], (unsigned long)bad_at);
    }
    return NULL;
  }

  s = ARENA_NEW(a, source);
  text_copy = arena_dup(a, text.p, text.n);
  if (s == NULL || text_copy == NULL) {
    return NULL;
  }
  s->path = slice_make(cpath, path.n);
  s->text = slice_make(text_copy, text.n);

  if (!build_line_index(s, a)) {
    return NULL;
  }
  return s;
}

source *source_from_file(arena *a, slice path, diag_sink *sink) {
  const char *cpath;
  FILE *f;
  buf contents;
  char chunk[16384];
  source *s;

  DOOT_ASSERT(a != NULL);
  cpath = slice_cstr(a, path);
  if (cpath == NULL) {
    return NULL;
  }

  f = fopen(cpath, "rb");
  if (f == NULL) {
    if (sink != NULL) {
      (void)diag_report(sink, DIAG_CANNOT_READ_FILE, NULL, span_none(), "cannot read `%s`", cpath);
    }
    return NULL;
  }

  buf_init(&contents, a, 65536u);
  for (;;) {
    size_t got = fread(chunk, 1u, sizeof(chunk), f);
    if (got != 0 && !buf_append(&contents, chunk, got)) {
      (void)fclose(f);
      return NULL;
    }
    if (got != sizeof(chunk)) {
      break;
    }
    if (contents.len > DOOT_MAX_SOURCE_BYTES) {
      break; /* source_from_memory reports the size diagnostic */
    }
  }
  if (ferror(f) != 0) {
    (void)fclose(f);
    if (sink != NULL) {
      (void)diag_report(sink, DIAG_CANNOT_READ_FILE, NULL, span_none(), "error reading `%s`",
                        cpath);
    }
    return NULL;
  }
  (void)fclose(f);

  s = source_from_memory(a, path, buf_slice(&contents), sink);
  return s;
}

slice source_path(const source *s) {
  DOOT_ASSERT(s != NULL);
  return s->path;
}

slice source_text(const source *s) {
  DOOT_ASSERT(s != NULL);
  return s->text;
}

uint32_t source_size(const source *s) {
  DOOT_ASSERT(s != NULL);
  return (uint32_t)s->text.n;
}

uint32_t source_line_count(const source *s) {
  DOOT_ASSERT(s != NULL);
  return s->line_count;
}

uint32_t source_line_start(const source *s, uint32_t line) {
  DOOT_ASSERT(s != NULL);
  if (line == 0 || line > s->line_count) {
    return (uint32_t)s->text.n;
  }
  return s->line_starts[line - 1u];
}

line_col source_line_col(const source *s, uint32_t offset) {
  line_col lc;
  uint32_t lo = 0;
  uint32_t hi;
  uint32_t start;
  uint32_t i;

  DOOT_ASSERT(s != NULL);
  if (offset > (uint32_t)s->text.n) {
    offset = (uint32_t)s->text.n;
  }

  /* Binary search for the last line start at or before offset. */
  hi = s->line_count - 1u;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo + 1u) / 2u;
    if (s->line_starts[mid] <= offset) {
      lo = mid;
    } else {
      hi = mid - 1u;
    }
  }

  lc.line = lo + 1u;
  start = s->line_starts[lo];
  lc.col = 1u;
  for (i = start; i < offset; i++) {
    /* Count characters, not bytes: skip UTF-8 continuation bytes. */
    if (((unsigned char)s->text.p[i] & 0xc0u) != 0x80u) {
      lc.col++;
    }
  }
  return lc;
}

slice source_line(const source *s, uint32_t line) {
  uint32_t start;
  uint32_t end;

  DOOT_ASSERT(s != NULL);
  if (line == 0 || line > s->line_count) {
    return SLICE_EMPTY;
  }
  start = s->line_starts[line - 1u];
  end = line == s->line_count ? (uint32_t)s->text.n : s->line_starts[line];

  while (end > start && (s->text.p[end - 1u] == '\n' || s->text.p[end - 1u] == '\r')) {
    end--;
  }
  return slice_make(s->text.p + start, (size_t)(end - start));
}
