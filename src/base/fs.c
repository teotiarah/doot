/* The POSIX interfaces this file needs are not visible under a bare
 * -std=c99 -pedantic, so the feature test macro comes before every include. It is
 * scoped to this translation unit, which is the whole point of confining the
 * operating system to one file. */
#define _POSIX_C_SOURCE 200809L

#include "fs.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "assert.h"
#include "buf.h"
#include "diag.h"

bool fs_is_dir(arena *a, slice path) {
  const char *cpath;
  struct stat st;

  DOOT_ASSERT(a != NULL);
  cpath = slice_cstr(a, path);
  if (cpath == NULL) {
    return false;
  }
  if (stat(cpath, &st) != 0) {
    return false;
  }
  return S_ISDIR(st.st_mode) != 0;
}

/* Inserted in name order as entries arrive. A directory in a doot project holds a
 * handful of files, so an insertion sort over a linked list is the right shape:
 * no second pass, no array to grow, and the arena never has to move anything. */
static void insert_sorted(fs_entries *out, fs_entry *e) {
  fs_entry *prev = NULL;
  fs_entry *cur = out->first;

  while (cur != NULL && slice_cmp(cur->name, e->name) < 0) {
    prev = cur;
    cur = cur->next;
  }
  e->next = cur;
  if (prev == NULL) {
    out->first = e;
  } else {
    prev->next = e;
  }
  if (cur == NULL) {
    out->last = e;
  }
  out->count++;
}

bool fs_read_dir(arena *a, slice path, fs_entries *out, diag_sink *sink) {
  const char *cpath;
  DIR *dir;
  struct dirent *ent;

  DOOT_ASSERT(a != NULL && out != NULL);
  memset(out, 0, sizeof(*out));

  cpath = slice_cstr(a, path);
  if (cpath == NULL) {
    return false;
  }
  dir = opendir(cpath);
  if (dir == NULL) {
    if (sink != NULL) {
      (void)diag_report(sink, DIAG_CANNOT_READ_DIR, NULL, span_none(), "cannot read `%s`", cpath);
    }
    return false;
  }

  while ((ent = readdir(dir)) != NULL) {
    fs_entry *e;
    size_t len = strlen(ent->d_name);
    char *name;

    /* Skips `.`, `..`, and every dotfile, which is what keeps `.git` out of a
     * project walk without needing a list of directories to ignore. */
    if (len == 0u || ent->d_name[0] == '.') {
      continue;
    }
    e = ARENA_NEW(a, fs_entry);
    name = arena_dup(a, ent->d_name, len);
    if (e == NULL || name == NULL) {
      (void)closedir(dir);
      return false;
    }
    e->name = slice_make(name, len);
    e->is_dir = fs_is_dir(a, fs_join(a, path, e->name));
    insert_sorted(out, e);
  }
  (void)closedir(dir);
  return true;
}

slice fs_join(arena *a, slice dir, slice name) {
  buf out;

  DOOT_ASSERT(a != NULL);
  if (slice_is_empty(dir)) {
    return name;
  }
  buf_init(&out, a, dir.n + name.n + 2u);
  (void)buf_append_slice(&out, dir);
  if (dir.p[dir.n - 1u] != '/') {
    (void)buf_append_byte(&out, '/');
  }
  (void)buf_append_slice(&out, name);
  return buf_slice(&out);
}

bool fs_write_file(arena *a, slice path, slice contents, diag_sink *sink) {
  const char *cpath;
  const char *ctmp;
  slice tmp;
  buf name;
  FILE *f;
  size_t written;

  DOOT_ASSERT(a != NULL);
  cpath = slice_cstr(a, path);
  if (cpath == NULL) {
    return false;
  }

  /* A sibling of the target, so the rename stays on one filesystem. */
  buf_init(&name, a, path.n + 8u);
  (void)buf_append_slice(&name, path);
  (void)buf_append_cstr(&name, ".dootfmt");
  tmp = buf_slice(&name);
  ctmp = slice_cstr(a, tmp);
  if (ctmp == NULL) {
    return false;
  }

  f = fopen(ctmp, "wb");
  if (f == NULL) {
    if (sink != NULL) {
      (void)diag_report(sink, DIAG_CANNOT_WRITE_FILE, NULL, span_none(), "cannot write `%s`",
                        cpath);
    }
    return false;
  }
  written = contents.n == 0u ? 0u : fwrite(contents.p, 1u, contents.n, f);
  if (written != contents.n || fclose(f) != 0) {
    (void)remove(ctmp);
    if (sink != NULL) {
      (void)diag_report(sink, DIAG_CANNOT_WRITE_FILE, NULL, span_none(), "cannot write `%s`",
                        cpath);
    }
    return false;
  }
  if (rename(ctmp, cpath) != 0) {
    (void)remove(ctmp);
    if (sink != NULL) {
      (void)diag_report(sink, DIAG_CANNOT_WRITE_FILE, NULL, span_none(), "cannot replace `%s`",
                        cpath);
    }
    return false;
  }
  return true;
}
