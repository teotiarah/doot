#include "parse.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "../base/assert.h"
#include "../base/buf.h"
#include "../base/diag.h"
#include "../lex/token.h"

/* ---- state ------------------------------------------------------------- */

typedef struct {
  arena *a;
  const source *src;
  diag_sink *sink;
  lexer *lx;
  unit_ast *unit;

  token tok; /* the current token; lex_peek gives one more */
  uint32_t depth;

  /* Syntax errors cascade, so a second one at a position already reported is
   * noise rather than information. */
  bool has_error_pos;
  uint32_t last_error_at;
  bool depth_reported;

  /* Syntactic context, which is all the parser needs to discharge grammar rules
   * 10, 11, and 12 (D064). */
  uint32_t loop_depth;
  bool in_method;
  bool in_stream;

  /* A `{` after a path is a struct literal, except where a block can follow --
   * the condition of `if`, `while`, `for`, and `match`. */
  bool no_struct;
} parser;

static expr *parse_expr(parser *p);
static stmt_list parse_block(parser *p);
static type_ref *parse_type(parser *p);
static markup_node *parse_markup_element(parser *p, bool entering);

/* ---- reporting --------------------------------------------------------- */

static bool report_ok(parser *p, span at) {
  if (p->sink == NULL) {
    return false;
  }
  if (p->has_error_pos && p->last_error_at == at.start) {
    return false;
  }
  p->has_error_pos = true;
  p->last_error_at = at.start;
  return true;
}

/* Always reported: a specific rule was violated, and two different rules can
 * legitimately fire at one position. The format string stays a literal at the call
 * site so -Wformat=2 can check it. */
#define P_REPORT(p, code, at_, ...)                                                                \
  do {                                                                                             \
    if ((p)->sink != NULL) {                                                                       \
      (void)diag_report((p)->sink, (code), (p)->src, (at_), __VA_ARGS__);                          \
    }                                                                                              \
  } while (0)

/* Deduplicated: the generic "did not expect this" family, which cascades. */
#define P_SYNTAX(p, code, at_, ...)                                                                \
  do {                                                                                             \
    span at__ = (at_);                                                                             \
    if (report_ok((p), at__)) {                                                                    \
      (void)diag_report((p)->sink, (code), (p)->src, at__, __VA_ARGS__);                           \
    }                                                                                              \
  } while (0)

/* ---- token helpers ----------------------------------------------------- */

static void advance(parser *p) {
  p->tok = lex_next(p->lx);
}

static bool check(const parser *p, token_kind k) {
  return p->tok.kind == k;
}

static bool at_eof(const parser *p) {
  return p->tok.kind == TOK_EOF;
}

static bool match(parser *p, token_kind k) {
  if (check(p, k)) {
    advance(p);
    return true;
  }
  return false;
}

static slice tok_text(parser *p, token t) {
  return lex_text(p->lx, t);
}

/* A name position is one where only a name can appear: after `.`, as a field name,
 * as an enum variant. A reserved word is an ordinary identifier there.
 *
 * The reservation exists so that reaching for a foreign construct fails clearly
 * (D042), and in a name position nobody is reaching for a construct -- `x.require`
 * is unambiguously a member access. Enforcing it here would instead make real code
 * unwritable: the standard library has `auth.require`, `uuid.new`, and `chan.new`,
 * every one of which is a reserved word after a dot. See D062. */
static bool at_name(const parser *p) {
  return p->tok.kind == TOK_IDENT || p->tok.kind == TOK_RESERVED;
}

static bool peek_is_name(parser *p) {
  token_kind k = lex_peek(p->lx).kind;

  return k == TOK_IDENT || k == TOK_RESERVED;
}

static void unexpected(parser *p, const char *want) {
  if (at_eof(p)) {
    P_SYNTAX(p, DIAG_UNEXPECTED_EOF, p->tok.at, "expected %s, but the file ends here", want);
    return;
  }
  P_SYNTAX(p, DIAG_UNEXPECTED_TOKEN, p->tok.at, "expected %s, found %s", want,
           token_kind_name(p->tok.kind));
}

static bool expect(parser *p, token_kind k, const char *want) {
  if (check(p, k)) {
    advance(p);
    return true;
  }
  unexpected(p, want);
  return false;
}

static void skip_newlines(parser *p) {
  while (check(p, TOK_NEWLINE)) {
    advance(p);
  }
}

/* Statement end is a newline, or a lookahead of `}` or end of input, neither
 * consumed (D060). The last statement in a block has no newline after it, because
 * `}` is a follow token and the lexer therefore suppressed it. */
static bool at_stmt_end(const parser *p) {
  return check(p, TOK_NEWLINE) || check(p, TOK_RBRACE) || at_eof(p);
}

static void expect_stmt_end(parser *p) {
  if (check(p, TOK_NEWLINE)) {
    advance(p);
    return;
  }
  if (check(p, TOK_RBRACE) || at_eof(p)) {
    return;
  }
  P_SYNTAX(p, DIAG_EXPECTED_STMT_END, p->tok.at, "expected the statement to end, found %s",
           token_kind_name(p->tok.kind));
}

/* Token dispatch in this file is written as if-chains rather than switches, on
 * purpose. `-Wswitch-enum` requires every enumerator to be listed even when a
 * `default:` is present (D046), and that is the right rule where a site genuinely
 * must handle every token kind -- token_kind_name and token_flags in src/lex. A
 * parser's dispatch is not such a site: adding a token kind should not force
 * every production to mention it. Switches over the AST kinds stay exhaustive,
 * because there the tripwire is exactly what is wanted. */
static bool starts_declaration(token_kind k) {
  return k == TOK_KW_FN || k == TOK_KW_TYPE || k == TOK_KW_LET || k == TOK_KW_VAR ||
         k == TOK_KW_ROUTE || k == TOK_KW_STREAM || k == TOK_KW_GROUP || k == TOK_KW_TEST ||
         k == TOK_KW_PUB;
}

/* Skip to the next plausible statement or declaration boundary, so one mistake
 * does not turn into a page of diagnostics.
 *
 * This deliberately stops *before* a boundary token rather than consuming it, so
 * the caller can parse the construct that follows. It therefore cannot guarantee
 * progress on its own -- see ensure_progress. */
static void synchronize(parser *p) {
  while (!at_eof(p)) {
    token_kind k = p->tok.kind;

    if (k == TOK_NEWLINE) {
      advance(p);
      return;
    }
    if (k == TOK_RBRACE || starts_declaration(k) || k == TOK_KW_RETURN || k == TOK_KW_IF ||
        k == TOK_KW_FOR || k == TOK_KW_WHILE || k == TOK_KW_MATCH) {
      return;
    }
    advance(p);
  }
}

/* Recovery must always move the cursor. Without this, a token that both fails to
 * parse and is a synchronization boundary -- a stray `}` at the top level is the
 * simple case -- leaves the position unchanged and the enclosing loop spins
 * forever. Found by fuzz_parse on its first run. */
static void ensure_progress(parser *p, uint32_t before) {
  if (at_eof(p) || p->tok.at.start != before) {
    return;
  }
  synchronize(p);
  if (!at_eof(p) && p->tok.at.start == before) {
    advance(p);
  }
  skip_newlines(p);
}

static bool depth_enter(parser *p) {
  if (p->depth >= PARSE_MAX_DEPTH) {
    if (!p->depth_reported) {
      p->depth_reported = true;
      P_REPORT(p, DIAG_NESTING_TOO_DEEP, p->tok.at, "expressions nest deeper than %u levels",
               (unsigned)PARSE_MAX_DEPTH);
    }
    return false;
  }
  p->depth++;
  return true;
}

static void depth_leave(parser *p) {
  DOOT_ASSERT(p->depth > 0u);
  p->depth--;
}

/* ---- literal decoding -------------------------------------------------- */

static void encode_utf8(buf *out, uint32_t cp) {
  if (cp < 0x80u) {
    (void)buf_append_byte(out, (char)cp);
  } else if (cp < 0x800u) {
    (void)buf_append_byte(out, (char)(0xc0u | (cp >> 6)));
    (void)buf_append_byte(out, (char)(0x80u | (cp & 0x3fu)));
  } else if (cp < 0x10000u) {
    (void)buf_append_byte(out, (char)(0xe0u | (cp >> 12)));
    (void)buf_append_byte(out, (char)(0x80u | ((cp >> 6) & 0x3fu)));
    (void)buf_append_byte(out, (char)(0x80u | (cp & 0x3fu)));
  } else {
    (void)buf_append_byte(out, (char)(0xf0u | (cp >> 18)));
    (void)buf_append_byte(out, (char)(0x80u | ((cp >> 12) & 0x3fu)));
    (void)buf_append_byte(out, (char)(0x80u | ((cp >> 6) & 0x3fu)));
    (void)buf_append_byte(out, (char)(0x80u | (cp & 0x3fu)));
  }
}

static uint32_t hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return (uint32_t)(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return (uint32_t)(c - 'a') + 10u;
  }
  return (uint32_t)(c - 'A') + 10u;
}

static bool is_hex_digit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* Resolves escapes in a string text run. The lexer left them raw so that these
 * diagnostics land on the escape rather than on the opening quote (D058). */
