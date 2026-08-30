#include "print.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../base/assert.h"
#include "../base/buf.h"

/* ---- state ------------------------------------------------------------- */

typedef struct {
  arena *a;
  const source *src;
  buf out;
  uint32_t indent;
  size_t line_start;
  bool at_line_start;

  /* Comment interleaving. Comments are not in the tree, so they are merged back
   * in by position: the cursor advances through the unit's list as nodes are
   * printed, and `last_end` is how far through the source the output has got. */
  const lex_comment *comment;
  uint32_t last_end;
  bool has_last_end;
} printer;

/* ---- emit -------------------------------------------------------------- */

static void pr_nl(printer *p) {
  (void)buf_append_byte(&p->out, '\n');
  p->line_start = p->out.len;
  p->at_line_start = true;
}

/* The indent is written lazily, when a line's first content arrives. That is what
 * makes "no trailing whitespace" and "no indent on a blank line" structural rather
 * than something to remember. */
static void pr_bytes(printer *p, const char *s, size_t n) {
  if (n == 0u) {
    return;
  }
  if (p->at_line_start) {
    uint32_t i;

    for (i = 0; i < p->indent * 2u; i++) {
      (void)buf_append_byte(&p->out, ' ');
    }
    p->at_line_start = false;
  }
  (void)buf_append(&p->out, s, n);
}

static void pr(printer *p, const char *s) {
  pr_bytes(p, s, strlen(s));
}

static void pr_slice(printer *p, slice s) {
  pr_bytes(p, s.p, s.n);
}

static void pr_byte(printer *p, char c) {
  pr_bytes(p, &c, 1u);
}

static void pr_pad(printer *p, size_t n) {
  size_t i;

  for (i = 0; i < n; i++) {
    pr_byte(p, ' ');
  }
}

static size_t pr_column(const printer *p) {
  return p->out.len - p->line_start;
}

/* ---- source lookback --------------------------------------------------- */

static uint32_t line_of(const printer *p, uint32_t offset) {
  return source_line_col(p->src, offset).line;
}

/* True when the author left at least one empty line between two source offsets. */
static bool blank_line_between(const printer *p, uint32_t from, uint32_t to) {
  slice text = source_text(p->src);
  uint32_t i;
  uint32_t newlines = 0u;

  if (to > text.n) {
    to = (uint32_t)text.n;
  }
  for (i = from; i < to; i++) {
    if (text.p[i] == '\n') {
      newlines++;
      if (newlines >= 2u) {
        return true;
      }
    }
  }
  return false;
}

/* Whether the author put a line break between two source offsets. The printer
 * cannot wrap a long line -- only markup attributes have a specified wrap rule --
 * so it must not *join* one either, or a line the author broke deliberately would
 * become unbreakable. Argument lists and collection literals therefore keep the
 * layout they were written with, exactly as gofmt keeps composite literals. */
static bool spans_newline(const printer *p, uint32_t from, uint32_t to) {
  slice text = source_text(p->src);
  uint32_t i;

  if (to > (uint32_t)text.n) {
    to = (uint32_t)text.n;
  }
  for (i = from; i < to; i++) {
    if (text.p[i] == '\n') {
      return true;
    }
  }
  return false;
}

/* Whether the author broke the line between the opening delimiter and the first
 * element, or between any two elements.
 *
 * A newline *inside* an element does not count. Asking only whether the whole
 * construct spans a newline is wrong: a call whose argument is a multi-line markup
 * literal always would, so `layout(room, <div>` would explode into one argument
 * per line even though the author never broke the argument list. */
static bool expr_list_broken(const printer *p, uint32_t after_open, const expr_list *l) {
  const expr *e;
  uint32_t prev = after_open;

  for (e = l->first; e != NULL; e = e->next) {
    if (spans_newline(p, prev, e->at.start)) {
      return true;
    }
    prev = e->at.end;
  }
  return false;
}

static bool map_list_broken(const printer *p, uint32_t after_open, const map_entry_list *l) {
  const map_entry *e;
  uint32_t prev = after_open;

  for (e = l->first; e != NULL; e = e->next) {
    if (spans_newline(p, prev, e->at.start)) {
      return true;
    }
    prev = e->at.end;
  }
  return false;
}

static bool field_list_broken(const printer *p, uint32_t after_open, const field_init_list *l) {
  const field_init *f;
  uint32_t prev = after_open;

  for (f = l->first; f != NULL; f = f->next) {
    if (spans_newline(p, prev, f->at.start)) {
      return true;
    }
    prev = f->at.end;
  }
  return false;
}

static void mark_end(printer *p, uint32_t end) {
  p->last_end = end;
  p->has_last_end = true;
}

/* ---- comments ---------------------------------------------------------- */

static slice comment_text(const printer *p, const lex_comment *c) {
  slice text = source_text(p->src);
  uint32_t end = c->at.end > (uint32_t)text.n ? (uint32_t)text.n : c->at.end;

  if (c->at.start >= end) {
    return SLICE_EMPTY;
  }
  return slice_trim_ascii(slice_make(text.p + c->at.start, (size_t)(end - c->at.start)));
}

/* Comments that belong above the construct starting at `offset`, each on its own
 * line, preserving a single blank line where the author left one.
 *
 * Returns whether any comment was emitted. Callers need that to decide about the
 * blank line between the comments and the construct itself: their own
 * "is this the first item" guard exists to suppress a blank line at the very top
 * of a file or immediately after a `{`, and once a comment has been printed
 * neither of those situations still applies. */
static bool pr_comments_before(printer *p, uint32_t offset) {
  bool any = false;

  while (p->comment != NULL && p->comment->at.start < offset) {
    if (p->has_last_end && blank_line_between(p, p->last_end, p->comment->at.start)) {
      pr_nl(p);
    }
    pr_slice(p, comment_text(p, p->comment));
    pr_nl(p);
    mark_end(p, p->comment->at.end);
    p->comment = p->comment->next;
    any = true;
  }
  return any;
}

