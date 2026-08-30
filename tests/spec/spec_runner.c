/* spec_runner.c -- the specification-test runner.
 *
 * Drives the real `doot` binary over every `.do` file under tests/spec and
 * compares its structured output against the expectations declared in each
 * file's leading comment block. Specified in docs/11-spec-tests.md; the
 * decisions are D070-D080.
 *
 * It links nothing from src/ (D070). Not a style preference: a test tool that
 * reads its own expectations through the implementation under test cannot fail
 * independently of it, and most of these files exist precisely because they fail
 * to lex, so their directives have to be readable without a lexer. The same
 * argument covers discovery and the JSON reader, so the directory walk, the
 * byte-offset-to-line/column mapping, and the JSON parser are all local.
 *
 * It fails closed (D071). An unknown directive, an unknown JSON key, a missing
 * mode, a file that asserts nothing, or any limit below being exceeded is a
 * failure -- never a silent pass. A spec test that quietly asserts nothing is
 * worse than a missing one, because it reports as coverage.
 *
 * POSIX is used for directory traversal and for the exit status of system(),
 * matching src/base/fs.c; Windows arrives with the rest of v0.5.
 */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Fixed caps, generous for a suite of small files. Exceeding one is a failure
 * with a specific message rather than a truncation, so the suite can never
 * silently test less than the file asked for. */
#define SPEC_MAX_PATH 1024
#define SPEC_MAX_LINE 4096
#define SPEC_MAX_MSG 2048
#define SPEC_MAX_EXPECT 64
#define SPEC_MAX_DIAGS 256
#define SPEC_MAX_FILES 4096
#define SPEC_ROOT "tests/spec"

/* Mirrors DOOT_NORETURN, DOOT_PRINTF, and DOOT_VPRINTF without including
 * src/base/plat.h, which D070 forbids.
 *
 * SPEC_NORETURN is load-bearing for more than codegen: without it a static
 * analyser assumes execution continues past a failed bounds check into the
 * memcpy it guards. SPEC_VPRINTF marks the format argument of a function taking a
 * va_list, which is what stops clang's -Wformat-nonliteral firing inside the
 * callee -- gcc accepts these without it, so omitting it breaks only the clang
 * half of the build matrix. */
#if defined(__GNUC__) || defined(__clang__)
#define SPEC_NORETURN __attribute__((noreturn))
#define SPEC_PRINTF(fmt_idx, first_arg) __attribute__((format(printf, fmt_idx, first_arg)))
#define SPEC_VPRINTF(fmt_idx) __attribute__((format(printf, fmt_idx, 0)))
#else
#define SPEC_NORETURN
#define SPEC_PRINTF(fmt_idx, first_arg)
#define SPEC_VPRINTF(fmt_idx)
#endif

/* ---- growable byte buffer ------------------------------------------------ */

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} sbuf;

static SPEC_NORETURN void die(const char *fmt, ...) SPEC_PRINTF(1, 2);

static void die(const char *fmt, ...) {
  va_list ap;

  (void)fputs("spec_runner: ", stderr);
  va_start(ap, fmt);
  (void)vfprintf(stderr, fmt, ap);
  va_end(ap);
  (void)fputc('\n', stderr);
  exit(2);
}

static void *xmalloc(size_t n) {
  void *p = malloc(n);

  if (p == NULL) {
    die("out of memory");
  }
  return p;
}

static void sbuf_init(sbuf *b) {
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

static void sbuf_free(sbuf *b) {
  free(b->data);
  sbuf_init(b);
}

static void sbuf_reserve(sbuf *b, size_t extra) {
  size_t want = b->len + extra + 1u;
  char *p;

  if (want <= b->cap) {
    return;
  }
  while (b->cap < want) {
    b->cap = b->cap == 0 ? 256u : b->cap * 2u;
  }
  p = realloc(b->data, b->cap);
  if (p == NULL) {
    die("out of memory");
  }
  b->data = p;
}

static void sbuf_add(sbuf *b, const char *s, size_t n) {
  sbuf_reserve(b, n);
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
}

static void sbuf_adds(sbuf *b, const char *s) {
  sbuf_add(b, s, strlen(s));
}

static void sbuf_addf(sbuf *b, const char *fmt, ...) SPEC_PRINTF(2, 3);

static void sbuf_addf(sbuf *b, const char *fmt, ...) {
  char line[SPEC_MAX_LINE];
  va_list ap;
  int n;

  va_start(ap, fmt);
  n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (n < 0) {
    die("vsnprintf failed");
  }
  sbuf_adds(b, line);
}

/* ---- files and paths ---------------------------------------------------- */

/* Reads the whole file as bytes. Spec files legitimately contain NUL bytes and
 * invalid UTF-8 -- two of them exist to produce DT0003 and DT0001 -- so nothing
 * here may treat the contents as a C string. */
static bool read_whole_file(const char *path, sbuf *out) {
  FILE *f = fopen(path, "rb");
  char chunk[8192];
  size_t n;

  sbuf_init(out);
  if (f == NULL) {
    return false;
  }
  while ((n = fread(chunk, 1u, sizeof(chunk), f)) > 0u) {
    sbuf_add(out, chunk, n);
  }
  /* A short read from an error rather than end-of-file would silently truncate
   * the contents, and the runner would then compare expectations against a file
   * it only partly read -- a false pass in the one direction that matters. */
  if (ferror(f) != 0) {
    (void)fclose(f);
    die("error reading %s", path);
  }
  (void)fclose(f);
  return true;
}

static bool write_whole_file(const char *path, const char *data, size_t len) {
  FILE *f = fopen(path, "wb");
  bool ok;

  if (f == NULL) {
    return false;
  }
  ok = len == 0u || fwrite(data, 1u, len, f) == len;
  return fclose(f) == 0 && ok;
}

static void path_join(char *out, size_t cap, const char *a, const char *b) {
  if ((size_t)snprintf(out, cap, "%s/%s", a, b) >= cap) {
    die("path too long: %s/%s", a, b);
  }
}

static bool is_dir(const char *path) {
  struct stat st;

  return stat(path, &st) == 0 && S_ISDIR(st.st_mode) != 0;
}

static void make_dir(const char *path) {
  if (mkdir(path, 0777) != 0 && !is_dir(path)) {
    die("cannot create directory %s", path);
  }
}

/* Depth-first unlink. The scratch tree is cleared at the start of a run rather
 * than the end, so a failing run leaves its artifacts behind to inspect. */
static void remove_tree(const char *path) {
  DIR *d = opendir(path);
  struct dirent *e;

  if (d == NULL) {
    return;
  }
  while ((e = readdir(d)) != NULL) {
    char child[SPEC_MAX_PATH];

    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
      continue;
    }
    path_join(child, sizeof(child), path, e->d_name);
    if (is_dir(child)) {
      remove_tree(child);
    } else {
      (void)remove(child);
    }
  }
  (void)closedir(d);
  (void)rmdir(path);
}