static slice decode_escapes(parser *p, token t) {
  slice raw = tok_text(p, t);
  buf out;
  size_t i = 0;

  buf_init(&out, p->a, raw.n + 1u);
  while (i < raw.n) {
    char c = raw.p[i];
    uint32_t base;

    if (c != '\\') {
      (void)buf_append_byte(&out, c);
      i++;
      continue;
    }
    base = t.at.start + (uint32_t)i;
    i++;
    if (i >= raw.n) {
      P_REPORT(p, DIAG_UNKNOWN_ESCAPE, span_make(base, base + 1u),
               "a backslash must be followed by an escape character");
      break;
    }
    c = raw.p[i];
    i++;
    switch (c) {
    case 'n':
      (void)buf_append_byte(&out, '\n');
      break;
    case 't':
      (void)buf_append_byte(&out, '\t');
      break;
    case 'r':
      (void)buf_append_byte(&out, '\r');
      break;
    case '\\':
      (void)buf_append_byte(&out, '\\');
      break;
    case '"':
      (void)buf_append_byte(&out, '"');
      break;
    case '$':
      (void)buf_append_byte(&out, '$');
      break;
    case 'u': {
      uint32_t cp = 0u;
      uint32_t digits = 0u;

      if (i >= raw.n || raw.p[i] != '{') {
        P_REPORT(p, DIAG_MALFORMED_UNICODE_ESCAPE, span_make(base, base + 2u),
                 "a unicode escape is written `\\u{...}` with braces");
        break;
      }
      i++;
      while (i < raw.n && is_hex_digit(raw.p[i])) {
        if (digits < 8u) {
          cp = (cp << 4) | hex_value(raw.p[i]);
        }
        digits++;
        i++;
      }
      if (digits == 0u || i >= raw.n || raw.p[i] != '}') {
        P_REPORT(p, DIAG_MALFORMED_UNICODE_ESCAPE, span_make(base, t.at.start + (uint32_t)i),
                 "a unicode escape needs at least one hex digit and a closing `}`");
        break;
      }
      i++;
      if (digits > 6u || cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu)) {
        P_REPORT(p, DIAG_UNICODE_ESCAPE_RANGE, span_make(base, t.at.start + (uint32_t)i),
                 "U+%04lX is not a scalar value", (unsigned long)cp);
        break;
      }
      encode_utf8(&out, cp);
      break;
    }
    default:
      P_REPORT(p, DIAG_UNKNOWN_ESCAPE, span_make(base, base + 2u), "`\\%c` is not an escape", c);
      break;
    }
  }
  return buf_slice(&out);
}

/* Digits with `_` separators removed. The lexer already rejected misplaced ones
 * (DT0020), so this only has to strip them. */
static slice strip_separators(arena *a, slice s) {
  buf out;
  size_t i;

  buf_init(&out, a, s.n + 1u);
  for (i = 0; i < s.n; i++) {
    if (s.p[i] != '_') {
      (void)buf_append_byte(&out, s.p[i]);
    }
  }
  return buf_slice(&out);
}

/* `negated` folds a unary minus into the literal, which is the only way to write
 * the most negative int without the magnitude overflowing on its own (D003 makes
 * overflow a fault, so it must not be reachable from a well-formed literal). */
static expr *parse_int_literal(parser *p, token t, bool negated) {
  expr *e = ast_expr(p->a, EXPR_INT, t.at);
  slice text = strip_separators(p->a, tok_text(p, t));
  uint64_t value = 0u;
  uint64_t limit = negated ? 0x8000000000000000u : 0x7fffffffffffffffu;
  uint32_t radix = 10u;
  size_t i = 0;
  bool overflow = false;

  if (text.n > 2u && text.p[0] == '0' && (text.p[1] == 'x' || text.p[1] == 'X')) {
    radix = 16u;
    i = 2u;
  } else if (text.n > 2u && text.p[0] == '0' && (text.p[1] == 'b' || text.p[1] == 'B')) {
    radix = 2u;
    i = 2u;
  }
  for (; i < text.n; i++) {
    uint32_t d;

    if (!is_hex_digit(text.p[i])) {
      break;
    }
    d = hex_value(text.p[i]);
    if (d >= radix) {
      break;
    }
    if (value > (limit - d) / radix) {
      overflow = true;
      break;
    }
    value = value * radix + d;
  }
  if (overflow) {
    P_REPORT(p, DIAG_INT_LITERAL_RANGE, t.at,
             "this literal does not fit in `int`, which is signed 64-bit");
    e->as.int_value = 0;
    return e;
  }
  if (negated) {
    e->as.int_value = value == 0x8000000000000000u ? (-0x7fffffffffffffff - 1) : -(int64_t)value;
  } else {
    e->as.int_value = (int64_t)value;
  }
  return e;
}

static expr *parse_float_literal(parser *p, token t, bool negated) {
  expr *e = ast_expr(p->a, EXPR_FLOAT, t.at);
  slice text = strip_separators(p->a, tok_text(p, t));
  const char *cstr = slice_cstr(p->a, text);
  double value;
  char *end = NULL;

  if (cstr == NULL) {
    e->as.float_value = 0.0;
    return e;
  }
  errno = 0;
  value = strtod(cstr, &end);
  if (errno == ERANGE) {
    P_REPORT(p, DIAG_FLOAT_LITERAL_RANGE, t.at,
             "this literal is outside the range of `float`, which is IEEE 754 double");
    e->as.float_value = 0.0;
    return e;
  }
  e->as.float_value = negated ? -value : value;
  return e;
}

/* ---- string literals --------------------------------------------------- */

/* A literal is a token sequence (D058), so it assembles into a part list: runs of
 * decoded text interleaved with interpolated expressions. */
static expr *parse_string(parser *p) {
  span open = p->tok.at;
  expr *e = ast_expr(p->a, EXPR_STR, open);

  advance(p); /* past TOK_STR_START */
  for (;;) {
    if (check(p, TOK_STR_TEXT)) {
      str_part *part = ast_str_part(p->a, p->tok.at);
      part->text = decode_escapes(p, p->tok);
      str_part_list_push(&e->as.str, part);
      advance(p);
      continue;
    }
    if (check(p, TOK_INTERP_START)) {
      str_part *part = ast_str_part(p->a, p->tok.at);
      advance(p);
      part->value = parse_expr(p);
      (void)expect(p, TOK_INTERP_END, "`}` to close the interpolation");
      str_part_list_push(&e->as.str, part);
      continue;
    }
    break;
  }
  if (check(p, TOK_STR_END)) {
    e->at = span_join(open, p->tok.at);
    advance(p);
  }
  return e;
}

/* ---- types ------------------------------------------------------------- */

static path parse_path(parser *p) {
  path result;

  memset(&result, 0, sizeof(result));
  if (!check(p, TOK_IDENT)) {
    unexpected(p, "a name");
    return result;
  }
  path_push(&result, ast_path_seg(p->a, tok_text(p, p->tok), p->tok.at));
  advance(p);
  while (check(p, TOK_DOT)) {
    if (!peek_is_name(p)) {
      break;
    }
    advance(p);
    path_push(&result, ast_path_seg(p->a, tok_text(p, p->tok), p->tok.at));
    advance(p);
  }
  return result;
}

static type_ref *parse_type(parser *p) {
  type_ref *t;
  span start = p->tok.at;

  if (!depth_enter(p)) {
    return ast_type(p->a, TYPE_PATH, start);
  }

  if (check(p, TOK_LBRACKET)) {
    advance(p);
    t = ast_type(p->a, TYPE_LIST, start);
    t->as.list.elem = parse_type(p);
    (void)expect(p, TOK_RBRACKET, "`]` to close the list type");
  } else if (check(p, TOK_LBRACE)) {
    advance(p);
    t = ast_type(p->a, TYPE_MAP, start);
    t->as.map.key = parse_type(p);
    (void)expect(p, TOK_COLON, "`:` between the key and value types");
    t->as.map.val = parse_type(p);
    (void)expect(p, TOK_RBRACE, "`}` to close the map type");
  } else if (check(p, TOK_KW_FN)) {
    advance(p);
    t = ast_type(p->a, TYPE_FN, start);
    (void)expect(p, TOK_LPAREN, "`(` to open the parameter types");
    if (!check(p, TOK_RPAREN)) {
      do {
        skip_newlines(p);
        if (check(p, TOK_RPAREN)) {
          break;
        }
        type_list_push(&t->as.fn.params, parse_type(p));
        skip_newlines(p);
      } while (match(p, TOK_COMMA));
    }
    (void)expect(p, TOK_RPAREN, "`)` to close the parameter types");
    if (match(p, TOK_ARROW)) {
      t->as.fn.ret = parse_type(p);
      t->as.fn.fallible = match(p, TOK_BANG);
    }
  } else if (check(p, TOK_LPAREN)) {
    advance(p);
    t = parse_type(p);
    (void)expect(p, TOK_RPAREN, "`)` to close the type");
  } else {
    t = ast_type(p->a, TYPE_PATH, start);
    t->as.p.segs = parse_path(p);
    if (check(p, TOK_LBRACKET)) {
      advance(p);
      do {
        type_list_push(&t->as.p.args, parse_type(p));
      } while (match(p, TOK_COMMA));
      (void)expect(p, TOK_RBRACKET, "`]` to close the type arguments");
    }
  }

  if (check(p, TOK_QUESTION)) {
    t->optional = true;
    t->at = span_join(t->at, p->tok.at);
    advance(p);
  } else {
    t->at = span_join(t->at, p->tok.at);
  }
  depth_leave(p);
  return t;
}

/* ---- patterns ---------------------------------------------------------- */

static pattern *parse_single_pattern(parser *p) {
  span start = p->tok.at;
  pattern *pat;

  if (check(p, TOK_DOT)) {
    advance(p);
    pat = ast_pattern(p->a, PAT_VARIANT, start);
    if (at_name(p)) {
      pat->as.variant = tok_text(p, p->tok);
      pat->at = span_join(start, p->tok.at);
      advance(p);
    } else {
      unexpected(p, "a variant name");
    }
    return pat;
  }
  if (check(p, TOK_INT)) {
    expr *lit = parse_int_literal(p, p->tok, false);
    pat = ast_pattern(p->a, PAT_INT, p->tok.at);
    pat->as.int_value = lit->as.int_value;
    advance(p);
    return pat;
  }
  if (check(p, TOK_MINUS) && lex_peek(p->lx).kind == TOK_INT) {
    advance(p);
    {
      expr *lit = parse_int_literal(p, p->tok, true);
      pat = ast_pattern(p->a, PAT_INT, span_join(start, p->tok.at));
      pat->as.int_value = lit->as.int_value;
      advance(p);
    }
    return pat;
  }
  if (check(p, TOK_STR_START)) {
    expr *s = parse_string(p);
    pat = ast_pattern(p->a, PAT_STR, s->at);
    pat->as.str_value = s->as.str.count == 1u && s->as.str.first->value == NULL
                            ? s->as.str.first->text
                            : SLICE_EMPTY;
    if (s->as.str.count > 0u && s->as.str.first->value != NULL) {
      P_REPORT(p, DIAG_UNEXPECTED_TOKEN, s->at,
               "a match pattern must be a constant, so it cannot interpolate");
    }
    return pat;
  }
  if (check(p, TOK_KW_TRUE) || check(p, TOK_KW_FALSE)) {
    pat = ast_pattern(p->a, PAT_BOOL, start);
    pat->as.bool_value = check(p, TOK_KW_TRUE);
    advance(p);
    return pat;
  }
  unexpected(p, "a pattern");
  return ast_pattern(p->a, PAT_VARIANT, start);
}