/* A comment sitting on the same source line as the construct just printed stays
 * on the same output line. */
static void pr_trailing_comment(printer *p, uint32_t end) {
  if (p->comment == NULL || p->comment->at.start < end) {
    return;
  }
  if (line_of(p, p->comment->at.start) != line_of(p, end)) {
    return;
  }
  pr(p, "  ");
  pr_slice(p, comment_text(p, p->comment));
  mark_end(p, p->comment->at.end);
  p->comment = p->comment->next;
}

/* ---- literals ---------------------------------------------------------- */

/* Re-encodes a decoded string into source form. The AST holds resolved text, so
 * every escape has to be put back; `$` is escaped only when a `{` follows it,
 * because that is the only position where a bare `$` would start something. */
static void pr_escaped(printer *p, slice s) {
  size_t i;

  for (i = 0; i < s.n; i++) {
    unsigned char c = (unsigned char)s.p[i];

    switch (c) {
    case '\n':
      pr(p, "\\n");
      break;
    case '\t':
      pr(p, "\\t");
      break;
    case '\r':
      pr(p, "\\r");
      break;
    case '\\':
      pr(p, "\\\\");
      break;
    case '"':
      pr(p, "\\\"");
      break;
    case '$':
      if (i + 1u < s.n && s.p[i + 1u] == '{') {
        pr(p, "\\$");
      } else {
        pr_byte(p, '$');
      }
      break;
    default:
      if (c < 0x20u) {
        char tmp[16];

        (void)sprintf(tmp, "\\u{%x}", (unsigned)c);
        pr(p, tmp);
      } else {
        pr_byte(p, (char)c);
      }
      break;
    }
  }
}

static void pr_int(printer *p, int64_t v) {
  char tmp[32];

  (void)sprintf(tmp, "%" PRId64, v);
  pr(p, tmp);
}

/* The shortest representation that reads back as the same double. Trying 15, 16,
 * then 17 significant digits is the standard approach: 17 always round-trips, and
 * the shorter forms are what a person actually wrote. */
static void pr_float(printer *p, double v) {
  char tmp[64];
  int prec;

  for (prec = 15; prec < 17; prec++) {
    (void)sprintf(tmp, "%.*g", prec, v);
    if (strtod(tmp, NULL) == v) {
      break;
    }
  }
  if (prec >= 17) {
    (void)sprintf(tmp, "%.17g", v);
  }
  pr(p, tmp);
  /* A float must keep a `.` or an exponent, or it would lex back as an int. */
  if (strpbrk(tmp, ".eE") == NULL) {
    pr(p, ".0");
  }
}

/* ---- forward declarations ---------------------------------------------- */

static void pr_expr(printer *p, const expr *e, int min_prec);
static void pr_block(printer *p, const stmt_list *body);
static void pr_type(printer *p, const type_ref *t);
static void pr_markup(printer *p, const markup_node *m);

/* ---- paths, types, attributes ------------------------------------------ */

static void pr_path(printer *p, const name_path *q) {
  const name_seg *seg;

  for (seg = q->first; seg != NULL; seg = seg->next) {
    if (seg != q->first) {
      pr_byte(p, '.');
    }
    pr_slice(p, seg->name);
  }
}

static void pr_type_list(printer *p, const type_list *l) {
  const type_ref *t;

  for (t = l->first; t != NULL; t = t->next) {
    if (t != l->first) {
      pr(p, ", ");
    }
    pr_type(p, t);
  }
}

static void pr_type(printer *p, const type_ref *t) {
  if (t == NULL) {
    return;
  }
  switch (t->kind) {
  case TYPE_PATH:
    pr_path(p, &t->as.p.segs);
    if (t->as.p.args.count > 0u) {
      pr_byte(p, '[');
      pr_type_list(p, &t->as.p.args);
      pr_byte(p, ']');
    }
    break;
  case TYPE_LIST:
    pr_byte(p, '[');
    pr_type(p, t->as.list.elem);
    pr_byte(p, ']');
    break;
  case TYPE_MAP:
    pr_byte(p, '{');
    pr_type(p, t->as.map.key);
    pr(p, ": ");
    pr_type(p, t->as.map.val);
    pr_byte(p, '}');
    break;
  case TYPE_FN:
    pr(p, "fn(");
    pr_type_list(p, &t->as.fn.params);
    pr_byte(p, ')');
    if (t->as.fn.ret != NULL) {
      pr(p, " -> ");
      pr_type(p, t->as.fn.ret);
      if (t->as.fn.fallible) {
        pr_byte(p, '!');
      }
    }
    break;
  case TYPE_KIND_COUNT:
    DOOT_UNREACHABLE();
  }
  if (t->optional) {
    pr_byte(p, '?');
  }
}

static void pr_attrs_inline(printer *p, const attr_list *l) {
  const attr *a;

  for (a = l->first; a != NULL; a = a->next) {
    pr(p, " @");
    pr_slice(p, a->name);
    if (a->args.count > 0u) {
      const expr *arg;

      pr_byte(p, '(');
      for (arg = a->args.first; arg != NULL; arg = arg->next) {
        if (arg != a->args.first) {
          pr(p, ", ");
        }
        pr_expr(p, arg, 1);
      }
      pr_byte(p, ')');
    }
  }
}

/* ---- patterns ---------------------------------------------------------- */

