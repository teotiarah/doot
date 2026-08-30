# Front end

Implementation specification for the lexer, the parser, the AST, the front-end diagnostics, and the printer. Decisions are recorded as [D056](01-decisions.md#d056)–[D069](01-decisions.md#d069). The specification-test suite that exercises all of it is [11-spec-tests.md](11-spec-tests.md).

This document covers the first three stages of the pipeline in [05-runtime.md](05-runtime.md#compiler-pipeline) and stops there. The resolver, the typechecker, the schema checker, the register allocator, and the emitter are specified in a later pass, as are the opcode table, the native-call ABI, the frame-map encoding, and the runtime value layout. Nothing here depends on those, which is the point: the front end is the part of the compiler that can be settled and finished while the back end is still open.

Three defects in the frozen line-structure rule surfaced while writing this, and are corrected in [03-grammar.md](03-grammar.md#line-structure) under [D060](01-decisions.md#d060).

---

## The token

```c
typedef struct {
  token_kind kind;
  span at;
} token;
```

Twelve bytes, and that is the whole of it. A token carries **what it is and where it is, and nothing else** ([D057](01-decisions.md#d057)).

There is no decoded value in a token — no `int64_t` for an integer literal, no unescaped string for a string literal. Decoding happens in the parser, reading the bytes back out of the span. Three things follow, and together they are worth more than the saved re-scan:

- **The lexer allocates nothing.** It touches the arena only to report a diagnostic. That makes `fuzz_lex` a pure function of its input bytes and removes allocation failure from every lexer path.
- **The token stream is reproducible.** Lexing the same bytes twice produces byte-identical output, which is what lets the spec runner assert on spans exactly rather than approximately.
- **One source of truth for literal text.** `slice` into `source_text` is already how the rest of the compiler refers to source bytes, so a token needs no second representation of the same characters.

Decoding is not free, but it happens once per literal in the parser rather than once per literal in the lexer, which is the same count.

### Token kind families

Enum members are `TOK_*` per the convention in [09-engineering.md](09-engineering.md#style). Every `switch` over `token_kind` must list every enumerator, because `-Wswitch-enum` is on and `default:` is not permitted for an enum — that is the mechanism that makes adding a token kind produce a compile error at each site that must handle it ([D046](01-decisions.md#d046)).

| Family | Members |
| --- | --- |
| Structural | `TOK_EOF`, `TOK_NEWLINE` |
| Names and numbers | `TOK_IDENT`, `TOK_INT`, `TOK_FLOAT` |
| Strings | `TOK_STR_START`, `TOK_STR_TEXT`, `TOK_STR_END`, `TOK_RAW_STR`, `TOK_INTERP_START`, `TOK_INTERP_END` |
| Keywords | `TOK_KW_AND` … `TOK_KW_WITH`, one per keyword, thirty-one in total |
| Reserved | `TOK_RESERVED`, one kind for all thirty-five ([D062](01-decisions.md#d062)) |
| Delimiters | `TOK_LPAREN`, `TOK_RPAREN`, `TOK_LBRACKET`, `TOK_RBRACKET`, `TOK_LBRACE`, `TOK_RBRACE` |
| Separators | `TOK_COMMA`, `TOK_DOT`, `TOK_COLON` |
| Arrows | `TOK_ARROW`, `TOK_FAT_ARROW` |
| Assignment | `TOK_EQ`, `TOK_PLUS_EQ`, `TOK_MINUS_EQ`, `TOK_STAR_EQ`, `TOK_SLASH_EQ`, `TOK_PERCENT_EQ` |
| Comparison | `TOK_EQ_EQ`, `TOK_BANG_EQ`, `TOK_LT`, `TOK_LE`, `TOK_GT`, `TOK_GE` |
| Arithmetic | `TOK_PLUS`, `TOK_MINUS`, `TOK_STAR`, `TOK_SLASH`, `TOK_PERCENT` |
| Postfix and marks | `TOK_BANG`, `TOK_QUESTION`, `TOK_AT`, `TOK_PIPE` |
| Markup | `TOK_MARKUP_START`, `TOK_TAG_NAME`, `TOK_TAG_END`, `TOK_TAG_SELF_CLOSE`, `TOK_TAG_CLOSE_START`, `TOK_ATTR_NAME`, `TOK_ATTR_EQ`, `TOK_MARKUP_TEXT`, `TOK_MARKUP_COMMENT`, `TOK_CTRL_START`, `TOK_CTRL_END`, `TOK_ELLIPSIS` |

Keyword kinds are spelled `TOK_KW_*` rather than `TOK_*` so that `TOK_KW_IN` and `TOK_INT` cannot be confused at a glance. The thirty-one keywords are frozen ([D042](01-decisions.md#d042)), so this part of the enum never grows.

---

## The lexer

**Pull-based, with an explicit mode stack** ([D056](01-decisions.md#d056)).

```c
token lex_next(lexer *lx);
token lex_peek(lexer *lx); /* not const: filling the lookahead slot mutates */
```

The parser drives the lexer one token at a time rather than receiving a finished array. This is not a performance choice — it is forced by markup. Deciding whether `<` opens a markup literal requires knowing whether the parser is in expression position ([D059](01-decisions.md#d059)), and a batch lexer has no way to know that. Once the lexer must be interleaved with the parser for markup, making it pull-based everywhere is simpler than having two entry points.

The lexer holds no global state; the `lexer` context is passed explicitly, per [D046](01-decisions.md#d046).

### Modes

| Mode | Entered | Newlines are |
| --- | --- | --- |
| `LEX_NORMAL` | the initial mode | significant, subject to the line-structure rule |
| `LEX_STR` | `"` in `LEX_NORMAL` | content |
| `LEX_MARKUP_TAG` | `TOK_MARKUP_START`, and after `TOK_TAG_CLOSE_START` | insignificant whitespace, so attributes may wrap |
| `LEX_MARKUP_CONTENT` | `TOK_TAG_END` | content |

`${` pushes `LEX_NORMAL` from `LEX_STR`, `LEX_MARKUP_CONTENT`, or an attribute value. The pushed entry carries a **brace depth**, so that `${ {"a": 1} }` closes the interpolation at the correct `}` rather than at the map literal's. `}` at depth zero emits `TOK_INTERP_END` and pops.

The stack is a fixed array of 64 entries. Exceeding it is `DT0022`, not an assertion failure: nesting depth is attacker-controlled through any endpoint that compiles source, so it is user input and gets a diagnostic ([D048](01-decisions.md#d048) draws that line — an assertion is for an invariant violation in doot itself). A fixed bound also keeps the lexer's stack depth statically known, which [D046](01-decisions.md#d046) requires.

### Words

Identifier-shaped runs are classified by an exact-match lookup, in this order:

1. One of the thirty-one **keywords** → its `TOK_KW_*` kind.
2. One of the thirty-five **reserved words** → `TOK_RESERVED`.
3. Anything else → `TOK_IDENT`.

`true`, `false`, and `nil` are keywords, not literals, because the frozen keyword list says so.

**Contextual words lex as `TOK_IDENT`** ([D061](01-decisions.md#d061)). Two sets exist, and neither consumes keyword budget:

- **HTTP methods.** `GET`, `POST`, `PUT`, `PATCH`, `DELETE`, `HEAD`, `OPTIONS` are recognized by the *parser*, by comparing the text of an identifier that appears immediately after `route` or `stream`. The lexer stays context-free, and the parser can say "expected an HTTP method after `route`, found `Get`" instead of producing a bare parse failure.
- **`end`**, which closes a markup control block. It is worth being explicit that `end` is **not** one of the thirty-one keywords — [D022](01-decisions.md#d022) says markup control flow reuses statement keywords, and `if`, `else`, and `for` do, but `end` has no statement equivalent. It is matched by text in markup control position, the same way stdlib module names and type names are predeclared identifiers rather than keywords ([02-syntax.md](02-syntax.md#keywords)).

### Comments are recorded, not emitted

Comments are **never discarded and never tokens**. The lexer appends each one to an optional caller-supplied list:

```c
lexer *lex_new(arena *a, const source *src, diag_sink *sink, lex_comments *comments);
```

`comments` may be NULL to discard them, the same way `sink` may be NULL to scan silently — a convention `source_from_memory` already uses. `doot fmt` passes a list; everything else passes NULL and allocates nothing.

Preserving them is not optional: `doot fmt` is canonical and idempotent ([D039](01-decisions.md#d039)), and a formatter that silently deletes every comment is not a formatter.

**Emitting them as tokens, however, does not work**, and this document said it did until the line-structure algorithm was implemented against it. The rule needs the next *significant* token across a run of newlines, and an unbounded number of comments can sit inside that run:

```do
foo()
// a
// b
// c
bar()
```

Deciding whether that newline survives means scanning past three comments to reach `bar`, and every token scanned past has to be held for later delivery. The lookahead queue is therefore as long as the comment run — unbounded, so heap-allocated, on the scanning path. That contradicts [D057](01-decisions.md#d057), whose whole point is that the lexer allocates nothing. A side list has the same cost only when a caller actually wants comments, and none of the properties are lost: comments arrive in source order with exact spans, which is all a printer needs to place them.

Block comments **nest**, per [03-grammar.md](03-grammar.md#comments). An unterminated one is `DT0013`, reported at the outermost `/*` so the span points at the comment that actually failed to close rather than at end of file.

Trivia is never the preceding or following token for line-structure purposes. See below.

### Strings

A string literal is a **token sequence, not one token** ([D058](01-decisions.md#d058)), because interpolation makes it structurally a tree:

```
"hello"          ->  STR_START  STR_TEXT("hello")  STR_END
"a${x}b"         ->  STR_START  STR_TEXT("a")  INTERP_START  IDENT(x)  INTERP_END
                     STR_TEXT("b")  STR_END
""               ->  STR_START  STR_END
```

Non-interpolated strings get the same shape as interpolated ones rather than a shortcut single-token form. Uniformity costs two tokens and buys a parser that handles one case, which is [goal 1](00-vision.md#the-nine-goals) applied to the implementation.

`STR_TEXT` spans **raw source bytes, escapes unresolved.** The parser resolves escapes when it builds the literal, using `buf` over the compilation arena. Escape errors are `DT0014`–`DT0016`, and they are reported by the parser at the escape's own span, not at the start of the literal.

Raw strings are `TOK_RAW_STR`, a single token: backticks admit neither escapes nor interpolation, so there is no interior to tokenize.

---

## Line structure

The rule in [03-grammar.md](03-grammar.md#line-structure) is a token-pair rule, and implementing it needs one significant token of lookahead **past** the newline. Precisely:

```
on a run of one or more newlines:
  if there is no preceding significant token          -> suppress   (leading blank lines)
  if the preceding significant token is a continuation -> suppress
  scan the next significant token T, hold it in the pushback slot
  if T is a follow token, or T is TOK_EOF              -> suppress
  otherwise                                            -> emit one TOK_NEWLINE
```

"Significant" excludes `TOK_COMMENT`, `TOK_BLOCK_COMMENT`, and `TOK_NEWLINE` itself. A run of consecutive newlines, with any interleaved whitespace or comments, produces **at most one** `TOK_NEWLINE`.

The emitted `TOK_NEWLINE`'s **span covers the entire consumed run.** That is deliberate and load-bearing for the formatter: `doot fmt` permits at most one blank line between declarations, and it recovers how many blank lines the author wrote by counting `\n` inside the token's span. No extra field on `token`, and the span is honestly accurate rather than merely a placeholder.

Two one-token slots are needed, not one. A *stash* holds the significant token scanned past a newline run; a *peek* slot holds a token produced for `lex_peek`. Sharing a single slot loses the stash, because filling the peek slot overwrites whatever the same call just wrote there. Neither is more than one token deep, so this still matches the "one token of lookahead" the pipeline in [05-runtime.md](05-runtime.md#compiler-pipeline) specifies.

### The two sets

**Continuation** — a newline *after* one of these is suppressed:

```
( [ { , . : -> => = += -= *= /= %= == != < <= > >= + - * / % | and or not
```

**Follow** — a newline *before* one of these is suppressed:

```
) ] } . else
```

Three corrections to the set as originally written, all argued in [D060](01-decisions.md#d060):

- **Postfix `!` is removed from the continuation set.** It ended `let u = create(name)!`, the single most common error-handling statement in the language, by swallowing the newline and joining the next line to it. Since prefix `!` does not exist ([03-grammar.md](03-grammar.md#precedence)) and `!=` is listed separately, postfix propagation was the only `!` the entry could have meant, and it is a statement-terminating postfix, never a continuation.
- **The compound assignments are added.** `=` was in the set and `+=` was not, which made `total +=` followed by a newline a parse error while `total =` followed by a newline was fine. They are the same construct.
- **`|` is added**, so a multi-line `match` pattern breaks after the alternation bar like every other binary operator.

`<` and `>` remain in the continuation set and are unambiguous there, because markup delimiters are **distinct token kinds** and never `TOK_LT` or `TOK_GT` ([D059](01-decisions.md#d059)). Without that separation, `return <p>hi</p>` would end on a `>` in the continuation set and swallow its own terminator.

### Statement end

`}` in the follow set is what makes multi-line map literals, struct literals, and blocks work — a `TOK_NEWLINE` before the closing brace would break the enclosing expression. It also means the last statement in a block has no `TOK_NEWLINE` after it, so the grammar's `expr NEWLINE` productions cannot match it as written.

The parser therefore accepts a **statement end**, defined as any of:

- `TOK_NEWLINE`, consumed; or
- a lookahead of `TOK_RBRACE`, not consumed; or
- a lookahead of `TOK_EOF`, not consumed.

The same three-way terminator applies everywhere [03-grammar.md](03-grammar.md#statements) writes `NEWLINE` in a statement production. Failing to find one is `DT0032`.

---

## Markup

The `<` disambiguation in [03-grammar.md](03-grammar.md#disambiguation) has three conditions. **Two of them are lexical and one is not**, and splitting them at that seam is what keeps both halves simple ([D059](01-decisions.md#d059)).

Conditions 2 and 3 — immediately followed by a letter, `_`, or `/`, with no intervening whitespace, forming a valid tag name followed by whitespace, `>`, `/`, or `=` — are decidable from the byte stream alone. The lexer checks them and emits `TOK_MARKUP_START` for the `<` when they hold, and `TOK_LT` when they do not.

Condition 1 — appears in expression position — is the parser's, because only the parser knows. The parser resolves it by position:

- In **expression** position, `TOK_MARKUP_START` opens a markup literal, and the lexer is pushed into `LEX_MARKUP_TAG`.
- In **operator** position, `TOK_MARKUP_START` is read as less-than. Its span covers only the `<`, so this reinterpretation needs no re-lexing and loses nothing.

So `x = <div>` is markup, `a <b` is a comparison, and neither requires the lexer to know anything about the grammar. One token of lookahead suffices, exactly as [03-grammar.md](03-grammar.md#disambiguation) claims.

Control blocks inside markup emit `TOK_CTRL_START` for `{` and `TOK_CTRL_END` for `}`, with ordinary expression tokens between them — so `{if u.bio != nil}` is `CTRL_START KW_IF IDENT DOT IDENT BANG_EQ KW_NIL CTRL_END`. `{end}` is `CTRL_START IDENT(end) CTRL_END`, matched by text per [D061](01-decisions.md#d061).

Element nesting is tracked by the parser, not the lexer, because it is a tree-shaped constraint and the parser is already building the tree. `</>` closing the most recent open element is therefore a parser rule with the open-element stack right there. Mismatched and unclosed tags are `DT0060`–`DT0062`, and the diagnostic attaches the open tag's span as a related label so the human output shows both ends of the mistake — which is what the `related` field in [06-tooling.md](06-tooling.md#diagnostics) exists for.

---

## The AST

**Arena-allocated tagged unions, in separate node families, with intrusive child lists** ([D063](01-decisions.md#d063)).

Seven families: `decl`, `stmt`, `expr`, `type_ref`, `pattern`, `markup_node`, `attr`. Separate families rather than one universal node, so that a function taking an `expr *` cannot be handed a statement, and so that each family's `switch` is exhaustive over only what it can actually contain.

Each node is a `kind` tag, a `span at`, a union of the per-kind payloads, and a `next` pointer:

```c
typedef struct expr expr;

struct expr {
  expr_kind kind;
  span at;
  union { /* per-kind payloads */ } as;
  expr *next;
};

typedef struct {
  expr *first;
  expr *last;
  uint32_t count;
} expr_list;
```

Children are **intrusive singly-linked lists with `first`, `last`, and `count`** — the shape `diag_sink` and `diag_label` already use in the base layer. The reason is the arena: `arena_extend` only grows the arena's most recent allocation, so building a contiguous array of children while simultaneously allocating those children is not possible. A linked list appends in O(1) with no resize and no second pass, and every consumer of an AST list walks it in order anyway. Where a consumer needs a count, `count` is already there.

`span at` on every node, widened with the existing `span_join`, which is `span_none`-tolerant so a node with no children still gets a sensible span.

### Node constructors do not fail

The AST is allocated from the compilation arena, which is created with `arena_new_fatal` and therefore aborts on exhaustion rather than returning `NULL` ([D047](01-decisions.md#d047)). Node constructors consequently **return a node, never an error**, and the parser carries no allocation-failure plumbing.

This is a deliberate divergence from the base layer, where every allocation is checked. The distinction is that `arena`, `slice`, `buf`, `source`, and `diag` are shared with the **runtime**, where a request arena returns `NULL` so the VM can convert it to a `budget_exceeded` fault. The AST is compiler-only and can rely on the failure policy its arena was constructed with — which is the entire reason [D047](01-decisions.md#d047) puts the policy on the arena instead of at the call site.

---

## Front-end diagnostics

The lexical and syntactic range is `DT0001`–`DT0099` ([06-tooling.md](06-tooling.md#code-ranges)). It is **allocated in full here, now** ([D064](01-decisions.md#d064)), so that independent workstreams cannot collide on a number, and so that a code is chosen by where it belongs rather than by what happened to be free.

Sub-ranges:

| Sub-range | Category |
| --- | --- |
| `DT0001`–`DT0009` | source intake |
| `DT0010`–`DT0029` | lexical |
| `DT0030`–`DT0059` | syntactic |
| `DT0060`–`DT0079` | markup syntax |
| `DT0080`–`DT0099` | held for front-end growth |

Markup *semantic* errors — a `bool` in a text position, an attribute that takes a `{str: str}` given something else — are types and routes, and live in `DT0200`–`DT0299` and `DT0500`–`DT0599`. Only markup **syntax** is here.

### Source intake

| Code | Meaning |
| --- | --- |
| `DT0001` | source is not valid UTF-8 |
| `DT0002` | source file is too large |
| `DT0003` | source contains a NUL byte |
| `DT0004`–`DT0009` | held |

These three exist and are implemented in `src/base/source.c`.

### Lexical

| Code | Meaning |
| --- | --- |
| `DT0010` | unexpected character |
| `DT0011` | unterminated string literal |
| `DT0012` | unterminated raw string literal |
| `DT0013` | unterminated block comment |
| `DT0014` | unknown escape sequence |
| `DT0015` | malformed unicode escape |
| `DT0016` | unicode escape is out of range or a surrogate |
| `DT0017` | malformed number literal |
| `DT0018` | integer literal is out of range for `int` |
| `DT0019` | float literal is out of range for `float` |
| `DT0020` | misplaced underscore in a numeric literal |
| `DT0021` | unterminated interpolation |
| `DT0022` | interpolation or markup is nested too deeply |
| `DT0023` | reserved word |
| `DT0024`–`DT0029` | held |

`DT0018` matters more than its position in the list suggests: it is the compile-time half of [D002](01-decisions.md#d002)'s objection to truncated integers. A literal that does not fit in an i64 is rejected at the source rather than silently wrapped.

`DT0023` is one code for all thirty-five reserved words, with the specific message supplied per word from a table ([D062](01-decisions.md#d062)).

### Syntactic

| Code | Meaning |
| --- | --- |
| `DT0030` | unexpected token |
| `DT0031` | unexpected end of input |
| `DT0032` | expected a newline to end the statement |
| `DT0033` | a top-level binding must be `let`, not `var` |
| `DT0034` | a method's first parameter must be `self` |
| `DT0035` | `send` outside a `stream` body |
| `DT0036` | `self` outside a method body |
| `DT0037` | `break` outside a loop |
| `DT0038` | `continue` outside a loop |
| `DT0039` | comparison operators do not chain |
| `DT0040` | `spawn` requires a call expression |
| `DT0041` | `defer` requires a call expression |
| `DT0042` | duplicate parameter name |
| `DT0043` | duplicate field name |
| `DT0044` | duplicate enum variant |
| `DT0045` | unknown attribute |
| `DT0046` | this feature is not available in this version |
| `DT0047` | `pub` is not allowed here |
| `DT0048`–`DT0059` | held |

`DT0046` is what [D042](01-decisions.md#d042) and [07-roadmap.md](07-roadmap.md#the-release-model) promise: `spawn`, `send`, and `stream` are keywords and parse correctly in v0.1, and using one produces a specific message about when it lands rather than a confusing syntax error.

`DT0039` implements the non-associativity of `cmp_expr` — `a < b < c` is rejected with an explanation rather than silently parsed.

### Markup syntax

| Code | Meaning |
| --- | --- |
| `DT0060` | unclosed markup element |
| `DT0061` | closing tag does not match the open tag |
| `DT0062` | closing tag with no open element |
| `DT0063` | a void element must not have a closing tag |
| `DT0064` | malformed tag name |
| `DT0065` | malformed attribute name |
| `DT0066` | an attribute value must be a string or an interpolation |
| `DT0067` | duplicate attribute |
| `DT0068` | unknown markup control directive |
| `DT0069` | missing `{end}` |
| `DT0070` | `{else}` with no matching `{if}` or `{for}` |
| `DT0071` | `{else if}` after `{else}` |
| `DT0072` | unterminated markup comment |
| `DT0073`–`DT0079` | held |

Two codes reserved above turned out to be unreachable and are therefore **not registered**, per [D065](01-decisions.md#d065):

- `DT0062` (closing tag with no open element) — the lexer only produces `TOK_TAG_CLOSE_START` inside element content, so a close tag with nothing open cannot be lexed.
- `DT0073` (`...` outside a tag) — content mode lexes `...` as ordinary text and never as `TOK_ELLIPSIS`, so the spread token only exists inside a tag in the first place.

Both numbers stay retired rather than being reused, because a code is permanent once assigned ([D050](01-decisions.md#d050)). The reservation was cheap; registering an explanation the compiler can never emit would not have been.

### Which well-formedness rules the parser discharges

[03-grammar.md](03-grammar.md#well-formedness-rules) lists sixteen rules and says they are enforced "in the resolver and typechecker." Five of them need **only syntactic context** — a flag saying whether the parser is inside a loop, a method, or a stream body — and are enforced in the parser instead:

| Rule | Code |
| --- | --- |
| 1. a top-level binding must be `let` | `DT0033` |
| 9. a method's first parameter must be `self` | `DT0034` |
| 10. `send` only inside a `stream` body | `DT0035` |
| 11. `self` only inside a method body | `DT0036` |
| 12. `break` and `continue` only inside a loop | `DT0037`, `DT0038` |

Earlier is better here: the parser has the tightest span, it already tracks the context, and moving these out means the resolver carries no loop or method state purely to produce an error the parser could already see. The remaining eleven rules need names, types, the route table, or control-flow analysis, and are allocated codes in `DT0100`+ in the semantic pass.

### A code enters the registry with its test, not before

The allocation above is a **reservation, not a registration** ([D065](01-decisions.md#d065)).

`src/base/diag_codes.h` gains a row only in the same change that adds the code's implementation and the test that produces it. This is forced, and it is right: `tools/check-docs.sh` fails when a code in the registry is not produced by a **spec** test, which is [D049](01-decisions.md#d049) made mechanical — with an explicit four-code allowlist for the ones that describe the driver's environment rather than source text and so cannot be reached from a `.do` file at all ([D079](01-decisions.md#d079)). Pre-populating the registry with forty rows would break the `docs` gate immediately, and it would also be a stub table — forty codes that `doot explain` describes and the compiler never emits ([D054](01-decisions.md#d054)).

So the number is decided here, in the document, and the row is written later, with the code that emits it. The reservation is what keeps two workstreams from both taking `DT0031`; the gate is what keeps the registry honest.

---

## Specification tests

`tests/spec/` is the primary suite ([D049](01-decisions.md#d049)) and it is **specified in [11-spec-tests.md](11-spec-tests.md)** — the directive grammar, the matching rules, the runner's interface, the layout, and the gates.

It has its own document because it outlives this one. Every later milestone adds files to it and adds no new mechanism, so the mechanism belongs somewhere that is not a front-end document. What matters here is only the part that constrains the front end: **the suite drives `doot fmt`**, because that is the first command that is complete rather than partial, and every lexical and syntactic diagnostic is therefore reachable and spec-testable at the parser milestone rather than waiting for the typechecker ([D067](01-decisions.md#d067)).

---

## Implementation order

Three steps, in this order, each finished to the [definition of done](09-engineering.md#definition-of-done) before the next begins ([D067](01-decisions.md#d067)):

1. **The lexer.** Unit tests over the token stream and the line-structure algorithm, plus `fuzz_lex`. No user-facing command changes.
2. **The parser.** Unit tests over AST shape, plus `fuzz_parse`. Still no user-facing command changes.
3. **`doot fmt`, the spec runner, and `tests/spec/`.** The first user-facing command, and the point at which every lexical and syntactic diagnostic becomes both reachable and spec-testable.

The third step is where the sequencing had to be worked out rather than assumed. [09-engineering.md](09-engineering.md#testing) previously said spec tests arrive "with the lexer," and they cannot: a spec test drives a real command, `doot check` is the command the directives assume, and `doot check` cannot ship until it genuinely typechecks ([D054](01-decisions.md#d054)). Waiting for the typechecker would leave the primary suite until last, which is worse.

`doot fmt` resolves it. Formatting needs the lexer, the parser, and a printer, and nothing else — so it is **complete, not partial**, at the parser milestone. It reports lexical and syntactic diagnostics through the same sink and the same `--json` schema as every other command, `expect-fmt-stable` is already a specified directive, and idempotent formatting is a genuinely demanding test of whether the AST and the trivia list preserve everything they should.

### Fuzz targets

Both are named in [09-engineering.md](09-engineering.md#5-fuzzing--fuzz) and land with their stage, with hand-written seeds committed to `fuzz/corpus/<target>/`:

| Target | Drives |
| --- | --- |
| `fuzz_lex` | `lex_next` to `TOK_EOF` in `LEX_NORMAL`, asserting the stream terminates and every diagnostic has a span inside the source |
| `fuzz_parse` | a full parse, asserting arbitrary input yields diagnostics rather than a crash, a hang, or unbounded memory |

`fuzz_lex` covers the mode stack only through string interpolation, since markup mode is entered by the parser; markup tokenization is reached through `fuzz_parse`. That is a real limitation of the split in [D059](01-decisions.md#d059), and it is the correct trade: the alternative is a lexer that guesses at expression position.


---

## The printer

`src/parse/print.c`, behind `doot fmt`. One format, no options ([D039](01-decisions.md#d039)); what it normalizes and what it preserves is [D068](01-decisions.md#d068).

The printer works from the AST, not the token stream, so it can only reproduce what the tree records. Three things are therefore read back out of the source through spans:

| Recovered from the source | Why the AST cannot hold it |
| --- | --- |
| comments | they are not tokens ([D067](01-decisions.md#d067)); the lexer collects them into the unit's comment list |
| whether a blank line separated two declarations | whitespace is not a node |
| whether the author broke an argument list or a markup body | line structure is not a node either, and [D068](01-decisions.md#d068) preserves it |

Two of those need a precise question rather than an obvious one. "Did the author break this list" is **not** "does this construct span a newline": a call whose argument is a multi-line markup literal always spans one, so `layout(room, <div>` would explode into one argument per line even though the argument list was never broken. The question is whether a newline falls *between* the opening delimiter and the first element, or between two elements.

### Parentheses are re-derived

The AST does not record the author's parentheses, so the printer puts back exactly the ones the tree requires, from the precedence table in [03-grammar.md](03-grammar.md#precedence). A left-associative operator prints its right operand one level tighter; `else` is right-associative and prints its left operand one level tighter.

This makes the printer a check on the parser. If a tree were shaped wrongly — a mis-associated chain, an operator at the wrong level — the reprinted parentheses would move, and `expect-fmt-stable` would catch it. `(1 + 2) * 3` keeps its parentheses and `1 + (2 * 3)` loses them, because only one of the two is load-bearing.

### Idempotence

Every printer test asserts that formatting the output again is a no-op, rather than leaving that to one test of its own. It is the strongest available check that the AST and the comment list together capture everything a source file means: anything the tree quietly drops shows up as a second pass that differs from the first.

The strongest single case is that **the chat application in [02-syntax.md](02-syntax.md#a-complete-application) formats to itself, byte for byte.** If the printer and the documentation disagreed, one of them would be wrong, and this is the assertion that says which.

---

## The filesystem boundary

ISO C has no directory traversal, and `doot fmt` has to walk a project, so `src/base/fs.h` is the first part of doot that needs an operating system interface — and deliberately the only one. The POSIX implementation covers Linux and macOS; Windows arrives with the rest of [v0.5](07-roadmap.md#v05--everywhere) and needs `FindFirstFile` in that one file, which is why the API returns a plain list rather than exposing a handle.

Three properties are worth stating, because each of them is a decision rather than an implementation detail:

- **Entries come back sorted by name.** `readdir` order is whatever the filesystem chooses, and a tool that rewrites source files must visit them in the same order every time, or its report changes between machines for no reason.
- **A write replaces the file through a temporary and a rename.** A formatter that truncates a file it then fails to rewrite is worse than one that does nothing. `rename` is ISO C, so this costs nothing in portability.
- **Dotfiles are skipped**, which keeps `.git` out of a project walk without needing a list of directories to ignore.

`fs_read_dir` and `fs_write_file` take a `diag_sink` and report `DT1003` and `DT1002` themselves, the same way `source_from_file` reports `DT1001`. That is not symmetry for its own sake: a code that only the CLI can reach cannot be unit-tested, and [D049](01-decisions.md#d049) does not allow a registered code with no test.

### A file that does not parse is not formatted

`doot fmt` skips it and reports the diagnostics. The parser recovers in order to find more than one error per run ([parse.h](../src/parse/parse.h)), so a failed parse leaves a tree with holes in it — and printing that tree would produce plausible-looking source that silently differs from what the author wrote. Refusing is the only safe answer, and it is why `fmt_unit` documents that its caller must check the sink first.
