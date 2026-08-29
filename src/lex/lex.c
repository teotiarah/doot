#include "lex.h"

#include "../base/assert.h"
#include "../base/diag.h"

/* ---- state ------------------------------------------------------------- */

typedef enum { LEX_NORMAL, LEX_STR, LEX_MARKUP_TAG, LEX_MARKUP_CONTENT } lex_mode;

/* One entry per nesting level. The fields beyond `mode` are what let four modes
 * cover every context: an interior remembers how to close, a tag remembers
 * whether it is a closing tag and whether its name has been seen. */
typedef struct {
  lex_mode mode;
  uint32_t open_at; /* offset that opened this level, for its diagnostic */
  uint32_t depth;   /* LEX_NORMAL: brace nesting inside an interior */
  token_kind close; /* LEX_NORMAL: INTERP_END or CTRL_END; TOK_EOF at the base */
  bool close_tag;   /* LEX_MARKUP_TAG: `>` pops instead of entering content */
  bool named;       /* LEX_MARKUP_TAG: the tag name has been emitted */
} lex_frame;

struct lexer {
  arena *a;
  const source *src;
  diag_sink *sink;
  lex_comments *comments;

  const char *p;
  uint32_t n;
  uint32_t i;

  /* Two distinct one-token slots. `stash` holds the significant token scanned
   * past a newline run to decide whether that newline survives; `peeked` holds a
   * token produced for lex_peek. They cannot share a slot: filling the peek slot
   * would clobber a stash written by the same call. */
  bool has_stash;
  token stash;
  bool has_peek;
  token peeked;

  bool has_prev;
  token_kind prev; /* previous significant token, for the line-structure rule */

  lex_frame stack[LEX_MAX_DEPTH];
  uint32_t sp;
  bool depth_reported;
};

/* ---- character classes ------------------------------------------------- */

static bool is_digit(char c) {
  return c >= '0' && c <= '9';
}

