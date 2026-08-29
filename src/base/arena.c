#include "arena.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assert.h"

#define ARENA_DEFAULT_CHUNK ((size_t)32u * 1024u)
#define ARENA_MAX_CHUNK ((size_t)1024u * 1024u)

typedef struct arena_chunk {
  struct arena_chunk *next;
  size_t cap;  /* usable bytes in data */
  size_t used; /* bytes handed out from data */
  char *data;
} arena_chunk;

struct arena {
  arena_chunk *current; /* chunk being allocated from */
  arena_chunk *spare;   /* chunks kept across arena_reset */
  size_t next_chunk;    /* capacity to request for the next chunk */
  size_t reserved;      /* total bytes held from the system */
  size_t peak;
  size_t limit; /* 0 = unlimited */
  bool fatal;
  bool exhausted;
};

/* Chunk headers are allocated together with their payload so that a chunk is a
 * single system allocation. */
static size_t header_size(void) {
  size_t s = sizeof(arena_chunk);
  size_t rem = s % DOOT_ALIGN_MAX;
  return rem == 0 ? s : s + (DOOT_ALIGN_MAX - rem);
}

static bool is_power_of_two(size_t v) {
  return v != 0 && (v & (v - 1)) == 0;
}

DOOT_NORETURN static void arena_die(const arena *a, size_t size) {
  DOOT_FATAL("out of memory: cannot allocate %lu bytes (reserved %lu, limit %lu)",
             (unsigned long)size, (unsigned long)a->reserved, (unsigned long)a->limit);
}

static arena *arena_create(size_t chunk_size, bool fatal) {
  arena *a = (arena *)calloc(1, sizeof(arena));
  if (a == NULL) {
    if (fatal) {
      DOOT_FATAL("out of memory: cannot allocate an arena");
    }
    return NULL;
  }
  a->next_chunk = chunk_size == 0 ? ARENA_DEFAULT_CHUNK : chunk_size;
  a->fatal = fatal;
  return a;
}

arena *arena_new(size_t chunk_size) {
  return arena_create(chunk_size, false);
}
arena *arena_new_fatal(size_t chunk_size) {
  return arena_create(chunk_size, true);
}

void arena_set_limit(arena *a, size_t max_bytes) {
  DOOT_ASSERT(a != NULL);
  a->limit = max_bytes;
}

static void chunk_list_free(arena_chunk *c) {
  while (c != NULL) {
    arena_chunk *next = c->next;
    free(c);
    c = next;
  }
}

void arena_destroy(arena *a) {
  if (a == NULL) {
    return;
  }
  chunk_list_free(a->current);
  chunk_list_free(a->spare);
  free(a);
}

/* Takes a chunk with at least `need` usable bytes, preferring the spare list. */
static bool arena_grow(arena *a, size_t need) {
  size_t cap = a->next_chunk;
  arena_chunk *prev = NULL;
  arena_chunk *c;
  size_t hdr;

  for (c = a->spare; c != NULL; prev = c, c = c->next) {
    if (c->cap >= need) {
      if (prev == NULL) {
        a->spare = c->next;
      } else {
        prev->next = c->next;
      }
      c->used = 0;
      c->next = a->current;
      a->current = c;
      return true;
    }
  }

  while (cap < need) {
    if (cap > ARENA_MAX_CHUNK / 2u) {
      cap = need;
      break;
    }
    cap *= 2u;
  }
  if (a->next_chunk < ARENA_MAX_CHUNK) {
    a->next_chunk = a->next_chunk * 2u > ARENA_MAX_CHUNK ? ARENA_MAX_CHUNK : a->next_chunk * 2u;
  }

  hdr = header_size();
  if (cap > SIZE_MAX - hdr) {
    a->exhausted = true;
    return false;
  }
  if (a->limit != 0 && a->reserved + hdr + cap > a->limit) {
    a->exhausted = true;
    return false;
  }

  /* Header and payload are one system allocation, with the payload starting at
   * a max-aligned offset past the header. */
  c = (arena_chunk *)malloc(hdr + cap);
  if (c == NULL) {
    a->exhausted = true;
    return false;
  }

  c->cap = cap;
  c->used = 0;
  c->data = (char *)c + hdr;
  c->next = a->current;
  a->current = c;

  a->reserved += hdr + cap;
  if (a->reserved > a->peak) {
    a->peak = a->reserved;
  }
  return true;
}

