#include "../../src/base/arena.h"

#include <string.h>

#include "unit.h"

static void alignment_is_honored(unit *t) {
  arena *a = arena_new(1024u);
  size_t aligns[4];
  size_t i;

  aligns[0] = 1u;
  aligns[1] = 2u;
  aligns[2] = 8u;
  aligns[3] = 16u;

  for (i = 0; i < 4u; i++) {
    /* An odd 1-byte allocation between each so the bump pointer is misaligned. */
    (void)arena_alloc(a, 1u, 1u);
    UNIT_EQ_INT(t, (size_t)(uintptr_t)arena_alloc(a, 24u, aligns[i]) % aligns[i], 0);
  }
  arena_destroy(a);
}

static void zero_length_gives_distinct_pointers(unit *t) {
  arena *a = arena_new(1024u);
  void *p = arena_alloc(a, 0, 1u);
  void *q = arena_alloc(a, 0, 1u);

  UNIT_NOT_NULL(t, p);
  UNIT_NOT_NULL(t, q);
  UNIT_TRUE(t, p != q);
  arena_destroy(a);
}

static void allocations_larger_than_a_chunk_succeed(unit *t) {
  arena *a = arena_new(64u);
  char *big = (char *)arena_alloc(a, 100000u, 8u);

  UNIT_NOT_NULL(t, big);
  memset(big, 0x5a, 100000u);
  UNIT_EQ_INT(t, (unsigned char)big[99999], 0x5a);
  UNIT_TRUE(t, arena_reserved(a) >= 100000u);
  arena_destroy(a);
}

static void zeroed_allocation_is_zero(unit *t) {
  arena *a = arena_new(1024u);
  unsigned char *p;
  size_t i;
  bool all_zero = true;

  (void)arena_alloc(a, 300u, 1u); /* dirty the chunk first */
  arena_reset(a);
  p = (unsigned char *)arena_alloc_zero(a, 300u, 8u);
  UNIT_NOT_NULL(t, p);
  for (i = 0; i < 300u; i++) {
    if (p[i] != 0) {
      all_zero = false;
    }
  }
  UNIT_TRUE(t, all_zero);
  arena_destroy(a);
}

static void reset_reuses_chunks(unit *t) {
  arena *a = arena_new(1024u);
  size_t reserved_after_first;

  (void)arena_alloc(a, 900u, 8u);
  (void)arena_alloc(a, 900u, 8u); /* forces a second chunk */
  reserved_after_first = arena_reserved(a);
  UNIT_TRUE(t, arena_used(a) >= 1800u);

  arena_reset(a);
  UNIT_EQ_INT(t, arena_used(a), 0);

  (void)arena_alloc(a, 900u, 8u);
  (void)arena_alloc(a, 900u, 8u);
  /* Chunks came from the spare list, so nothing new was taken from the system. */
  UNIT_EQ_INT(t, arena_reserved(a), reserved_after_first);
  arena_destroy(a);
}

static void mark_and_rollback_restore_position(unit *t) {
  arena *a = arena_new(1024u);
  arena_mark m;
  void *first;
  void *again;

  (void)arena_alloc(a, 16u, 8u);
  m = arena_save(a);
  first = arena_alloc(a, 32u, 8u);
  (void)arena_alloc(a, 4000u, 8u); /* spills into further chunks */

  arena_rollback(a, m);
  again = arena_alloc(a, 32u, 8u);
  UNIT_EQ_PTR(t, again, first);
  arena_destroy(a);
}

static void extend_grows_in_place_when_last(unit *t) {
  arena *a = arena_new(4096u);
  char *p = (char *)arena_alloc(a, 10u, 1u);
  char *grown;

  memcpy(p, "0123456789", 10u);
  grown = (char *)arena_extend(a, p, 10u, 20u, 1u);
  UNIT_EQ_PTR(t, grown, p);
  UNIT_EQ_SLICE(t, slice_make(grown, 10u), "0123456789");
  arena_destroy(a);
}