static void pr_pattern(printer *p, const pattern *pat) {
  if (pat == NULL) {
    pr(p, "else");
    return;
  }
  switch (pat->kind) {
  case PAT_VARIANT:
    pr_byte(p, '.');
    pr_slice(p, pat->as.variant);
    break;
  case PAT_INT:
    pr_int(p, pat->as.int_value);
    break;
  case PAT_STR:
    pr_byte(p, '"');
    pr_escaped(p, pat->as.str_value);
    pr_byte(p, '"');
    break;
  case PAT_BOOL:
    pr(p, pat->as.bool_value ? "true" : "false");
    break;
  case PAT_ALT: {
    const pattern *alt;

    for (alt = pat->as.alts.first; alt != NULL; alt = alt->next) {
      if (alt != pat->as.alts.first) {
        pr(p, " | ");
      }
      pr_pattern(p, alt);
    }
    break;
  }
  case PAT_KIND_COUNT:
    DOOT_UNREACHABLE();
  }
}

/* ---- expression precedence --------------------------------------------- */

/* The levels in docs/03-grammar.md#precedence, loosest to tightest. The AST does
 * not record the author's parentheses, so they are re-derived from precedence on
 * the way out -- which is also what makes the printer a check on the parser: if
 * the tree shape were wrong, the reprinted parentheses would move. */
#define PREC_COALESCE 1
#define PREC_OR 2
#define PREC_AND 3
#define PREC_NOT 4
#define PREC_CMP_LEVEL 5
#define PREC_ADD_LEVEL 6
#define PREC_MUL_LEVEL 7
#define PREC_AS 8
#define PREC_NEG 9
#define PREC_POSTFIX 10
#define PREC_ATOM 11

static int binop_prec(binop op) {
  switch (op) {
  case BINOP_OR:
    return PREC_OR;
  case BINOP_AND:
    return PREC_AND;
  case BINOP_EQ:
  case BINOP_NE:
  case BINOP_LT:
  case BINOP_LE:
  case BINOP_GT:
  case BINOP_GE:
  case BINOP_IN:
    return PREC_CMP_LEVEL;
  case BINOP_ADD:
  case BINOP_SUB:
    return PREC_ADD_LEVEL;
  case BINOP_MUL:
  case BINOP_DIV:
  case BINOP_MOD:
    return PREC_MUL_LEVEL;
  case BINOP_KIND_COUNT:
    break;
  }
  DOOT_UNREACHABLE();
}

static const char *binop_text(binop op) {
  switch (op) {
  case BINOP_ADD:
    return " + ";
  case BINOP_SUB:
    return " - ";
  case BINOP_MUL:
    return " * ";
  case BINOP_DIV:
    return " / ";
  case BINOP_MOD:
    return " % ";
  case BINOP_EQ:
    return " == ";
  case BINOP_NE:
    return " != ";
  case BINOP_LT:
    return " < ";
  case BINOP_LE:
    return " <= ";
  case BINOP_GT:
    return " > ";
  case BINOP_GE:
    return " >= ";
  case BINOP_IN:
    return " in ";
  case BINOP_AND:
    return " and ";
  case BINOP_OR:
    return " or ";
  case BINOP_KIND_COUNT:
    break;
  }
  DOOT_UNREACHABLE();
}

static int expr_prec(const expr *e) {
  switch (e->kind) {
  /* A lambda shares the loosest level with `else`, because its body extends as far
   * right as it can: that is what keeps `fn(x) => x` parenthesized when it appears
   * as an operand rather than as a whole argument. */
  case EXPR_COALESCE:
  case EXPR_LAMBDA:
    return PREC_COALESCE;
  case EXPR_BINARY:
    return binop_prec(e->as.binary.op);
  case EXPR_UNARY:
    return e->as.unary.op == UNOP_NOT ? PREC_NOT : PREC_NEG;
  case EXPR_CAST:
    return PREC_AS;
  case EXPR_CALL:
  case EXPR_INDEX:
  case EXPR_FIELD:
  case EXPR_PROPAGATE:
  case EXPR_WITH:
    return PREC_POSTFIX;
  case EXPR_INT:
  case EXPR_FLOAT:
  case EXPR_STR:
  case EXPR_RAW_STR:
  case EXPR_BOOL:
  case EXPR_NIL:
  case EXPR_IDENT:
  case EXPR_SELF:
  case EXPR_VARIANT:
  case EXPR_LIST:
  case EXPR_MAP:
  case EXPR_STRUCT:
  case EXPR_MARKUP:
    return PREC_ATOM;
  case EXPR_KIND_COUNT:
    break;
  }
  DOOT_UNREACHABLE();
}

/* ---- expressions ------------------------------------------------------- */

static void pr_field_inits(printer *p, const field_init_list *l, bool multiline) {
  const field_init *init;
  size_t widest = 0u;

  if (!multiline) {
    for (init = l->first; init != NULL; init = init->next) {
      if (init != l->first) {
        pr(p, ", ");
      }
      pr_slice(p, init->name);
      pr(p, ": ");
      pr_expr(p, init->value, PREC_COALESCE);
    }
    return;
  }
  /* Newline-separated rather than comma-separated, and aligned on the colon: the
   * grammar accepts either separator, and this is the form the configuration
   * example is written in. */
  for (init = l->first; init != NULL; init = init->next) {
    if (init->name.n > widest) {
      widest = init->name.n;
    }
  }
  p->indent++;
  for (init = l->first; init != NULL; init = init->next) {
    pr_nl(p);
    pr_slice(p, init->name);
    pr_byte(p, ':');
    pr_pad(p, widest - init->name.n + 1u);
    pr_expr(p, init->value, PREC_COALESCE);
  }
  p->indent--;
  pr_nl(p);
}