static pattern *parse_pattern(parser *p) {
  pattern *first = parse_single_pattern(p);
  pattern *alt;

  if (!check(p, TOK_PIPE)) {
    return first;
  }
  alt = ast_pattern(p->a, PAT_ALT, first->at);
  pattern_list_push(&alt->as.alts, first);
  while (match(p, TOK_PIPE)) {
    pattern *next = parse_single_pattern(p);
    alt->at = span_join(alt->at, next->at);
    pattern_list_push(&alt->as.alts, next);
  }
  return alt;
}

/* ---- expression helpers ------------------------------------------------ */

static bool expr_to_path(const expr *e, path *out) {
  /* An `a.b.c` chain reinterpreted as a dotted name, which is how a struct
   * literal's type and a type argument are recovered without backtracking. */
  if (e->kind == EXPR_IDENT) {
    *out = e->as.ident;
    return true;
  }
  return false;
}

static field_init_list parse_field_inits(parser *p) {
  field_init_list list;

  memset(&list, 0, sizeof(list));
  skip_newlines(p);
  while (!check(p, TOK_RBRACE) && !at_eof(p)) {
    field_init *init;

    if (!at_name(p)) {
      unexpected(p, "a field name");
      break;
    }
    init = ast_field_init(p->a, tok_text(p, p->tok), p->tok.at);
    advance(p);
    (void)expect(p, TOK_COLON, "`:` after the field name");
    init->value = parse_expr(p);
    init->at = span_join(init->at, init->value->at);
    field_init_list_push(&list, init);
    if (!match(p, TOK_COMMA)) {
      if (!check(p, TOK_NEWLINE)) {
        break;
      }
    }
    skip_newlines(p);
  }
  skip_newlines(p);
  return list;
}

static param_list parse_params(parser *p);

static expr *parse_lambda(parser *p) {
  span start = p->tok.at;
  expr *e;

  advance(p); /* past `fn` */
  e = ast_expr(p->a, EXPR_LAMBDA, start);
  (void)expect(p, TOK_LPAREN, "`(` to open the parameter list");
  e->as.lambda.params = parse_params(p);
  (void)expect(p, TOK_RPAREN, "`)` to close the parameter list");
  if (match(p, TOK_ARROW)) {
    e->as.lambda.ret = parse_type(p);
    e->as.lambda.fallible = match(p, TOK_BANG);
  }
  if (match(p, TOK_FAT_ARROW)) {
    e->as.lambda.body_expr = parse_expr(p);
    e->at = span_join(start, e->as.lambda.body_expr->at);
  } else if (check(p, TOK_LBRACE)) {
    e->as.lambda.has_block = true;
    e->as.lambda.body = parse_block(p);
    e->at = span_join(start, p->tok.at);
  } else {
    unexpected(p, "`=>` or a block for the lambda body");
  }
  return e;
}

static expr *parse_primary(parser *p) {
  span start = p->tok.at;
  expr *e;

  if (check(p, TOK_INT)) {
    e = parse_int_literal(p, p->tok, false);
    advance(p);
    return e;
  }
  if (check(p, TOK_FLOAT)) {
    e = parse_float_literal(p, p->tok, false);
    advance(p);
    return e;
  }
  if (check(p, TOK_STR_START)) {
    return parse_string(p);
  }
  if (check(p, TOK_RAW_STR)) {
    slice raw = tok_text(p, p->tok);
    e = ast_expr(p->a, EXPR_RAW_STR, start);
    /* Strip the backticks; the interior is verbatim by definition. */
    e->as.raw_str = raw.n >= 2u ? slice_sub(raw, 1u, raw.n - 2u) : SLICE_EMPTY;
    advance(p);
    return e;
  }
  if (check(p, TOK_KW_TRUE) || check(p, TOK_KW_FALSE)) {
    e = ast_expr(p->a, EXPR_BOOL, start);
    e->as.bool_value = check(p, TOK_KW_TRUE);
    advance(p);
    return e;
  }
  if (check(p, TOK_KW_NIL)) {
    advance(p);
    return ast_expr(p->a, EXPR_NIL, start);
  }
  if (check(p, TOK_KW_SELF)) {
    if (!p->in_method) {
      P_REPORT(p, DIAG_SELF_OUTSIDE_METHOD, start, "`self` is only available inside a method body");
    }
    advance(p);
    return ast_expr(p->a, EXPR_SELF, start);
  }
  if (check(p, TOK_IDENT)) {
    e = ast_expr(p->a, EXPR_IDENT, start);
    e->as.ident = parse_path(p);
    e->at = span_join(start, p->tok.at);
    if (e->as.ident.last != NULL) {
      e->at = span_join(start, e->as.ident.last->at);
    }
    return e;
  }
  if (check(p, TOK_DOT)) {
    advance(p);
    e = ast_expr(p->a, EXPR_VARIANT, start);
    if (at_name(p)) {
      e->as.variant = tok_text(p, p->tok);
      e->at = span_join(start, p->tok.at);
      advance(p);
    } else {
      unexpected(p, "a variant name after `.`");
    }
    return e;
  }
  if (check(p, TOK_LBRACKET)) {
    advance(p);
    e = ast_expr(p->a, EXPR_LIST, start);
    skip_newlines(p);
    while (!check(p, TOK_RBRACKET) && !at_eof(p)) {
      expr_list_push(&e->as.list, parse_expr(p));
      skip_newlines(p);
      if (!match(p, TOK_COMMA)) {
        break;
      }
      skip_newlines(p);
    }
    skip_newlines(p);
    e->at = span_join(start, p->tok.at);
    (void)expect(p, TOK_RBRACKET, "`]` to close the list");
    return e;
  }
  if (check(p, TOK_LBRACE)) {
    advance(p);
    e = ast_expr(p->a, EXPR_MAP, start);
    skip_newlines(p);
    while (!check(p, TOK_RBRACE) && !at_eof(p)) {
      map_entry *entry = ast_map_entry(p->a, p->tok.at);
      entry->key = parse_expr(p);
      (void)expect(p, TOK_COLON, "`:` between the key and the value");
      entry->value = parse_expr(p);
      entry->at = span_join(entry->key->at, entry->value->at);
      map_entry_list_push(&e->as.map, entry);
      skip_newlines(p);
      if (!match(p, TOK_COMMA)) {
        break;
      }
      skip_newlines(p);
    }
    skip_newlines(p);
    e->at = span_join(start, p->tok.at);
    (void)expect(p, TOK_RBRACE, "`}` to close the map");
    return e;
  }
  if (check(p, TOK_KW_FN)) {
    return parse_lambda(p);
  }
  if (check(p, TOK_MARKUP_START)) {
    markup_node *m = parse_markup_element(p, true);
    e = ast_expr(p->a, EXPR_MARKUP, m->at);
    e->as.markup = m;
    return e;
  }
  if (check(p, TOK_LPAREN)) {
    bool saved = p->no_struct;
    advance(p);
    p->no_struct = false;
    skip_newlines(p);
    e = parse_expr(p);
    skip_newlines(p);
    p->no_struct = saved;
    (void)expect(p, TOK_RPAREN, "`)` to close the group");
    return e;
  }
  /* docs/02-syntax.md#keywords: "all stdlib module names are predeclared
   * identifiers, not keywords". Two of the thirty-eight collide with a word that is
   * otherwise spoken for, and in expression position the module wins -- neither
   * word can begin an expression any other way, so there is no ambiguity:
   *
   *   `test`   is keyword #27 and the assertions module, used as `test.eq(...)`
   *   `static` is a reserved word and the file-serving module
   *
   * Both spellings are required by documented code. Nothing else in the module list
   * collides with a keyword or a reserved word. */
  if (check(p, TOK_KW_TEST) ||
      (check(p, TOK_RESERVED) && slice_eq_cstr(tok_text(p, p->tok), "static"))) {
    e = ast_expr(p->a, EXPR_IDENT, start);
    path_push(&e->as.ident, ast_path_seg(p->a, tok_text(p, p->tok), p->tok.at));
    advance(p);
    while (check(p, TOK_DOT) && peek_is_name(p)) {
      advance(p);
      path_push(&e->as.ident, ast_path_seg(p->a, tok_text(p, p->tok), p->tok.at));
      e->at = span_join(start, p->tok.at);
      advance(p);
    }
    return e;
  }
  if (check(p, TOK_RESERVED)) {
    slice word = tok_text(p, p->tok);
    const char *help = token_reserved_help(word);

    P_REPORT(p, DIAG_RESERVED_WORD, start, "`%.*s` is reserved: %s", (int)word.n, word.p,
             help != NULL ? help : "it has no meaning in doot");
    advance(p);
    return ast_expr(p->a, EXPR_NIL, start);
  }
  unexpected(p, "an expression");
  if (!at_stmt_end(p)) {
    advance(p);
  }
  return ast_expr(p->a, EXPR_NIL, start);
}

