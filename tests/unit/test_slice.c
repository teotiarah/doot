#include "../../src/base/slice.h"

#include "unit.h"

static void equality_and_comparison(unit *t) {
  UNIT_TRUE(t, slice_eq(SLICE_LIT("route"), SLICE_LIT("route")));
  UNIT_FALSE(t, slice_eq(SLICE_LIT("route"), SLICE_LIT("router")));
  UNIT_TRUE(t, slice_eq(SLICE_EMPTY, SLICE_EMPTY));

  /* An embedded NUL is data, not a terminator. */
  UNIT_FALSE(t, slice_eq(slice_make("a\0b", 3u), slice_make("a\0c", 3u)));

  UNIT_TRUE(t, slice_cmp(SLICE_LIT("a"), SLICE_LIT("b")) < 0);
  UNIT_TRUE(t, slice_cmp(SLICE_LIT("b"), SLICE_LIT("a")) > 0);
  UNIT_EQ_INT(t, slice_cmp(SLICE_LIT("ab"), SLICE_LIT("ab")), 0);
  UNIT_TRUE(t, slice_cmp(SLICE_LIT("ab"), SLICE_LIT("abc")) < 0);
}

static void hash_is_stable_and_distinguishes(unit *t) {
  UNIT_EQ_INT(t, slice_hash(SLICE_LIT("route")) == slice_hash(SLICE_LIT("route")), 1);
  UNIT_TRUE(t, slice_hash(SLICE_LIT("route")) != slice_hash(SLICE_LIT("stream")));
  /* FNV-1a of the empty string is the offset basis. */
  UNIT_TRUE(t, slice_hash(SLICE_EMPTY) == 0xcbf29ce484222325ull);
}

static void prefix_and_suffix(unit *t) {
  slice s = SLICE_LIT("routes/users.do");

  UNIT_TRUE(t, slice_starts_with(s, SLICE_LIT("routes/")));
  UNIT_FALSE(t, slice_starts_with(s, SLICE_LIT("models/")));
  UNIT_TRUE(t, slice_ends_with(s, SLICE_LIT(".do")));
  UNIT_FALSE(t, slice_ends_with(s, SLICE_LIT(".dot")));
  UNIT_TRUE(t, slice_starts_with(s, SLICE_EMPTY));
  UNIT_FALSE(t, slice_starts_with(SLICE_LIT("ab"), SLICE_LIT("abc")));
}

static void sub_clamps_instead_of_faulting(unit *t) {
  slice s = SLICE_LIT("abcdef");

  UNIT_EQ_SLICE(t, slice_sub(s, 0, 3u), "abc");
  UNIT_EQ_SLICE(t, slice_sub(s, 3u, 3u), "def");
  UNIT_EQ_SLICE(t, slice_sub(s, 4u, 100u), "ef");
  UNIT_EQ_SLICE(t, slice_sub(s, 6u, 1u), "");
  UNIT_EQ_SLICE(t, slice_sub(s, 100u, 1u), "");
}

static void trim_removes_ascii_whitespace(unit *t) {
  UNIT_EQ_SLICE(t, slice_trim_ascii(SLICE_LIT("  hello \t\n")), "hello");
  UNIT_EQ_SLICE(t, slice_trim_ascii(SLICE_LIT("hello")), "hello");
  UNIT_EQ_SLICE(t, slice_trim_ascii(SLICE_LIT("   ")), "");
  UNIT_EQ_SLICE(t, slice_trim_ascii(SLICE_EMPTY), "");
}

static void cut_splits_at_the_first_separator(unit *t) {
  slice before;
  slice after;

  UNIT_TRUE(t, slice_cut(SLICE_LIT("models.user.find"), '.', &before, &after));
  UNIT_EQ_SLICE(t, before, "models");
  UNIT_EQ_SLICE(t, after, "user.find");

  UNIT_FALSE(t, slice_cut(SLICE_LIT("models"), '.', &before, &after));
  UNIT_EQ_SLICE(t, before, "models");
  UNIT_EQ_SLICE(t, after, "");

  UNIT_TRUE(t, slice_cut(SLICE_LIT(".leading"), '.', &before, &after));
  UNIT_EQ_SLICE(t, before, "");
  UNIT_EQ_SLICE(t, after, "leading");

  UNIT_TRUE(t, slice_cut(SLICE_LIT("trailing."), '.', &before, &after));
  UNIT_EQ_SLICE(t, before, "trailing");
  UNIT_EQ_SLICE(t, after, "");
}

static void cstr_copy_is_terminated(unit *t) {
  arena *a = arena_new(64u);
  const char *c = slice_cstr(a, slice_make("abcdef", 3u));

  UNIT_EQ_STR(t, c, "abc");
  arena_destroy(a);
}

static const unit_case cases[] = {
    {"equality_and_comparison", equality_and_comparison},
    {"hash_is_stable_and_distinguishes", hash_is_stable_and_distinguishes},
    {"prefix_and_suffix", prefix_and_suffix},
    {"sub_clamps_instead_of_faulting", sub_clamps_instead_of_faulting},
    {"trim_removes_ascii_whitespace", trim_removes_ascii_whitespace},
    {"cut_splits_at_the_first_separator", cut_splits_at_the_first_separator},
    {"cstr_copy_is_terminated", cstr_copy_is_terminated},
};

UNIT_SUITE(suite_slice, "slice", cases);