static void pr_params(printer *p, const param_list *l) {
  const param *q;

  for (q = l->first; q != NULL; q = q->next) {
    if (q != l->first) {
      pr(p, ", ");
    }
    if (q->is_self) {
      pr(p, "self");
      continue;
    }
    pr_slice(p, q->name);
    pr(p, ": ");
    pr_type(p, q->type);
    if (q->dflt != NULL) {
      pr(p, " = ");
      pr_expr(p, q->dflt, PREC_COALESCE);
    }
    pr_attrs_inline(p, &q->attrs);
  }
}

static void pr_expr_bare(printer *p, const expr *e) {
  switch (e->kind) {
  case EXPR_INT:
    pr_int(p, e->as.int_value);
    break;
  case EXPR_FLOAT:
    pr_float(p, e->as.float_value);
    break;
  case EXPR_STR: {
    const str_part *part;

    pr_byte(p, '"');
    for (part = e->as.str.first; part != NULL; part = part->next) {
      if (part->value == NULL) {
        pr_escaped(p, part->text);
      } else {
        pr(p, "${");
        pr_expr(p, part->value, PREC_COALESCE);
        pr_byte(p, '}');
      }
    }
    pr_byte(p, '"');
    break;
  }
  case EXPR_RAW_STR:
    pr_byte(p, '`');
    pr_slice(p, e->as.raw_str);
    pr_byte(p, '`');
    break;
  case EXPR_BOOL:
    pr(p, e->as.bool_value ? "true" : "false");
    break;
  case EXPR_NIL:
    pr(p, "nil");
    break;
  case EXPR_IDENT:
    pr_path(p, &e->as.ident);
    break;
  case EXPR_SELF:
    pr(p, "self");
    break;
  case EXPR_VARIANT:
    pr_byte(p, '.');
    pr_slice(p, e->as.variant);
    break;
  case EXPR_LIST: {
    const expr *item;
    bool multiline = expr_list_broken(p, e->at.start, &e->as.list);

    pr_byte(p, '[');
    if (multiline) {
      p->indent++;
    }
    for (item = e->as.list.first; item != NULL; item = item->next) {
      if (multiline) {
        pr_nl(p);
      } else if (item != e->as.list.first) {
        pr(p, ", ");
      }
      pr_expr(p, item, PREC_COALESCE);
      if (multiline) {
        pr_byte(p, ',');
      }
    }
    if (multiline) {
      p->indent--;
      pr_nl(p);
    }
    pr_byte(p, ']');
    break;
  }
  case EXPR_MAP: {
    const map_entry *entry;
    bool multiline = map_list_broken(p, e->at.start, &e->as.map);

    pr_byte(p, '{');
    if (multiline) {
      p->indent++;
    }
    for (entry = e->as.map.first; entry != NULL; entry = entry->next) {
      if (multiline) {
        pr_nl(p);
      } else if (entry != e->as.map.first) {
        pr(p, ", ");
      }
      pr_expr(p, entry->key, PREC_COALESCE);
      pr(p, ": ");
      pr_expr(p, entry->value, PREC_COALESCE);
      if (multiline) {
        pr_byte(p, ',');
      }
    }
    if (multiline) {
      p->indent--;
      pr_nl(p);
    }
    pr_byte(p, '}');
    break;
  }
  case EXPR_STRUCT: {
    bool multiline = field_list_broken(p, e->at.start, &e->as.struct_lit.fields);

    pr_path(p, &e->as.struct_lit.type_name);
    if (multiline) {
      pr(p, " {");
      pr_field_inits(p, &e->as.struct_lit.fields, true);
      pr_byte(p, '}');
    } else {
      pr(p, " { ");
      pr_field_inits(p, &e->as.struct_lit.fields, false);
      pr(p, " }");
    }
    break;
  }
  case EXPR_LAMBDA:
    pr(p, "fn(");
    pr_params(p, &e->as.lambda.params);
    pr_byte(p, ')');
    if (e->as.lambda.ret != NULL) {
      pr(p, " -> ");
      pr_type(p, e->as.lambda.ret);
      if (e->as.lambda.fallible) {
        pr_byte(p, '!');
      }
    }
    if (e->as.lambda.has_block) {
      pr_byte(p, ' ');
      pr_block(p, &e->as.lambda.body);
    } else {
      pr(p, " => ");
      pr_expr(p, e->as.lambda.body_expr, PREC_COALESCE);
    }
    break;
  case EXPR_MARKUP:
    pr_markup(p, e->as.markup);
    break;
  case EXPR_UNARY:
    if (e->as.unary.op == UNOP_NOT) {
      pr(p, "not ");
      pr_expr(p, e->as.unary.operand, PREC_NOT);
    } else {
      pr_byte(p, '-');
      pr_expr(p, e->as.unary.operand, PREC_NEG);
    }
    break;
  case EXPR_BINARY: {
    int level = binop_prec(e->as.binary.op);

    /* Left-associative, so the right operand needs one level tighter. The
     * comparisons are non-associative, which the parser already rejects, so the
     * same rule prints them correctly. */
    pr_expr(p, e->as.binary.lhs, level);
    pr(p, binop_text(e->as.binary.op));
    pr_expr(p, e->as.binary.rhs, level + 1);
    break;
  }
  case EXPR_CAST:
    pr_expr(p, e->as.cast.value, PREC_AS);
    pr(p, " as ");
    pr_type(p, e->as.cast.type);
    break;
  case EXPR_CALL: {
    const expr *arg;
    bool multiline;

    pr_expr(p, e->as.call.callee, PREC_POSTFIX);
    if (e->as.call.type_args.count > 0u) {
      pr_byte(p, '[');
      pr_type_list(p, &e->as.call.type_args);
      pr_byte(p, ']');
    }
    multiline = expr_list_broken(p, e->as.call.callee->at.end, &e->as.call.args);
    pr_byte(p, '(');
    if (multiline) {
      p->indent++;
    }
    for (arg = e->as.call.args.first; arg != NULL; arg = arg->next) {
      if (multiline) {
        pr_nl(p);
      } else if (arg != e->as.call.args.first) {
        pr(p, ", ");
      }
      pr_expr(p, arg, PREC_COALESCE);
      if (multiline) {
        pr_byte(p, ',');
      }
    }
    if (multiline) {
      p->indent--;
      pr_nl(p);
    }
    pr_byte(p, ')');
    break;
  }
  case EXPR_INDEX:
    pr_expr(p, e->as.index.target, PREC_POSTFIX);
    pr_byte(p, '[');
    pr_expr(p, e->as.index.index, PREC_COALESCE);
    pr_byte(p, ']');
    break;
  case EXPR_FIELD:
    pr_expr(p, e->as.field.target, PREC_POSTFIX);
    pr_byte(p, '.');
    pr_slice(p, e->as.field.name);
    break;
  case EXPR_PROPAGATE:
    pr_expr(p, e->as.propagate, PREC_POSTFIX);
    pr_byte(p, '!');
    break;
  case EXPR_COALESCE:
    /* Right-associative: the left operand needs one level tighter. */
    pr_expr(p, e->as.coalesce.value, PREC_COALESCE + 1);
    pr(p, " else ");
    if (e->as.coalesce.form == COALESCE_VALUE) {
      pr_expr(p, e->as.coalesce.fallback, PREC_COALESCE);
      break;
    }
    if (e->as.coalesce.binds_err) {
      pr_slice(p, e->as.coalesce.err_name);
      pr_byte(p, ' ');
    }
    pr_block(p, &e->as.coalesce.block);
    break;
  case EXPR_WITH:
    pr_expr(p, e->as.with.value, PREC_POSTFIX);
    pr(p, " with { ");
    pr_field_inits(p, &e->as.with.fields, false);
    pr(p, " }");
    break;
  case EXPR_KIND_COUNT:
    DOOT_UNREACHABLE();
  }
}

