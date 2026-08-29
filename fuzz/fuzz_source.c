/* fuzz_source.c -- arbitrary bytes as doot source.
 *
 * Exercises UTF-8 validation, the line index, offset-to-line/column mapping, and
 * diagnostic rendering with spans taken from the input itself.
 *
 * The invariant, as for every doot fuzz target: arbitrary input produces a
 * diagnostic, never a crash, a hang, or unbounded memory growth.
 */
#include <stdint.h>
#include <stdlib.h>

#include "../src/base/arena.h"
#include "../src/base/buf.h"
#include "../src/base/diag.h"
#include "../src/base/source.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  arena *a = arena_new(4096u);
  diag_sink sink;
  source *s;

  if (a == NULL) {
    return 0;
  }
  /* Bound memory so a pathological input fails a budget rather than the host. */
  arena_set_limit(a, 64u * 1024u * 1024u);
  diag_sink_init(&sink, a, 0);

  s = source_from_memory(a, SLICE_LIT("fuzz.do"), slice_make((const char *)data, size), &sink);

  if (s == NULL) {
    /* A rejected source must always say why. */
    if (!arena_exhausted(a) && diag_error_count(&sink) == 0) {
      abort();
    }
  } else {
    buf out;
    uint32_t n = source_size(s);
    uint32_t i;
    uint32_t line;

    /* Every byte offset, plus offsets past the end, must map without faulting. */
    for (i = 0; i <= n; i++) {
      line_col lc = source_line_col(s, i);
      if (lc.line == 0 || lc.col == 0 || lc.line > source_line_count(s)) {
        abort();
      }
    }
    (void)source_line_col(s, n + 1u);
    (void)source_line_col(s, UINT32_MAX - 1u);

    /* Every line must be retrievable and within the text. */
    for (line = 1u; line <= source_line_count(s); line++) {
      slice l = source_line(s, line);
      if (l.n > (size_t)n) {
        abort();
      }
    }
    (void)source_line(s, 0);
    (void)source_line(s, source_line_count(s) + 1u);

    /* Render a diagnostic over a span derived from the input, including spans
     * that run past the end of a line or the end of the file. */
    (void)diag_report(&sink, DIAG_INVALID_UTF8, s,
                      span_make(size > 0 ? (uint32_t)(size / 2u) : 0, (uint32_t)size + 3u),
                      "fuzz span");
    buf_init(&out, a, 256u);
    diag_render_human(&sink, &out, false);
    buf_clear(&out);
    diag_render_json(&sink, &out);
  }

  arena_destroy(a);
  return 0;
}