/* ---- discovery ---------------------------------------------------------- */

typedef struct {
  char *paths[SPEC_MAX_FILES];
  size_t count;
} spec_list;

static int cmp_str(const void *a, const void *b) {
  const char *const *x = (const char *const *)a;
  const char *const *y = (const char *const *)b;

  return strcmp(*x, *y);
}

static char *dup_str(const char *s) {
  size_t n = strlen(s) + 1u;
  char *p = xmalloc(n);

  memcpy(p, s, n);
  return p;
}

static bool ends_with_do(const char *name) {
  size_t n = strlen(name);

  return n > 3u && strcmp(name + n - 3u, ".do") == 0;
}

/* Files in a directory before its subdirectories, each group sorted by name.
 * That ordering is what makes the per-directory report contiguous, and it makes
 * a run reproducible: readdir order is whatever the filesystem chose. */
static void collect(const char *dir, spec_list *out) {
  DIR *d = opendir(dir);
  struct dirent *e;
  char *files[SPEC_MAX_FILES];
  char *subs[SPEC_MAX_FILES];
  size_t nfiles = 0;
  size_t nsubs = 0;
  size_t i;

  if (d == NULL) {
    die("cannot read directory %s", dir);
  }
  while ((e = readdir(d)) != NULL) {
    char child[SPEC_MAX_PATH];

    if (e->d_name[0] == '.') {
      continue;
    }
    path_join(child, sizeof(child), dir, e->d_name);
    if (is_dir(child)) {
      if (nsubs == SPEC_MAX_FILES) {
        die("too many directories under %s", dir);
      }
      subs[nsubs++] = dup_str(child);
    } else if (ends_with_do(e->d_name)) {
      if (nfiles == SPEC_MAX_FILES) {
        die("too many files in %s", dir);
      }
      files[nfiles++] = dup_str(child);
    }
  }
  (void)closedir(d);

  qsort((void *)files, nfiles, sizeof(files[0]), cmp_str);
  qsort((void *)subs, nsubs, sizeof(subs[0]), cmp_str);

  for (i = 0; i < nfiles; i++) {
    if (out->count == SPEC_MAX_FILES) {
      die("too many spec files");
    }
    out->paths[out->count++] = files[i];
  }
  for (i = 0; i < nsubs; i++) {
    collect(subs[i], out);
    free(subs[i]);
  }
}

/* ---- expectations ------------------------------------------------------- */

typedef enum { SPEC_MODE_CHECK, SPEC_MODE_RUN, SPEC_MODE_FMT, SPEC_MODE_ROUTES } spec_mode;

typedef struct {
  char code[16];
  bool warning;
  bool has_pos; /* false for a diagnostic that carries no span at all */
  unsigned long line;
  unsigned long col;
  char message[SPEC_MAX_MSG];
  bool matched;
} expect_diag;

typedef struct {
  unsigned long line;
  unsigned long col;
  unsigned long end_line;
  unsigned long end_col;
  char text[SPEC_MAX_MSG];
  bool matched;
} expect_sugg;

typedef struct {
  spec_mode mode;
  bool has_mode;
  bool expect_ok;
  bool fmt_stable;
  bool has_output;
  sbuf output;
  expect_diag diags[SPEC_MAX_EXPECT];
  size_t ndiags;
  expect_sugg suggs[SPEC_MAX_EXPECT];
  size_t nsuggs;
  size_t nfaults;
} expectations;

typedef struct {
  char code[16];
  char severity[16];
  bool has_pos;
  unsigned long line;
  unsigned long col;
  char message[SPEC_MAX_MSG];
  bool has_sugg;
  unsigned long sugg_start;
  unsigned long sugg_end;
  char sugg_text[SPEC_MAX_MSG];
  bool matched;
} actual_diag;

typedef struct {
  actual_diag diags[SPEC_MAX_DIAGS];
  size_t count;
} actual_set;

/* ---- byte offset -> line and character column ---------------------------- */

/* Mirrors source_line_col: lines are delimited by '\n' alone, and a column
 * counts characters rather than bytes by skipping UTF-8 continuation bytes
 * (D073). Ten duplicated lines, deliberately, for the reason in D070 -- and the
 * only place the runner reimplements compiler logic. */
static void offset_to_line_col(const char *text, size_t len, unsigned long off, unsigned long *line,
                               unsigned long *col) {
  size_t i;
  size_t start = 0;

  if ((size_t)off > len) {
    off = (unsigned long)len;
  }
  *line = 1u;
  for (i = 0; i < (size_t)off; i++) {
    if (text[i] == '\n') {
      (*line)++;
      start = i + 1u;
    }
  }
  *col = 1u;
  for (i = start; i < (size_t)off; i++) {
    if (((unsigned char)text[i] & 0xc0u) != 0x80u) {
      (*col)++;
    }
  }
}

/* ---- the JSON reader ---------------------------------------------------- */

/* Understands exactly the schema pinned in docs/06-tooling.md#diagnostics and
 * rejects everything else, unknown keys included (D066, D071). It is not the
 * `json` stdlib module and never will be: a suite that parsed its input with the
 * implementation under test could report success because of a bug in it. */