static expr *parse_postfix(parser *p) {
  expr *e = parse_primary(p);

  for (;;) {
    if (check(p, TOK_DOT)) {
      expr *f;
      advance(p);
      f = ast_expr(p->a, EXPR_FIELD, e->at);
      f->as.field.target = e;
      if (at_name(p)) {
        f->as.field.name = tok_text(p, p->tok);
        f->at = span_join(e->at, p->tok.at);
        advance(p);
      } else {
        unexpected(p, "a field or method name after `.`");
      }
      e = f;
      continue;
    }
    if (check(p, TOK_LBRACKET)) {
      /* `[` is either a type argument list before a call, or an index. There is no
       * backtracking available, so the contents are parsed as an expression and
       * reinterpreted as a type when a `(` follows. Every stdlib slot that takes a
       * type argument takes a named type, and user code cannot introduce type
       * parameters (D019), so nothing expressible is lost. */
      expr_list items;
      span open = p->tok.at;

      memset(&items, 0, sizeof(items));
      advance(p);
      skip_newlines(p);
      while (!check(p, TOK_RBRACKET) && !at_eof(p)) {
        expr_list_push(&items, parse_expr(p));
        if (!match(p, TOK_COMMA)) {
          break;
        }
        skip_newlines(p);
      }
      (void)expect(p, TOK_RBRACKET, "`]`");

      if (check(p, TOK_LPAREN)) {
        expr *call = ast_expr(p->a, EXPR_CALL, e->at);
        expr *item;

        call->as.call.callee = e;
        for (item = items.first; item != NULL; item = item->next) {
          type_ref *ty = ast_type(p->a, TYPE_PATH, item->at);
          if (!expr_to_path(item, &ty->as.p.segs)) {
            P_REPORT(p, DIAG_UNEXPECTED_TOKEN, item->at, "a type argument must be a named type");
          }
          type_list_push(&call->as.call.type_args, ty);
        }
        advance(p); /* past `(` */
        skip_newlines(p);
        while (!check(p, TOK_RPAREN) && !at_eof(p)) {
          expr_list_push(&call->as.call.args, parse_expr(p));
          skip_newlines(p);
          if (!match(p, TOK_COMMA)) {
            break;
          }
          skip_newlines(p);
        }
        call->at = span_join(e->at, p->tok.at);
        (void)expect(p, TOK_RPAREN, "`)` to close the arguments");
        e = call;
        continue;
      }

      {
        expr *idx = ast_expr(p->a, EXPR_INDEX, span_join(e->at, open));
        idx->as.index.target = e;
        idx->as.index.index = items.first;
        if (items.count != 1u) {
          P_REPORT(p, DIAG_UNEXPECTED_TOKEN, open, "an index takes exactly one expression");
        }
        if (items.first != NULL) {
          idx->at = span_join(idx->at, items.first->at);
        }
        e = idx;
      }
      continue;
    }
    if (check(p, TOK_LPAREN)) {
      expr *call = ast_expr(p->a, EXPR_CALL, e->at);
      call->as.call.callee = e;
      advance(p);
      skip_newlines(p);
      while (!check(p, TOK_RPAREN) && !at_eof(p)) {
        expr_list_push(&call->as.call.args, parse_expr(p));
        skip_newlines(p);
        if (!match(p, TOK_COMMA)) {
          break;
        }
        skip_newlines(p);
      }
      call->at = span_join(e->at, p->tok.at);
      (void)expect(p, TOK_RPAREN, "`)` to close the arguments");
      e = call;
      continue;
    }
    if (check(p, TOK_BANG)) {
      expr *prop = ast_expr(p->a, EXPR_PROPAGATE, span_join(e->at, p->tok.at));
      prop->as.propagate = e;
      advance(p);
      e = prop;
      continue;
    }
    if (check(p, TOK_KW_WITH)) {
      expr *w;
      advance(p);
      w = ast_expr(p->a, EXPR_WITH, e->at);
      w->as.with.value = e;
      (void)expect(p, TOK_LBRACE, "`{` to open the field updates");
      w->as.with.fields = parse_field_inits(p);
      w->at = span_join(e->at, p->tok.at);
      (void)expect(p, TOK_RBRACE, "`}` to close the field updates");
      e = w;
      continue;
    }
    if (check(p, TOK_LBRACE) && !p->no_struct) {
      path type_name;
      expr *lit;

      memset(&type_name, 0, sizeof(type_name));
      if (!expr_to_path(e, &type_name)) {
        break;
      }
      advance(p);
      lit = ast_expr(p->a, EXPR_STRUCT, e->at);
      lit->as.struct_lit.type_name = type_name;
      lit->as.struct_lit.fields = parse_field_inits(p);
      lit->at = span_join(e->at, p->tok.at);
      (void)expect(p, TOK_RBRACE, "`}` to close the struct literal");
      e = lit;
      continue;
    }
    break;
  }
  return e;
}

static expr *parse_unary(parser *p) {
  span start = p->tok.at;

  if (check(p, TOK_MINUS)) {
    token peeked = lex_peek(p->lx);
    expr *e;

    /* Fold the sign into a numeric literal so that the most negative int is
     * expressible without its magnitude overflowing on the way in. */
    if (peeked.kind == TOK_INT) {
      advance(p);
      e = parse_int_literal(p, p->tok, true);
      e->at = span_join(start, p->tok.at);
      advance(p);
      return e;
    }
    if (peeked.kind == TOK_FLOAT) {
      advance(p);
      e = parse_float_literal(p, p->tok, true);
      e->at = span_join(start, p->tok.at);
      advance(p);
      return e;
    }
    advance(p);
    e = ast_expr(p->a, EXPR_UNARY, start);
    e->as.unary.op = UNOP_NEG;
    e->as.unary.operand = parse_unary(p);
    e->at = span_join(start, e->as.unary.operand->at);
    return e;
  }
  return parse_postfix(p);
}

static expr *parse_cast(parser *p) {
  expr *e = parse_unary(p);

  while (check(p, TOK_KW_AS)) {
    expr *c;
    advance(p);
    c = ast_expr(p->a, EXPR_CAST, e->at);
    c->as.cast.value = e;
    c->as.cast.type = parse_type(p);
    c->at = span_join(e->at, c->as.cast.type->at);
    e = c;
  }
  return e;
}

typedef struct {
  token_kind tok;
  binop op;
} binop_row;

/* TOK_MARKUP_START maps to less-than: in operator position the lexer's markup
 * start is a comparison, and its span covers only the `<`, so reinterpreting it
 * needs no re-lexing (D059). */
static const binop_row binop_table[] = {
    {TOK_PLUS, BINOP_ADD},   {TOK_MINUS, BINOP_SUB},   {TOK_STAR, BINOP_MUL},
    {TOK_SLASH, BINOP_DIV},  {TOK_PERCENT, BINOP_MOD}, {TOK_EQ_EQ, BINOP_EQ},
    {TOK_BANG_EQ, BINOP_NE}, {TOK_LT, BINOP_LT},       {TOK_MARKUP_START, BINOP_LT},
    {TOK_LE, BINOP_LE},      {TOK_GT, BINOP_GT},       {TOK_GE, BINOP_GE},
    {TOK_KW_IN, BINOP_IN},
};

static bool binop_of(token_kind k, binop *out) {
  size_t i;

  for (i = 0; i < sizeof(binop_table) / sizeof(binop_table[0]); i++) {
    if (binop_table[i].tok == k) {
      *out = binop_table[i].op;
      return true;
    }
  }
  return false;
}

static expr *make_binary(parser *p, binop op, expr *lhs, expr *rhs) {
  expr *e = ast_expr(p->a, EXPR_BINARY, span_join(lhs->at, rhs->at));

  e->as.binary.op = op;
  e->as.binary.lhs = lhs;
  e->as.binary.rhs = rhs;
  return e;
}

static expr *parse_mul(parser *p) {
  expr *e = parse_cast(p);

  while (check(p, TOK_STAR) || check(p, TOK_SLASH) || check(p, TOK_PERCENT)) {
    binop op;
    (void)binop_of(p->tok.kind, &op);
    advance(p);
    skip_newlines(p);
    e = make_binary(p, op, e, parse_cast(p));
  }
  return e;
}

static expr *parse_add(parser *p) {
  expr *e = parse_mul(p);

  while (check(p, TOK_PLUS) || check(p, TOK_MINUS)) {
    binop op;
    (void)binop_of(p->tok.kind, &op);
    advance(p);
    skip_newlines(p);
    e = make_binary(p, op, e, parse_mul(p));
  }
  return e;
}

static bool is_comparison(token_kind k) {
  return k == TOK_EQ_EQ || k == TOK_BANG_EQ || k == TOK_LT || k == TOK_LE || k == TOK_GT ||
         k == TOK_GE || k == TOK_MARKUP_START || k == TOK_KW_IN;
}

/* Non-associative: `a < b < c` is a syntax error rather than a surprise. */
static expr *parse_cmp(parser *p) {
  expr *e = parse_add(p);
  binop op;

  if (!is_comparison(p->tok.kind)) {
    return e;
  }
  (void)binop_of(p->tok.kind, &op);
  advance(p);
  skip_newlines(p);
  e = make_binary(p, op, e, parse_add(p));

  if (is_comparison(p->tok.kind)) {
    P_REPORT(p, DIAG_COMPARISON_CHAIN, p->tok.at,
             "comparison operators do not chain; write `a < b and b < c`");
    while (is_comparison(p->tok.kind)) {
      advance(p);
      (void)parse_add(p);
    }
  }
  return e;
}

static expr *parse_not(parser *p) {
  span start = p->tok.at;

  if (check(p, TOK_KW_NOT)) {
    expr *e;
    advance(p);
    e = ast_expr(p->a, EXPR_UNARY, start);
    e->as.unary.op = UNOP_NOT;
    e->as.unary.operand = parse_not(p);
    e->at = span_join(start, e->as.unary.operand->at);
    return e;
  }
  return parse_cmp(p);
}

static expr *parse_and(parser *p) {
  expr *e = parse_not(p);

  while (check(p, TOK_KW_AND)) {
    advance(p);
    skip_newlines(p);
    e = make_binary(p, BINOP_AND, e, parse_not(p));
  }
  return e;
}

static expr *parse_or(parser *p) {
  expr *e = parse_and(p);

  while (check(p, TOK_KW_OR)) {
    advance(p);
    skip_newlines(p);
    e = make_binary(p, BINOP_OR, e, parse_and(p));
  }
  return e;
}

static stmt *parse_stmt(parser *p);

/* `else` is the loosest operator and right-associative. The block form must
 * diverge on every path, which the resolver checks; keeping the two forms as
 * distinct shapes is what lets it do that without re-deriving them. */