static void pr_expr(printer *p, const expr *e, int min_prec) {
  bool parens;

  if (e == NULL) {
    return;
  }
  parens = expr_prec(e) < min_prec;
  if (parens) {
    pr_byte(p, '(');
  }
  pr_expr_bare(p, e);
  if (parens) {
    pr_byte(p, ')');
  }
}

/* ---- markup ------------------------------------------------------------ */

static const char *const void_tags[] = {"area",  "base", "br",   "col",   "embed",  "hr",    "img",
                                        "input", "link", "meta", "param", "source", "track", "wbr"};

/* Elements whose content is whitespace-significant. Re-indenting inside one of
 * these would change what the page renders, so they are emitted verbatim. */
static const char *const raw_text_tags[] = {"pre", "textarea"};

static bool tag_in(slice tag, const char *const *table, size_t n) {
  size_t i;

  for (i = 0; i < n; i++) {
    if (slice_eq_cstr(tag, table[i])) {
      return true;
    }
  }
  return false;
}

static bool is_void_tag(slice tag) {
  return tag_in(tag, void_tags, sizeof(void_tags) / sizeof(void_tags[0]));
}

static bool is_raw_text_tag(slice tag) {
  return tag_in(tag, raw_text_tags, sizeof(raw_text_tags) / sizeof(raw_text_tags[0]));
}

static bool slice_has_newline(slice s) {
  return slice_contains_byte(s, '\n');
}

/* An element is laid out over several lines exactly when the author put a line
 * break among its children.
 *
 * The printer does not decide for itself where markup should break, and that is
 * deliberate. Whitespace between elements is rendered content: it collapses to a
 * single space in inline context, so *removing* or *adding* a break changes the
 * page, while changing the amount of indentation does not. Reformatting markup
 * safely would need an inline-versus-block element table and an understanding of
 * CSS `white-space`, which is a large surface to be wrong in, and being wrong in
 * it means silently changing what a user's page looks like. So line structure is
 * preserved and only indentation is normalized. */
static bool list_is_multiline(const markup_list *l) {
  const markup_node *child;

  for (child = l->first; child != NULL; child = child->next) {
    if (child->kind == MARKUP_TEXT && slice_has_newline(child->as.text)) {
      return true;
    }
  }
  return false;
}

static void pr_markup_attrs(printer *p, const markup_node *m, bool multiline_tag) {
  const markup_attr *a;

  for (a = m->as.element.attrs.first; a != NULL; a = a->next) {
    if (multiline_tag) {
      pr_nl(p);
    } else {
      pr_byte(p, ' ');
    }
    if (a->is_spread) {
      pr(p, "...");
      if (a->value != NULL) {
        if (a->value->kind == EXPR_IDENT) {
          pr_path(p, &a->value->as.ident);
        } else {
          pr(p, "${");
          pr_expr(p, a->value, PREC_COALESCE);
          pr_byte(p, '}');
        }
      }
      continue;
    }
    pr_slice(p, a->name);
    if (a->value == NULL) {
      continue;
    }
    pr_byte(p, '=');
    if (a->value->kind == EXPR_STR) {
      pr_expr_bare(p, a->value);
    } else {
      pr(p, "${");
      pr_expr(p, a->value, PREC_COALESCE);
      pr_byte(p, '}');
    }
  }
}

/* The width a tag would occupy on one line, for the wrap decision. */
static size_t markup_attrs_width(printer *p, const markup_node *m) {
  printer probe;
  size_t width;

  probe = *p;
  buf_init(&probe.out, p->a, 64u);
  probe.at_line_start = false;
  probe.line_start = 0u;
  probe.comment = NULL;
  pr_markup_attrs(&probe, m, false);
  width = probe.out.len;
  return width;
}

static void pr_markup_children(printer *p, const markup_list *children);