typedef struct {
  const char *start;
  const char *p;
  const char *end;
  char err[SPEC_MAX_MSG];
} jsonr;

static bool jfail(jsonr *r, const char *fmt, ...) SPEC_PRINTF(2, 3);

static bool jfail(jsonr *r, const char *fmt, ...) {
  va_list ap;

  if (r->err[0] == '\0') {
    va_start(ap, fmt);
    (void)vsnprintf(r->err, sizeof(r->err), fmt, ap);
    va_end(ap);
  }
  return false;
}

static void jskip_ws(jsonr *r) {
  while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r')) {
    r->p++;
  }
}

static bool jlit(jsonr *r, char c) {
  jskip_ws(r);
  if (r->p >= r->end || *r->p != c) {
    return jfail(r, "expected `%c` at byte %lu", c, (unsigned long)(r->p - r->start));
  }
  r->p++;
  return true;
}

static void utf8_encode(unsigned long cp, char *out, size_t *n) {
  if (cp < 0x80u) {
    out[(*n)++] = (char)cp;
  } else if (cp < 0x800u) {
    out[(*n)++] = (char)(0xc0u | (cp >> 6));
    out[(*n)++] = (char)(0x80u | (cp & 0x3fu));
  } else {
    out[(*n)++] = (char)(0xe0u | (cp >> 12));
    out[(*n)++] = (char)(0x80u | ((cp >> 6) & 0x3fu));
    out[(*n)++] = (char)(0x80u | (cp & 0x3fu));
  }
}

static bool jstring(jsonr *r, char *out, size_t cap) {
  size_t n = 0;

  if (!jlit(r, '"')) {
    return false;
  }
  while (r->p < r->end && *r->p != '"') {
    char c = *r->p++;

    if (n + 4u >= cap) {
      return jfail(r, "string exceeds %lu bytes", (unsigned long)cap);
    }
    if (c != '\\') {
      out[n++] = c;
      continue;
    }
    if (r->p >= r->end) {
      return jfail(r, "truncated escape");
    }
    c = *r->p++;
    switch (c) {
    case '"':
    case '\\':
    case '/':
      out[n++] = c;
      break;
    case 'n':
      out[n++] = '\n';
      break;
    case 'r':
      out[n++] = '\r';
      break;
    case 't':
      out[n++] = '\t';
      break;
    case 'b':
      out[n++] = '\b';
      break;
    case 'f':
      out[n++] = '\f';
      break;
    case 'u': {
      unsigned long cp = 0;
      int i;

      if (r->end - r->p < 4) {
        return jfail(r, "truncated \\u escape");
      }
      for (i = 0; i < 4; i++) {
        char h = *r->p++;
        unsigned long d;

        if (h >= '0' && h <= '9') {
          d = (unsigned long)(h - '0');
        } else if (h >= 'a' && h <= 'f') {
          d = (unsigned long)(h - 'a') + 10u;
        } else if (h >= 'A' && h <= 'F') {
          d = (unsigned long)(h - 'A') + 10u;
        } else {
          return jfail(r, "bad hex digit in \\u escape");
        }
        cp = cp * 16u + d;
      }
      if (cp >= 0xd800u && cp <= 0xdfffu) {
        return jfail(r, "surrogate in \\u escape");
      }
      utf8_encode(cp, out, &n);
      break;
    }
    default:
      return jfail(r, "unknown escape `\\%c`", c);
    }
  }
  if (!jlit(r, '"')) {
    return false;
  }
  out[n] = '\0';
  return true;
}

static bool juint(jsonr *r, unsigned long *out) {
  unsigned long v = 0;
  bool any = false;

  jskip_ws(r);
  while (r->p < r->end && *r->p >= '0' && *r->p <= '9') {
    v = v * 10u + (unsigned long)(*r->p++ - '0');
    any = true;
  }
  if (!any) {
    return jfail(r, "expected a number");
  }
  *out = v;
  return true;
}

static bool jbool(jsonr *r, bool *out) {
  jskip_ws(r);
  if ((size_t)(r->end - r->p) >= 4u && strncmp(r->p, "true", 4u) == 0) {
    r->p += 4;
    *out = true;
    return true;
  }
  if ((size_t)(r->end - r->p) >= 5u && strncmp(r->p, "false", 5u) == 0) {
    r->p += 5;
    *out = false;
    return true;
  }
  return jfail(r, "expected true or false");
}

static bool jkey(jsonr *r, char *out, size_t cap) {
  return jstring(r, out, cap) && jlit(r, ':');
}

/* `{"start":N,"end":N,"line":N,"col":N}` */
static bool jspan(jsonr *r, unsigned long *line, unsigned long *col) {
  bool first = true;

  if (!jlit(r, '{')) {
    return false;
  }
  for (;;) {
    char key[64];
    unsigned long v;

    jskip_ws(r);
    if (r->p < r->end && *r->p == '}') {
      break;
    }
    if (!first && !jlit(r, ',')) {
      return false;
    }
    first = false;
    if (!jkey(r, key, sizeof(key)) || !juint(r, &v)) {
      return false;
    }
    if (strcmp(key, "line") == 0) {
      *line = v;
    } else if (strcmp(key, "col") == 0) {
      *col = v;
    } else if (strcmp(key, "start") != 0 && strcmp(key, "end") != 0) {
      return jfail(r, "unknown key `%s` in span", key);
    }
  }
  return jlit(r, '}');
}

/* `{"replace_span":[N,N],"with":"..."}` */
static bool jsuggestion(jsonr *r, actual_diag *d) {
  bool first = true;

  if (!jlit(r, '{')) {
    return false;
  }
  for (;;) {
    char key[64];

    jskip_ws(r);
    if (r->p < r->end && *r->p == '}') {
      break;
    }
    if (!first && !jlit(r, ',')) {
      return false;
    }
    first = false;
    if (!jkey(r, key, sizeof(key))) {
      return false;
    }
    if (strcmp(key, "replace_span") == 0) {
      if (!jlit(r, '[') || !juint(r, &d->sugg_start) || !jlit(r, ',') || !juint(r, &d->sugg_end) ||
          !jlit(r, ']')) {
        return false;
      }
    } else if (strcmp(key, "with") == 0) {
      if (!jstring(r, d->sugg_text, sizeof(d->sugg_text))) {
        return false;
      }
    } else {
      return jfail(r, "unknown key `%s` in suggestion", key);
    }
  }
  d->has_sugg = true;
  return jlit(r, '}');
}

