#include "diag.h"

#include <stdarg.h>
#include <string.h>

#include "assert.h"

#define SNIPPET_TAB_WIDTH 4u

/* ---- registry ---------------------------------------------------------- */

typedef struct {
  const char *code;
  diag_severity severity;
  const char *brief;
  const char *explain;
} diag_info;

#define DOOT_DIAG_ROW(id, code, sev, brief, explain) {code, sev, brief, explain},
static const diag_info diag_table[DIAG_CODE_COUNT] = {DOOT_DIAG_CODES(DOOT_DIAG_ROW)};
#undef DOOT_DIAG_ROW

static const diag_info *info_for(diag_code code) {
  DOOT_ASSERTF(code >= 0 && code < DIAG_CODE_COUNT, "diagnostic code %d out of range", (int)code);
  return &diag_table[code];
}

const char *diag_code_str(diag_code code) {
  return info_for(code)->code;
}
diag_severity diag_code_severity(diag_code code) {
  return info_for(code)->severity;
}
const char *diag_code_brief(diag_code code) {
  return info_for(code)->brief;
}
const char *diag_code_explain(diag_code code) {
  return info_for(code)->explain;
}

bool diag_code_parse(slice text, diag_code *out) {
  int i;

  DOOT_ASSERT(out != NULL);
  for (i = 0; i < (int)DIAG_CODE_COUNT; i++) {
    if (slice_eq_cstr(text, diag_table[i].code)) {
      *out = (diag_code)i;
      return true;
    }
  }
  return false;
}

const char *diag_severity_str(diag_severity sev) {
  switch (sev) {
  case DIAG_ERROR:
    return "error";
  case DIAG_WARNING:
    return "warning";
  case DIAG_NOTE:
    return "note";
  case DIAG_HELP:
    return "help";
  }
  DOOT_UNREACHABLE();
}

/* ---- sink -------------------------------------------------------------- */

void diag_sink_init(diag_sink *s, arena *a, size_t limit) {
  DOOT_ASSERT(s != NULL && a != NULL);
  memset(s, 0, sizeof(*s));
  s->a = a;
  s->limit = limit == 0 ? DIAG_DEFAULT_LIMIT : limit;
}

diag *diag_report(diag_sink *s, diag_code code, const source *src, span at, const char *fmt, ...) {
  va_list ap;
  diag *d;
  char *msg;

  DOOT_ASSERT(s != NULL && s->a != NULL);

  if (s->count >= s->limit) {
    s->truncated = true;
    return NULL;
  }

  d = ARENA_NEW(s->a, diag);
  if (d == NULL) {
    return NULL;
  }

  va_start(ap, fmt);
  msg = arena_vprintf(s->a, fmt, &ap);
  va_end(ap);
  if (msg == NULL) {
    return NULL;
  }

  d->code = code;
  d->severity = diag_code_severity(code);
  d->src = src;
  d->at = at;
  d->message = slice_from_cstr(msg);

  if (s->last == NULL) {
    s->first = d;
  } else {
    s->last->next = d;
  }
  s->last = d;
  s->count++;

  switch (d->severity) {
  case DIAG_ERROR:
    s->errors++;
    break;
  case DIAG_WARNING:
    s->warnings++;
    break;
  case DIAG_NOTE:
  case DIAG_HELP:
    break;
  }
  return d;
}

void diag_fix(diag_sink *s, diag *d, span at, slice replacement) {
  char *copy;

  DOOT_ASSERT(s != NULL);
  if (d == NULL) {
    return;
  }
  copy = arena_dup(s->a, replacement.p, replacement.n);
  if (copy == NULL) {
    return;
  }
  d->has_fix = true;
  d->fix_at = at;
  d->fix_text = slice_make(copy, replacement.n);
}

void diag_label_add(diag_sink *s, diag *d, const source *src, span at, const char *fmt, ...) {
  va_list ap;
  diag_label *l;
  char *msg;

  DOOT_ASSERT(s != NULL);
  if (d == NULL) {
    return;
  }

  l = ARENA_NEW(s->a, diag_label);
  if (l == NULL) {
    return;
  }
  va_start(ap, fmt);
  msg = arena_vprintf(s->a, fmt, &ap);
  va_end(ap);
  if (msg == NULL) {
    return;
  }

  l->src = src;
  l->at = at;
  l->message = slice_from_cstr(msg);
  if (d->labels_tail == NULL) {
    d->labels = l;
  } else {
    d->labels_tail->next = l;
  }
  d->labels_tail = l;
}

bool diag_has_errors(const diag_sink *s) {
  DOOT_ASSERT(s != NULL);
  return s->errors != 0;
}