static void extend_copies_when_not_last(unit *t) {
  arena *a = arena_new(4096u);
  char *p = (char *)arena_alloc(a, 10u, 1u);
  char *grown;

  memcpy(p, "0123456789", 10u);
  (void)arena_alloc(a, 8u, 8u); /* p is no longer the most recent allocation */
  grown = (char *)arena_extend(a, p, 10u, 20u, 1u);
  UNIT_TRUE(t, grown != p);
  UNIT_EQ_SLICE(t, slice_make(grown, 10u), "0123456789");
  arena_destroy(a);
}

static void limit_is_enforced_without_aborting(unit *t) {
  arena *a = arena_new(1024u);
  void *p;

  arena_set_limit(a, 8192u);
  UNIT_FALSE(t, arena_exhausted(a));

  p = arena_alloc(a, 100000u, 8u);
  UNIT_NULL(t, p);
  UNIT_TRUE(t, arena_exhausted(a));
  UNIT_TRUE(t, arena_reserved(a) <= 8192u);
  arena_destroy(a);
}

static void limit_allows_allocation_up_to_the_cap(unit *t) {
  arena *a = arena_new(1024u);
  size_t granted = 0;
  int i;

  arena_set_limit(a, 64u * 1024u);
  for (i = 0; i < 200; i++) {
    if (arena_alloc(a, 512u, 8u) != NULL) {
      granted += 512u;
    }
  }
  UNIT_TRUE(t, granted > 32u * 1024u);
  UNIT_TRUE(t, arena_reserved(a) <= 64u * 1024u);
  arena_destroy(a);
}

static void printf_formats_into_the_arena(unit *t) {
  arena *a = arena_new(16u); /* smaller than the result, so it must grow */
  char *s = arena_printf(a, "%s/%d/%s", "route", 42, "users");

  UNIT_NOT_NULL(t, s);
  UNIT_EQ_STR(t, s, "route/42/users");
  arena_destroy(a);
}

static void dup_nul_terminates(unit *t) {
  arena *a = arena_new(64u);
  char *s = arena_dup(a, "abcdef", 3u);

  UNIT_NOT_NULL(t, s);
  UNIT_EQ_INT(t, strlen(s), 3);
  UNIT_EQ_STR(t, s, "abc");
  arena_destroy(a);
}

static void peak_tracks_high_water_mark(unit *t) {
  arena *a = arena_new(1024u);
  size_t peak;

  (void)arena_alloc(a, 5000u, 8u);
  peak = arena_peak(a);
  UNIT_TRUE(t, peak >= 5000u);
  arena_reset(a);
  UNIT_EQ_INT(t, arena_peak(a), peak); /* reset keeps chunks, so peak is unchanged */
  arena_destroy(a);
}

static const unit_case cases[] = {
    {"alignment_is_honored", alignment_is_honored},
    {"zero_length_gives_distinct_pointers", zero_length_gives_distinct_pointers},
    {"allocations_larger_than_a_chunk_succeed", allocations_larger_than_a_chunk_succeed},
    {"zeroed_allocation_is_zero", zeroed_allocation_is_zero},
    {"reset_reuses_chunks", reset_reuses_chunks},
    {"mark_and_rollback_restore_position", mark_and_rollback_restore_position},
    {"extend_grows_in_place_when_last", extend_grows_in_place_when_last},
    {"extend_copies_when_not_last", extend_copies_when_not_last},
    {"limit_is_enforced_without_aborting", limit_is_enforced_without_aborting},
    {"limit_allows_allocation_up_to_the_cap", limit_allows_allocation_up_to_the_cap},
    {"printf_formats_into_the_arena", printf_formats_into_the_arena},
    {"dup_nul_terminates", dup_nul_terminates},
    {"peak_tracks_high_water_mark", peak_tracks_high_water_mark},
};

UNIT_SUITE(suite_arena, "arena", cases);