static expr *parse_coalesce(parser *p) {
  expr *value = parse_or(p);
  expr *e;

  if (!check(p, TOK_KW_ELSE)) {
    return value;
  }
  advance(p);
  e = ast_expr(p->a, EXPR_COALESCE, value->at);
  e->as.coalesce.value = value;

  if (check(p, TOK_KW_RETURN)) {
    /* `else return x` is the block form with one statement, so that "diverges on
     * every path" has one representation rather than two. */
    stmt *ret = parse_stmt(p);
    e->as.coalesce.form = COALESCE_BLOCK;
    stmt_list_push(&e->as.coalesce.block, ret);
    e->at = span_join(value->at, ret->at);
    return e;
  }
  if (check(p, TOK_IDENT) && lex_peek(p->lx).kind == TOK_LBRACE) {
    e->as.coalesce.binds_err = true;
    e->as.coalesce.err_name = tok_text(p, p->tok);
    advance(p);
  }
  if (check(p, TOK_LBRACE)) {
    e->as.coalesce.form = COALESCE_BLOCK;
    e->as.coalesce.block = parse_block(p);
    e->at = span_join(value->at, p->tok.at);
    return e;
  }
  if (e->as.coalesce.binds_err) {
    unexpected(p, "a block after the error binding");
    return e;
  }
  e->as.coalesce.form = COALESCE_VALUE;
  e->as.coalesce.fallback = parse_coalesce(p);
  e->at = span_join(value->at, e->as.coalesce.fallback->at);
  return e;
}

static expr *parse_expr(parser *p) {
  expr *e;

  if (!depth_enter(p)) {
    return ast_expr(p->a, EXPR_NIL, p->tok.at);
  }
  e = parse_coalesce(p);
  depth_leave(p);
  return e;
}

/* ---- markup ------------------------------------------------------------ */

static const char *const void_elements[] = {"area",  "base",   "br",    "col",  "embed",
                                            "hr",    "img",    "input", "link", "meta",
                                            "param", "source", "track", "wbr"};

static bool is_void_element(slice tag) {
  size_t i;

  for (i = 0; i < sizeof(void_elements) / sizeof(void_elements[0]); i++) {
    if (slice_eq_cstr(tag, void_elements[i])) {
      return true;
    }
  }
  return false;
}

static bool attr_name_taken(const markup_attr_list *list, slice name) {
  const markup_attr *a;

  for (a = list->first; a != NULL; a = a->next) {
    if (!a->is_spread && slice_eq(a->name, name)) {
      return true;
    }
  }
  return false;
}

static void parse_markup_children(parser *p, markup_list *out, slice tag);

/* `{if}` / `{else if}` / `{else}` / `{end}`, and the `{for}` form with its
 * `{else}` empty arm. The lexer has already pushed an expression interior, so the
 * directive's condition is ordinary expression syntax. */
static markup_node *parse_markup_ctrl(parser *p, slice tag, bool *closed_block) {
  span start = p->tok.at;
  markup_node *node;

  advance(p); /* past TOK_CTRL_START */

  if (check(p, TOK_KW_IF)) {
    markup_branch *branch;
    bool seen_else = false;

    advance(p);
    node = ast_markup(p->a, MARKUP_IF, start);
    branch = ast_markup_branch(p->a, start);
    branch->cond = parse_expr(p);
    (void)expect(p, TOK_CTRL_END, "`}` to close the directive");
    parse_markup_children(p, &branch->body, tag);
    markup_branch_list_push(&node->as.if_.branches, branch);

    while (check(p, TOK_CTRL_START)) {
      span arm_at = p->tok.at;
      advance(p);
      if (check(p, TOK_KW_ELSE)) {
        advance(p);
        if (check(p, TOK_KW_IF)) {
          advance(p);
          if (seen_else) {
            P_REPORT(p, DIAG_MARKUP_ELSE_AFTER_ELSE, arm_at, "`{else if}` cannot follow `{else}`");
          }
          branch = ast_markup_branch(p->a, arm_at);
          branch->cond = parse_expr(p);
          (void)expect(p, TOK_CTRL_END, "`}` to close the directive");
          parse_markup_children(p, &branch->body, tag);
          markup_branch_list_push(&node->as.if_.branches, branch);
          continue;
        }
        if (seen_else) {
          P_REPORT(p, DIAG_MARKUP_ELSE_AFTER_ELSE, arm_at, "a `{if}` chain has one `{else}`");
        }
        seen_else = true;
        branch = ast_markup_branch(p->a, arm_at);
        (void)expect(p, TOK_CTRL_END, "`}` to close `{else}`");
        parse_markup_children(p, &branch->body, tag);
        markup_branch_list_push(&node->as.if_.branches, branch);
        continue;
      }
      if (check(p, TOK_IDENT) && slice_eq_cstr(tok_text(p, p->tok), "end")) {
        advance(p);
        node->at = span_join(start, p->tok.at);
        (void)expect(p, TOK_CTRL_END, "`}` to close `{end}`");
        *closed_block = true;
        return node;
      }
      P_REPORT(p, DIAG_MARKUP_UNKNOWN_DIRECTIVE, arm_at,
               "expected `{else}`, `{else if}`, or `{end}`");
      return node;
    }
    P_REPORT(p, DIAG_MARKUP_MISSING_END, start, "this `{if}` has no `{end}`");
    return node;
  }

  if (check(p, TOK_KW_FOR)) {
    advance(p);
    node = ast_markup(p->a, MARKUP_FOR, start);
    if (check(p, TOK_IDENT)) {
      node->as.for_.first_name = tok_text(p, p->tok);
      advance(p);
    } else {
      unexpected(p, "a loop variable");
    }
    if (match(p, TOK_COMMA)) {
      node->as.for_.has_second = true;
      if (check(p, TOK_IDENT)) {
        node->as.for_.second_name = tok_text(p, p->tok);
        advance(p);
      } else {
        unexpected(p, "a second loop variable");
      }
    }
    (void)expect(p, TOK_KW_IN, "`in` before the collection");
    node->as.for_.iter = parse_expr(p);
    (void)expect(p, TOK_CTRL_END, "`}` to close the directive");
    parse_markup_children(p, &node->as.for_.body, tag);

    while (check(p, TOK_CTRL_START)) {
      span arm_at = p->tok.at;
      advance(p);
      if (check(p, TOK_KW_ELSE)) {
        advance(p);
        if (node->as.for_.has_empty) {
          P_REPORT(p, DIAG_MARKUP_ELSE_AFTER_ELSE, arm_at, "a `{for}` has one `{else}`");
        }
        node->as.for_.has_empty = true;
        (void)expect(p, TOK_CTRL_END, "`}` to close `{else}`");
        parse_markup_children(p, &node->as.for_.empty, tag);
        continue;
      }
      if (check(p, TOK_IDENT) && slice_eq_cstr(tok_text(p, p->tok), "end")) {
        advance(p);
        node->at = span_join(start, p->tok.at);
        (void)expect(p, TOK_CTRL_END, "`}` to close `{end}`");
        *closed_block = true;
        return node;
      }
      P_REPORT(p, DIAG_MARKUP_UNKNOWN_DIRECTIVE, arm_at, "expected `{else}` or `{end}`");
      return node;
    }
    P_REPORT(p, DIAG_MARKUP_MISSING_END, start, "this `{for}` has no `{end}`");
    return node;
  }

  if (check(p, TOK_KW_ELSE) || (check(p, TOK_IDENT) && slice_eq_cstr(tok_text(p, p->tok), "end"))) {
    /* The caller's loop handles these; reaching here means one appeared with no
     * `{if}` or `{for}` open. */
    P_REPORT(p, DIAG_MARKUP_ELSE_WITHOUT_IF, start,
             "`{else}` and `{end}` need an open `{if}` or `{for}`");
    advance(p);
    (void)match(p, TOK_CTRL_END);
    return ast_markup(p->a, MARKUP_TEXT, start);
  }

  P_REPORT(p, DIAG_MARKUP_UNKNOWN_DIRECTIVE, start,
           "a markup directive is `{if}`, `{else}`, `{for}`, or `{end}`");
  while (!check(p, TOK_CTRL_END) && !at_eof(p)) {
    advance(p);
  }
  (void)match(p, TOK_CTRL_END);
  return ast_markup(p->a, MARKUP_TEXT, start);
}

static void parse_markup_children(parser *p, markup_list *out, slice tag) {
  for (;;) {
    if (at_eof(p)) {
      return;
    }
    if (check(p, TOK_TAG_CLOSE_START)) {
      return;
    }
    if (check(p, TOK_MARKUP_TEXT)) {
      markup_node *n = ast_markup(p->a, MARKUP_TEXT, p->tok.at);
      n->as.text = tok_text(p, p->tok);
      markup_list_push(out, n);
      advance(p);
      continue;
    }
    if (check(p, TOK_MARKUP_COMMENT)) {
      markup_node *n = ast_markup(p->a, MARKUP_COMMENT, p->tok.at);
      n->as.text = tok_text(p, p->tok);
      markup_list_push(out, n);
      advance(p);
      continue;
    }
    if (check(p, TOK_INTERP_START)) {
      markup_node *n = ast_markup(p->a, MARKUP_INTERP, p->tok.at);
      advance(p);
      n->as.interp = parse_expr(p);
      n->at = span_join(n->at, p->tok.at);
      (void)expect(p, TOK_INTERP_END, "`}` to close the interpolation");
      markup_list_push(out, n);
      continue;
    }
    if (check(p, TOK_MARKUP_START)) {
      /* Inside content the lexer entered the nested tag itself, so the parser must
       * not open it again. */
      markup_list_push(out, parse_markup_element(p, false));
      continue;
    }
    if (check(p, TOK_CTRL_START)) {
      token peeked = lex_peek(p->lx);
      bool closed = false;
      markup_node *n;

      /* An `{else}` or `{end}` belongs to the enclosing control block. */
      if (peeked.kind == TOK_KW_ELSE) {
        return;
      }
      if (peeked.kind == TOK_IDENT && slice_eq_cstr(lex_text(p->lx, peeked), "end")) {
        return;
      }
      n = parse_markup_ctrl(p, tag, &closed);
      markup_list_push(out, n);
      continue;
    }
    return;
  }
}