static void pr_markup_element(printer *p, const markup_node *m) {
  slice tag = m->as.element.tag;
  bool selfclose = m->as.element.self_closing || is_void_tag(tag);
  bool wrap_attrs;

  /* Attributes wrap past the wrap column, measured with the tag and the indent. */
  wrap_attrs = m->as.element.attrs.count > 1u &&
               pr_column(p) + 1u + tag.n + markup_attrs_width(p, m) + 2u > FMT_WRAP_COLUMN;

  pr_byte(p, '<');
  pr_slice(p, tag);
  if (wrap_attrs) {
    p->indent++;
    pr_markup_attrs(p, m, true);
    p->indent--;
    pr_nl(p);
  } else {
    pr_markup_attrs(p, m, false);
  }

  if (selfclose) {
    /* Normalized: both `<br>` and `<br/>` print as `<br/>`. */
    pr(p, "/>");
    return;
  }
  pr_byte(p, '>');

  if (is_raw_text_tag(tag)) {
    const markup_node *child;

    for (child = m->as.element.children.first; child != NULL; child = child->next) {
      if (child->kind == MARKUP_TEXT) {
        pr_slice(p, child->as.text);
      } else {
        pr_markup(p, child);
      }
    }
    pr(p, "</");
    pr_slice(p, tag);
    pr_byte(p, '>');
    return;
  }

  pr_markup_children(p, &m->as.element.children);
  pr(p, "</");
  pr_slice(p, tag);
  pr_byte(p, '>');
}

/* Emits a text node that spans lines: each line's surrounding horizontal
 * whitespace is dropped and the line is re-indented. That is safe because a run of
 * whitespace containing a newline collapses the same way regardless of its length,
 * and it is what lets nested markup be re-indented at all. */
static void pr_markup_text_lines(printer *p, slice text) {
  size_t i = 0;

  while (i < text.n) {
    size_t start = i;
    slice line;

    while (i < text.n && text.p[i] != '\n') {
      i++;
    }
    line = slice_trim_ascii(slice_make(text.p + start, i - start));
    if (!slice_is_empty(line)) {
      pr_nl(p);
      pr_slice(p, line);
    }
    if (i < text.n) {
      i++;
    }
  }
}

/* Emits a child list, indenting and closing the line itself when the list is
 * laid out over several lines. Owning that decision here is what keeps an element
 * body, an `{if}` arm, and a `{for}` body laying out identically. */
static void pr_markup_children(printer *p, const markup_list *children) {
  bool multiline = list_is_multiline(children);
  const markup_node *child;

  if (multiline) {
    p->indent++;
  }
  for (child = children->first; child != NULL; child = child->next) {
    if (child->kind == MARKUP_TEXT) {
      if (slice_has_newline(child->as.text)) {
        pr_markup_text_lines(p, child->as.text);
      } else {
        /* No newline: inline whitespace and inline content, both verbatim,
         * because a single space between two inline elements is content. */
        pr_slice(p, child->as.text);
      }
      continue;
    }
    if (multiline) {
      pr_nl(p);
    }
    pr_markup(p, child);
  }
  if (multiline) {
    p->indent--;
    pr_nl(p);
  }
}

static void pr_markup(printer *p, const markup_node *m) {
  if (m == NULL) {
    return;
  }
  switch (m->kind) {
  case MARKUP_ELEMENT:
    pr_markup_element(p, m);
    break;
  case MARKUP_TEXT:
    pr_slice(p, m->as.text);
    break;
  case MARKUP_INTERP:
    pr(p, "${");
    pr_expr(p, m->as.interp, PREC_COALESCE);
    pr_byte(p, '}');
    break;
  case MARKUP_COMMENT:
    pr_slice(p, m->as.text);
    break;
  case MARKUP_IF: {
    const markup_branch *b;

    for (b = m->as.if_.branches.first; b != NULL; b = b->next) {
      if (b == m->as.if_.branches.first) {
        pr(p, "{if ");
        pr_expr(p, b->cond, PREC_COALESCE);
        pr_byte(p, '}');
      } else if (b->cond != NULL) {
        pr(p, "{else if ");
        pr_expr(p, b->cond, PREC_COALESCE);
        pr_byte(p, '}');
      } else {
        pr(p, "{else}");
      }
      pr_markup_children(p, &b->body);
    }
    pr(p, "{end}");
    break;
  }
  case MARKUP_FOR:
    pr(p, "{for ");
    pr_slice(p, m->as.for_.first_name);
    if (m->as.for_.has_second) {
      pr(p, ", ");
      pr_slice(p, m->as.for_.second_name);
    }
    pr(p, " in ");
    pr_expr(p, m->as.for_.iter, PREC_COALESCE);
    pr_byte(p, '}');
    pr_markup_children(p, &m->as.for_.body);
    if (m->as.for_.has_empty) {
      pr(p, "{else}");
      pr_markup_children(p, &m->as.for_.empty);
    }
    pr(p, "{end}");
    break;
  case MARKUP_KIND_COUNT:
    DOOT_UNREACHABLE();
  }
}

/* ---- statements -------------------------------------------------------- */

static const char *assign_text(assign_op op) {
  switch (op) {
  case ASSIGN_SET:
    return " = ";
  case ASSIGN_ADD:
    return " += ";
  case ASSIGN_SUB:
    return " -= ";
  case ASSIGN_MUL:
    return " *= ";
  case ASSIGN_DIV:
    return " /= ";
  case ASSIGN_MOD:
    return " %= ";
  case ASSIGN_KIND_COUNT:
    break;
  }
  DOOT_UNREACHABLE();
}

static void pr_stmt(printer *p, const stmt *s);

