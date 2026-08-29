/* diag.h -- diagnostics.
 *
 * Diagnostics are a first-class subsystem designed for machine consumption as
 * much as human (D038). Every one carries a stable code, an exact span, a
 * message, and optionally a machine-applicable fix and related spans.
 *
 * Everything is allocated in the sink's arena and lives as long as it does.
 * Diagnostics are held in report order, so output is deterministic.
 */
#ifndef DOOT_DIAG_H
#define DOOT_DIAG_H

#include <stdbool.h>

#include "arena.h"
#include "buf.h"
#include "plat.h"
#include "slice.h"
#include "source.h"

typedef enum { DIAG_ERROR, DIAG_WARNING, DIAG_NOTE, DIAG_HELP } diag_severity;

#include "diag_codes.h"

#define DOOT_DIAG_ENUM(id, code, sev, brief, explain) id,
typedef enum { DOOT_DIAG_CODES(DOOT_DIAG_ENUM) DIAG_CODE_COUNT } diag_code;
#undef DOOT_DIAG_ENUM

/* An additional span attached to a diagnostic: "declared here", "conflicts with
 * this route", "table declared in this migration". */
typedef struct diag_label {
  const source *src;
  span at;
  slice message;
  struct diag_label *next;
} diag_label;

typedef struct diag {
  diag_code code;
  diag_severity severity;
  const source *src; /* NULL for diagnostics with no source position */
  span at;
  slice message;
  bool has_fix;
  span fix_at;
  slice fix_text;
  diag_label *labels;
  diag_label *labels_tail;
  struct diag *next;
} diag;

/* Declared as a typedef in source.h, which needs the name before this header can
 * be included; C99 forbids repeating a typedef, so only the struct is defined
 * here. */
struct diag_sink {
  arena *a;
  diag *first;
  diag *last;
  size_t count;
  size_t errors;
  size_t warnings;
  size_t limit; /* stop collecting past this many; 0 uses the default */
  bool truncated;
};

#define DIAG_DEFAULT_LIMIT 100u

void diag_sink_init(diag_sink *s, arena *a, size_t limit);

/* Reports a diagnostic. `src` may be NULL and `at` may be span_none() for
 * diagnostics with no position. Returns the diagnostic so a fix or labels can be
 * attached, or NULL when the sink is full. */
diag *diag_report(diag_sink *s, diag_code code, const source *src, span at, const char *fmt, ...)
    DOOT_PRINTF(5, 6);

/* Attaches a machine-applicable edit: replace `at` with `replacement`. This is
 * what lets an agent apply a fix without parsing prose (D038). */
void diag_fix(diag_sink *s, diag *d, span at, slice replacement);

/* Attaches a related span. */
void diag_label_add(diag_sink *s, diag *d, const source *src, span at, const char *fmt, ...)
    DOOT_PRINTF(5, 6);

bool diag_has_errors(const diag_sink *s);
size_t diag_error_count(const diag_sink *s);
size_t diag_warning_count(const diag_sink *s);

/* Registry access, all derived from diag_codes.h. */
const char *diag_code_str(diag_code code);
diag_severity diag_code_severity(diag_code code);
const char *diag_code_brief(diag_code code);
const char *diag_code_explain(diag_code code);
bool diag_code_parse(slice text, diag_code *out); /* "DT0001" -> DIAG_INVALID_UTF8 */
const char *diag_severity_str(diag_severity sev);

/* Human-readable rendering with a source snippet and caret. */
void diag_render_human(const diag_sink *s, buf *out, bool color);

/* Structured rendering, the schema in docs/06-tooling.md#diagnostics. */
void diag_render_json(const diag_sink *s, buf *out);

#endif /* DOOT_DIAG_H */