size_t diag_error_count(const diag_sink *s) {
  DOOT_ASSERT(s != NULL);
  return s->errors;
}

size_t diag_warning_count(const diag_sink *s) {
  DOOT_ASSERT(s != NULL);
  return s->warnings;
}

/* ---- human rendering --------------------------------------------------- */

static const char *color_for(diag_severity sev) {
  switch (sev) {
  case DIAG_ERROR:
    return "\033[1;31m";
  case DIAG_WARNING:
    return "\033[1;33m";
  case DIAG_NOTE:
    return "\033[1;36m";
  case DIAG_HELP:
    return "\033[1;32m";
  }
  DOOT_UNREACHABLE();
}

static size_t decimal_width(uint32_t v) {
  size_t w = 1;
  while (v >= 10u) {
    v /= 10u;
    w++;
  }
  return w;
}

/* Writes the line with tabs expanded, so the caret below it lines up. */
static void write_expanded(buf *out, slice line) {
  size_t i;
  size_t col = 0;

  for (i = 0; i < line.n; i++) {
    if (line.p[i] == '\t') {
      size_t pad = SNIPPET_TAB_WIDTH - (col % SNIPPET_TAB_WIDTH);
      (void)buf_append_repeat(out, ' ', pad);
      col += pad;
    } else {
      (void)buf_append_byte(out, line.p[i]);
      col++;
    }
  }
}

/* Display column of a byte offset within a line, tabs expanded, 0-based. */
static size_t display_col(slice line, size_t byte_off) {
  size_t i;
  size_t col = 0;

  for (i = 0; i < byte_off && i < line.n; i++) {
    if (line.p[i] == '\t') {
      col += SNIPPET_TAB_WIDTH - (col % SNIPPET_TAB_WIDTH);
    } else if (((unsigned char)line.p[i] & 0xc0u) != 0x80u) {
      col++;
    }
  }
  return col;
}

static void render_snippet(buf *out, const source *src, span at, size_t gutter, const char *accent,
                           const char *reset, slice label) {
  line_col lc = source_line_col(src, at.start);
  slice line = source_line(src, lc.line);
  uint32_t line_start = source_line_start(src, lc.line);
  size_t caret_start = display_col(line, (size_t)(at.start - line_start));
  size_t caret_end;
  size_t caret_len;
  uint32_t end = at.end > at.start ? at.end : at.start + 1u;

  if (end > line_start + (uint32_t)line.n) {
    end = line_start + (uint32_t)line.n;
  }
  caret_end = display_col(line, (size_t)(end > line_start ? end - line_start : 0));
  caret_len = caret_end > caret_start ? caret_end - caret_start : 1u;

  (void)buf_printf(out, "%*s |\n", (int)gutter, "");
  (void)buf_printf(out, "%*lu | ", (int)gutter, (unsigned long)lc.line);
  write_expanded(out, line);
  (void)buf_append_byte(out, '\n');
  (void)buf_printf(out, "%*s | ", (int)gutter, "");
  (void)buf_append_repeat(out, ' ', caret_start);
  (void)buf_append_cstr(out, accent);
  (void)buf_append_repeat(out, '^', caret_len);
  if (!slice_is_empty(label)) {
    (void)buf_append_byte(out, ' ');
    (void)buf_append_slice(out, label);
  }
  (void)buf_append_cstr(out, reset);
  (void)buf_append_byte(out, '\n');
}

