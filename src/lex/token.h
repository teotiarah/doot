/* token.h -- the lexical token: what it is and where it is (D057).
 *
 * A token is a kind and a span, twelve bytes, and nothing else. There is no
 * decoded value in it: no int64_t for an integer literal, no unescaped text for
 * a string. The parser decodes literals by reading the bytes back out of the
 * span. That keeps the lexer free of allocation, makes the token stream a pure
 * function of the input bytes, and leaves source text with exactly one
 * representation -- a slice into source_text.
 *
 * Ownership: a token owns nothing. Its span indexes the source it came from,
 * and is valid for as long as that source is.
 *
 * The keyword and reserved-word lists are frozen at v0.1 (D042), so both tables
 * below are complete and will not grow.
 */
#ifndef DOOT_TOKEN_H
#define DOOT_TOKEN_H

#include <stdbool.h>

#include "../base/plat.h"
#include "../base/slice.h"
#include "../base/source.h"

/* The thirty-one keywords, in the order docs/02-syntax.md#keywords lists them.
 * Fields: enum id, spelling. */
#define DOOT_KEYWORDS(X)                                                                           \
  X(TOK_KW_AND, "and")                                                                             \
  X(TOK_KW_AS, "as")                                                                               \
  X(TOK_KW_BREAK, "break")                                                                         \
  X(TOK_KW_CONTINUE, "continue")                                                                   \
  X(TOK_KW_DEFER, "defer")                                                                         \
  X(TOK_KW_ELSE, "else")                                                                           \
  X(TOK_KW_ENUM, "enum")                                                                           \
  X(TOK_KW_FALSE, "false")                                                                         \
  X(TOK_KW_FN, "fn")                                                                               \
  X(TOK_KW_FOR, "for")                                                                             \
  X(TOK_KW_GROUP, "group")                                                                         \
  X(TOK_KW_IF, "if")                                                                               \
  X(TOK_KW_IN, "in")                                                                               \
  X(TOK_KW_LET, "let")                                                                             \
  X(TOK_KW_MATCH, "match")                                                                         \
  X(TOK_KW_NIL, "nil")                                                                             \
  X(TOK_KW_NOT, "not")                                                                             \
  X(TOK_KW_OR, "or")                                                                               \
  X(TOK_KW_PUB, "pub")                                                                             \
  X(TOK_KW_RETURN, "return")                                                                       \
  X(TOK_KW_ROUTE, "route")                                                                         \
  X(TOK_KW_SELF, "self")                                                                           \
  X(TOK_KW_SEND, "send")                                                                           \
  X(TOK_KW_SPAWN, "spawn")                                                                         \
  X(TOK_KW_STREAM, "stream")                                                                       \
  X(TOK_KW_TEST, "test")                                                                           \
  X(TOK_KW_TRUE, "true")                                                                           \
  X(TOK_KW_TYPE, "type")                                                                           \
  X(TOK_KW_VAR, "var")                                                                             \
  X(TOK_KW_WHILE, "while")                                                                         \
  X(TOK_KW_WITH, "with")

/* The thirty-five reserved words (D042, D062). All share TOK_RESERVED and one
 * diagnostic code; the help text names the doot construct to reach for instead,
 * which is the entire reason the list exists.
 * Fields: spelling, help. */
#define DOOT_RESERVED_WORDS(X)                                                                     \
  X("async", "doot has no async/await: a task blocks and the runtime schedules around it")         \
  X("await", "doot has no async/await: a task blocks and the runtime schedules around it")         \
  X("class", "doot has structs and free functions, not classes")                                   \
  X("const", "top-level bindings are `let`, which is already immutable")                           \
  X("do", "doot has no do-while loop; use `while`")                                                \
  X("extends", "doot has no inheritance; compose with struct fields and free functions")           \
  X("finally", "doot has no exceptions; use `defer` for cleanup")                                  \
  X("foreign", "doot has no foreign function interface, permanently")                              \
  X("go", "use `spawn` to start a task")                                                           \
  X("impl", "attach a method with `fn TypeName.method(self)`")                                     \
  X("import", "doot has no import statements; stdlib modules are pre-bound and user "              \
              "modules are addressed by path")                                                     \
  X("interface", "doot has no interfaces; there are no third-party libraries to abstract over")    \
  X("loop", "use `while true`")                                                                    \
  X("macro", "doot has no metaprogramming; attributes are a closed set")                           \
  X("module", "a file is a module, addressed by its path")                                         \
  X("mut", "parameters are immutable; bind locally with `var` and return a new value")             \
  X("new", "construct with a struct literal: `User { id: 1 }`")                                    \
  X("null", "the absent value is spelled `nil`")                                                   \
  X("package", "doot has no package system and no registry")                                       \
  X("panic", "doot has no panic keyword; faults are raised by the runtime")                        \
  X("private", "a declaration is private unless marked `pub`")                                     \
  X("protected", "doot has no inheritance, so there is nothing to protect from")                   \
  X("require", "doot has no import statements")                                                    \
  X("select", "doot has no select statement; iterate a channel or a subscription")                 \
  X("static", "there are no globals and no statics; state belongs in SQLite or request scope")     \
  X("switch", "use `match`")                                                                       \
  X("this", "the receiver is spelled `self`")                                                      \
  X("throw", "errors are values: declare `!` and propagate with `!`")                              \
  X("trait", "doot has no traits or interfaces")                                                   \
  X("try", "propagate with a postfix `!`, or handle with `else`")                                  \
  X("typeof", "doot has no runtime type inspection")                                               \
  X("unsafe", "doot has no unsafe escape hatch")                                                   \
  X("use", "doot has no import statements")                                                        \
  X("where", "doot has no user-defined generics, so there are no constraints to write")            \
  X("yield", "doot has no generators")