static markup_node *parse_markup_element(parser *p, bool entering) {
  span open = p->tok.at;
  markup_node *node = ast_markup(p->a, MARKUP_ELEMENT, open);
  bool saved_no_struct = p->no_struct;

  if (!depth_enter(p)) {
    return node;
  }
  p->no_struct = false;

  if (entering) {
    lex_open_markup(p->lx, open);
  }
  advance(p); /* past TOK_MARKUP_START, now inside the tag */

  if (check(p, TOK_TAG_NAME)) {
    node->as.element.tag = tok_text(p, p->tok);
    advance(p);
  } else {
    unexpected(p, "a tag name");
  }

  /* Attributes. */
  for (;;) {
    if (check(p, TOK_ATTR_NAME)) {
      markup_attr *a = ast_markup_attr(p->a, p->tok.at);
      a->name = tok_text(p, p->tok);
      if (attr_name_taken(&node->as.element.attrs, a->name)) {
        P_REPORT(p, DIAG_MARKUP_DUPLICATE_ATTR, p->tok.at, "`%.*s` is set twice on this element",
                 (int)a->name.n, a->name.p);
      }
      advance(p);
      if (check(p, TOK_ATTR_EQ)) {
        advance(p);
        if (check(p, TOK_STR_START)) {
          a->value = parse_string(p);
        } else if (check(p, TOK_INTERP_START)) {
          advance(p);
          a->value = parse_expr(p);
          (void)expect(p, TOK_INTERP_END, "`}` to close the interpolation");
        } else {
          P_REPORT(p, DIAG_MARKUP_BAD_ATTR_VALUE, p->tok.at,
                   "an attribute value must be a quoted string or `${...}`");
        }
      }
      if (a->value != NULL) {
        a->at = span_join(a->at, a->value->at);
      }
      markup_attr_list_push(&node->as.element.attrs, a);
      continue;
    }
    if (check(p, TOK_ELLIPSIS)) {
      markup_attr *a = ast_markup_attr(p->a, p->tok.at);

      a->is_spread = true;
      advance(p);
      /* Inside a tag the lexer classifies an identifier run as an attribute name,
       * so a bare spread source arrives as TOK_ATTR_NAME. Anything more than a name
       * is written `...${expr}`, which pushes an expression interior and therefore
       * lexes as ordinary code. */
      if (check(p, TOK_ATTR_NAME)) {
        expr *name = ast_expr(p->a, EXPR_IDENT, p->tok.at);

        path_push(&name->as.ident, ast_path_seg(p->a, tok_text(p, p->tok), p->tok.at));
        a->value = name;
        advance(p);
      } else if (check(p, TOK_INTERP_START)) {
        advance(p);
        a->value = parse_expr(p);
        (void)expect(p, TOK_INTERP_END, "`}` to close the interpolation");
      } else {
        P_REPORT(p, DIAG_MARKUP_BAD_ATTR_VALUE, p->tok.at, "a spread takes a name or `${...}`");
      }
      if (a->value != NULL) {
        a->at = span_join(a->at, a->value->at);
      }
      markup_attr_list_push(&node->as.element.attrs, a);
      continue;
    }
    break;
  }

  if (check(p, TOK_TAG_SELF_CLOSE)) {
    node->as.element.self_closing = true;
    node->at = span_join(open, p->tok.at);
    advance(p);
    p->no_struct = saved_no_struct;
    depth_leave(p);
    return node;
  }

  if (!check(p, TOK_TAG_END)) {
    unexpected(p, "`>` or `/>` to close the tag");
    node->at = span_join(open, p->tok.at);
    p->no_struct = saved_no_struct;
    depth_leave(p);
    return node;
  }
  advance(p); /* past `>`, now in content */

  parse_markup_children(p, &node->as.element.children, node->as.element.tag);

  /* parse_markup_children stops at a control directive it does not own, which means
   * an `{else}` or `{end}` with no `{if}` or `{for}` open. */
  while (check(p, TOK_CTRL_START)) {
    P_REPORT(p, DIAG_MARKUP_ELSE_WITHOUT_IF, p->tok.at,
             "`{else}` and `{end}` need an open `{if}` or `{for}`");
    advance(p);
    while (!check(p, TOK_CTRL_END) && !at_eof(p)) {
      advance(p);
    }
    (void)match(p, TOK_CTRL_END);
    parse_markup_children(p, &node->as.element.children, node->as.element.tag);
  }

  if (check(p, TOK_TAG_CLOSE_START)) {
    span close_at = p->tok.at;
    advance(p);
    if (check(p, TOK_TAG_NAME)) {
      slice name = tok_text(p, p->tok);
      if (!slice_eq(name, node->as.element.tag)) {
        diag *d = NULL;
        if (p->sink != NULL) {
          d = diag_report(p->sink, DIAG_MARKUP_TAG_MISMATCH, p->src, p->tok.at,
                          "closing `%.*s` does not match the open tag", (int)name.n, name.p);
          diag_label_add(p->sink, d, p->src, open, "`%.*s` opened here",
                         (int)node->as.element.tag.n, node->as.element.tag.p);
        }
      }
      if (is_void_element(node->as.element.tag)) {
        P_REPORT(p, DIAG_MARKUP_VOID_WITH_CLOSE, close_at,
                 "`%.*s` is a void element and takes no closing tag", (int)node->as.element.tag.n,
                 node->as.element.tag.p);
      }
      advance(p);
    }
    node->at = span_join(open, p->tok.at);
    (void)expect(p, TOK_TAG_END, "`>` to close the closing tag");
  }

  p->no_struct = saved_no_struct;
  depth_leave(p);
  return node;
}

/* ---- attributes -------------------------------------------------------- */

static const char *const known_attrs[] = {"len",          "min",    "max",   "one_of",
                                          "email",        "url",    "trim",  "max_size",
                                          "content_type", "before", "after", "deprecated"};

static attr_list parse_attrs(parser *p) {
  attr_list list;

  memset(&list, 0, sizeof(list));
  while (check(p, TOK_AT)) {
    span start = p->tok.at;
    attr *a;
    slice name;
    size_t i;
    bool known = false;

    advance(p);
    if (!check(p, TOK_IDENT)) {
      unexpected(p, "an attribute name after `@`");
      break;
    }
    name = tok_text(p, p->tok);
    a = ast_attr(p->a, name, span_join(start, p->tok.at));
    advance(p);

    for (i = 0; i < sizeof(known_attrs) / sizeof(known_attrs[0]); i++) {
      if (slice_eq_cstr(name, known_attrs[i])) {
        known = true;
        break;
      }
    }
    if (!known) {
      P_REPORT(p, DIAG_UNKNOWN_ATTRIBUTE, a->at,
               "`@%.*s` is not one of the twelve built-in attributes", (int)name.n, name.p);
    }

    if (check(p, TOK_LPAREN)) {
      advance(p);
      skip_newlines(p);
      while (!check(p, TOK_RPAREN) && !at_eof(p)) {
        expr_list_push(&a->args, parse_expr(p));
        skip_newlines(p);
        if (!match(p, TOK_COMMA)) {
          break;
        }
        skip_newlines(p);
      }
      a->at = span_join(a->at, p->tok.at);
      (void)expect(p, TOK_RPAREN, "`)` to close the attribute arguments");
    }
    attr_list_push(&list, a);
    skip_newlines(p);
  }
  return list;
}

/* ---- statements -------------------------------------------------------- */

typedef struct {
  token_kind tok;
  assign_op op;
} assign_row;

static const assign_row assign_table[] = {
    {TOK_EQ, ASSIGN_SET},      {TOK_PLUS_EQ, ASSIGN_ADD},  {TOK_MINUS_EQ, ASSIGN_SUB},
    {TOK_STAR_EQ, ASSIGN_MUL}, {TOK_SLASH_EQ, ASSIGN_DIV}, {TOK_PERCENT_EQ, ASSIGN_MOD},
};

static bool assign_op_of(token_kind k, assign_op *out) {
  size_t i;

  for (i = 0; i < sizeof(assign_table) / sizeof(assign_table[0]); i++) {
    if (assign_table[i].tok == k) {
      *out = assign_table[i].op;
      return true;
    }
  }
  return false;
}

/* Parses an expression that will be followed by a block, so a `{` is the block
 * rather than a struct literal. */
static expr *parse_cond(parser *p) {
  bool saved = p->no_struct;
  expr *e;

  p->no_struct = true;
  e = parse_expr(p);
  p->no_struct = saved;
  return e;
}

static stmt *parse_let_stmt(parser *p) {
  span start = p->tok.at;
  bool is_var = check(p, TOK_KW_VAR);
  stmt *s = ast_stmt(p->a, STMT_LET, start);

  advance(p);
  s->as.let.is_var = is_var;
  if (check(p, TOK_IDENT)) {
    s->as.let.name = tok_text(p, p->tok);
    advance(p);
  } else {
    unexpected(p, "a name");
  }
  if (match(p, TOK_COLON)) {
    s->as.let.type = parse_type(p);
  }
  (void)expect(p, TOK_EQ, "`=` and an initial value");
  s->as.let.value = parse_expr(p);
  s->at = span_join(start, s->as.let.value->at);
  return s;
}

static stmt *parse_if_stmt(parser *p) {
  span start = p->tok.at;
  stmt *s = ast_stmt(p->a, STMT_IF, start);

  advance(p);
  s->as.if_.cond = parse_cond(p);
  s->as.if_.then_body = parse_block(p);
  s->at = span_join(start, p->tok.at);
  if (check(p, TOK_KW_ELSE)) {
    advance(p);
    s->as.if_.has_else = true;
    if (check(p, TOK_KW_IF)) {
      s->as.if_.else_if = parse_if_stmt(p);
      s->at = span_join(start, s->as.if_.else_if->at);
    } else {
      s->as.if_.else_body = parse_block(p);
      s->at = span_join(start, p->tok.at);
    }
  }
  return s;
}

static stmt *parse_match_stmt(parser *p) {
  span start = p->tok.at;
  stmt *s = ast_stmt(p->a, STMT_MATCH, start);

  advance(p);
  s->as.match.value = parse_cond(p);
  (void)expect(p, TOK_LBRACE, "`{` to open the match arms");
  skip_newlines(p);
  while (!check(p, TOK_RBRACE) && !at_eof(p)) {
    match_arm *arm = ast_match_arm(p->a, p->tok.at);

    if (check(p, TOK_KW_ELSE)) {
      advance(p);
    } else {
      arm->pat = parse_pattern(p);
    }
    (void)expect(p, TOK_ARROW, "`->` after the pattern");
    if (check(p, TOK_LBRACE)) {
      arm->has_block = true;
      arm->body = parse_block(p);
    } else {
      arm->value = parse_expr(p);
      arm->at = span_join(arm->at, arm->value->at);
    }
    match_arm_list_push(&s->as.match.arms, arm);
    skip_newlines(p);
  }
  s->at = span_join(start, p->tok.at);
  (void)expect(p, TOK_RBRACE, "`}` to close the match");
  return s;
}

