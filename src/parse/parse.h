/* parse.h -- recursive descent over the token stream.
 *
 * Hand-written, no generator dependency (D035). One token of lookahead, which the
 * lexer supplies; the parser drives the lexer rather than consuming a finished
 * array, because entering markup is the parser's decision (D059).
 *
 * Ownership: the returned tree lives in the arena passed in, together with every
 * slice it holds that is not a view into the source. The source must outlive the
 * tree, because spans index it.
 *
 * Failure: parsing always returns a unit. A malformed input yields a tree with
 * holes and a populated sink, never NULL and never a fault -- so a caller reports
 * diagnostics rather than distinguishing degrees of failure. Check
 * diag_has_errors() to decide whether the tree is worth walking.
 *
 * Recovery: on an unexpected token the parser reports once, then skips to the next
 * statement or declaration boundary and continues, so one mistake does not
 * cascade into a page of noise. Repeated failures at the same position are
 * suppressed.
 */
#ifndef DOOT_PARSE_H
#define DOOT_PARSE_H

#include "../base/arena.h"
#include "../base/plat.h"
#include "../base/source.h"
#include "ast.h"

/* The nesting bound for expressions, blocks, and markup. Recursive descent uses
 * the C stack, and input depth is attacker-controlled, so the limit is a
 * diagnostic (DT0022) rather than a stack overflow -- the same argument as the
 * lexer's mode stack (D056). */
#define PARSE_MAX_DEPTH 128u

/* `sink` may be NULL to parse silently. The unit's comment list is populated for
 * `doot fmt` (D067). */
unit_ast *parse_unit(arena *a, const source *src, diag_sink *sink);

#endif /* DOOT_PARSE_H */