/* Related spans are read and validated, then discarded: no directive asserts on
 * them yet, and accepting them loosely would defeat the unknown-key rule. */
static bool jrelated(jsonr *r) {
  bool first_item = true;

  if (!jlit(r, '[')) {
    return false;
  }
  for (;;) {
    bool first = true;

    jskip_ws(r);
    if (r->p < r->end && *r->p == ']') {
      break;
    }
    if (!first_item && !jlit(r, ',')) {
      return false;
    }
    first_item = false;
    if (!jlit(r, '{')) {
      return false;
    }
    for (;;) {
      char key[64];

      jskip_ws(r);
      if (r->p < r->end && *r->p == '}') {
        break;
      }
      if (!first && !jlit(r, ',')) {
        return false;
      }
      first = false;
      if (!jkey(r, key, sizeof(key))) {
        return false;
      }
      if (strcmp(key, "span") == 0) {
        unsigned long l = 0;
        unsigned long c = 0;

        if (!jspan(r, &l, &c)) {
          return false;
        }
      } else if (strcmp(key, "file") == 0 || strcmp(key, "message") == 0) {
        char tmp[SPEC_MAX_MSG];

        if (!jstring(r, tmp, sizeof(tmp))) {
          return false;
        }
      } else {
        return jfail(r, "unknown key `%s` in related", key);
      }
    }
    if (!jlit(r, '}')) {
      return false;
    }
  }
  return jlit(r, ']');
}

static bool jdiagnostic(jsonr *r, actual_diag *d) {
  bool first = true;

  memset(d, 0, sizeof(*d));
  if (!jlit(r, '{')) {
    return false;
  }
  for (;;) {
    char key[64];

    jskip_ws(r);
    if (r->p < r->end && *r->p == '}') {
      break;
    }
    if (!first && !jlit(r, ',')) {
      return false;
    }
    first = false;
    if (!jkey(r, key, sizeof(key))) {
      return false;
    }
    if (strcmp(key, "code") == 0) {
      if (!jstring(r, d->code, sizeof(d->code))) {
        return false;
      }
    } else if (strcmp(key, "severity") == 0) {
      if (!jstring(r, d->severity, sizeof(d->severity))) {
        return false;
      }
    } else if (strcmp(key, "message") == 0) {
      if (!jstring(r, d->message, sizeof(d->message))) {
        return false;
      }
    } else if (strcmp(key, "file") == 0) {
      char tmp[SPEC_MAX_PATH];

      if (!jstring(r, tmp, sizeof(tmp))) {
        return false;
      }
    } else if (strcmp(key, "span") == 0) {
      if (!jspan(r, &d->line, &d->col)) {
        return false;
      }
      d->has_pos = true;
    } else if (strcmp(key, "suggestion") == 0) {
      if (!jsuggestion(r, d)) {
        return false;
      }
    } else if (strcmp(key, "related") == 0) {
      if (!jrelated(r)) {
        return false;
      }
    } else {
      return jfail(r, "unknown key `%s` in diagnostic", key);
    }
  }
  return jlit(r, '}');
}

static bool jsummary(jsonr *r) {
  bool first = true;

  if (!jlit(r, '{')) {
    return false;
  }
  for (;;) {
    char key[64];

    jskip_ws(r);
    if (r->p < r->end && *r->p == '}') {
      break;
    }
    if (!first && !jlit(r, ',')) {
      return false;
    }
    first = false;
    if (!jkey(r, key, sizeof(key))) {
      return false;
    }
    if (strcmp(key, "errors") == 0 || strcmp(key, "warnings") == 0) {
      unsigned long v;

      if (!juint(r, &v)) {
        return false;
      }
    } else if (strcmp(key, "truncated") == 0) {
      bool b;

      if (!jbool(r, &b)) {
        return false;
      }
    } else {
      return jfail(r, "unknown key `%s` in summary", key);
    }
  }
  return jlit(r, '}');
}

static bool json_parse(const char *text, size_t len, actual_set *out, char *err, size_t errcap) {
  jsonr r;
  bool first = true;

  r.start = text;
  r.p = text;
  r.end = text + len;
  r.err[0] = '\0';
  out->count = 0;

  if (!jlit(&r, '{')) {
    goto fail;
  }
  for (;;) {
    char key[64];

    jskip_ws(&r);
    if (r.p < r.end && *r.p == '}') {
      break;
    }
    if (!first && !jlit(&r, ',')) {
      goto fail;
    }
    first = false;
    if (!jkey(&r, key, sizeof(key))) {
      goto fail;
    }
    if (strcmp(key, "diagnostics") == 0) {
      bool first_d = true;

      if (!jlit(&r, '[')) {
        goto fail;
      }
      for (;;) {
        jskip_ws(&r);
        if (r.p < r.end && *r.p == ']') {
          break;
        }
        if (!first_d && !jlit(&r, ',')) {
          goto fail;
        }
        first_d = false;
        if (out->count == SPEC_MAX_DIAGS) {
          (void)jfail(&r, "more than %d diagnostics", SPEC_MAX_DIAGS);
          goto fail;
        }
        if (!jdiagnostic(&r, &out->diags[out->count])) {
          goto fail;
        }
        out->count++;
      }
      if (!jlit(&r, ']')) {
        goto fail;
      }
    } else if (strcmp(key, "summary") == 0) {
      if (!jsummary(&r)) {
        goto fail;
      }
    } else {
      (void)jfail(&r, "unknown top-level key `%s`", key);
      goto fail;
    }
  }
  if (!jlit(&r, '}')) {
    goto fail;
  }
  jskip_ws(&r);
  if (r.p != r.end) {
    (void)jfail(&r, "trailing bytes after the JSON document");
    goto fail;
  }
  return true;

fail:
  (void)snprintf(err, errcap, "%s", r.err[0] == '\0' ? "malformed JSON" : r.err);
  return false;
}

