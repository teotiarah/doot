/* print.h -- the canonical printer behind `doot fmt` (D039).
 *
 * One format, no options. The gofmt argument applies twice over here: a single
 * non-negotiable format ends style debate, and it makes agent output
 * deterministic and diffs meaningful, which is goal 2 directly.
 *
 * Fixed choices, from docs/06-tooling.md#doot-fmt: two-space indentation, no
 * semicolons, no trailing whitespace, at most one blank line between
 * declarations, struct fields aligned on the colon, markup indented as markup
 * with attributes wrapped past 100 columns, void elements normalized to `<br/>`.
 *
 * The printer works from the AST, not the token stream, so it cannot preserve
 * anything the AST does not record. Two things are therefore recovered from the
 * source rather than the tree: comments, which the lexer collects into the unit's
 * comment list (D067), and whether the author left a blank line between two
 * declarations. Both are read back through spans.
 *
 * Idempotence is a property, not an aspiration: printing a formatted file must
 * produce the same bytes. It is the strongest available check that the AST and
 * the comment list together capture everything a source file means, which is why
 * the `expect-fmt-stable` spec directive exists.
 *
 * Ownership: the returned slice lives in the arena passed in.
 */
#ifndef DOOT_PRINT_H
#define DOOT_PRINT_H

#include "../base/arena.h"
#include "../base/plat.h"
#include "../base/slice.h"
#include "ast.h"

/* The column past which markup attributes wrap. */
#define FMT_WRAP_COLUMN 100u

/* Formats a parsed unit. The unit's source must still be alive, because comment
 * text and blank-line detection read through it.
 *
 * A unit that failed to parse must not be printed: the tree has holes where the
 * parser recovered, and printing it would produce plausible-looking source that
 * silently differs from the input. Callers check diag_has_errors() first; `doot
 * fmt` refuses to write a file that did not parse. */
slice fmt_unit(arena *a, const unit_ast *unit);

#endif /* DOOT_PRINT_H */
