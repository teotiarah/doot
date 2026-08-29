#include "../../src/base/source.h"

#include <stdlib.h>
#include <string.h>

#include "../../src/base/diag.h"
#include "unit.h"

static source *load(arena *a, const char *text) {
  return source_from_memory(a, SLICE_LIT("app.do"), slice_from_cstr(text), NULL);
}

static void line_index_counts_lines(unit *t) {
  arena *a = arena_new(4096u);

  UNIT_EQ_INT(t, source_line_count(load(a, "")), 1);
  UNIT_EQ_INT(t, source_line_count(load(a, "one")), 1);
  UNIT_EQ_INT(t, source_line_count(load(a, "one\n")), 1);
  UNIT_EQ_INT(t, source_line_count(load(a, "one\ntwo")), 2);
  UNIT_EQ_INT(t, source_line_count(load(a, "one\ntwo\n")), 2);
  UNIT_EQ_INT(t, source_line_count(load(a, "\n\n")), 2);
  arena_destroy(a);
}

static void line_text_excludes_terminators(unit *t) {
  arena *a = arena_new(4096u);
  source *s = load(a, "first\nsecond\r\nthird");

  UNIT_EQ_SLICE(t, source_line(s, 1u), "first");
  UNIT_EQ_SLICE(t, source_line(s, 2u), "second");
  UNIT_EQ_SLICE(t, source_line(s, 3u), "third");
  UNIT_EQ_SLICE(t, source_line(s, 0), "");
  UNIT_EQ_SLICE(t, source_line(s, 99u), "");
  arena_destroy(a);
}

static void line_col_maps_offsets(unit *t) {
  arena *a = arena_new(4096u);
  source *s = load(a, "let x = 1\nlet y = 2\n");
  line_col lc;

  lc = source_line_col(s, 0);
  UNIT_EQ_INT(t, lc.line, 1);
  UNIT_EQ_INT(t, lc.col, 1);

  lc = source_line_col(s, 4u); /* the 'x' */
  UNIT_EQ_INT(t, lc.line, 1);
  UNIT_EQ_INT(t, lc.col, 5);

  lc = source_line_col(s, 10u); /* first byte of line 2 */
  UNIT_EQ_INT(t, lc.line, 2);
  UNIT_EQ_INT(t, lc.col, 1);

  lc = source_line_col(s, 14u); /* the 'y' */
  UNIT_EQ_INT(t, lc.line, 2);
  UNIT_EQ_INT(t, lc.col, 5);
  arena_destroy(a);
}

static void columns_are_counted_in_characters(unit *t) {
  arena *a = arena_new(4096u);
  /* "héllo = 1" where e-acute occupies bytes 1 and 2, so byte offsets run ahead
   * of character columns from that point on. */
  source *s = load(a, "h\xc3\xa9llo = 1");

  UNIT_EQ_INT(t, source_line_col(s, 0).col, 1u);  /* h */
  UNIT_EQ_INT(t, source_line_col(s, 1u).col, 2u); /* é, lead byte */
  UNIT_EQ_INT(t, source_line_col(s, 3u).col, 3u); /* first l */
  UNIT_EQ_INT(t, source_line_col(s, 5u).col, 5u); /* o, byte 5 but column 5 */
  UNIT_EQ_INT(t, source_line_col(s, 6u).col, 6u); /* space */
  UNIT_EQ_INT(t, source_line_col(s, 1u).line, 1u);
  arena_destroy(a);
}

static void offsets_past_the_end_clamp(unit *t) {
  arena *a = arena_new(4096u);
  source *s = load(a, "one\ntwo");
  line_col lc = source_line_col(s, 9999u);

  UNIT_EQ_INT(t, lc.line, 2);
  UNIT_EQ_INT(t, lc.col, 4);
  arena_destroy(a);
}

static void valid_utf8_is_accepted(unit *t) {
  arena *a = arena_new(4096u);

  UNIT_NOT_NULL(t, load(a, "let s = \"caf\xc3\xa9\""));      /* 2-byte */
  UNIT_NOT_NULL(t, load(a, "let s = \"\xe2\x82\xac\""));     /* 3-byte, euro */
  UNIT_NOT_NULL(t, load(a, "let s = \"\xf0\x9f\x8e\x89\"")); /* 4-byte, emoji */
  arena_destroy(a);
}

