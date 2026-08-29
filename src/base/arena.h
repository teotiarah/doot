/* arena.h -- chunked bump allocator.
 *
 * The allocator described in docs/05-runtime.md. Used by the compiler as well as
 * the runtime (D047): allocation is a pointer increment, and release is one
 * operation over the whole arena. There is no per-object free.
 *
 * Ownership: an arena owns every byte it hands out. Pointers into an arena are
 * valid until arena_reset, arena_rollback past them, or arena_destroy. Nothing
 * else may free them.
 *
 * Failure policy is chosen at construction and never at the call site:
 *   - arena_new             -> returns NULL on exhaustion; the caller must check.
 *   - arena_new_fatal       -> aborts on exhaustion, for the compiler (D047).
 */
#ifndef DOOT_ARENA_H
#define DOOT_ARENA_H

#include <stdarg.h>
#include <stdbool.h>

#include "plat.h"

typedef struct arena arena;

/* A saved allocation position. Valid only for the arena it came from, and only
 * until that arena is reset or destroyed. */
typedef struct {
  void *chunk;
  size_t used;
} arena_mark;

/* chunk_size is the first chunk's capacity; later chunks grow geometrically.
 * Pass 0 for the default (32 KB, matching the per-request chunk size). */
arena *arena_new(size_t chunk_size);
arena *arena_new_fatal(size_t chunk_size);
void arena_destroy(arena *a);

/* Caps total bytes reserved from the system. 0 means unlimited. Used to enforce
 * per-request memory budgets (D005). */
void arena_set_limit(arena *a, size_t max_bytes);

/* align must be a power of two. Returns NULL on exhaustion unless the arena is
 * fatal-on-exhaustion, in which case it aborts. */
void *arena_alloc(arena *a, size_t size, size_t align);
void *arena_alloc_zero(arena *a, size_t size, size_t align);

/* Grows a previous allocation, in place when p was the most recent allocation
 * from this arena -- the common case for an output buffer, which is why
 * appending to a buffer does not copy. Otherwise allocates and copies. */
void *arena_extend(arena *a, void *p, size_t old_size, size_t new_size, size_t align);

/* Copies len bytes and appends a NUL that is not counted in len. */
char *arena_dup(arena *a, const void *src, size_t len);
char *arena_vprintf(arena *a, const char *fmt, va_list *ap) DOOT_VPRINTF(2);
char *arena_printf(arena *a, const char *fmt, ...) DOOT_PRINTF(2, 3);

/* Releases everything allocated but keeps the chunks for reuse. This is the
 * O(1) end-of-request operation. */
void arena_reset(arena *a);

arena_mark arena_save(const arena *a);
void arena_rollback(arena *a, arena_mark m);

size_t arena_used(const arena *a);     /* bytes handed out */
size_t arena_reserved(const arena *a); /* bytes held from the system */
size_t arena_peak(const arena *a);     /* high-water mark of reserved bytes */
bool arena_exhausted(const arena *a);  /* a request has failed since construction */

#define ARENA_NEW(a, T) ((T *)arena_alloc_zero((a), sizeof(T), DOOT_ALIGN_MAX))
#define ARENA_NEW_N(a, T, n) ((T *)arena_alloc_zero((a), sizeof(T) * (size_t)(n), DOOT_ALIGN_MAX))

#endif /* DOOT_ARENA_H */