static stmt *parse_stmt(parser *p) {
  span start = p->tok.at;
  stmt *s;

  if (check(p, TOK_KW_LET) || check(p, TOK_KW_VAR)) {
    return parse_let_stmt(p);
  }
  if (check(p, TOK_KW_IF)) {
    return parse_if_stmt(p);
  }
  if (check(p, TOK_KW_MATCH)) {
    return parse_match_stmt(p);
  }
  if (check(p, TOK_KW_WHILE)) {
    advance(p);
    s = ast_stmt(p->a, STMT_WHILE, start);
    s->as.while_.cond = parse_cond(p);
    p->loop_depth++;
    s->as.while_.body = parse_block(p);
    p->loop_depth--;
    s->at = span_join(start, p->tok.at);
    return s;
  }
  if (check(p, TOK_KW_FOR)) {
    advance(p);
    s = ast_stmt(p->a, STMT_FOR, start);
    if (check(p, TOK_IDENT)) {
      s->as.for_.first_name = tok_text(p, p->tok);
      advance(p);
    } else {
      unexpected(p, "a loop variable");
    }
    if (match(p, TOK_COMMA)) {
      s->as.for_.has_second = true;
      if (check(p, TOK_IDENT)) {
        s->as.for_.second_name = tok_text(p, p->tok);
        advance(p);
      } else {
        unexpected(p, "a second loop variable");
      }
    }
    (void)expect(p, TOK_KW_IN, "`in` before the collection");
    s->as.for_.iter = parse_cond(p);
    p->loop_depth++;
    s->as.for_.body = parse_block(p);
    p->loop_depth--;
    s->at = span_join(start, p->tok.at);
    return s;
  }
  if (check(p, TOK_KW_RETURN)) {
    advance(p);
    s = ast_stmt(p->a, STMT_RETURN, start);
    if (!at_stmt_end(p)) {
      s->as.ret = parse_expr(p);
      s->at = span_join(start, s->as.ret->at);
    }
    return s;
  }
  if (check(p, TOK_KW_BREAK)) {
    if (p->loop_depth == 0u) {
      P_REPORT(p, DIAG_BREAK_OUTSIDE_LOOP, start, "`break` is only valid inside a loop");
    }
    advance(p);
    return ast_stmt(p->a, STMT_BREAK, start);
  }
  if (check(p, TOK_KW_CONTINUE)) {
    if (p->loop_depth == 0u) {
      P_REPORT(p, DIAG_CONTINUE_OUTSIDE_LOOP, start, "`continue` is only valid inside a loop");
    }
    advance(p);
    return ast_stmt(p->a, STMT_CONTINUE, start);
  }
  if (check(p, TOK_KW_SEND)) {
    if (!p->in_stream) {
      P_REPORT(p, DIAG_SEND_OUTSIDE_STREAM, start, "`send` is only valid inside a `stream` body");
    }
    P_REPORT(p, DIAG_FEATURE_UNAVAILABLE, start,
             "`send` lands in v0.2 together with `stream` and SSE");
    advance(p);
    s = ast_stmt(p->a, STMT_SEND, start);
    s->as.send.value = parse_expr(p);
    if (match(p, TOK_COMMA)) {
      s->as.send.name = s->as.send.value;
      s->as.send.value = parse_expr(p);
    }
    s->at = span_join(start, s->as.send.value->at);
    return s;
  }
  if (check(p, TOK_KW_SPAWN)) {
    P_REPORT(p, DIAG_FEATURE_UNAVAILABLE, start, "`spawn` lands in v0.2 together with tasks");
    advance(p);
    s = ast_stmt(p->a, STMT_SPAWN, start);
    s->as.spawn = parse_expr(p);
    if (s->as.spawn->kind != EXPR_CALL) {
      P_REPORT(p, DIAG_SPAWN_NEEDS_CALL, s->as.spawn->at, "`spawn` takes a function call");
    }
    s->at = span_join(start, s->as.spawn->at);
    return s;
  }
  if (check(p, TOK_KW_DEFER)) {
    advance(p);
    s = ast_stmt(p->a, STMT_DEFER, start);
    s->as.defer = parse_expr(p);
    if (s->as.defer->kind != EXPR_CALL) {
      P_REPORT(p, DIAG_DEFER_NEEDS_CALL, s->as.defer->at, "`defer` takes a function call");
    }
    s->at = span_join(start, s->as.defer->at);
    return s;
  }

  {
    expr *e = parse_expr(p);
    assign_op op;

    if (assign_op_of(p->tok.kind, &op)) {
      advance(p);
      s = ast_stmt(p->a, STMT_ASSIGN, e->at);
      s->as.assign.op = op;
      s->as.assign.target = e;
      s->as.assign.value = parse_expr(p);
      s->at = span_join(e->at, s->as.assign.value->at);
      return s;
    }
    s = ast_stmt(p->a, STMT_EXPR, e->at);
    s->as.expression = e;
    return s;
  }
}

static stmt_list parse_block(parser *p) {
  stmt_list list;

  memset(&list, 0, sizeof(list));
  if (!depth_enter(p)) {
    return list;
  }
  if (!expect(p, TOK_LBRACE, "`{` to open the block")) {
    depth_leave(p);
    return list;
  }
  skip_newlines(p);
  while (!check(p, TOK_RBRACE) && !at_eof(p)) {
    uint32_t before = p->tok.at.start;
    stmt *s = parse_stmt(p);

    stmt_list_push(&list, s);
    expect_stmt_end(p);
    skip_newlines(p);
    if (!check(p, TOK_RBRACE)) {
      ensure_progress(p, before);
    }
  }
  (void)expect(p, TOK_RBRACE, "`}` to close the block");
  depth_leave(p);
  return list;
}

/* ---- declarations ------------------------------------------------------ */

static bool param_name_taken(const param_list *list, slice name) {
  const param *q;

  for (q = list->first; q != NULL; q = q->next) {
    if (slice_eq(q->name, name)) {
      return true;
    }
  }
  return false;
}

static param_list parse_params(parser *p) {
  param_list list;

  memset(&list, 0, sizeof(list));
  skip_newlines(p);
  while (!check(p, TOK_RPAREN) && !at_eof(p)) {
    param *q = ast_param(p->a, p->tok.at);

    if (check(p, TOK_KW_SELF)) {
      q->is_self = true;
      q->name = tok_text(p, p->tok);
      advance(p);
    } else if (check(p, TOK_IDENT)) {
      q->name = tok_text(p, p->tok);
      advance(p);
      (void)expect(p, TOK_COLON, "`:` and a type");
      q->type = parse_type(p);
      q->at = span_join(q->at, q->type->at);
      if (match(p, TOK_EQ)) {
        q->dflt = parse_expr(p);
        q->at = span_join(q->at, q->dflt->at);
      }
      q->attrs = parse_attrs(p);
    } else {
      unexpected(p, "a parameter");
      break;
    }
    if (param_name_taken(&list, q->name)) {
      P_REPORT(p, DIAG_DUPLICATE_PARAM, q->at, "`%.*s` is declared twice", (int)q->name.n,
               q->name.p);
    }
    param_list_push(&list, q);
    skip_newlines(p);
    if (!match(p, TOK_COMMA)) {
      break;
    }
    skip_newlines(p);
  }
  skip_newlines(p);
  return list;
}

static decl *parse_fn_decl(parser *p, span start) {
  decl *d = ast_decl(p->a, DECL_FN, start);
  bool saved_method = p->in_method;

  advance(p); /* past `fn` */
  if (check(p, TOK_IDENT)) {
    slice first = tok_text(p, p->tok);
    advance(p);
    if (check(p, TOK_DOT)) {
      advance(p);
      d->as.fn.has_recv = true;
      d->as.fn.recv = first;
      if (at_name(p)) {
        d->as.fn.name = tok_text(p, p->tok);
        advance(p);
      } else {
        unexpected(p, "a method name");
      }
    } else {
      d->as.fn.name = first;
    }
  } else {
    unexpected(p, "a function name");
  }

  p->in_method = d->as.fn.has_recv;
  (void)expect(p, TOK_LPAREN, "`(` to open the parameter list");
  d->as.fn.params = parse_params(p);
  (void)expect(p, TOK_RPAREN, "`)` to close the parameter list");

  /* Grammar rule 9, discharged here because it needs only syntax (D064). */
  if (d->as.fn.has_recv && (d->as.fn.params.first == NULL || !d->as.fn.params.first->is_self)) {
    P_REPORT(p, DIAG_METHOD_NEEDS_SELF, d->at, "a method's first parameter must be `self`");
  }
  if (!d->as.fn.has_recv && d->as.fn.params.first != NULL && d->as.fn.params.first->is_self) {
    P_REPORT(p, DIAG_METHOD_NEEDS_SELF, d->as.fn.params.first->at,
             "`self` is only a parameter of a method, written `fn Type.name(self)`");
  }

  if (match(p, TOK_ARROW)) {
    d->as.fn.ret = parse_type(p);
    d->as.fn.fallible = match(p, TOK_BANG);
  }
  d->as.fn.body = parse_block(p);
  d->at = span_join(start, p->tok.at);
  p->in_method = saved_method;
  return d;
}