void diag_render_human(const diag_sink *s, buf *out, bool color) {
  const diag *d;
  const char *bold = color ? "\033[1m" : "";
  const char *reset = color ? "\033[0m" : "";

  DOOT_ASSERT(s != NULL && out != NULL);

  for (d = s->first; d != NULL; d = d->next) {
    const char *accent = color ? color_for(d->severity) : "";
    size_t gutter = 2;
    const diag_label *l;

    if (d->src != NULL && !span_is_none(d->at)) {
      gutter = decimal_width(source_line_col(d->src, d->at.start).line);
    }

    (void)buf_printf(out, "%s%s[%s]%s%s: ", accent, diag_severity_str(d->severity),
                     diag_code_str(d->code), reset, bold);
    (void)buf_append_slice(out, d->message);
    (void)buf_printf(out, "%s\n", reset);

    if (d->src != NULL && !span_is_none(d->at)) {
      line_col lc = source_line_col(d->src, d->at.start);
      (void)buf_printf(out, "%*s--> ", (int)gutter, "");
      (void)buf_append_slice(out, source_path(d->src));
      (void)buf_printf(out, ":%lu:%lu\n", (unsigned long)lc.line, (unsigned long)lc.col);
      render_snippet(out, d->src, d->at, gutter, accent, reset, SLICE_EMPTY);
    }

    for (l = d->labels; l != NULL; l = l->next) {
      if (l->src != NULL && !span_is_none(l->at)) {
        line_col lc = source_line_col(l->src, l->at.start);
        (void)buf_printf(out, "%*s--> ", (int)gutter, "");
        (void)buf_append_slice(out, source_path(l->src));
        (void)buf_printf(out, ":%lu:%lu\n", (unsigned long)lc.line, (unsigned long)lc.col);
        render_snippet(out, l->src, l->at, gutter, color ? "\033[1;36m" : "", reset, l->message);
      } else {
        (void)buf_printf(out, "%*snote: ", (int)gutter, "");
        (void)buf_append_slice(out, l->message);
        (void)buf_append_byte(out, '\n');
      }
    }

    if (d->has_fix) {
      (void)buf_printf(out, "%*shelp: replace with `", (int)gutter, "");
      (void)buf_append_slice(out, d->fix_text);
      (void)buf_append_cstr(out, "`\n");
    }

    (void)buf_printf(out, "%*shelp: run `doot explain %s` for more\n\n", (int)gutter, "",
                     diag_code_str(d->code));
  }

  if (s->truncated) {
    (void)buf_printf(out, "note: stopped after %lu diagnostics\n", (unsigned long)s->limit);
  }
}

/* ---- json rendering ---------------------------------------------------- */

static void json_span(buf *out, const source *src, span at) {
  line_col lc = source_line_col(src, at.start);
  (void)buf_printf(out, "{\"start\":%lu,\"end\":%lu,\"line\":%lu,\"col\":%lu}",
                   (unsigned long)at.start, (unsigned long)at.end, (unsigned long)lc.line,
                   (unsigned long)lc.col);
}

static void json_string(buf *out, slice s) {
  (void)buf_append_byte(out, '"');
  (void)buf_append_json_escaped(out, s);
  (void)buf_append_byte(out, '"');
}

void diag_render_json(const diag_sink *s, buf *out) {
  const diag *d;
  bool first = true;

  DOOT_ASSERT(s != NULL && out != NULL);

  (void)buf_append_cstr(out, "{\"diagnostics\":[");
  for (d = s->first; d != NULL; d = d->next) {
    const diag_label *l;
    bool first_label = true;

    if (!first) {
      (void)buf_append_byte(out, ',');
    }
    first = false;

    (void)buf_printf(out,
                     "{\"code\":\"%s\",\"severity\":\"%s\",\"message\":", diag_code_str(d->code),
                     diag_severity_str(d->severity));
    json_string(out, d->message);

    if (d->src != NULL) {
      (void)buf_append_cstr(out, ",\"file\":");
      json_string(out, source_path(d->src));
      if (!span_is_none(d->at)) {
        (void)buf_append_cstr(out, ",\"span\":");
        json_span(out, d->src, d->at);
      }
    }

    if (d->has_fix && d->src != NULL) {
      (void)buf_append_cstr(out, ",\"suggestion\":{\"replace_span\":[");
      (void)buf_printf(out, "%lu,%lu],\"with\":", (unsigned long)d->fix_at.start,
                       (unsigned long)d->fix_at.end);
      json_string(out, d->fix_text);
      (void)buf_append_byte(out, '}');
    }

    if (d->labels != NULL) {
      (void)buf_append_cstr(out, ",\"related\":[");
      for (l = d->labels; l != NULL; l = l->next) {
        if (!first_label) {
          (void)buf_append_byte(out, ',');
        }
        first_label = false;
        (void)buf_append_byte(out, '{');
        if (l->src != NULL) {
          (void)buf_append_cstr(out, "\"file\":");
          json_string(out, source_path(l->src));
          if (!span_is_none(l->at)) {
            (void)buf_append_cstr(out, ",\"span\":");
            json_span(out, l->src, l->at);
          }
          (void)buf_append_byte(out, ',');
        }
        (void)buf_append_cstr(out, "\"message\":");
        json_string(out, l->message);
        (void)buf_append_byte(out, '}');
      }
      (void)buf_append_byte(out, ']');
    }

    (void)buf_append_byte(out, '}');
  }

  (void)buf_printf(out, "],\"summary\":{\"errors\":%lu,\"warnings\":%lu,\"truncated\":%s}}\n",
                   (unsigned long)s->errors, (unsigned long)s->warnings,
                   s->truncated ? "true" : "false");
}