static bool is_hex(char c) {
  return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool is_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_word(char c) {
  return is_alpha(c) || is_digit(c);
}

static bool is_hspace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

/* ---- cursor ------------------------------------------------------------ */

static bool at_end(const lexer *lx) {
  return lx->i >= lx->n;
}

static char peek_at(const lexer *lx, uint32_t off) {
  uint32_t j = lx->i + off;
  return j < lx->n ? lx->p[j] : '\0';
}

static char cur(const lexer *lx) {
  return peek_at(lx, 0u);
}

static bool looking_at(const lexer *lx, const char *lit, uint32_t len) {
  uint32_t k;

  if (lx->i + len > lx->n) {
    return false;
  }
  for (k = 0; k < len; k++) {
    if (lx->p[lx->i + k] != lit[k]) {
      return false;
    }
  }
  return true;
}

/* The width of the UTF-8 character starting at the cursor. The source is already
 * validated (DT0001), so a lead byte's advertised width is trustworthy; this is
 * only used to make a diagnostic caret cover a whole character. */
static uint32_t char_width(const lexer *lx) {
  unsigned char c;
  uint32_t w = 1u;

  if (at_end(lx)) {
    return 0u;
  }
  c = (unsigned char)cur(lx);
  if (c >= 0xf0u) {
    w = 4u;
  } else if (c >= 0xe0u) {
    w = 3u;
  } else if (c >= 0xc0u) {
    w = 2u;
  }
  if (lx->i + w > lx->n) {
    w = lx->n - lx->i;
  }
  return w == 0u ? 1u : w;
}

/* ---- frames ------------------------------------------------------------ */

static lex_frame *top(lexer *lx) {
  return &lx->stack[lx->sp];
}

/* A macro rather than a function so the format string stays a literal at the call
 * site, which is what -Wformat=2 needs in order to check it. The sink is optional
 * (lex_new documents NULL as "scan silently"), and diag_report requires one, so
 * the guard lives here instead of at every call. */
#define LEX_REPORT(lx, code, at, ...)                                                              \
  do {                                                                                             \
    if ((lx)->sink != NULL) {                                                                      \
      (void)diag_report((lx)->sink, (code), (lx)->src, (at), __VA_ARGS__);                         \
    }                                                                                              \
  } while (0)

static bool push_frame(lexer *lx, lex_mode mode, uint32_t open_at, token_kind close) {
  lex_frame *f;

  if (lx->sp + 1u >= LEX_MAX_DEPTH) {
    if (!lx->depth_reported) {
      lx->depth_reported = true;
      LEX_REPORT(lx, DIAG_NESTING_TOO_DEEP, span_make(open_at, open_at + 1u),
                 "nesting is deeper than 64 levels");
    }
    return false;
  }
  lx->sp++;
  f = top(lx);
  f->mode = mode;
  f->open_at = open_at;
  f->depth = 0u;
  f->close = close;
  f->close_tag = false;
  f->named = false;
  return true;
}

static void pop_frame(lexer *lx) {
  if (lx->sp > 0u) {
    lx->sp--;
  }
}

/* ---- comments ---------------------------------------------------------- */

static void record_comment(lexer *lx, span at, bool block) {
  lex_comment *c;

  if (lx->comments == NULL) {
    return;
  }
  c = ARENA_NEW(lx->a, lex_comment);
  if (c == NULL) {
    return;
  }
  c->at = at;
  c->block = block;
  c->next = NULL;
  if (lx->comments->last == NULL) {
    lx->comments->first = c;
  } else {
    lx->comments->last->next = c;
  }
  lx->comments->last = c;
  lx->comments->count++;
}

/* Consumes `// ...`. The cursor is on the first slash. */
static void skip_line_comment(lexer *lx) {
  uint32_t start = lx->i;

  lx->i += 2u;
  while (!at_end(lx) && cur(lx) != '\n') {
    lx->i++;
  }
  record_comment(lx, span_make(start, lx->i), false);
}

/* Consumes a nesting block comment. The cursor is on the opening slash. An
 * unterminated one is reported at the outermost opener, so the span points at the
 * comment that actually failed to close rather than at end of input. */
static void skip_block_comment(lexer *lx) {
  uint32_t start = lx->i;
  uint32_t nest = 1u;

  lx->i += 2u;
  while (!at_end(lx) && nest > 0u) {
    if (looking_at(lx, "/*", 2u)) {
      nest++;
      lx->i += 2u;
    } else if (looking_at(lx, "*/", 2u)) {
      nest--;
      lx->i += 2u;
    } else {
      lx->i++;
    }
  }
  if (nest > 0u) {
    LEX_REPORT(lx, DIAG_UNTERMINATED_BLOCK_COMMENT, span_make(start, start + 2u),
               "this block comment is never closed");
  }
  record_comment(lx, span_make(start, lx->i), true);
}

/* ---- words ------------------------------------------------------------- */

/* Scans an identifier-shaped run and classifies it: keyword, then reserved, then
 * identifier (D061 keeps contextual words -- the HTTP methods and `end` -- in the
 * identifier bucket, to be matched by text in the parser). */
static token scan_word(lexer *lx) {
  uint32_t start = lx->i;
  token_kind kind;
  slice word;

  while (!at_end(lx) && is_word(cur(lx))) {
    lx->i++;
  }
  word = slice_make(lx->p + start, (size_t)(lx->i - start));
  if (token_keyword_lookup(word, &kind)) {
    return token_make(kind, span_make(start, lx->i));
  }
  if (token_reserved_lookup(word)) {
    return token_make(TOK_RESERVED, span_make(start, lx->i));
  }
  return token_make(TOK_IDENT, span_make(start, lx->i));
}

/* A tag or attribute name: IDENT ("-" IDENT)*. */
static bool scan_markup_name(lexer *lx) {
  if (at_end(lx) || !is_alpha(cur(lx))) {
    return false;
  }
  while (!at_end(lx) && is_word(cur(lx))) {
    lx->i++;
  }
  while (peek_at(lx, 0u) == '-' && is_word(peek_at(lx, 1u))) {
    lx->i++;
    while (!at_end(lx) && is_word(cur(lx))) {
      lx->i++;
    }
  }
  return true;
}

/* ---- numbers ----------------------------------------------------------- */

/* Underscores must sit between two digits. The grammar's (DIGIT | "_")* would
 * admit `1_` and `1__0`; DT0020 exists to reject them, so this is where that
 * rule is actually decided. */
static bool scan_digits(lexer *lx, bool (*ok)(char), uint32_t *count) {
  bool prev_us = true; /* a separator may not lead */
  bool bad_us = false;

  *count = 0u;
  while (!at_end(lx)) {
    char c = cur(lx);
    if (c == '_') {
      if (prev_us) {
        bad_us = true;
      }
      prev_us = true;
      lx->i++;
      continue;
    }
    if (!ok(c)) {
      break;
    }
    (*count)++;
    prev_us = false;
    lx->i++;
  }
  if (prev_us && *count > 0u) {
    bad_us = true; /* a separator may not trail */
  }
  return !bad_us;
}

static bool is_bin(char c) {
  return c == '0' || c == '1';
}

static token scan_number(lexer *lx) {
  uint32_t start = lx->i;
  uint32_t digits = 0u;
  bool clean;
  bool is_float = false;

  if (cur(lx) == '0' && (peek_at(lx, 1u) == 'x' || peek_at(lx, 1u) == 'b')) {
    bool hex = peek_at(lx, 1u) == 'x';
    lx->i += 2u;
    clean = scan_digits(lx, hex ? is_hex : is_bin, &digits);
    if (digits == 0u) {
      LEX_REPORT(lx, DIAG_MALFORMED_NUMBER, span_make(start, lx->i),
                 hex ? "a hexadecimal literal needs at least one digit after `0x`"
                     : "a binary literal needs at least one digit after `0b`");
    } else if (!clean) {
      LEX_REPORT(lx, DIAG_MISPLACED_UNDERSCORE, span_make(start, lx->i),
                 "`_` may only separate two digits");
    }
    return token_make(TOK_INT, span_make(start, lx->i));
  }

  /* `0X` / `0B` would otherwise lex as `0` followed by an identifier, which
   * produces a confusing error a long way from the cause. */
  if (cur(lx) == '0' && (peek_at(lx, 1u) == 'X' || peek_at(lx, 1u) == 'B')) {
    lx->i += 2u;
    (void)scan_digits(lx, is_hex, &digits);
    LEX_REPORT(lx, DIAG_MALFORMED_NUMBER, span_make(start, lx->i),
               "a radix prefix is lowercase: write `0x` or `0b`");
    return token_make(TOK_INT, span_make(start, lx->i));
  }

  clean = scan_digits(lx, is_digit, &digits);

  /* A `.` is part of the number only when a digit follows it. That is what keeps
   * `16.mb` a method call on an int rather than a malformed float. */
  if (cur(lx) == '.' && is_digit(peek_at(lx, 1u))) {
    uint32_t frac = 0u;
    is_float = true;
    lx->i++;
    if (!scan_digits(lx, is_digit, &frac)) {
      clean = false;
    }
  }

  if (cur(lx) == 'e' || cur(lx) == 'E') {
    uint32_t expo = 0u;
    lx->i++;
    if (cur(lx) == '+' || cur(lx) == '-') {
      lx->i++;
    }
    if (!scan_digits(lx, is_digit, &expo)) {
      clean = false;
    }
    if (expo == 0u) {
      LEX_REPORT(lx, DIAG_MALFORMED_NUMBER, span_make(start, lx->i),
                 "an exponent needs at least one digit");
    }
    is_float = true;
  }

  if (!clean) {
    LEX_REPORT(lx, DIAG_MISPLACED_UNDERSCORE, span_make(start, lx->i),
               "`_` may only separate two digits");
  }
  return token_make(is_float ? TOK_FLOAT : TOK_INT, span_make(start, lx->i));
}

/* ---- strings ----------------------------------------------------------- */

static token scan_raw_string(lexer *lx) {
  uint32_t start = lx->i;

  lx->i++;
  while (!at_end(lx) && cur(lx) != '`') {
    lx->i++;
  }
  if (at_end(lx)) {
    LEX_REPORT(lx, DIAG_UNTERMINATED_RAW_STRING, span_make(start, start + 1u),
               "this raw string is never closed");
    return token_make(TOK_RAW_STR, span_make(start, lx->i));
  }
  lx->i++;
  return token_make(TOK_RAW_STR, span_make(start, lx->i));
}

/* Inside a string literal. Text runs span raw bytes with escapes unresolved: the
 * parser resolves them, so it can report DT0014-DT0016 at the escape's own span
 * rather than at the opening quote (D058). */
static token scan_in_string(lexer *lx) {
  uint32_t start = lx->i;
  lex_frame *f = top(lx);

  if (at_end(lx) || cur(lx) == '\n') {
    LEX_REPORT(lx, DIAG_UNTERMINATED_STRING, span_make(f->open_at, f->open_at + 1u),
               "this string is never closed");
    pop_frame(lx);
    return token_make(TOK_STR_END, span_make(lx->i, lx->i));
  }
  if (cur(lx) == '"') {
    lx->i++;
    pop_frame(lx);
    return token_make(TOK_STR_END, span_make(start, lx->i));
  }
  if (cur(lx) == '$' && peek_at(lx, 1u) == '{') {
    lx->i += 2u;
    (void)push_frame(lx, LEX_NORMAL, start, TOK_INTERP_END);
    return token_make(TOK_INTERP_START, span_make(start, lx->i));
  }

  while (!at_end(lx) && cur(lx) != '"' && cur(lx) != '\n') {
    if (cur(lx) == '\\') {
      lx->i++;
      if (!at_end(lx) && cur(lx) != '\n') {
        lx->i++;
      }
      continue;
    }
    if (cur(lx) == '$' && peek_at(lx, 1u) == '{') {
      break;
    }
    lx->i++;
  }
  return token_make(TOK_STR_TEXT, span_make(start, lx->i));
}

/* ---- markup ------------------------------------------------------------ */

/* Conditions 2 and 3 of the `<` rule, both decidable from bytes alone: followed
 * immediately by a letter, `_`, or `/`, and forming a tag name terminated by
 * whitespace, `>`, `/`, or `=`. Condition 1 -- expression position -- is the
 * parser's (D059). */
static bool markup_ahead(const lexer *lx) {
  uint32_t j = lx->i + 1u;

  if (j >= lx->n) {
    return false;
  }
  if (lx->p[j] == '/') {
    return true;
  }
  if (!is_alpha(lx->p[j])) {
    return false;
  }
  while (j < lx->n && (is_word(lx->p[j]) || lx->p[j] == '-')) {
    j++;
  }
  if (j >= lx->n) {
    return false;
  }
  return is_hspace(lx->p[j]) || lx->p[j] == '\n' || lx->p[j] == '>' || lx->p[j] == '/' ||
         lx->p[j] == '=';
}

static token scan_in_tag(lexer *lx) {
  lex_frame *f;
  uint32_t start;

  while (!at_end(lx) && (is_hspace(cur(lx)) || cur(lx) == '\n')) {
    lx->i++;
  }
  f = top(lx);
  start = lx->i;

  if (at_end(lx)) {
    LEX_REPORT(lx, DIAG_UNCLOSED_ELEMENT, span_make(f->open_at, f->open_at + 1u),
               "this element is never closed");
    pop_frame(lx);
    return token_make(TOK_EOF, span_make(lx->i, lx->i));
  }
  if (looking_at(lx, "/>", 2u)) {
    lx->i += 2u;
    pop_frame(lx);
    return token_make(TOK_TAG_SELF_CLOSE, span_make(start, lx->i));
  }
  if (cur(lx) == '>') {
    lx->i++;
    if (f->close_tag) {
      pop_frame(lx);
    } else {
      f->mode = LEX_MARKUP_CONTENT;
    }
    return token_make(TOK_TAG_END, span_make(start, lx->i));
  }
  if (looking_at(lx, "...", 3u)) {
    lx->i += 3u;
    return token_make(TOK_ELLIPSIS, span_make(start, lx->i));
  }
  if (cur(lx) == '=') {
    lx->i++;
    return token_make(TOK_ATTR_EQ, span_make(start, lx->i));
  }
  if (cur(lx) == '"') {
    lx->i++;
    (void)push_frame(lx, LEX_STR, start, TOK_EOF);
    return token_make(TOK_STR_START, span_make(start, lx->i));
  }
  if (cur(lx) == '$' && peek_at(lx, 1u) == '{') {
    lx->i += 2u;
    (void)push_frame(lx, LEX_NORMAL, start, TOK_INTERP_END);
    return token_make(TOK_INTERP_START, span_make(start, lx->i));
  }
  if (scan_markup_name(lx)) {
    if (f->named) {
      return token_make(TOK_ATTR_NAME, span_make(start, lx->i));
    }
    f->named = true;
    return token_make(TOK_TAG_NAME, span_make(start, lx->i));
  }

  /* A byte that cannot begin a name. Consume it so scanning always advances. */
  lx->i += char_width(lx);
  if (f->named) {
    LEX_REPORT(lx, DIAG_MALFORMED_ATTR_NAME, span_make(start, lx->i),
               "an attribute name must start with a letter or `_`, not byte 0x%02x",
               (unsigned char)lx->p[start]);
    return token_make(TOK_ATTR_NAME, span_make(start, lx->i));
  }
  LEX_REPORT(lx, DIAG_MALFORMED_TAG_NAME, span_make(start, lx->i),
             "a tag name must start with a letter or `_`, not byte 0x%02x",
             (unsigned char)lx->p[start]);
  return token_make(TOK_TAG_NAME, span_make(start, lx->i));
}

static token scan_markup_comment(lexer *lx) {
  uint32_t start = lx->i;

  lx->i += 4u;
  while (!at_end(lx) && !looking_at(lx, "-->", 3u)) {
    lx->i++;
  }
  if (at_end(lx)) {
    LEX_REPORT(lx, DIAG_UNTERMINATED_MARKUP_COMMENT, span_make(start, start + 4u),
               "this markup comment is never closed");
    return token_make(TOK_MARKUP_COMMENT, span_make(start, lx->i));
  }
  lx->i += 3u;
  return token_make(TOK_MARKUP_COMMENT, span_make(start, lx->i));
}

static token scan_in_content(lexer *lx) {
  lex_frame *f = top(lx);
  uint32_t start = lx->i;

  if (at_end(lx)) {
    LEX_REPORT(lx, DIAG_UNCLOSED_ELEMENT, span_make(f->open_at, f->open_at + 1u),
               "this element is never closed");
    pop_frame(lx);
    return token_make(TOK_EOF, span_make(lx->i, lx->i));
  }
  if (looking_at(lx, "<!--", 4u)) {
    return scan_markup_comment(lx);
  }
  if (looking_at(lx, "</", 2u)) {
    lx->i += 2u;
    f->mode = LEX_MARKUP_TAG;
    f->close_tag = true;
    f->named = false;
    return token_make(TOK_TAG_CLOSE_START, span_make(start, lx->i));
  }
  if (cur(lx) == '<' && is_alpha(peek_at(lx, 1u))) {
    lx->i++;
    (void)push_frame(lx, LEX_MARKUP_TAG, start, TOK_EOF);
    return token_make(TOK_MARKUP_START, span_make(start, lx->i));
  }
  if (cur(lx) == '$' && peek_at(lx, 1u) == '{') {
    lx->i += 2u;
    (void)push_frame(lx, LEX_NORMAL, start, TOK_INTERP_END);
    return token_make(TOK_INTERP_START, span_make(start, lx->i));
  }
  if (cur(lx) == '{') {
    lx->i++;
    (void)push_frame(lx, LEX_NORMAL, start, TOK_CTRL_END);
    return token_make(TOK_CTRL_START, span_make(start, lx->i));
  }

  /* A text run. Always consumes at least one byte, so a `<` or `$` that begins
   * nothing is text rather than a stall. */
  lx->i++;
  while (!at_end(lx)) {
    char c = cur(lx);
    if (c == '{') {
      break;
    }
    if (c == '<' &&
        (is_alpha(peek_at(lx, 1u)) || peek_at(lx, 1u) == '/' || peek_at(lx, 1u) == '!')) {
      break;
    }
    if (c == '$' && peek_at(lx, 1u) == '{') {
      break;
    }
    lx->i++;
  }
  return token_make(TOK_MARKUP_TEXT, span_make(start, lx->i));
}

/* ---- normal mode ------------------------------------------------------- */

/* One token in normal mode. Trivia is already skipped. Loops rather than
 * recursing on an unexpected byte, so a long run of them costs no stack -- which
 * fuzz_lex will generate. */
static token scan_normal_token(lexer *lx) {
  for (;;) {
    lex_frame *f = top(lx);
    uint32_t start = lx->i;
    char c;

    if (at_end(lx)) {
      if (lx->sp > 0u && f->close == TOK_INTERP_END) {
        LEX_REPORT(lx, DIAG_UNTERMINATED_INTERP, span_make(f->open_at, f->open_at + 2u),
                   "this interpolation is never closed");
      }
      return token_make(TOK_EOF, span_make(lx->i, lx->i));
    }

    c = cur(lx);

    if (is_alpha(c)) {
      return scan_word(lx);
    }
    if (is_digit(c)) {
      return scan_number(lx);
    }
    if (c == '"') {
      lx->i++;
      (void)push_frame(lx, LEX_STR, start, TOK_EOF);
      return token_make(TOK_STR_START, span_make(start, lx->i));
    }
    if (c == '`') {
      return scan_raw_string(lx);
    }

    lx->i++;
    switch (c) {
    case '(':
      return token_make(TOK_LPAREN, span_make(start, lx->i));
    case ')':
      return token_make(TOK_RPAREN, span_make(start, lx->i));
    case '[':
      return token_make(TOK_LBRACKET, span_make(start, lx->i));
    case ']':
      return token_make(TOK_RBRACKET, span_make(start, lx->i));
    case '{':
      f->depth++;
      return token_make(TOK_LBRACE, span_make(start, lx->i));
    case '}':
      if (f->depth > 0u) {
        f->depth--;
        return token_make(TOK_RBRACE, span_make(start, lx->i));
      }
      if (lx->sp > 0u && f->close != TOK_EOF) {
        token_kind close = f->close;
        pop_frame(lx);
        return token_make(close, span_make(start, lx->i));
      }
      return token_make(TOK_RBRACE, span_make(start, lx->i));
    case ',':
      return token_make(TOK_COMMA, span_make(start, lx->i));
    case '.':
      return token_make(TOK_DOT, span_make(start, lx->i));
    case ':':
      return token_make(TOK_COLON, span_make(start, lx->i));
    case '@':
      return token_make(TOK_AT, span_make(start, lx->i));
    case '|':
      return token_make(TOK_PIPE, span_make(start, lx->i));
    case '?':
      return token_make(TOK_QUESTION, span_make(start, lx->i));
    case '+':
      if (cur(lx) == '=') {
        lx->i++;
        return token_make(TOK_PLUS_EQ, span_make(start, lx->i));
      }
      return token_make(TOK_PLUS, span_make(start, lx->i));
    case '-':
      if (cur(lx) == '>') {
        lx->i++;
        return token_make(TOK_ARROW, span_make(start, lx->i));
      }
      if (cur(lx) == '=') {
        lx->i++;
        return token_make(TOK_MINUS_EQ, span_make(start, lx->i));
      }
      return token_make(TOK_MINUS, span_make(start, lx->i));
    case '*':
      if (cur(lx) == '=') {
        lx->i++;
        return token_make(TOK_STAR_EQ, span_make(start, lx->i));
      }
      return token_make(TOK_STAR, span_make(start, lx->i));
    case '/':
      if (cur(lx) == '=') {
        lx->i++;
        return token_make(TOK_SLASH_EQ, span_make(start, lx->i));
      }
      return token_make(TOK_SLASH, span_make(start, lx->i));
    case '%':
      if (cur(lx) == '=') {
        lx->i++;
        return token_make(TOK_PERCENT_EQ, span_make(start, lx->i));
      }
      return token_make(TOK_PERCENT, span_make(start, lx->i));
    case '=':
      if (cur(lx) == '=') {
        lx->i++;
        return token_make(TOK_EQ_EQ, span_make(start, lx->i));
      }
      if (cur(lx) == '>') {
        lx->i++;
        return token_make(TOK_FAT_ARROW, span_make(start, lx->i));
      }
      return token_make(TOK_EQ, span_make(start, lx->i));
    case '!':
      if (cur(lx) == '=') {
        lx->i++;
        return token_make(TOK_BANG_EQ, span_make(start, lx->i));
      }
      return token_make(TOK_BANG, span_make(start, lx->i));
    case '>':
      if (cur(lx) == '=') {
        lx->i++;
        return token_make(TOK_GE, span_make(start, lx->i));
      }
      return token_make(TOK_GT, span_make(start, lx->i));
    case '<':
      if (cur(lx) == '=') {
        lx->i++;
        return token_make(TOK_LE, span_make(start, lx->i));
      }
      lx->i = start;
      if (markup_ahead(lx)) {
        lx->i++;
        return token_make(TOK_MARKUP_START, span_make(start, lx->i));
      }
      lx->i++;
      return token_make(TOK_LT, span_make(start, lx->i));
    default:
      break;
    }

    /* Not the start of any token. Rewind over the single byte consumed above,
     * skip the whole UTF-8 character so the caret covers it, and try again. */
    lx->i = start;
    lx->i += char_width(lx);
    LEX_REPORT(lx, DIAG_UNEXPECTED_CHAR, span_make(start, lx->i),
               "byte 0x%02x cannot begin a token", (unsigned char)c);
  }
}

/* Skips horizontal whitespace, comments, and newlines, reporting whether any
 * newline was crossed and over what span. */
static void skip_trivia(lexer *lx, bool *saw_newline, span *run) {
  uint32_t nl_start = 0u;
  bool seen = false;

  for (;;) {
    if (at_end(lx)) {
      break;
    }
    if (is_hspace(cur(lx))) {
      lx->i++;
      continue;
    }
    if (cur(lx) == '\n') {
      if (!seen) {
        seen = true;
        nl_start = lx->i;
      }
      lx->i++;
      continue;
    }
    if (looking_at(lx, "//", 2u)) {
      skip_line_comment(lx);
      continue;
    }
    if (looking_at(lx, "/*", 2u)) {
      skip_block_comment(lx);
      continue;
    }
    break;
  }
  *saw_newline = seen;
  *run = seen ? span_make(nl_start, lx->i) : span_none();
}

/* The line-structure rule (D060). A newline survives only when the token before
 * it does not continue and the token after it does not follow, so deciding needs
 * one significant token of lookahead past the run. */
static token advance_normal(lexer *lx) {
  bool saw_newline;
  span run;

  skip_trivia(lx, &saw_newline, &run);
  if (saw_newline && lx->has_prev && !token_is_continuation(lx->prev)) {
    token next = scan_normal_token(lx);
    if (next.kind == TOK_EOF || token_is_follow(next.kind)) {
      return next;
    }
    lx->stash = next;
    lx->has_stash = true;
    return token_make(TOK_NEWLINE, run);
  }
  return scan_normal_token(lx);
}

static token lex_advance(lexer *lx) {
  switch (top(lx)->mode) {
  case LEX_NORMAL:
    return advance_normal(lx);
  case LEX_STR:
    return scan_in_string(lx);
  case LEX_MARKUP_TAG:
    return scan_in_tag(lx);
  case LEX_MARKUP_CONTENT:
    return scan_in_content(lx);
  }
  DOOT_UNREACHABLE();
}

/* Tokens in stream order, drawing from the newline stash first. */
static token next_raw(lexer *lx) {
  if (lx->has_stash) {
    lx->has_stash = false;
    return lx->stash;
  }
  return lex_advance(lx);
}

/* ---- public ------------------------------------------------------------ */

lexer *lex_new(arena *a, const source *src, diag_sink *sink, lex_comments *comments) {
  lexer *lx;
  slice text;

  DOOT_ASSERT(a != NULL && src != NULL);
  lx = ARENA_NEW(a, lexer);
  if (lx == NULL) {
    return NULL;
  }
  text = source_text(src);
  lx->a = a;
  lx->src = src;
  lx->sink = sink;
  lx->comments = comments;
  lx->p = text.p;
  lx->n = (uint32_t)text.n;
  lx->stack[0].mode = LEX_NORMAL;
  lx->stack[0].close = TOK_EOF;
  return lx;
}

token lex_next(lexer *lx) {
  token t;

  DOOT_ASSERT(lx != NULL);
  if (lx->has_peek) {
    lx->has_peek = false;
    t = lx->peeked;
  } else {
    t = next_raw(lx);
  }
  if (t.kind != TOK_NEWLINE) {
    lx->prev = t.kind;
    lx->has_prev = true;
  }
  return t;
}

token lex_peek(lexer *lx) {
  DOOT_ASSERT(lx != NULL);
  if (!lx->has_peek) {
    lx->peeked = next_raw(lx);
    lx->has_peek = true;
  }
  return lx->peeked;
}

void lex_open_markup(lexer *lx, span at) {
  DOOT_ASSERT(lx != NULL);
  (void)push_frame(lx, LEX_MARKUP_TAG, at.start, TOK_EOF);
}

slice lex_text(const lexer *lx, token t) {
  DOOT_ASSERT(lx != NULL);
  if (span_is_none(t.at) || t.at.start > lx->n) {
    return SLICE_EMPTY;
  }
  return slice_make(lx->p + t.at.start,
                    (size_t)((t.at.end > lx->n ? lx->n : t.at.end) - t.at.start));
}
