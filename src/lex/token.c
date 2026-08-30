#include "token.h"

#include "../base/assert.h"

/* ---- names ------------------------------------------------------------- */

/* Written as an exhaustive switch rather than a table so that -Wswitch-enum
 * turns a new token kind into a compile error here, which is the mechanism
 * docs/09-engineering.md relies on to keep a growing front end consistent. */
const char *token_kind_name(token_kind kind) {
  switch (kind) {
  case TOK_EOF:
    return "end of input";
  case TOK_NEWLINE:
    return "end of line";

  case TOK_IDENT:
    return "identifier";
  case TOK_INT:
    return "integer literal";
  case TOK_FLOAT:
    return "float literal";

  case TOK_STR_START:
    return "start of string";
  case TOK_STR_TEXT:
    return "string text";
  case TOK_STR_END:
    return "end of string";
  case TOK_RAW_STR:
    return "raw string literal";
  case TOK_INTERP_START:
    return "${";
  case TOK_INTERP_END:
    return "}";

#define DOOT_KW_NAME(id, text)                                                                     \
  case id:                                                                                         \
    return text;
    DOOT_KEYWORDS(DOOT_KW_NAME)
#undef DOOT_KW_NAME

  case TOK_RESERVED:
    return "reserved word";

  case TOK_LPAREN:
    return "(";
  case TOK_RPAREN:
    return ")";
  case TOK_LBRACKET:
    return "[";
  case TOK_RBRACKET:
    return "]";
  case TOK_LBRACE:
    return "{";
  case TOK_RBRACE:
    return "}";

  case TOK_COMMA:
    return ",";
  case TOK_DOT:
    return ".";
  case TOK_COLON:
    return ":";
  case TOK_ARROW:
    return "->";
  case TOK_FAT_ARROW:
    return "=>";

  case TOK_EQ:
    return "=";
  case TOK_PLUS_EQ:
    return "+=";
  case TOK_MINUS_EQ:
    return "-=";
  case TOK_STAR_EQ:
    return "*=";
  case TOK_SLASH_EQ:
    return "/=";
  case TOK_PERCENT_EQ:
    return "%=";

  case TOK_EQ_EQ:
    return "==";
  case TOK_BANG_EQ:
    return "!=";
  case TOK_LT:
    return "<";
  case TOK_LE:
    return "<=";
  case TOK_GT:
    return ">";
  case TOK_GE:
    return ">=";

  case TOK_PLUS:
    return "+";
  case TOK_MINUS:
    return "-";
  case TOK_STAR:
    return "*";
  case TOK_SLASH:
    return "/";
  case TOK_PERCENT:
    return "%";

  case TOK_BANG:
    return "!";
  case TOK_QUESTION:
    return "?";
  case TOK_AT:
    return "@";
  case TOK_PIPE:
    return "|";

  case TOK_MARKUP_START:
    return "<";
  case TOK_TAG_NAME:
    return "tag name";
  case TOK_TAG_END:
    return ">";
  case TOK_TAG_SELF_CLOSE:
    return "/>";
  case TOK_TAG_CLOSE_START:
    return "</";
  case TOK_ATTR_NAME:
    return "attribute name";
  case TOK_ATTR_EQ:
    return "=";
  case TOK_MARKUP_TEXT:
    return "markup text";
  case TOK_MARKUP_COMMENT:
    return "markup comment";
  case TOK_CTRL_START:
    return "{";
  case TOK_CTRL_END:
    return "}";
  case TOK_ELLIPSIS:
    return "...";

  case TOK_KIND_COUNT:
    break;
  }
  DOOT_UNREACHABLE();
}

/* ---- classification ---------------------------------------------------- */

#define TF_NONE 0u
#define TF_CONTINUATION 1u /* a newline after this token is suppressed */
#define TF_FOLLOW 2u       /* a newline before this token is suppressed */

/* Also an exhaustive switch, and for the same reason: a new token kind must be
 * classified deliberately rather than defaulting to "neither", which would be a
 * silent wrong answer for any new operator. -Wswitch-enum fires even when a
 * `default:` label is present, so listing every kind is the only shape that
 * keeps that guarantee. */
