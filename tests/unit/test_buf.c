#include "../../src/base/buf.h"

#include "unit.h"

static void append_forms_accumulate(unit *t) {
  arena *a = arena_new(1024u);
  buf b;

  buf_init(&b, a, 16u);
  (void)buf_append_cstr(&b, "GET ");
  (void)buf_append_slice(&b, SLICE_LIT("/users"));
  (void)buf_append_byte(&b, ' ');
  (void)buf_printf(&b, "HTTP/%d.%d", 1, 1);
  UNIT_EQ_SLICE(t, buf_slice(&b), "GET /users HTTP/1.1");
  UNIT_FALSE(t, buf_failed(&b));
  arena_destroy(a);
}

static void growth_preserves_content(unit *t) {
  arena *a = arena_new(64u);
  buf b;
  int i;
  slice got;
  bool all_x = true;
  size_t k;

  buf_init(&b, a, 8u);
  for (i = 0; i < 5000; i++) {
    (void)buf_append_byte(&b, 'x');
  }
  got = buf_slice(&b);
  UNIT_EQ_INT(t, got.n, 5000);
  for (k = 0; k < got.n; k++) {
    if (got.p[k] != 'x') {
      all_x = false;
    }
  }
  UNIT_TRUE(t, all_x);
  arena_destroy(a);
}

static void cstr_terminates_without_changing_length(unit *t) {
  arena *a = arena_new(1024u);
  buf b;
  const char *c;

  buf_init(&b, a, 4u); /* exact capacity, so terminating must reserve */
  (void)buf_append_cstr(&b, "abcd");
  c = buf_cstr(&b);
  UNIT_EQ_STR(t, c, "abcd");
  UNIT_EQ_INT(t, b.len, 4);
  arena_destroy(a);
}

static void repeat_appends_n_bytes(unit *t) {
  arena *a = arena_new(1024u);
  buf b;

  buf_init(&b, a, 4u);
  (void)buf_append_repeat(&b, '^', 5u);
  (void)buf_append_repeat(&b, '-', 0);
  UNIT_EQ_SLICE(t, buf_slice(&b), "^^^^^");
  arena_destroy(a);
}

static void clear_keeps_capacity(unit *t) {
  arena *a = arena_new(1024u);
  buf b;
  size_t cap;

  buf_init(&b, a, 128u);
  (void)buf_append_cstr(&b, "discarded");
  cap = b.cap;
  buf_clear(&b);
  UNIT_EQ_INT(t, b.len, 0);
  UNIT_EQ_INT(t, b.cap, cap);
  UNIT_EQ_SLICE(t, buf_slice(&b), "");
  arena_destroy(a);
}

static void json_escaping_covers_control_bytes(unit *t) {
  arena *a = arena_new(1024u);
  buf b;

  buf_init(&b, a, 32u);
  (void)buf_append_json_escaped(&b, SLICE_LIT("a\"b\\c\nd\te"));
  UNIT_EQ_SLICE(t, buf_slice(&b), "a\\\"b\\\\c\\nd\\te");

  buf_clear(&b);
  (void)buf_append_json_escaped(&b, slice_make("\x01\x1f", 2u));
  UNIT_EQ_SLICE(t, buf_slice(&b), "\\u0001\\u001f");

  /* Valid UTF-8 passes through unescaped. */
  buf_clear(&b);
  (void)buf_append_json_escaped(&b, SLICE_LIT("caf\xc3\xa9"));
  UNIT_EQ_SLICE(t, buf_slice(&b), "caf\xc3\xa9");
  arena_destroy(a);
}

static void exhaustion_is_sticky_and_reported_once(unit *t) {
  arena *a = arena_new(1024u);
  buf b;

  arena_set_limit(a, 4096u);
  buf_init(&b, a, 64u);
  UNIT_FALSE(t, buf_failed(&b));

  while (!buf_failed(&b)) {
    (void)buf_append_repeat(&b, 'z', 512u);
  }
  UNIT_TRUE(t, buf_failed(&b));
  /* Still safe to call, still reports failure, never crashes. */
  UNIT_FALSE(t, buf_append_cstr(&b, "more"));
  UNIT_TRUE(t, buf_failed(&b));
  UNIT_EQ_STR(t, buf_cstr(&b), "");
  arena_destroy(a);
}

static const unit_case cases[] = {
    {"append_forms_accumulate", append_forms_accumulate},
    {"growth_preserves_content", growth_preserves_content},
    {"cstr_terminates_without_changing_length", cstr_terminates_without_changing_length},
    {"repeat_appends_n_bytes", repeat_appends_n_bytes},
    {"clear_keeps_capacity", clear_keeps_capacity},
    {"json_escaping_covers_control_bytes", json_escaping_covers_control_bytes},
    {"exhaustion_is_sticky_and_reported_once", exhaustion_is_sticky_and_reported_once},
};

UNIT_SUITE(suite_buf, "buf", cases);
