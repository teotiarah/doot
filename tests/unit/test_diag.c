#include "../../src/base/diag.h"

#include <string.h>

#include "unit.h"

/* The registry is the single source of truth for every diagnostic (D050), so its
 * well-formedness is worth asserting mechanically rather than by review. */
static void registry_is_well_formed(unit *t) {
  int i;
  int j;

  UNIT_TRUE(t, (int)DIAG_CODE_COUNT > 0);

  for (i = 0; i < (int)DIAG_CODE_COUNT; i++) {
    diag_code c = (diag_code)i;
    const char *code = diag_code_str(c);
    diag_code parsed;

    /* Shaped DTnnnn. */
    UNIT_EQ_INT(t, strlen(code), 6);
    UNIT_TRUE(t, code[0] == 'D' && code[1] == 'T');
    UNIT_TRUE(t, code[2] >= '0' && code[2] <= '9');
    UNIT_TRUE(t, code[5] >= '0' && code[5] <= '9');

    /* Every code has a brief and a full explanation; neither may be empty. */
    UNIT_TRUE(t, strlen(diag_code_brief(c)) > 0);
    UNIT_TRUE(t, strlen(diag_code_explain(c)) > 8);

    /* A brief is a sentence fragment: no capital, no trailing period. */
    UNIT_TRUE(t, diag_code_brief(c)[0] < 'A' || diag_code_brief(c)[0] > 'Z');
    UNIT_TRUE(t, diag_code_brief(c)[strlen(diag_code_brief(c)) - 1u] != '.');

    /* Round-trips through the parser used by `doot explain`. */
    UNIT_TRUE(t, diag_code_parse(slice_from_cstr(code), &parsed));
    UNIT_EQ_INT(t, (int)parsed, i);

    /* Codes are unique. */
    for (j = 0; j < i; j++) {
      UNIT_FALSE(t, strcmp(code, diag_code_str((diag_code)j)) == 0);
    }
  }
}

static void unknown_codes_do_not_parse(unit *t) {
  diag_code c;

  UNIT_FALSE(t, diag_code_parse(SLICE_LIT("DT9999"), &c));
  UNIT_FALSE(t, diag_code_parse(SLICE_LIT("dt0001"), &c));
  UNIT_FALSE(t, diag_code_parse(SLICE_LIT(""), &c));
  UNIT_FALSE(t, diag_code_parse(SLICE_LIT("DT0001 "), &c));
}

static void counts_track_severity(unit *t) {
  arena *a = arena_new(4096u);
  diag_sink sink;

  diag_sink_init(&sink, a, 0);
  UNIT_FALSE(t, diag_has_errors(&sink));
  UNIT_EQ_INT(t, diag_error_count(&sink), 0);

  (void)diag_report(&sink, DIAG_INVALID_UTF8, NULL, span_none(), "one");
  (void)diag_report(&sink, DIAG_CANNOT_READ_FILE, NULL, span_none(), "two");
  UNIT_TRUE(t, diag_has_errors(&sink));
  UNIT_EQ_INT(t, diag_error_count(&sink), 2);
  UNIT_EQ_INT(t, diag_warning_count(&sink), 0);
  arena_destroy(a);
}

static void reports_are_kept_in_order(unit *t) {
  arena *a = arena_new(4096u);
  diag_sink sink;
  const diag *d;

  diag_sink_init(&sink, a, 0);
  (void)diag_report(&sink, DIAG_INVALID_UTF8, NULL, span_none(), "first");
  (void)diag_report(&sink, DIAG_INVALID_UTF8, NULL, span_none(), "second");
  (void)diag_report(&sink, DIAG_INVALID_UTF8, NULL, span_none(), "third");

  d = sink.first;
  UNIT_EQ_SLICE(t, d->message, "first");
  UNIT_EQ_SLICE(t, d->next->message, "second");
  UNIT_EQ_SLICE(t, d->next->next->message, "third");
  UNIT_NULL(t, d->next->next->next);
  arena_destroy(a);
}

static void the_limit_truncates_rather_than_growing(unit *t) {
  arena *a = arena_new(4096u);
  diag_sink sink;
  int i;

  diag_sink_init(&sink, a, 3u);
  for (i = 0; i < 10; i++) {
    (void)diag_report(&sink, DIAG_INVALID_UTF8, NULL, span_none(), "d%d", i);
  }
  UNIT_EQ_INT(t, sink.count, 3);
  UNIT_TRUE(t, sink.truncated);
  arena_destroy(a);
}

/* The exact human rendering is a contract: docs/06-tooling.md shows this shape,
 * and an agent reading stderr depends on it. */
static void human_rendering_places_the_caret(unit *t) {
  arena *a = arena_new(65536u);
  diag_sink sink;
  source *s;
  diag *d;
  buf out;

  s = source_from_memory(a, SLICE_LIT("routes/users.do"),
                         SLICE_LIT("route GET \"/\" () -> html! {\n"
                                   "  let u = db.one[User](\"select emial from users\")!\n"
                                   "}\n"),
                         NULL);
  UNIT_NOT_NULL(t, s);

  diag_sink_init(&sink, a, 0);
  d = diag_report(&sink, DIAG_INVALID_UTF8, s, span_make(59u, 64u),
                  "column `emial` does not exist on table `users`");
  diag_fix(&sink, d, span_make(59u, 64u), SLICE_LIT("email"));
  diag_label_add(&sink, d, s, span_make(0, 5u), "route declared here");

  buf_init(&out, a, 1024u);
  diag_render_human(&sink, &out, false);

  UNIT_EQ_SLICE(t, buf_slice(&out),
                "error[DT0001]: column `emial` does not exist on table `users`\n"
                " --> routes/users.do:2:32\n"
                "  |\n"
                "2 |   let u = db.one[User](\"select emial from users\")!\n"
                "  |                                ^^^^^\n"
                " --> routes/users.do:1:1\n"
                "  |\n"
                "1 | route GET \"/\" () -> html! {\n"
                "  | ^^^^^ route declared here\n"
                " help: replace with `email`\n"
                " help: run `doot explain DT0001` for more\n"
                "\n");
  arena_destroy(a);
}