static void pr_block(printer *p, const stmt_list *body) {
  const stmt *s;

  if (body->count == 0u) {
    pr(p, "{}");
    return;
  }
  pr_byte(p, '{');
  p->indent++;
  for (s = body->first; s != NULL; s = s->next) {
    bool had_comment;

    /* Open the line first: a comment above a statement belongs on a line of its
     * own, not appended to whatever the previous line ended with. */
    pr_nl(p);
    had_comment = pr_comments_before(p, s->at.start);
    if ((had_comment || s != body->first) && blank_line_between(p, p->last_end, s->at.start)) {
      pr_nl(p);
    }
    pr_stmt(p, s);
    mark_end(p, s->at.end);
    pr_trailing_comment(p, s->at.end);
  }
  p->indent--;
  pr_nl(p);
  pr_byte(p, '}');
}

static void pr_stmt(printer *p, const stmt *s) {
  switch (s->kind) {
  case STMT_LET:
    pr(p, s->as.let.is_var ? "var " : "let ");
    pr_slice(p, s->as.let.name);
    if (s->as.let.type != NULL) {
      pr(p, ": ");
      pr_type(p, s->as.let.type);
    }
    pr(p, " = ");
    pr_expr(p, s->as.let.value, PREC_COALESCE);
    break;
  case STMT_ASSIGN:
    pr_expr(p, s->as.assign.target, PREC_POSTFIX);
    pr(p, assign_text(s->as.assign.op));
    pr_expr(p, s->as.assign.value, PREC_COALESCE);
    break;
  case STMT_IF:
    pr(p, "if ");
    pr_expr(p, s->as.if_.cond, PREC_COALESCE);
    pr_byte(p, ' ');
    pr_block(p, &s->as.if_.then_body);
    if (s->as.if_.has_else) {
      pr(p, " else ");
      if (s->as.if_.else_if != NULL) {
        pr_stmt(p, s->as.if_.else_if);
      } else {
        pr_block(p, &s->as.if_.else_body);
      }
    }
    break;
  case STMT_FOR:
    pr(p, "for ");
    pr_slice(p, s->as.for_.first_name);
    if (s->as.for_.has_second) {
      pr(p, ", ");
      pr_slice(p, s->as.for_.second_name);
    }
    pr(p, " in ");
    pr_expr(p, s->as.for_.iter, PREC_COALESCE);
    pr_byte(p, ' ');
    pr_block(p, &s->as.for_.body);
    break;
  case STMT_WHILE:
    pr(p, "while ");
    pr_expr(p, s->as.while_.cond, PREC_COALESCE);
    pr_byte(p, ' ');
    pr_block(p, &s->as.while_.body);
    break;
  case STMT_MATCH: {
    const match_arm *arm;

    pr(p, "match ");
    pr_expr(p, s->as.match.value, PREC_COALESCE);
    pr(p, " {");
    p->indent++;
    for (arm = s->as.match.arms.first; arm != NULL; arm = arm->next) {
      pr_nl(p);
      pr_pattern(p, arm->pat);
      pr(p, " -> ");
      if (arm->has_block) {
        pr_block(p, &arm->body);
      } else {
        pr_expr(p, arm->value, PREC_COALESCE);
      }
    }
    p->indent--;
    pr_nl(p);
    pr_byte(p, '}');
    break;
  }
  case STMT_RETURN:
    pr(p, "return");
    if (s->as.ret != NULL) {
      pr_byte(p, ' ');
      pr_expr(p, s->as.ret, PREC_COALESCE);
    }
    break;
  case STMT_SEND:
    pr(p, "send ");
    if (s->as.send.name != NULL) {
      pr_expr(p, s->as.send.name, PREC_COALESCE);
      pr(p, ", ");
    }
    pr_expr(p, s->as.send.value, PREC_COALESCE);
    break;
  case STMT_SPAWN:
    pr(p, "spawn ");
    pr_expr(p, s->as.spawn, PREC_COALESCE);
    break;
  case STMT_DEFER:
    pr(p, "defer ");
    pr_expr(p, s->as.defer, PREC_COALESCE);
    break;
  case STMT_BREAK:
    pr(p, "break");
    break;
  case STMT_CONTINUE:
    pr(p, "continue");
    break;
  case STMT_EXPR:
    pr_expr(p, s->as.expression, PREC_COALESCE);
    break;
  case STMT_KIND_COUNT:
    DOOT_UNREACHABLE();
  }
}

/* ---- declarations ------------------------------------------------------ */

static void pr_signature_tail(printer *p, const type_ref *ret, bool fallible,
                              const stmt_list *body) {
  if (ret != NULL) {
    pr(p, " -> ");
    pr_type(p, ret);
    if (fallible) {
      pr_byte(p, '!');
    }
  }
  pr_byte(p, ' ');
  pr_block(p, body);
}

static void pr_decl(printer *p, const decl *d);

static void pr_decl_attrs(printer *p, const attr_list *l) {
  const attr *a;

  for (a = l->first; a != NULL; a = a->next) {
    pr_byte(p, '@');
    pr_slice(p, a->name);
    if (a->args.count > 0u) {
      const expr *arg;

      pr_byte(p, '(');
      for (arg = a->args.first; arg != NULL; arg = arg->next) {
        if (arg != a->args.first) {
          pr(p, ", ");
        }
        pr_expr(p, arg, PREC_COALESCE);
      }
      pr_byte(p, ')');
    }
    pr_nl(p);
  }
}