static decl *parse_type_decl(parser *p, span start) {
  decl *d;
  slice name;

  advance(p); /* past `type` */
  if (!check(p, TOK_IDENT)) {
    unexpected(p, "a type name");
    return ast_decl(p->a, DECL_ALIAS, start);
  }
  name = tok_text(p, p->tok);
  advance(p);

  if (check(p, TOK_KW_ENUM)) {
    advance(p);
    d = ast_decl(p->a, DECL_ENUM, start);
    d->as.enum_.name = name;
    (void)expect(p, TOK_LBRACE, "`{` to open the variants");
    skip_newlines(p);
    while (!check(p, TOK_RBRACE) && !at_eof(p)) {
      if (!at_name(p)) {
        unexpected(p, "a variant name");
        break;
      }
      {
        slice vname = tok_text(p, p->tok);
        const variant *seen;
        bool dup = false;

        for (seen = d->as.enum_.variants.first; seen != NULL; seen = seen->next) {
          if (slice_eq(seen->name, vname)) {
            dup = true;
            break;
          }
        }
        if (dup) {
          P_REPORT(p, DIAG_DUPLICATE_VARIANT, p->tok.at, "`%.*s` is declared twice", (int)vname.n,
                   vname.p);
        }
        variant_list_push(&d->as.enum_.variants, ast_variant(p->a, vname, p->tok.at));
      }
      advance(p);
      skip_newlines(p);
      if (!match(p, TOK_COMMA)) {
        break;
      }
      skip_newlines(p);
    }
    skip_newlines(p);
    d->at = span_join(start, p->tok.at);
    (void)expect(p, TOK_RBRACE, "`}` to close the variants");
    return d;
  }

  if (check(p, TOK_EQ)) {
    advance(p);
    d = ast_decl(p->a, DECL_ALIAS, start);
    d->as.alias.name = name;
    d->as.alias.target = parse_type(p);
    d->at = span_join(start, d->as.alias.target->at);
    return d;
  }

  d = ast_decl(p->a, DECL_STRUCT, start);
  d->as.struct_.name = name;
  (void)expect(p, TOK_LBRACE, "`{` to open the fields");
  skip_newlines(p);
  while (!check(p, TOK_RBRACE) && !at_eof(p)) {
    field *f;
    slice fname;
    const field *seen;
    bool dup = false;

    if (!at_name(p)) {
      unexpected(p, "a field name");
      break;
    }
    fname = tok_text(p, p->tok);
    for (seen = d->as.struct_.fields.first; seen != NULL; seen = seen->next) {
      if (slice_eq(seen->name, fname)) {
        dup = true;
        break;
      }
    }
    if (dup) {
      P_REPORT(p, DIAG_DUPLICATE_FIELD, p->tok.at, "`%.*s` is declared twice", (int)fname.n,
               fname.p);
    }
    f = ast_field(p->a, fname, p->tok.at);
    advance(p);
    (void)expect(p, TOK_COLON, "`:` and a type");
    f->type = parse_type(p);
    f->at = span_join(f->at, f->type->at);
    if (match(p, TOK_EQ)) {
      f->dflt = parse_expr(p);
      f->at = span_join(f->at, f->dflt->at);
    }
    f->attrs = parse_attrs(p);
    field_list_push(&d->as.struct_.fields, f);
    (void)match(p, TOK_COMMA);
    skip_newlines(p);
  }
  d->at = span_join(start, p->tok.at);
  (void)expect(p, TOK_RBRACE, "`}` to close the fields");
  return d;
}

static decl *parse_route_decl(parser *p, span start, bool is_stream) {
  decl *d = ast_decl(p->a, is_stream ? DECL_STREAM : DECL_ROUTE, start);
  bool saved_stream = p->in_stream;

  if (is_stream) {
    P_REPORT(p, DIAG_FEATURE_UNAVAILABLE, start, "`stream` lands in v0.2, when SSE arrives");
  }
  advance(p); /* past `route` / `stream` */

  /* The HTTP method is a contextual word, matched by text (D061). */
  if (check(p, TOK_IDENT)) {
    slice m = tok_text(p, p->tok);
    if (slice_eq_cstr(m, "GET") || slice_eq_cstr(m, "POST") || slice_eq_cstr(m, "PUT") ||
        slice_eq_cstr(m, "PATCH") || slice_eq_cstr(m, "DELETE") || slice_eq_cstr(m, "HEAD") ||
        slice_eq_cstr(m, "OPTIONS")) {
      d->as.route.method = m;
      d->as.route.method_at = p->tok.at;
      advance(p);
    } else {
      P_SYNTAX(p, DIAG_UNEXPECTED_TOKEN, p->tok.at,
               "expected an HTTP method after `%s`, found `%.*s`", is_stream ? "stream" : "route",
               (int)m.n, m.p);
      advance(p);
    }
  } else {
    unexpected(p, "an HTTP method");
  }

  if (check(p, TOK_STR_START)) {
    expr *pat = parse_string(p);
    d->as.route.pattern_at = pat->at;
    if (pat->as.str.count == 1u && pat->as.str.first->value == NULL) {
      d->as.route.pattern = pat->as.str.first->text;
    } else if (pat->as.str.count != 0u) {
      P_REPORT(p, DIAG_UNEXPECTED_TOKEN, pat->at,
               "a route pattern is a plain string, so it cannot interpolate");
    }
  } else {
    unexpected(p, "a quoted path pattern");
  }

  (void)expect(p, TOK_LPAREN, "`(` to open the parameter list");
  d->as.route.params = parse_params(p);
  (void)expect(p, TOK_RPAREN, "`)` to close the parameter list");
  if (match(p, TOK_ARROW)) {
    d->as.route.ret = parse_type(p);
    d->as.route.fallible = match(p, TOK_BANG);
  }
  p->in_stream = is_stream;
  d->as.route.body = parse_block(p);
  p->in_stream = saved_stream;
  d->at = span_join(start, p->tok.at);
  return d;
}

static decl *parse_decl(parser *p);

static decl *parse_group_decl(parser *p, span start) {
  decl *d = ast_decl(p->a, DECL_GROUP, start);

  advance(p); /* past `group` */
  if (check(p, TOK_STR_START)) {
    expr *prefix = parse_string(p);
    if (prefix->as.str.count == 1u && prefix->as.str.first->value == NULL) {
      d->as.group.prefix = prefix->as.str.first->text;
    }
  } else {
    unexpected(p, "a quoted path prefix");
  }
  (void)expect(p, TOK_LBRACE, "`{` to open the group");
  skip_newlines(p);
  while (!check(p, TOK_RBRACE) && !at_eof(p)) {
    uint32_t before = p->tok.at.start;
    decl *item = parse_decl(p);

    if (item != NULL) {
      if (item->kind != DECL_ROUTE && item->kind != DECL_STREAM) {
        P_REPORT(p, DIAG_UNEXPECTED_TOKEN, item->at, "a group contains routes and streams only");
      }
      decl_list_push(&d->as.group.items, item);
    }
    skip_newlines(p);
    if (!check(p, TOK_RBRACE)) {
      ensure_progress(p, before);
    }
  }
  d->at = span_join(start, p->tok.at);
  (void)expect(p, TOK_RBRACE, "`}` to close the group");
  return d;
}

static decl *parse_decl(parser *p) {
  attr_list attrs = parse_attrs(p);
  span start = p->tok.at;
  bool is_pub = false;
  decl *d;

  if (check(p, TOK_KW_PUB)) {
    is_pub = true;
    advance(p);
  }

  if (check(p, TOK_KW_FN)) {
    d = parse_fn_decl(p, start);
  } else if (check(p, TOK_KW_TYPE)) {
    d = parse_type_decl(p, start);
  } else if (check(p, TOK_KW_LET) || check(p, TOK_KW_VAR)) {
    stmt *s;
    bool is_var = check(p, TOK_KW_VAR);

    if (is_var) {
      /* Grammar rule 1: there are no mutable globals (D008). */
      P_REPORT(p, DIAG_TOPLEVEL_MUST_BE_LET, start,
               "a top-level binding must be `let`; state belongs in SQLite, in request "
               "scope, or in a `cache` cell");
    }
    s = parse_let_stmt(p);
    d = ast_decl(p->a, DECL_LET, s->at);
    d->as.let.is_var = is_var;
    d->as.let.name = s->as.let.name;
    d->as.let.type = s->as.let.type;
    d->as.let.value = s->as.let.value;
  } else if (check(p, TOK_KW_ROUTE)) {
    d = parse_route_decl(p, start, false);
  } else if (check(p, TOK_KW_STREAM)) {
    d = parse_route_decl(p, start, true);
  } else if (check(p, TOK_KW_GROUP)) {
    d = parse_group_decl(p, start);
  } else if (check(p, TOK_KW_TEST)) {
    advance(p);
    d = ast_decl(p->a, DECL_TEST, start);
    if (check(p, TOK_STR_START)) {
      expr *name = parse_string(p);
      if (name->as.str.count == 1u && name->as.str.first->value == NULL) {
        d->as.test.name = name->as.str.first->text;
      }
    } else {
      unexpected(p, "a quoted test name");
    }
    d->as.test.body = parse_block(p);
    d->at = span_join(start, p->tok.at);
  } else if (check(p, TOK_RESERVED)) {
    slice word = tok_text(p, p->tok);
    const char *help = token_reserved_help(word);

    P_REPORT(p, DIAG_RESERVED_WORD, start, "`%.*s` is reserved: %s", (int)word.n, word.p,
             help != NULL ? help : "it has no meaning in doot");
    advance(p);
    synchronize(p);
    return NULL;
  } else {
    unexpected(p, "a declaration");
    synchronize(p);
    return NULL;
  }

  if (is_pub) {
    switch (d->kind) {
    case DECL_FN:
    case DECL_STRUCT:
    case DECL_ENUM:
    case DECL_ALIAS:
    case DECL_LET:
      d->is_pub = true;
      break;
    case DECL_ROUTE:
    case DECL_STREAM:
    case DECL_GROUP:
    case DECL_TEST:
    case DECL_KIND_COUNT:
      P_REPORT(p, DIAG_PUB_NOT_ALLOWED, start,
               "`pub` marks a function, type, or binding as exported; a %s is reachable by "
               "its path already",
               decl_kind_name(d->kind));
      break;
    }
  }
  d->attrs = attrs;
  return d;
}

/* ---- entry point ------------------------------------------------------- */

unit_ast *parse_unit(arena *a, const source *src, diag_sink *sink) {
  parser p;
  unit_ast *unit;

  DOOT_ASSERT(a != NULL && src != NULL);
  unit = ARENA_NEW(a, unit_ast);
  DOOT_ASSERT(unit != NULL);
  unit->src = src;

  memset(&p, 0, sizeof(p));
  p.a = a;
  p.src = src;
  p.sink = sink;
  p.unit = unit;
  p.lx = lex_new(a, src, sink, &unit->comments);
  DOOT_ASSERT(p.lx != NULL);

  advance(&p);
  skip_newlines(&p);
  while (!at_eof(&p)) {
    uint32_t before = p.tok.at.start;
    decl *d = parse_decl(&p);

    if (d != NULL) {
      decl_list_push(&unit->decls, d);
      expect_stmt_end(&p);
    }
    skip_newlines(&p);
    ensure_progress(&p, before);
  }
  return unit;
}