static void human_rendering_expands_tabs_for_caret_alignment(unit *t) {
  arena *a = arena_new(65536u);
  diag_sink sink;
  source *s;
  buf out;

  /* A tab then "xy"; the caret must sit under 'y' at display column 5. */
  s = source_from_memory(a, SLICE_LIT("t.do"), SLICE_LIT("\txy\n"), NULL);
  diag_sink_init(&sink, a, 0);
  (void)diag_report(&sink, DIAG_INVALID_UTF8, s, span_make(2u, 3u), "here");

  buf_init(&out, a, 1024u);
  diag_render_human(&sink, &out, false);

  UNIT_EQ_SLICE(t, buf_slice(&out),
                "error[DT0001]: here\n"
                " --> t.do:1:3\n"
                "  |\n"
                "1 |     xy\n"
                "  |      ^\n"
                " help: run `doot explain DT0001` for more\n"
                "\n");
  arena_destroy(a);
}

static void json_rendering_matches_the_documented_schema(unit *t) {
  arena *a = arena_new(65536u);
  diag_sink sink;
  source *s;
  diag *d;
  buf out;

  s = source_from_memory(a, SLICE_LIT("a.do"), SLICE_LIT("let x = 1\n"), NULL);
  diag_sink_init(&sink, a, 0);
  d = diag_report(&sink, DIAG_INVALID_UTF8, s, span_make(4u, 5u), "a \"quoted\" message");
  diag_fix(&sink, d, span_make(4u, 5u), SLICE_LIT("y"));
  diag_label_add(&sink, d, s, span_make(0, 3u), "declared here");

  buf_init(&out, a, 1024u);
  diag_render_json(&sink, &out);

  UNIT_EQ_SLICE(t, buf_slice(&out),
                "{\"diagnostics\":[{\"code\":\"DT0001\",\"severity\":\"error\","
                "\"message\":\"a \\\"quoted\\\" message\",\"file\":\"a.do\","
                "\"span\":{\"start\":4,\"end\":5,\"line\":1,\"col\":5},"
                "\"suggestion\":{\"replace_span\":[4,5],\"with\":\"y\"},"
                "\"related\":[{\"file\":\"a.do\","
                "\"span\":{\"start\":0,\"end\":3,\"line\":1,\"col\":1},"
                "\"message\":\"declared here\"}]}],"
                "\"summary\":{\"errors\":1,\"warnings\":0,\"truncated\":false}}\n");
  arena_destroy(a);
}

static void empty_sink_renders_valid_json(unit *t) {
  arena *a = arena_new(4096u);
  diag_sink sink;
  buf out;

  diag_sink_init(&sink, a, 0);
  buf_init(&out, a, 256u);
  diag_render_json(&sink, &out);
  UNIT_EQ_SLICE(t, buf_slice(&out),
                "{\"diagnostics\":[],\"summary\":{\"errors\":0,\"warnings\":0,"
                "\"truncated\":false}}\n");

  buf_clear(&out);
  diag_render_human(&sink, &out, false);
  UNIT_EQ_SLICE(t, buf_slice(&out), "");
  arena_destroy(a);
}

static void positionless_diagnostics_omit_the_snippet(unit *t) {
  arena *a = arena_new(4096u);
  diag_sink sink;
  buf out;

  diag_sink_init(&sink, a, 0);
  (void)diag_report(&sink, DIAG_CANNOT_READ_FILE, NULL, span_none(), "cannot read `missing.do`");
  buf_init(&out, a, 512u);
  diag_render_human(&sink, &out, false);
  UNIT_EQ_SLICE(t, buf_slice(&out),
                "error[DT1001]: cannot read `missing.do`\n"
                "  help: run `doot explain DT1001` for more\n"
                "\n");
  arena_destroy(a);
}

static const unit_case cases[] = {
    {"registry_is_well_formed", registry_is_well_formed},
    {"unknown_codes_do_not_parse", unknown_codes_do_not_parse},
    {"counts_track_severity", counts_track_severity},
    {"reports_are_kept_in_order", reports_are_kept_in_order},
    {"the_limit_truncates_rather_than_growing", the_limit_truncates_rather_than_growing},
    {"human_rendering_places_the_caret", human_rendering_places_the_caret},
    {"human_rendering_expands_tabs_for_caret_alignment",
     human_rendering_expands_tabs_for_caret_alignment},
    {"json_rendering_matches_the_documented_schema", json_rendering_matches_the_documented_schema},
    {"empty_sink_renders_valid_json", empty_sink_renders_valid_json},
    {"positionless_diagnostics_omit_the_snippet", positionless_diagnostics_omit_the_snippet},
};

UNIT_SUITE(suite_diag, "diag", cases);