static void invalid_utf8_is_rejected(unit *t) {
  arena *a = arena_new(4096u);
  diag_sink sink;
  const char *bad[8];
  size_t i;

  bad[0] = "\xff";                 /* invalid lead byte */
  bad[1] = "\x80";                 /* continuation in lead position */
  bad[2] = "\xc3";                 /* truncated 2-byte sequence */
  bad[3] = "\xe2\x82";             /* truncated 3-byte sequence */
  bad[4] = "\xc0\xaf";             /* overlong encoding of '/' */
  bad[5] = "\xed\xa0\x80";         /* UTF-16 surrogate U+D800 */
  bad[6] = "\xf4\x90\x80\x80";     /* above U+10FFFF */
  bad[7] = "ok \xe0\x80\x80 tail"; /* overlong 3-byte, mid-text */

  for (i = 0; i < 8u; i++) {
    diag_sink_init(&sink, a, 0);
    UNIT_NULL(t, source_from_memory(a, SLICE_LIT("app.do"), slice_from_cstr(bad[i]), &sink));
    UNIT_EQ_INT(t, diag_error_count(&sink), 1);
    UNIT_TRUE(t, sink.first != NULL && sink.first->code == DIAG_INVALID_UTF8);
  }
  arena_destroy(a);
}

static void oversized_sources_are_rejected(unit *t) {
  arena *a = arena_new(4096u);
  diag_sink sink;
  size_t too_big = DOOT_MAX_SOURCE_BYTES + 1u;
  char *big = (char *)malloc(too_big);

  if (big == NULL) {
    UNIT_TRUE(t, true); /* a host that cannot allocate 64 MB is not a doot bug */
    arena_destroy(a);
    return;
  }
  memset(big, 'x', too_big); /* valid UTF-8 and NUL-free, so only size can fail */

  diag_sink_init(&sink, a, 0);
  UNIT_NULL(t, source_from_memory(a, SLICE_LIT("huge.do"), slice_make(big, too_big), &sink));
  UNIT_EQ_INT(t, diag_error_count(&sink), 1);
  UNIT_TRUE(t, sink.first != NULL && sink.first->code == DIAG_SOURCE_TOO_LARGE);

  /* Exactly at the limit is accepted. */
  diag_sink_init(&sink, a, 0);
  UNIT_NOT_NULL(
      t, source_from_memory(a, SLICE_LIT("big.do"), slice_make(big, DOOT_MAX_SOURCE_BYTES), &sink));
  UNIT_EQ_INT(t, diag_error_count(&sink), 0);

  free(big);
  arena_destroy(a);
}

static void nul_bytes_are_rejected(unit *t) {
  arena *a = arena_new(4096u);
  diag_sink sink;

  diag_sink_init(&sink, a, 0);
  UNIT_NULL(t, source_from_memory(a, SLICE_LIT("app.do"), slice_make("let x\0= 1", 9u), &sink));
  UNIT_EQ_INT(t, diag_error_count(&sink), 1);
  UNIT_TRUE(t, sink.first != NULL && sink.first->code == DIAG_SOURCE_NUL_BYTE);
  arena_destroy(a);
}

static void unreadable_file_reports_a_diagnostic(unit *t) {
  arena *a = arena_new(4096u);
  diag_sink sink;

  diag_sink_init(&sink, a, 0);
  UNIT_NULL(t, source_from_file(a, SLICE_LIT("/nonexistent/doot/app.do"), &sink));
  UNIT_EQ_INT(t, diag_error_count(&sink), 1);
  UNIT_TRUE(t, sink.first != NULL && sink.first->code == DIAG_CANNOT_READ_FILE);
  arena_destroy(a);
}

static void path_and_text_are_copied(unit *t) {
  arena *a = arena_new(4096u);
  char mutable_text[8];
  source *s;

  mutable_text[0] = 'l';
  mutable_text[1] = 'e';
  mutable_text[2] = 't';
  mutable_text[3] = '\0';
  s = source_from_memory(a, SLICE_LIT("x.do"), slice_make(mutable_text, 3u), NULL);
  UNIT_NOT_NULL(t, s);
  mutable_text[0] = 'X'; /* mutating the caller's buffer must not affect the source */
  UNIT_EQ_SLICE(t, source_text(s), "let");
  UNIT_EQ_SLICE(t, source_path(s), "x.do");
  UNIT_EQ_INT(t, source_size(s), 3);
  arena_destroy(a);
}

static const unit_case cases[] = {
    {"line_index_counts_lines", line_index_counts_lines},
    {"line_text_excludes_terminators", line_text_excludes_terminators},
    {"line_col_maps_offsets", line_col_maps_offsets},
    {"columns_are_counted_in_characters", columns_are_counted_in_characters},
    {"offsets_past_the_end_clamp", offsets_past_the_end_clamp},
    {"oversized_sources_are_rejected", oversized_sources_are_rejected},
    {"valid_utf8_is_accepted", valid_utf8_is_accepted},
    {"invalid_utf8_is_rejected", invalid_utf8_is_rejected},
    {"nul_bytes_are_rejected", nul_bytes_are_rejected},
    {"unreadable_file_reports_a_diagnostic", unreadable_file_reports_a_diagnostic},
    {"path_and_text_are_copied", path_and_text_are_copied},
};

UNIT_SUITE(suite_source, "source", cases);