static void pr_struct_fields(printer *p, const field_list *fields) {
  const field *f;
  size_t widest = 0u;

  /* Fields align on the colon, so the widest name sets the column. */
  for (f = fields->first; f != NULL; f = f->next) {
    if (f->name.n > widest) {
      widest = f->name.n;
    }
  }
  for (f = fields->first; f != NULL; f = f->next) {
    pr_nl(p);
    pr_comments_before(p, f->at.start);
    pr_slice(p, f->name);
    pr_byte(p, ':');
    pr_pad(p, widest - f->name.n + 1u);
    pr_type(p, f->type);
    if (f->dflt != NULL) {
      pr(p, " = ");
      pr_expr(p, f->dflt, PREC_COALESCE);
    }
    pr_attrs_inline(p, &f->attrs);
    mark_end(p, f->at.end);
    pr_trailing_comment(p, f->at.end);
  }
}

static void pr_decl(printer *p, const decl *d) {
  pr_decl_attrs(p, &d->attrs);
  if (d->is_pub) {
    pr(p, "pub ");
  }
  switch (d->kind) {
  case DECL_FN:
    pr(p, "fn ");
    if (d->as.fn.has_recv) {
      pr_slice(p, d->as.fn.recv);
      pr_byte(p, '.');
    }
    pr_slice(p, d->as.fn.name);
    pr_byte(p, '(');
    pr_params(p, &d->as.fn.params);
    pr_byte(p, ')');
    pr_signature_tail(p, d->as.fn.ret, d->as.fn.fallible, &d->as.fn.body);
    break;
  case DECL_STRUCT:
    pr(p, "type ");
    pr_slice(p, d->as.struct_.name);
    pr(p, " {");
    p->indent++;
    pr_struct_fields(p, &d->as.struct_.fields);
    p->indent--;
    pr_nl(p);
    pr_byte(p, '}');
    break;
  case DECL_ENUM: {
    const variant *v;

    pr(p, "type ");
    pr_slice(p, d->as.enum_.name);
    pr(p, " enum { ");
    for (v = d->as.enum_.variants.first; v != NULL; v = v->next) {
      if (v != d->as.enum_.variants.first) {
        pr(p, ", ");
      }
      pr_slice(p, v->name);
    }
    pr(p, " }");
    break;
  }
  case DECL_ALIAS:
    pr(p, "type ");
    pr_slice(p, d->as.alias.name);
    pr(p, " = ");
    pr_type(p, d->as.alias.target);
    break;
  case DECL_LET:
    pr(p, d->as.let.is_var ? "var " : "let ");
    pr_slice(p, d->as.let.name);
    if (d->as.let.type != NULL) {
      pr(p, ": ");
      pr_type(p, d->as.let.type);
    }
    pr(p, " = ");
    pr_expr(p, d->as.let.value, PREC_COALESCE);
    break;
  case DECL_ROUTE:
  case DECL_STREAM:
    pr(p, d->kind == DECL_ROUTE ? "route " : "stream ");
    pr_slice(p, d->as.route.method);
    pr(p, " \"");
    pr_escaped(p, d->as.route.pattern);
    pr(p, "\" (");
    pr_params(p, &d->as.route.params);
    pr_byte(p, ')');
    pr_signature_tail(p, d->as.route.ret, d->as.route.fallible, &d->as.route.body);
    break;
  case DECL_GROUP: {
    const decl *item;

    pr(p, "group \"");
    pr_escaped(p, d->as.group.prefix);
    pr(p, "\" {");
    p->indent++;
    for (item = d->as.group.items.first; item != NULL; item = item->next) {
      bool had_comment;

      pr_nl(p);
      had_comment = pr_comments_before(p, item->at.start);
      if ((had_comment || item != d->as.group.items.first) &&
          blank_line_between(p, p->last_end, item->at.start)) {
        pr_nl(p);
      }
      pr_decl(p, item);
      mark_end(p, item->at.end);
      pr_trailing_comment(p, item->at.end);
    }
    p->indent--;
    pr_nl(p);
    pr_byte(p, '}');
    break;
  }
  case DECL_TEST:
    pr(p, "test \"");
    pr_escaped(p, d->as.test.name);
    pr(p, "\" ");
    pr_block(p, &d->as.test.body);
    break;
  case DECL_KIND_COUNT:
    DOOT_UNREACHABLE();
  }
}

/* ---- entry point ------------------------------------------------------- */

slice fmt_unit(arena *a, const unit_ast *unit) {
  printer p;
  const decl *d;

  DOOT_ASSERT(a != NULL && unit != NULL && unit->src != NULL);
  memset(&p, 0, sizeof(p));
  p.a = a;
  p.src = unit->src;
  p.comment = unit->comments.first;
  p.at_line_start = true;
  buf_init(&p.out, a, 4096u);

  for (d = unit->decls.first; d != NULL; d = d->next) {
    uint32_t start = d->attrs.first != NULL ? d->attrs.first->at.start : d->at.start;
    bool had_comment;

    /* End the previous declaration's line first: a comment between two
     * declarations belongs on a line of its own, not appended to the `}` above
     * it. */
    if (d != unit->decls.first) {
      pr_nl(&p);
    }
    had_comment = pr_comments_before(&p, start);
    if ((had_comment || d != unit->decls.first) && blank_line_between(&p, p.last_end, start)) {
      /* Exactly one blank line where the author left one or more. */
      pr_nl(&p);
    }
    pr_decl(&p, d);
    mark_end(&p, d->at.end);
    pr_trailing_comment(&p, d->at.end);
  }

  /* Trailing comments after the last declaration. */
  while (p.comment != NULL) {
    if (p.has_last_end && blank_line_between(&p, p.last_end, p.comment->at.start)) {
      pr_nl(&p);
    }
    pr_nl(&p);
    pr_slice(&p, comment_text(&p, p.comment));
    mark_end(&p, p.comment->at.end);
    p.comment = p.comment->next;
  }

  /* A file ends with exactly one newline. */
  if (!p.at_line_start) {
    pr_nl(&p);
  }
  return buf_slice(&p.out);
}