static unsigned token_flags(token_kind kind) {
  switch (kind) {
  /* Continuation: an expression may break after any operator or opening
   * bracket. Postfix `!` is deliberately absent -- it terminates a statement
   * (`let u = create(name)!`) rather than continuing one (D060). */
  case TOK_LPAREN:
  case TOK_LBRACKET:
  case TOK_LBRACE:
  case TOK_COMMA:
  case TOK_COLON:
  case TOK_ARROW:
  case TOK_FAT_ARROW:
  case TOK_EQ:
  case TOK_PLUS_EQ:
  case TOK_MINUS_EQ:
  case TOK_STAR_EQ:
  case TOK_SLASH_EQ:
  case TOK_PERCENT_EQ:
  case TOK_EQ_EQ:
  case TOK_BANG_EQ:
  case TOK_LT:
  case TOK_LE:
  case TOK_GT:
  case TOK_GE:
  case TOK_PLUS:
  case TOK_MINUS:
  case TOK_STAR:
  case TOK_SLASH:
  case TOK_PERCENT:
  case TOK_PIPE:
  case TOK_KW_AND:
  case TOK_KW_OR:
  case TOK_KW_NOT:
    return TF_CONTINUATION;

  /* Follow only. A closing bracket permits a break *before* it, so a multi-line
   * call or literal works -- but it must not suppress the newline *after* it, or
   * `f()` would swallow its own statement terminator and join the next line.
   *
   * The rule that decides membership here, learned the hard way twice:
   *
   *   A follow token must not be able to *begin* a construct.
   *
   * Suppressing the newline before a token means the parser can no longer tell
   * "this token continues the previous line" from "this token starts a new one".
   * For `)`, `]`, and `}` that distinction does not exist -- none of them can begin
   * anything -- so suppression is safe. `.` and `else` both can, and both were in
   * this set originally; see the two cases below. */
  case TOK_RPAREN:
  case TOK_RBRACKET:
  case TOK_RBRACE:
    return TF_FOLLOW;

  /* `.` continues a line but does not follow one, so a method chain breaks *after*
   * the dot rather than before it. A `match` arm's pattern begins with a dot:
   *
   *     match status {
   *       .active -> render()
   *       .banned -> deny()      <- joins to `render().banned` if `.` follows
   *     }
   *
   * `else` is the same problem one level up: it begins a match arm, so with the
   * newline suppressed the previous arm's value swallowed it as an `else`
   * coalesce and then found `->` where it wanted an expression:
   *
   *     match a {
   *       1 | 2 -> two()
   *       else -> other()        <- parsed as `two() else -> other()`
   *     }
   *
   * `else` needed suppression only for an `else` written on its own line after a
   * `}`, which is not doot style and appears in no example -- and the parser
   * accepts that form anyway by looking past a newline, with `doot fmt`
   * normalizing it to `} else {`. Required syntax wins over optional style. */
  case TOK_DOT:
  case TOK_KW_ELSE:
    return TF_CONTINUATION;

  /* Neither. */
  case TOK_EOF:
  case TOK_NEWLINE:
  case TOK_IDENT:
  case TOK_INT:
  case TOK_FLOAT:
  case TOK_STR_START:
  case TOK_STR_TEXT:
  case TOK_STR_END:
  case TOK_RAW_STR:
  case TOK_INTERP_START:
  case TOK_INTERP_END:
  case TOK_KW_AS:
  case TOK_KW_BREAK:
  case TOK_KW_CONTINUE:
  case TOK_KW_DEFER:
  case TOK_KW_ENUM:
  case TOK_KW_FALSE:
  case TOK_KW_FN:
  case TOK_KW_FOR:
  case TOK_KW_GROUP:
  case TOK_KW_IF:
  case TOK_KW_IN:
  case TOK_KW_LET:
  case TOK_KW_MATCH:
  case TOK_KW_NIL:
  case TOK_KW_PUB:
  case TOK_KW_RETURN:
  case TOK_KW_ROUTE:
  case TOK_KW_SELF:
  case TOK_KW_SEND:
  case TOK_KW_SPAWN:
  case TOK_KW_STREAM:
  case TOK_KW_TEST:
  case TOK_KW_TRUE:
  case TOK_KW_TYPE:
  case TOK_KW_VAR:
  case TOK_KW_WHILE:
  case TOK_KW_WITH:
  case TOK_RESERVED:
  case TOK_BANG:
  case TOK_QUESTION:
  case TOK_AT:
  case TOK_MARKUP_START:
  case TOK_TAG_NAME:
  case TOK_TAG_END:
  case TOK_TAG_SELF_CLOSE:
  case TOK_TAG_CLOSE_START:
  case TOK_ATTR_NAME:
  case TOK_ATTR_EQ:
  case TOK_MARKUP_TEXT:
  case TOK_MARKUP_COMMENT:
  case TOK_CTRL_START:
  case TOK_CTRL_END:
  case TOK_ELLIPSIS:
    return TF_NONE;

  case TOK_KIND_COUNT:
    break;
  }
  DOOT_UNREACHABLE();
}

bool token_is_continuation(token_kind kind) {
  return (token_flags(kind) & TF_CONTINUATION) != 0u;
}

bool token_is_follow(token_kind kind) {
  return (token_flags(kind) & TF_FOLLOW) != 0u;
}

/* ---- words ------------------------------------------------------------- */

typedef struct {
  const char *text;
  token_kind kind;
} keyword_row;

#define DOOT_KW_ROW(id, text) {text, id},
static const keyword_row keyword_table[] = {DOOT_KEYWORDS(DOOT_KW_ROW)};
#undef DOOT_KW_ROW

#define DOOT_KEYWORD_COUNT (sizeof(keyword_table) / sizeof(keyword_table[0]))

typedef struct {
  const char *text;
  const char *help;
} reserved_row;

#define DOOT_RESERVED_ROW(text, help) {text, help},
static const reserved_row reserved_table[] = {DOOT_RESERVED_WORDS(DOOT_RESERVED_ROW)};
#undef DOOT_RESERVED_ROW

#define DOOT_RESERVED_COUNT (sizeof(reserved_table) / sizeof(reserved_table[0]))

bool token_keyword_lookup(slice word, token_kind *out) {
  size_t i;

  DOOT_ASSERT(out != NULL);
  for (i = 0; i < DOOT_KEYWORD_COUNT; i++) {
    if (slice_eq_cstr(word, keyword_table[i].text)) {
      *out = keyword_table[i].kind;
      return true;
    }
  }
  return false;
}

bool token_reserved_lookup(slice word) {
  return token_reserved_help(word) != NULL;
}

const char *token_reserved_help(slice word) {
  size_t i;

  for (i = 0; i < DOOT_RESERVED_COUNT; i++) {
    if (slice_eq_cstr(word, reserved_table[i].text)) {
      return reserved_table[i].help;
    }
  }
  return NULL;
}
