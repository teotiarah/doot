#include "../../src/base/fs.h"

#include <stdio.h>
#include <string.h>

#include "../../src/base/diag.h"
#include "unit.h"

/* These tests run from the repository root, which is where `make test` invokes the
 * binary, so they use the tree itself as fixture data and write only under build/,
 * which always exists by the time the tests run and is git-ignored. */

static bool has_code(const diag_sink *s, diag_code code) {
  const diag *d;

  for (d = s->first; d != NULL; d = d->next) {
    if (d->code == code) {
      return true;
    }
  }
  return false;
}

static void a_directory_is_told_from_a_file(unit *t) {
  arena *a = arena_new(4096u);

  UNIT_TRUE(t, fs_is_dir(a, SLICE_LIT("src")));
  UNIT_TRUE(t, fs_is_dir(a, SLICE_LIT("src/base")));
  UNIT_FALSE(t, fs_is_dir(a, SLICE_LIT("src/base/fs.c")));
  UNIT_FALSE(t, fs_is_dir(a, SLICE_LIT("does/not/exist")));
  arena_destroy(a);
}

static void joining_inserts_one_separator(unit *t) {
  arena *a = arena_new(4096u);

  UNIT_EQ_SLICE(t, fs_join(a, SLICE_LIT("src"), SLICE_LIT("base")), "src/base");
  /* An existing separator is not doubled. */
  UNIT_EQ_SLICE(t, fs_join(a, SLICE_LIT("src/"), SLICE_LIT("base")), "src/base");
  /* An empty directory yields the name alone, so a walk can start at "". */
  UNIT_EQ_SLICE(t, fs_join(a, SLICE_EMPTY, SLICE_LIT("app.do")), "app.do");
  arena_destroy(a);
}

static void entries_come_back_sorted(unit *t) {
  /* readdir order is whatever the filesystem feels like. A tool that rewrites
   * source files has to visit them in the same order every time, or its output
   * changes between machines. */
  arena *a = arena_new(1u << 16);
  fs_entries entries;
  const fs_entry *e;
  slice prev = SLICE_EMPTY;

  UNIT_TRUE(t, fs_read_dir(a, SLICE_LIT("src"), &entries, NULL));
  UNIT_TRUE(t, entries.count >= 4u);
  for (e = entries.first; e != NULL; e = e->next) {
    if (!slice_is_empty(prev)) {
      UNIT_TRUE(t, slice_cmp(prev, e->name) < 0);
    }
    prev = e->name;
  }
  /* src/ holds only directories, and `base` sorts first. */
  UNIT_NOT_NULL(t, entries.first);
  if (entries.first != NULL) {
    UNIT_EQ_SLICE(t, entries.first->name, "base");
    UNIT_TRUE(t, entries.first->is_dir);
  }
  arena_destroy(a);
}

static void dotfiles_are_skipped(unit *t) {
  /* Which is what keeps `.git` out of a project walk with no ignore list. */
  arena *a = arena_new(1u << 16);
  fs_entries entries;
  const fs_entry *e;

  UNIT_TRUE(t, fs_read_dir(a, SLICE_LIT("."), &entries, NULL));
  for (e = entries.first; e != NULL; e = e->next) {
    UNIT_TRUE(t, e->name.n > 0u && e->name.p[0] != '.');
  }
  arena_destroy(a);
}

static void an_unreadable_directory_is_reported(unit *t) {
  arena *a = arena_new(4096u);
  diag_sink sink;
  fs_entries entries;

  diag_sink_init(&sink, a, 0u);
  UNIT_FALSE(t, fs_read_dir(a, SLICE_LIT("no/such/directory"), &entries, &sink));
  UNIT_TRUE(t, has_code(&sink, DIAG_CANNOT_READ_DIR));
  UNIT_EQ_INT(t, entries.count, 0);
  arena_destroy(a);
}

static void writing_replaces_a_file_atomically(unit *t) {
  arena *a = arena_new(1u << 16);
  diag_sink sink;
  source *back;

  diag_sink_init(&sink, a, 0u);
  UNIT_TRUE(t,
            fs_write_file(a, SLICE_LIT("build/test_fs_tmp.do"), SLICE_LIT("let a = 1\n"), &sink));
  back = source_from_file(a, SLICE_LIT("build/test_fs_tmp.do"), &sink);
  UNIT_NOT_NULL(t, back);
  if (back != NULL) {
    UNIT_EQ_SLICE(t, source_text(back), "let a = 1\n");
  }

  /* Replacing it leaves no temporary behind. */
  UNIT_TRUE(t,
            fs_write_file(a, SLICE_LIT("build/test_fs_tmp.do"), SLICE_LIT("let b = 2\n"), &sink));
  UNIT_FALSE(t, diag_has_errors(&sink));
  UNIT_NULL(t, source_from_file(a, SLICE_LIT("build/test_fs_tmp.do.dootfmt"), NULL));

  (void)remove("build/test_fs_tmp.do");
  arena_destroy(a);
}

static void an_unwritable_path_is_reported(unit *t) {
  arena *a = arena_new(4096u);
  diag_sink sink;

  diag_sink_init(&sink, a, 0u);
  /* The temporary file is a sibling of the target, so a missing directory fails at
   * the open rather than at the rename, leaving nothing behind either way. */
  UNIT_FALSE(t, fs_write_file(a, SLICE_LIT("no/such/directory/out.do"), SLICE_LIT("x"), &sink));
  UNIT_TRUE(t, has_code(&sink, DIAG_CANNOT_WRITE_FILE));
  arena_destroy(a);
}

static void an_empty_file_can_be_written(unit *t) {
  arena *a = arena_new(1u << 16);
  source *back;

  UNIT_TRUE(t, fs_write_file(a, SLICE_LIT("build/test_fs_empty.do"), SLICE_EMPTY, NULL));
  back = source_from_file(a, SLICE_LIT("build/test_fs_empty.do"), NULL);
  UNIT_NOT_NULL(t, back);
  if (back != NULL) {
    UNIT_EQ_INT(t, source_size(back), 0);
  }
  (void)remove("build/test_fs_empty.do");
  arena_destroy(a);
}

static const unit_case cases[] = {
    {"a_directory_is_told_from_a_file", a_directory_is_told_from_a_file},
    {"joining_inserts_one_separator", joining_inserts_one_separator},
    {"entries_come_back_sorted", entries_come_back_sorted},
    {"dotfiles_are_skipped", dotfiles_are_skipped},
    {"an_unreadable_directory_is_reported", an_unreadable_directory_is_reported},
    {"writing_replaces_a_file_atomically", writing_replaces_a_file_atomically},
    {"an_unwritable_path_is_reported", an_unwritable_path_is_reported},
    {"an_empty_file_can_be_written", an_empty_file_can_be_written},
};

UNIT_SUITE(suite_fs, "fs", cases);
