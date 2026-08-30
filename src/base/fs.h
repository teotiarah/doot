/* fs.h -- the small amount of filesystem the driver needs.
 *
 * Reading a file is source_from_file (source.h). This is the rest: telling a
 * directory from a file, listing one, and replacing one safely.
 *
 * ISO C has no directory traversal, so this is the first part of doot that needs
 * an operating system interface, and it is deliberately the only one. The POSIX
 * implementation covers Linux and macOS; Windows arrives with the rest of v0.5
 * (07-roadmap.md#v05--everywhere) and needs FindFirstFile here and nothing else,
 * which is why the API hands back a plain list rather than exposing a handle.
 *
 * Ownership: names and paths are allocated in the arena passed in. An fs_entry
 * list is valid until that arena is reset or destroyed.
 */
#ifndef DOOT_FS_H
#define DOOT_FS_H

#include <stdbool.h>

#include "arena.h"
#include "plat.h"
#include "slice.h"
#include "source.h" /* for diag_sink */

typedef struct fs_entry fs_entry;

struct fs_entry {
  slice name; /* the entry name only, not a path */
  bool is_dir;
  fs_entry *next;
};

typedef struct {
  fs_entry *first;
  fs_entry *last;
  uint32_t count;
} fs_entries;

bool fs_is_dir(arena *a, slice path);

/* Lists `path`, excluding `.`, `..`, and anything beginning with a dot.
 *
 * Entries come back **sorted by name**, because readdir order is whatever the
 * filesystem feels like and a tool that rewrites source files must visit them in
 * the same order every time. Without that, `doot fmt` on a directory would report
 * its results in an order that changed between machines.
 *
 * Reports DT1003 and returns false if the directory cannot be opened; `out` is
 * then empty. `sink` may be NULL to fail silently. */
bool fs_read_dir(arena *a, slice path, fs_entries *out, diag_sink *sink);

/* `dir` and `name` joined with a separator, unless `dir` is empty. */
slice fs_join(arena *a, slice dir, slice name);

/* Replaces the file at `path`.
 *
 * Writes a sibling temporary file and renames it over the target, so an
 * interrupted or failing write leaves the original intact. A formatter that
 * truncates a source file it then fails to rewrite is worse than one that does
 * nothing, and `rename` is ISO C, so this costs nothing in portability.
 *
 * Reports DT1002 and returns false on failure. `sink` may be NULL. */
bool fs_write_file(arena *a, slice path, slice contents, diag_sink *sink);

#endif /* DOOT_FS_H */