/* ---- the directive block ------------------------------------------------ */

/* Reads a `"..."`-quoted argument. `\"` and `\\` are the only escapes; nothing
 * else is interpreted, so a message containing a backslash sequence the compiler
 * emits literally stays literal here. */
static bool read_quoted(const char **pp, char *out, size_t cap, sbuf *errs, const char *what) {
  const char *p = *pp;
  size_t n = 0;

  while (*p == ' ') {
    p++;
  }
  if (*p != '"') {
    sbuf_addf(errs, "    %s must be quoted\n", what);
    return false;
  }
  p++;
  while (*p != '\0' && *p != '"') {
    char c = *p++;

    if (c == '\\' && (*p == '"' || *p == '\\')) {
      c = *p++;
    }
    if (n + 1u >= cap) {
      sbuf_addf(errs, "    %s exceeds %lu bytes\n", what, (unsigned long)cap);
      return false;
    }
    out[n++] = c;
  }
  if (*p != '"') {
    sbuf_addf(errs, "    %s is missing its closing quote\n", what);
    return false;
  }
  out[n] = '\0';
  *pp = p + 1;
  return true;
}

static bool read_pos(const char **pp, unsigned long *line, unsigned long *col, sbuf *errs) {
  const char *p = *pp;
  char *e;

  while (*p == ' ') {
    p++;
  }
  *line = strtoul(p, &e, 10);
  if (e == p || *e != ':') {
    sbuf_adds(errs, "    expected <line>:<col>\n");
    return false;
  }
  p = e + 1;
  *col = strtoul(p, &e, 10);
  if (e == p) {
    sbuf_adds(errs, "    expected <line>:<col>\n");
    return false;
  }
  *pp = e;
  return true;
}

static bool starts_with(const char *s, const char *prefix, const char **rest) {
  size_t n = strlen(prefix);

  if (strncmp(s, prefix, n) != 0) {
    return false;
  }
  *rest = s + n;
  return true;
}

/* The block is the maximal run of lines from line 1 that begin with `//`. The
 * first line that does not ends it, blank lines included. */
static bool parse_directives(const char *text, size_t len, expectations *x, sbuf *errs) {
  size_t pos = 0;
  bool in_output = false;
  bool any_assertion = false;

  memset(x, 0, sizeof(*x));
  sbuf_init(&x->output);

  while (pos < len) {
    char line[SPEC_MAX_LINE];
    size_t eol = pos;
    size_t n;
    /* Both are set by starts_with only when it matches, and only read inside the
     * branch that matched. Initialised anyway: a conditionally-set pointer costs
     * nothing to make definite and it is one less thing for a reader, or an
     * analyser, to have to prove. */
    const char *rest = NULL;
    const char *body = NULL;

    while (eol < len && text[eol] != '\n') {
      eol++;
    }
    n = eol - pos;
    if (n > 0u && text[pos + n - 1u] == '\r') {
      n--; /* one trailing CR, so an accidental CRLF checkout fails clearly */
    }
    if (n >= sizeof(line)) {
      sbuf_adds(errs, "    directive line is too long\n");
      return false;
    }
    memcpy(line, text + pos, n);
    line[n] = '\0';

    if (strncmp(line, "//", 2u) != 0) {
      break;
    }
    pos = eol + 1u;

    if (in_output) {
      /* Everything after `expect-output:` is literal stdout. `//` alone is an
       * empty line; `// x` is `x`. */
      if (strcmp(line, "//") == 0) {
        sbuf_adds(&x->output, "\n");
      } else if (starts_with(line, "// ", &body)) {
        sbuf_adds(&x->output, body);
        sbuf_adds(&x->output, "\n");
      } else {
        sbuf_addf(errs, "    expected `// ` before expected output, got `%s`\n", line);
        return false;
      }
      continue;
    }

    if (!starts_with(line, "// ", &body)) {
      sbuf_addf(errs, "    a directive needs exactly one space after `//`: `%s`\n", line);
      return false;
    }

    if (starts_with(body, "doot-spec:", &rest)) {
      while (*rest == ' ') {
        rest++;
      }
      if (x->has_mode) {
        sbuf_adds(errs, "    duplicate `doot-spec:`\n");
        return false;
      }
      if (strcmp(rest, "check") == 0) {
        x->mode = SPEC_MODE_CHECK;
      } else if (strcmp(rest, "run") == 0) {
        x->mode = SPEC_MODE_RUN;
      } else if (strcmp(rest, "fmt") == 0) {
        x->mode = SPEC_MODE_FMT;
      } else if (strcmp(rest, "routes") == 0) {
        x->mode = SPEC_MODE_ROUTES;
      } else {
        sbuf_addf(errs, "    unknown mode `%s`\n", rest);
        return false;
      }
      x->has_mode = true;
    } else if (strcmp(body, "expect-ok") == 0) {
      x->expect_ok = true;
      any_assertion = true;
    } else if (strcmp(body, "expect-fmt-stable") == 0) {
      x->fmt_stable = true;
      any_assertion = true;
    } else if (strcmp(body, "expect-output:") == 0) {
      x->has_output = true;
      any_assertion = true;
      in_output = true;
    } else if (starts_with(body, "expect-error:", &rest) ||
               starts_with(body, "expect-warning:", &rest)) {
      bool warn = body[7] == 'w';
      expect_diag *d;

      if (x->ndiags == SPEC_MAX_EXPECT) {
        sbuf_adds(errs, "    too many expected diagnostics\n");
        return false;
      }
      d = &x->diags[x->ndiags];
      memset(d, 0, sizeof(*d));
      d->warning = warn;
      while (*rest == ' ') {
        rest++;
      }
      if (strncmp(rest, "DT", 2u) != 0 || strlen(rest) < 6u) {
        sbuf_adds(errs, "    expected a DTnnnn code\n");
        return false;
      }
      memcpy(d->code, rest, 6u);
      d->code[6] = '\0';
      rest += 6;
      while (*rest == ' ') {
        rest++;
      }
      /* `at <line>:<col>` is omitted for a diagnostic that carries no span. The
       * source-intake codes are reported before the source object exists, so
       * there is no line index to resolve an offset against and none of them has
       * a position to assert -- the offset is in the message instead. Requiring
       * `at` would make those three codes inexpressible; inventing `0:0` for them
       * would assert a position that does not exist. */
      if (starts_with(rest, "at", &rest)) {
        if (!read_pos(&rest, &d->line, &d->col, errs)) {
          return false;
        }
        d->has_pos = true;
      }
      if (!read_quoted(&rest, d->message, sizeof(d->message), errs, "the message")) {
        return false;
      }
      x->ndiags++;
      any_assertion = true;
    } else if (starts_with(body, "expect-suggestion:", &rest)) {
      expect_sugg *s;

      if (x->nsuggs == SPEC_MAX_EXPECT) {
        sbuf_adds(errs, "    too many expected suggestions\n");
        return false;
      }
      s = &x->suggs[x->nsuggs];
      memset(s, 0, sizeof(*s));
      if (!read_pos(&rest, &s->line, &s->col, errs)) {
        return false;
      }
      if (*rest != '-') {
        sbuf_adds(errs, "    expected <line>:<col>-<line>:<col>\n");
        return false;
      }
      rest++;
      if (!read_pos(&rest, &s->end_line, &s->end_col, errs)) {
        return false;
      }
      while (*rest == ' ') {
        rest++;
      }
      if (!starts_with(rest, "->", &rest)) {
        sbuf_adds(errs, "    expected `->` before the replacement\n");
        return false;
      }
      if (!read_quoted(&rest, s->text, sizeof(s->text), errs, "the replacement")) {
        return false;
      }
      x->nsuggs++;
      any_assertion = true;
    } else if (starts_with(body, "expect-fault:", &rest)) {
      x->nfaults++;
      any_assertion = true;
    } else {
      sbuf_addf(errs, "    unknown directive `%s`\n", body);
      return false;
    }
  }

  if (!x->has_mode) {
    sbuf_adds(errs, "    no `// doot-spec: <mode>` directive\n");
    return false;
  }
  if (!any_assertion) {
    sbuf_adds(errs, "    the file asserts nothing\n");
    return false;
  }
  if (x->expect_ok && x->ndiags != 0u) {
    sbuf_adds(errs, "    `expect-ok` cannot be combined with an expected diagnostic\n");
    return false;
  }
  /* A file that did not parse is left byte for byte alone, so it would satisfy
   * `expect-fmt-stable` while proving nothing. `expect-ok` is what establishes
   * that the printer ran (D074). */
  if (x->fmt_stable && !x->expect_ok) {
    sbuf_adds(errs, "    `expect-fmt-stable` requires `expect-ok`\n");
    return false;
  }
  if (x->fmt_stable && x->mode != SPEC_MODE_FMT) {
    sbuf_adds(errs, "    `expect-fmt-stable` requires `doot-spec: fmt`\n");
    return false;
  }
  return true;
}

