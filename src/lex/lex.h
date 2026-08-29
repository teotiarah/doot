/* lex.h -- the scanner: source bytes to a token stream (D056).
 *
 * Pull-based, one token at a time, with a mode stack. The parser drives it
 * rather than receiving a finished array, because deciding whether `<` opens a
 * markup literal needs to know whether the parser is in expression position and
 * a batch lexer cannot know that. See docs/10-frontend.md.
 *
 * Ownership: the lexer borrows its source and holds no copy of the text. Every
 * token's span indexes that source, so both outlive the lexer only if the source
 * does. The lexer allocates from the arena only to record a comment or report a
 * diagnostic; scanning itself allocates nothing (D057).
 *
 * Failure policy: malformed input is always a diagnostic and never a fault. The
 * stream is infallible -- every call returns a token, and TOK_EOF is reached in a
 * bounded number of calls for any input, because every call consumes at least
 * one byte or is already at the end. That is the invariant fuzz_lex asserts.
 */
#ifndef DOOT_LEX_H
#define DOOT_LEX_H

#include <stdbool.h>

#include "../base/arena.h"
#include "../base/plat.h"
#include "../base/slice.h"
#include "../base/source.h"
#include "token.h"

/* The mode stack bound. Nesting is reachable from untrusted input, so exceeding
 * it is DT0022 rather than an assertion (D056). */
#define LEX_MAX_DEPTH 64u

/* A comment, recorded rather than emitted.
 *
 * Comments are not tokens: the line-structure rule needs the next *significant*
 * token across a run of newlines, and an unbounded number of comments can sit in
 * that run, so emitting them inline would require an unbounded lookahead queue
 * and therefore allocation on the scanning path -- which D057 rules out. They go
 * here instead, in source order, which is all a formatter needs to place them.
 */
typedef struct lex_comment lex_comment;

struct lex_comment {
  span at;
  bool block; /* a block comment rather than a line comment */
  lex_comment *next;
};

typedef struct {
  lex_comment *first;
  lex_comment *last;
  uint32_t count;
} lex_comments;

typedef struct lexer lexer;

/* `sink` may be NULL to scan silently. `comments` may be NULL to discard
 * comments, in which case scanning performs no allocation at all; the NULL-sink
 * convention here matches source_from_memory. */
lexer *lex_new(arena *a, const source *src, diag_sink *sink, lex_comments *comments);

token lex_next(lexer *lx);

/* Not const: filling the lookahead slot mutates the lexer. */
token lex_peek(lexer *lx);

/* Enter markup. The parser calls this immediately after consuming a
 * TOK_MARKUP_START that appeared in expression position, passing that token's
 * span; in operator position it calls nothing and reads the token as `<`
 * (D059). */
void lex_open_markup(lexer *lx, span at);

/* Close the innermost open element without a closing tag.
 *
 * A void element may be written `<br>` as well as `<br/>`, and both are accepted
 * (03-grammar.md#semantics). The lexer cannot tell the difference on its own: it
 * has no element table and, having just emitted `>`, has already put the frame
 * into content mode. So the parser -- which does know the void elements -- calls
 * this while `>` is still the current token, before advancing, so that the next
 * token is scanned in the enclosing element's context rather than as this
 * element's content. */
void lex_close_element(lexer *lx);

/* The source bytes a token covers. */
slice lex_text(const lexer *lx, token t);

#endif /* DOOT_LEX_H */