#define DOOT_TOKEN_KW_ENUM(id, text) id,

typedef enum {
  /* Structural. */
  TOK_EOF = 0,
  TOK_NEWLINE,

  /* Names and numbers. */
  TOK_IDENT,
  TOK_INT,
  TOK_FLOAT,

  /* Strings. A literal is a sequence, not one token (D058). */
  TOK_STR_START,
  TOK_STR_TEXT,
  TOK_STR_END,
  TOK_RAW_STR,
  TOK_INTERP_START,
  TOK_INTERP_END,

  /* Keywords. */
  DOOT_KEYWORDS(DOOT_TOKEN_KW_ENUM)

  /* One kind for all thirty-five reserved words (D062). */
  TOK_RESERVED,

  /* Delimiters. */
  TOK_LPAREN,
  TOK_RPAREN,
  TOK_LBRACKET,
  TOK_RBRACKET,
  TOK_LBRACE,
  TOK_RBRACE,

  /* Separators and arrows. */
  TOK_COMMA,
  TOK_DOT,
  TOK_COLON,
  TOK_ARROW,
  TOK_FAT_ARROW,

  /* Assignment. */
  TOK_EQ,
  TOK_PLUS_EQ,
  TOK_MINUS_EQ,
  TOK_STAR_EQ,
  TOK_SLASH_EQ,
  TOK_PERCENT_EQ,

  /* Comparison. */
  TOK_EQ_EQ,
  TOK_BANG_EQ,
  TOK_LT,
  TOK_LE,
  TOK_GT,
  TOK_GE,

  /* Arithmetic. */
  TOK_PLUS,
  TOK_MINUS,
  TOK_STAR,
  TOK_SLASH,
  TOK_PERCENT,

  /* Postfix and marks. */
  TOK_BANG,
  TOK_QUESTION,
  TOK_AT,
  TOK_PIPE,

  /* Markup. Delimiters here are deliberately distinct from the operator kinds
   * above, so that `<` and `>` in the statement-continuation set can only ever
   * mean comparison (D059). */
  TOK_MARKUP_START,
  TOK_TAG_NAME,
  TOK_TAG_END,
  TOK_TAG_SELF_CLOSE,
  TOK_TAG_CLOSE_START,
  TOK_ATTR_NAME,
  TOK_ATTR_EQ,
  TOK_MARKUP_TEXT,
  TOK_MARKUP_COMMENT,
  TOK_CTRL_START,
  TOK_CTRL_END,
  TOK_ELLIPSIS,

  TOK_KIND_COUNT
} token_kind;

#undef DOOT_TOKEN_KW_ENUM

typedef struct {
  token_kind kind;
  span at;
} token;

DOOT_INLINE token token_make(token_kind kind, span at) {
  token t;
  t.kind = kind;
  t.at = at;
  return t;
}

/* A stable spelling for diagnostics and tests. Operators and keywords render as
 * themselves (`->`, `let`); classes render as a description (`identifier`). */
const char *token_kind_name(token_kind kind);

/* Line structure (D060). A newline is suppressed when the preceding significant
 * token continues, or the following one follows. */
bool token_is_continuation(token_kind kind);
bool token_is_follow(token_kind kind);

/* Exact-match word classification, in the order the lexer applies it: keyword,
 * then reserved, then identifier. */
bool token_keyword_lookup(slice word, token_kind *out);
bool token_reserved_lookup(slice word);

/* The help text for a reserved word: what to write in doot instead. Returns
 * NULL when `word` is not reserved. */
const char *token_reserved_help(slice word);

#endif /* DOOT_TOKEN_H */