/* ---- invoking the binary under test ------------------------------------- */

static const char *mode_name(spec_mode m) {
  switch (m) {
  case SPEC_MODE_CHECK:
    return "check";
  case SPEC_MODE_RUN:
    return "run";
  case SPEC_MODE_FMT:
    return "fmt";
  case SPEC_MODE_ROUTES:
    return "routes";
  }
  return "?";
}

/* Rejected rather than escaped: the suite controls every path it passes, so a
 * shell metacharacter means something is wrong, not that quoting is needed. */
static void check_shell_safe(const char *path) {
  if (strpbrk(path, "\"\\$`'") != NULL) {
    die("path contains a shell metacharacter: %s", path);
  }
}

/* Runs `<doot> <mode> [--json] <file>` with the working directory set to the
 * file's own directory, so every reported path is a bare basename and the output
 * is identical across build profiles. */
static int run_doot(const char *doot, const char *dir, const char *base, spec_mode mode, bool json,
                    const char *out_path, const char *err_path) {
  char cmd[SPEC_MAX_PATH * 4];
  int rc;

  if ((size_t)snprintf(cmd, sizeof(cmd), "cd \"%s\" && \"%s\" %s %s\"%s\" > \"%s\" 2> \"%s\"", dir,
                       doot, mode_name(mode), json ? "--json " : "", base, out_path,
                       err_path) >= sizeof(cmd)) {
    die("command line too long for %s/%s", dir, base);
  }
  rc = system(cmd);
  if (rc == -1) {
    die("cannot run a shell");
  }
  if (WIFEXITED(rc) == 0) {
    die("`doot %s` did not exit normally", mode_name(mode));
  }
  return WEXITSTATUS(rc);
}

/* ---- comparison --------------------------------------------------------- */

static void report_expected(sbuf *errs, const expect_diag *d) {
  if (d->has_pos) {
    sbuf_addf(errs, "    missing    %s at %lu:%lu \"%s\"\n", d->code, d->line, d->col, d->message);
  } else {
    sbuf_addf(errs, "    missing    %s \"%s\"\n", d->code, d->message);
  }
}

static void report_actual(sbuf *errs, const actual_diag *d) {
  if (d->has_pos) {
    sbuf_addf(errs, "    unexpected %s at %lu:%lu \"%s\"\n", d->code, d->line, d->col, d->message);
  } else {
    sbuf_addf(errs, "    unexpected %s \"%s\"\n", d->code, d->message);
  }
}

/* Exact multiset in both directions (D072). Order is not asserted: it belongs to
 * diag_sink and is pinned by unit tests, and mirroring it here would make every
 * file brittle to a legitimate reordering of two independent checks. */
