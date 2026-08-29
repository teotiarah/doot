/* fuzz_lex.c -- arbitrary bytes through the lexer.
 *
 * Drives lex_next to TOK_EOF and asserts the properties the parser is entitled to
 * assume: the stream terminates, every span lies inside the source and is well
 * ordered, and malformed input always produces a diagnostic rather than a silent
 * accept.
 *
 * This target scans in the default mode, so it reaches string interpolation but
 * not markup: entering markup is the parser's decision (D059), so markup
 * tokenization is covered by fuzz_parse. That is a real limit of the split, and
 * the better side of the trade -- the alternative is a lexer that guesses at
 * expression position.
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
#include "../src/lex/lex.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  arena *a = arena_new(4096u);
  diag_sink sink;
  lex_comments comments;
  source *s;
  lexer *lx;
  uint32_t n;
  uint32_t prev_end = 0u;
  size_t steps = 0u;
  size_t budget;

  if (a == NULL) {
    return 0;
  }
  arena_set_limit(a, 64u * 1024u * 1024u);
  diag_sink_init(&sink, a, 0);

  s = source_from_memory(a, SLICE_LIT("fuzz.do"), slice_make((const char *)data, size), &sink);
  if (s == NULL) {
    /* Rejected before lexing: UTF-8, size, or a NUL byte. Always explained. */
    if (!arena_exhausted(a) && diag_error_count(&sink) == 0) {
      abort();
    }
    arena_destroy(a);
    return 0;
  }

  comments.first = NULL;
  comments.last = NULL;
  comments.count = 0u;

  lx = lex_new(a, s, &sink, &comments);
  if (lx == NULL) {
    arena_destroy(a);
    return 0;
  }

  n = source_size(s);
  /* Every call consumes at least one byte or is already at the end, so the token
   * count is bounded by the input length. A generous multiple still catches a
   * stall, because a stall is unbounded rather than merely large. */
  budget = (size_t)n * 4u + 64u;

  for (;;) {
    token t = lex_next(lx);

    if (++steps > budget) {
      abort(); /* the stream failed to terminate */
    }
    if (span_is_none(t.at)) {
      abort(); /* every token carries a real span */
    }
    if (t.at.start > t.at.end || t.at.end > n) {
      abort(); /* spans stay inside the source and stay ordered */
    }
    /* Spans never move backwards, so a formatter can walk them in order. */
    if (t.at.start < prev_end && t.kind != TOK_EOF) {
      abort();
    }
    prev_end = t.at.end;

    /* lex_text must be safe for any token the lexer produced. */
    (void)lex_text(lx, t);

    if (t.kind == TOK_EOF) {
      break;
    }
  }

  /* Recorded comments must also be in range and in source order. */
  {
    const lex_comment *c;
    uint32_t last = 0u;

    for (c = comments.first; c != NULL; c = c->next) {
      if (c->at.start > c->at.end || c->at.end > n || c->at.start < last) {
        abort();
      }
      last = c->at.start;
    }
  }

  /* Whatever was reported must render, both ways, without faulting. */
  {
    buf out;
    buf_init(&out, a, 256u);
    diag_render_human(&sink, &out, false);
    buf_clear(&out);
    diag_render_json(&sink, &out);
  }

  arena_destroy(a);
  return 0;
}