void *arena_alloc(arena *a, size_t size, size_t align) {
  size_t offset;
  size_t pad;
  char *p;

  DOOT_ASSERT(a != NULL);
  DOOT_ASSERTF(is_power_of_two(align), "alignment %lu is not a power of two", (unsigned long)align);

  if (size == 0) {
    size = 1; /* distinct, non-NULL pointers for zero-length objects */
  }

  if (a->current != NULL) {
    offset = (size_t)(uintptr_t)(a->current->data + a->current->used) & (align - 1u);
    pad = offset == 0 ? 0 : align - offset;
    if (pad <= a->current->cap - a->current->used &&
        size <= a->current->cap - a->current->used - pad) {
      p = a->current->data + a->current->used + pad;
      a->current->used += pad + size;
      return p;
    }
  }

  if (size > SIZE_MAX - align) {
    a->exhausted = true;
    if (a->fatal) {
      arena_die(a, size);
    }
    return NULL;
  }
  if (!arena_grow(a, size + align)) {
    if (a->fatal) {
      arena_die(a, size);
    }
    return NULL;
  }

  offset = (size_t)(uintptr_t)a->current->data & (align - 1u);
  pad = offset == 0 ? 0 : align - offset;
  DOOT_ASSERT(pad + size <= a->current->cap);
  p = a->current->data + pad;
  a->current->used = pad + size;
  return p;
}

void *arena_alloc_zero(arena *a, size_t size, size_t align) {
  void *p = arena_alloc(a, size, align);
  if (p != NULL) {
    memset(p, 0, size == 0 ? 1u : size);
  }
  return p;
}

void *arena_extend(arena *a, void *p, size_t old_size, size_t new_size, size_t align) {
  void *fresh;

  DOOT_ASSERT(a != NULL);
  DOOT_ASSERT(new_size >= old_size);

  if (p == NULL || old_size == 0) {
    return arena_alloc(a, new_size, align);
  }

  /* In place when p is the most recent allocation and the chunk has room. */
  if (a->current != NULL && (char *)p + old_size == a->current->data + a->current->used) {
    size_t extra = new_size - old_size;
    if (extra <= a->current->cap - a->current->used) {
      a->current->used += extra;
      return p;
    }
  }

  fresh = arena_alloc(a, new_size, align);
  if (fresh == NULL) {
    return NULL;
  }
  memcpy(fresh, p, old_size);
  return fresh;
}

char *arena_dup(arena *a, const void *src, size_t len) {
  char *p = (char *)arena_alloc(a, len + 1u, 1u);
  if (p == NULL) {
    return NULL;
  }
  if (len != 0) {
    memcpy(p, src, len);
  }
  p[len] = '\0';
  return p;
}

char *arena_vprintf(arena *a, const char *fmt, va_list *ap) {
  va_list probe;
  char *out;
  int n;

  va_copy(probe, *ap);
  n = vsnprintf(NULL, 0, fmt, probe);
  va_end(probe);
  if (n < 0) {
    DOOT_FATAL("vsnprintf failed formatting \"%s\"", fmt);
  }

  out = (char *)arena_alloc(a, (size_t)n + 1u, 1u);
  if (out == NULL) {
    return NULL;
  }
  (void)vsnprintf(out, (size_t)n + 1u, fmt, *ap);
  return out;
}

char *arena_printf(arena *a, const char *fmt, ...) {
  va_list ap;
  char *out;

  va_start(ap, fmt);
  out = arena_vprintf(a, fmt, &ap);
  va_end(ap);
  return out;
}

void arena_reset(arena *a) {
  arena_chunk *c;

  DOOT_ASSERT(a != NULL);
  c = a->current;
  while (c != NULL) {
    arena_chunk *next = c->next;
    c->used = 0;
    c->next = a->spare;
    a->spare = c;
    c = next;
  }
  a->current = NULL;
  a->exhausted = false;
}

arena_mark arena_save(const arena *a) {
  arena_mark m;

  DOOT_ASSERT(a != NULL);
  m.chunk = a->current;
  m.used = a->current == NULL ? 0 : a->current->used;
  return m;
}

void arena_rollback(arena *a, arena_mark m) {
  DOOT_ASSERT(a != NULL);

  /* Chunks allocated after the mark become spares. */
  while (a->current != m.chunk) {
    arena_chunk *c = a->current;
    DOOT_ASSERTF(c != NULL, "arena_rollback: mark does not belong to this arena");
    a->current = c->next;
    c->used = 0;
    c->next = a->spare;
    a->spare = c;
  }
  if (a->current != NULL) {
    DOOT_ASSERT(m.used <= a->current->used);
    a->current->used = m.used;
  }
}

size_t arena_used(const arena *a) {
  const arena_chunk *c;
  size_t total = 0;

  DOOT_ASSERT(a != NULL);
  for (c = a->current; c != NULL; c = c->next) {
    total += c->used;
  }
  return total;
}

size_t arena_reserved(const arena *a) {
  DOOT_ASSERT(a != NULL);
  return a->reserved;
}

size_t arena_peak(const arena *a) {
  DOOT_ASSERT(a != NULL);
  return a->peak;
}

bool arena_exhausted(const arena *a) {
  DOOT_ASSERT(a != NULL);
  return a->exhausted;
}