static bool compare_diags(expectations *x, actual_set *got, sbuf *errs) {
  size_t i;
  size_t j;
  bool ok = true;

  for (i = 0; i < x->ndiags; i++) {
    expect_diag *e = &x->diags[i];
    const char *want_sev = e->warning ? "warning" : "error";

    for (j = 0; j < got->count; j++) {
      actual_diag *a = &got->diags[j];

      if (!a->matched && strcmp(a->code, e->code) == 0 && strcmp(a->severity, want_sev) == 0 &&
          a->has_pos == e->has_pos && a->line == e->line && a->col == e->col &&
          strcmp(a->message, e->message) == 0) {
        a->matched = true;
        e->matched = true;
        break;
      }
    }
    if (!e->matched) {
      report_expected(errs, e);
      ok = false;
    }
  }
  for (j = 0; j < got->count; j++) {
    if (!got->diags[j].matched) {
      report_actual(errs, &got->diags[j]);
      ok = false;
    }
  }
  return ok;
}

static bool compare_suggs(expectations *x, actual_set *got, const char *text, size_t len,
                          sbuf *errs) {
  size_t i;
  size_t j;
  bool ok = true;
  size_t n_actual = 0;

  for (i = 0; i < x->nsuggs; i++) {
    expect_sugg *e = &x->suggs[i];

    for (j = 0; j < got->count; j++) {
      actual_diag *a = &got->diags[j];
      unsigned long sl;
      unsigned long sc;
      unsigned long el;
      unsigned long ec;

      /* Matched independently of which diagnostic carried the suggestion, so
       * the diagnostic's own `matched` flag is deliberately not consulted. */
      if (!a->has_sugg) {
        continue;
      }
      offset_to_line_col(text, len, a->sugg_start, &sl, &sc);
      offset_to_line_col(text, len, a->sugg_end, &el, &ec);
      if (sl == e->line && sc == e->col && el == e->end_line && ec == e->end_col &&
          strcmp(a->sugg_text, e->text) == 0) {
        e->matched = true;
        break;
      }
    }
    if (!e->matched) {
      sbuf_addf(errs, "    missing   suggestion %lu:%lu-%lu:%lu -> \"%s\"\n", e->line, e->col,
                e->end_line, e->end_col, e->text);
      ok = false;
    }
  }
  for (j = 0; j < got->count; j++) {
    if (got->diags[j].has_sugg) {
      n_actual++;
    }
  }
  if (n_actual != x->nsuggs) {
    sbuf_addf(errs, "    %lu suggestion(s) reported, %lu expected\n", (unsigned long)n_actual,
              (unsigned long)x->nsuggs);
    ok = false;
  }
  return ok;
}

/* ---- running one file --------------------------------------------------- */

typedef struct {
  const char *doot;  /* absolute path to the binary under test */
  const char *stage; /* scratch root, build/<profile>/spec-tmp */
} runner;

static void mangle(const char *path, char *out, size_t cap) {
  size_t i;
  size_t n = strlen(path);

  if (n + 1u > cap) {
    die("path too long to stage: %s", path);
  }
  for (i = 0; i < n; i++) {
    out[i] = path[i] == '/' ? '_' : path[i];
  }
  out[n] = '\0';
}

static bool run_one(const runner *r, const char *path, sbuf *errs) {
  sbuf source;
  sbuf produced;
  expectations x;
  actual_set got;
  char stage_dir[SPEC_MAX_PATH];
  char mangled[SPEC_MAX_PATH];
  char staged[SPEC_MAX_PATH];
  char json_path[SPEC_MAX_PATH];
  char out_path[SPEC_MAX_PATH];
  char jerr[SPEC_MAX_MSG];
  const char *base;
  int status;
  bool ok = true;

  if (!read_whole_file(path, &source)) {
    sbuf_addf(errs, "    cannot read %s\n", path);
    return false;
  }
  if (!parse_directives(source.data == NULL ? "" : source.data, source.len, &x, errs)) {
    sbuf_free(&x.output);
    sbuf_free(&source);
    return false;
  }

  mangle(path, mangled, sizeof(mangled));
  path_join(stage_dir, sizeof(stage_dir), r->stage, mangled);
  make_dir(stage_dir);

  base = strrchr(path, '/');
  base = base == NULL ? path : base + 1;
  path_join(staged, sizeof(staged), stage_dir, base);
  check_shell_safe(staged);
  if (!write_whole_file(staged, source.data, source.len)) {
    die("cannot stage %s", staged);
  }
  path_join(json_path, sizeof(json_path), stage_dir, "out.json");
  path_join(out_path, sizeof(out_path), stage_dir, "stdout.txt");

  status = run_doot(r->doot, stage_dir, base, x.mode, true, "out.json", "stderr.txt");
  if (status == 2) {
    sbuf_addf(errs, "    `doot %s` exited 2: the command does not exist in this build\n",
              mode_name(x.mode));
    sbuf_free(&source);
    sbuf_free(&x.output);
    return false;
  }

  if (!read_whole_file(json_path, &produced)) {
    die("cannot read %s", json_path);
  }
  if (!json_parse(produced.data == NULL ? "" : produced.data, produced.len, &got, jerr,
                  sizeof(jerr))) {
    sbuf_addf(errs, "    unreadable --json output: %s\n", jerr);
    ok = false;
  } else {
    if (!compare_diags(&x, &got, errs)) {
      ok = false;
    }
    if (!compare_suggs(&x, &got, source.data == NULL ? "" : source.data, source.len, errs)) {
      ok = false;
    }
    if (x.expect_ok && got.count != 0u) {
      sbuf_addf(errs, "    expected no diagnostics, got %lu\n", (unsigned long)got.count);
      ok = false;
    }
    /* Asserted only where the directives establish a basis for it: `expect-ok`
     * means a clean run, an expected diagnostic means a reported one. A file that
     * pins only stdout says nothing about the exit code, and inventing an
     * expectation for it would be the runner asserting on its own authority. */
    if (x.expect_ok && status != 0) {
      sbuf_addf(errs, "    exit code %d, expected 0\n", status);
      ok = false;
    } else if (x.ndiags != 0u && status != 1) {
      sbuf_addf(errs, "    exit code %d, expected 1\n", status);
      ok = false;
    }
  }

  if (x.fmt_stable) {
    sbuf after;

    if (!read_whole_file(staged, &after)) {
      die("cannot read %s", staged);
    }
    if (after.len != source.len ||
        (source.len != 0u && memcmp(after.data, source.data, source.len) != 0)) {
      sbuf_adds(errs, "    `doot fmt` changed the file; it is not canonical\n");
      ok = false;
    }
    sbuf_free(&after);
  }

  if (x.has_output) {
    sbuf actual_out;

    /* A second invocation, without --json, because --json occupies stdout
     * (D075). Re-stage first: the previous run may have rewritten the copy. */
    if (!write_whole_file(staged, source.data, source.len)) {
      die("cannot stage %s", staged);
    }
    (void)run_doot(r->doot, stage_dir, base, x.mode, false, "stdout.txt", "stderr.txt");
    if (!read_whole_file(out_path, &actual_out)) {
      die("cannot read %s", out_path);
    }
    if (actual_out.len != x.output.len ||
        (x.output.len != 0u && memcmp(actual_out.data, x.output.data, x.output.len) != 0)) {
      sbuf_adds(errs, "    stdout differs\n      expected:\n");
      sbuf_addf(errs, "        %s", x.output.len == 0u ? "(empty)\n" : x.output.data);
      sbuf_adds(errs, "      got:\n");
      sbuf_addf(errs, "        %s", actual_out.len == 0u ? "(empty)\n" : actual_out.data);
      ok = false;
    }
    sbuf_free(&actual_out);
  }

  sbuf_free(&produced);
  sbuf_free(&source);
  sbuf_free(&x.output);
  return ok;
}

/* ---- main --------------------------------------------------------------- */

static void dirname_of(const char *path, char *out, size_t cap) {
  const char *slash = strrchr(path, '/');
  size_t n = slash == NULL ? 0u : (size_t)(slash - path);

  if (n + 1u > cap) {
    die("path too long: %s", path);
  }
  memcpy(out, path, n);
  out[n] = '\0';
}

int main(int argc, char **argv) {
  runner r;
  spec_list files;
  char doot_abs[SPEC_MAX_PATH];
  char stage[SPEC_MAX_PATH];
  char group[SPEC_MAX_PATH] = "";
  const char *filter = NULL;
  size_t i;
  size_t total = 0;
  size_t failed = 0;
  size_t group_ran = 0;
  size_t group_failed = 0;
  sbuf group_errs;

  if (argc < 2) {
    (void)fprintf(stderr, "usage: doot_spec <path-to-doot> [filter]\n");
    return 2;
  }
  if (argc > 2) {
    filter = argv[2];
  }

  if (argv[1][0] == '/') {
    if ((size_t)snprintf(doot_abs, sizeof(doot_abs), "%s", argv[1]) >= sizeof(doot_abs)) {
      die("path too long: %s", argv[1]);
    }
  } else {
    char cwd[SPEC_MAX_PATH];

    if (getcwd(cwd, sizeof(cwd)) == NULL) {
      die("cannot determine the working directory");
    }
    if ((size_t)snprintf(doot_abs, sizeof(doot_abs), "%s/%s", cwd, argv[1]) >= sizeof(doot_abs)) {
      die("path too long: %s/%s", cwd, argv[1]);
    }
  }
  check_shell_safe(doot_abs);

  /* build/<profile>/spec-tmp, derived from the binary's own directory so that
   * which profile is under test needs no second argument. Built into a second
   * buffer rather than appended in place: appending needed `strlen(stage)` both
   * as the offset and in the capacity check, and snprintf changes it between the
   * two. */
  {
    char parent[SPEC_MAX_PATH];

    dirname_of(doot_abs, parent, sizeof(parent));
    path_join(stage, sizeof(stage), parent, "spec-tmp");
  }
  remove_tree(stage);
  make_dir(stage);
  check_shell_safe(stage);

  if (!is_dir(SPEC_ROOT)) {
    die("%s does not exist; run from the repository root", SPEC_ROOT);
  }

  r.doot = doot_abs;
  r.stage = stage;
  files.count = 0;
  collect(SPEC_ROOT, &files);

  sbuf_init(&group_errs);
  for (i = 0; i < files.count; i++) {
    const char *path = files.paths[i];
    char dir[SPEC_MAX_PATH];
    sbuf errs;
    bool ok;

    if (filter != NULL && strstr(path, filter) == NULL) {
      continue;
    }
    dirname_of(path, dir, sizeof(dir));
    if (strcmp(dir, group) != 0) {
      if (group_ran != 0u) {
        (void)printf("%-4s %-22s %lu tests\n", group_failed == 0u ? "ok" : "FAIL", group,
                     (unsigned long)group_ran);
        (void)fputs(group_errs.len == 0u ? "" : group_errs.data, stdout);
      }
      sbuf_free(&group_errs);
      sbuf_init(&group_errs);
      (void)snprintf(group, sizeof(group), "%s", dir);
      group_ran = 0;
      group_failed = 0;
    }

    sbuf_init(&errs);
    ok = run_one(&r, path, &errs);
    group_ran++;
    total++;
    if (!ok) {
      group_failed++;
      failed++;
      sbuf_addf(&group_errs, "  FAIL %s\n", path);
      sbuf_adds(&group_errs, errs.len == 0u ? "    (no detail)\n" : errs.data);
    }
    sbuf_free(&errs);
  }
  if (group_ran != 0u) {
    (void)printf("%-4s %-22s %lu tests\n", group_failed == 0u ? "ok" : "FAIL", group,
                 (unsigned long)group_ran);
    (void)fputs(group_errs.len == 0u ? "" : group_errs.data, stdout);
  }
  sbuf_free(&group_errs);

  for (i = 0; i < files.count; i++) {
    free(files.paths[i]);
  }

  if (total == 0u) {
    (void)printf("no spec tests matched%s%s\n", filter == NULL ? "" : " filter ",
                 filter == NULL ? "" : filter);
    return 1;
  }
  (void)printf("\n%lu spec tests, %lu failed\n", (unsigned long)total, (unsigned long)failed);
  return failed == 0u ? 0 : 1;
}
