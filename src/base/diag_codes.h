/* diag_codes.h -- the diagnostic registry (D050).
 *
 * The single source of truth for every diagnostic doot can emit. The compiler,
 * `doot explain`, `--json` output, and the generated reference all derive from
 * this table, so an explanation cannot drift from the code that emits it.
 *
 * Rules:
 *   - A code is permanent once assigned: never renumbered, reused, or repurposed.
 *   - A code does not exist until a spec test produces it (D049).
 *   - Ranges are assigned in docs/06-tooling.md#code-ranges.
 *
 * Fields: enum id, code string, severity, brief, explanation.
 * The brief is the canonical short description. The message passed to
 * diag_report() carries the specifics.
 */

#define DOOT_DIAG_CODES(X)                                                                         \
  X(DIAG_INVALID_UTF8, "DT0001", DIAG_ERROR, "source is not valid UTF-8",                          \
    "doot source files must be valid UTF-8. This file contains a byte sequence that is not.\n"     \
    "\n"                                                                                           \
    "Common causes are a file saved in a legacy single-byte encoding such as Latin-1 or\n"         \
    "Windows-1252, or a binary file given a .do extension.\n"                                      \
    "\n"                                                                                           \
    "doot rejects invalid UTF-8 rather than substituting replacement characters, because\n"        \
    "silently altering source text would make spans in later diagnostics point at\n"               \
    "positions that do not exist in your file.")                                                   \
                                                                                                   \
  X(DIAG_SOURCE_TOO_LARGE, "DT0002", DIAG_ERROR, "source file is too large",                       \
    "A single doot source file may not exceed 64 MB.\n"                                            \
    "\n"                                                                                           \
    "Source positions are 32-bit byte offsets throughout the compiler, which keeps every\n"        \
    "span to 8 bytes and every diagnostic cheap to carry. The limit is far above any\n"            \
    "hand-written file; reaching it usually means a generated or concatenated file.")              \
                                                                                                   \
  X(DIAG_SOURCE_NUL_BYTE, "DT0003", DIAG_ERROR, "source contains a NUL byte",                      \
    "doot source files may not contain NUL (0x00) bytes.\n"                                        \
    "\n"                                                                                           \
    "A NUL in source is almost always a truncated write or a binary file with a .do\n"             \
    "extension. Rejecting it also means that no part of the compiler has to distinguish\n"         \
    "between a byte in the text and the end of the text.")                                         \
                                                                                                   \
  X(DIAG_UNEXPECTED_CHAR, "DT0010", DIAG_ERROR, "unexpected character",                            \
    "This byte cannot begin any doot token.\n"                                                     \
    "\n"                                                                                           \
    "Common causes are a stray character from another language -- a `;` line terminator, a\n"      \
    "`#` comment, a `&` or `~` operator -- or a smart quote pasted from a document in\n"           \
    "place of a plain `\"`.\n"                                                                     \
    "\n"                                                                                           \
    "doot statements are newline-terminated and need no `;`. Comments are `//` and `/* */`.")      \
                                                                                                   \
  X(DIAG_UNTERMINATED_STRING, "DT0011", DIAG_ERROR, "unterminated string literal",                 \
    "A string literal opened with `\"` and was never closed.\n"                                    \
    "\n"                                                                                           \
    "A string may not span lines, so the literal ends at the end of the line whether you\n"        \
    "closed it or not. The span points at the opening quote, which is where the mistake\n"         \
    "is, rather than at the end of the line, which is only where it was noticed.\n"                \
    "\n"                                                                                           \
    "For text containing a `\"`, escape it as `\\\"`. For multi-line or literal text, use a\n"     \
    "raw string in backticks, which spans lines and interprets no escapes.")                       \
                                                                                                   \
  X(DIAG_UNTERMINATED_RAW_STRING, "DT0012", DIAG_ERROR, "unterminated raw string literal",         \
    "A raw string opened with a backtick and was never closed.\n"                                  \
    "\n"                                                                                           \
    "Raw strings span lines and interpret no escape sequences, so nothing inside one can\n"        \
    "close it except another backtick. A raw string therefore cannot contain a backtick.")         \
                                                                                                   \
  X(DIAG_UNTERMINATED_BLOCK_COMMENT, "DT0013", DIAG_ERROR, "unterminated block comment",           \
    "A block comment opened with `/*` and was never closed.\n"                                     \
    "\n"                                                                                           \
    "Block comments nest, so an inner `/*` consumes one of the following `*/`. Commenting\n"       \
    "out a region that already contains a block comment is safe, but every `/*` inside it\n"       \
    "still needs its own `*/`.\n"                                                                  \
    "\n"                                                                                           \
    "The span points at the outermost `/*`, which is the comment that actually failed to\n"        \
    "close.")                                                                                      \
                                                                                                   \
  X(DIAG_UNKNOWN_ESCAPE, "DT0014", DIAG_ERROR, "unknown escape sequence",                          \
    "The escapes are `\\n`, `\\t`, `\\r`, `\\\\`, `\\\"`, `\\$`, and `\\u{...}`.\n"                \
    "\n"                                                                                           \
    "`\\$` exists because `${` begins an interpolation, so a literal dollar followed by a\n"       \
    "brace needs escaping. A lone `$` needs nothing.\n"                                            \
    "\n"                                                                                           \
    "For text with many backslashes -- a path, a pattern -- a raw string in backticks\n"           \
    "interprets no escapes at all.")                                                               \
                                                                                                   \
  X(DIAG_MALFORMED_UNICODE_ESCAPE, "DT0015", DIAG_ERROR, "malformed unicode escape",               \
    "A unicode escape is written `\\u{...}` with braces and at least one hex digit:\n"             \
    "`\\u{41}`, `\\u{1f600}`.\n"                                                                   \
    "\n"                                                                                           \
    "The braces are required, and the digit count is not fixed, so there is one spelling\n"        \
    "for every scalar value rather than a short form and a long form.")                            \
                                                                                                   \
  X(DIAG_UNICODE_ESCAPE_RANGE, "DT0016", DIAG_ERROR, "unicode escape is out of range",             \
    "A unicode escape must name a scalar value: U+0000 to U+10FFFF, excluding the\n"               \
    "surrogate range U+D800 to U+DFFF.\n"                                                          \
    "\n"                                                                                           \
    "Surrogates exist only to encode astral characters in UTF-16. doot strings are UTF-8\n"        \
    "(D001 of the source rules: text is validated on load), so a surrogate is not a\n"             \
    "character and cannot be represented. Write the character itself instead: `\\u{1f600}`\n"      \
    "rather than a surrogate pair.")                                                               \
                                                                                                   \
  X(DIAG_MALFORMED_NUMBER, "DT0017", DIAG_ERROR, "malformed number literal",                       \
    "This numeric literal is not well formed.\n"                                                   \
    "\n"                                                                                           \
    "Integers are decimal (`42`), hexadecimal (`0xff`), or binary (`0b1011`). Radix\n"             \
    "prefixes are lowercase. A hexadecimal or binary literal needs at least one digit\n"           \
    "after its prefix, and an exponent needs at least one digit after `e`.\n"                      \
    "\n"                                                                                           \
    "Note that a `.` is part of a number only when a digit follows it, which is what\n"            \
    "makes `16.mb` a method call on an integer rather than a malformed float.")                    \
                                                                                                   \
  X(DIAG_INT_LITERAL_RANGE, "DT0018", DIAG_ERROR, "integer literal is out of range",               \
    "`int` is signed 64-bit, so a literal must lie between -9223372036854775808 and\n"             \
    "9223372036854775807.\n"                                                                       \
    "\n"                                                                                           \
    "doot rejects this at the source rather than truncating it. Silent truncation on an\n"         \
    "identifier field is exactly the class of bug that destroys trust in a language, and\n"        \
    "it is the reason doot does not use NaN boxing (D002).\n"                                      \
    "\n"                                                                                           \
    "There is no unsigned integer type and no bignum. For a value that does not fit, hold\n"       \
    "it as `str` or `bytes`.")                                                                     \
                                                                                                   \
  X(DIAG_FLOAT_LITERAL_RANGE, "DT0019", DIAG_ERROR, "float literal is out of range",               \
    "`float` is an IEEE 754 double, so a literal must be representable as one.\n"                  \
    "\n"                                                                                           \
    "Note that money is `int` in minor units and there is no decimal type (D020), so a\n"          \
    "very large or very precise monetary amount belongs in integer cents rather than in a\n"       \
    "float.")                                                                                      \
                                                                                                   \
  X(DIAG_MISPLACED_UNDERSCORE, "DT0020", DIAG_ERROR, "misplaced underscore in a number",           \
    "An underscore in a numeric literal must separate two digits.\n"                               \
    "\n"                                                                                           \
    "`1_000_000` is fine. A leading, trailing, or doubled underscore is not: `1_`, `1__0`,\n"      \
    "and `0x_ff` are all rejected.\n"                                                              \
    "\n"                                                                                           \
    "Underscores are a readability aid with no effect on the value, so permitting them in\n"       \
    "positions where they carry no meaning would only create two spellings of one\n"               \
    "number.")                                                                                     \
                                                                                                   \
  X(DIAG_UNTERMINATED_INTERP, "DT0021", DIAG_ERROR, "unterminated interpolation",                  \
    "An interpolation opened with `${` and was never closed.\n"                                    \
    "\n"                                                                                           \
    "Braces inside an interpolation nest, so a map or struct literal within one is fine:\n"        \
    "`\"${count({\"a\": 1})}\"` closes at the last `}`. Every `{` still needs its `}`.")           \
                                                                                                   \
  X(DIAG_NESTING_TOO_DEEP, "DT0022", DIAG_ERROR, "nested too deeply",                              \
    "Interpolation and markup may nest 64 levels deep.\n"                                          \
    "\n"                                                                                           \
    "The limit exists because scanning depth is reachable from any input the compiler is\n"        \
    "given, and an unbounded one would be a way to exhaust memory. Reaching it in\n"               \
    "hand-written code is not plausible; extract a function instead.\n"                            \
    "\n"                                                                                           \
    "This is a diagnostic rather than a crash precisely because the input is untrusted.")          \
                                                                                                   \
  X(DIAG_RESERVED_WORD, "DT0023", DIAG_ERROR, "reserved word",                                     \
    "This word is reserved and has no meaning in doot.\n"                                          \
    "\n"                                                                                           \
    "Thirty-five words are reserved but unused, so that reaching for a construct from\n"           \
    "another language fails with a specific message naming the doot equivalent instead of\n"       \
    "a confusing parse error (D042). The message above names it.\n"                                \
    "\n"                                                                                           \
    "They are also reserved so that no future addition to doot can be a breaking change.")         \
                                                                                                   \
  X(DIAG_UNEXPECTED_TOKEN, "DT0030", DIAG_ERROR, "unexpected token",                               \
    "The parser expected something else here. The message names what.\n"                           \
    "\n"                                                                                           \
    "Statements are newline-terminated and need no `;`. Blocks use braces, never\n"                \
    "indentation. Types are written `name: type`. A function's return type follows `->`.")         \
                                                                                                   \
  X(DIAG_UNEXPECTED_EOF, "DT0031", DIAG_ERROR, "unexpected end of input",                          \
    "The file ended in the middle of a construct.\n"                                               \
    "\n"                                                                                           \
    "Usually an unclosed brace, bracket, or parenthesis earlier in the file. The span here\n"      \
    "is the end of the file, which is where it was noticed rather than where it went\n"            \
    "wrong; look for the innermost construct that is still open.")                                 \
                                                                                                   \
  X(DIAG_EXPECTED_STMT_END, "DT0032", DIAG_ERROR, "expected the statement to end",                 \
    "A statement ends at a newline, or before a `}` that closes its block.\n"                      \
    "\n"                                                                                           \
    "Two statements on one line are not permitted, because there is no separator to write:\n"      \
    "doot is newline-terminated and has no `;` (D060).\n"                                          \
    "\n"                                                                                           \
    "An expression may still break across lines freely after any operator or opening\n"            \
    "bracket, with no continuation marker.")                                                       \
                                                                                                   \
  X(DIAG_TOPLEVEL_MUST_BE_LET, "DT0033", DIAG_ERROR, "a top-level binding must be `let`",          \
    "There are no mutable globals in doot, and this is the single most valuable\n"                 \
    "constraint in the design (D008).\n"                                                           \
    "\n"                                                                                           \
    "The reason is the shared-nothing runtime: with N workers, a module-level mutable\n"           \
    "global silently becomes per-worker. A counter would give correct answers with one\n"          \
    "worker in development and wrong answers with sixteen in production. Rather than\n"            \
    "document that hazard, the language makes the construct impossible.\n"                         \
    "\n"                                                                                           \
    "State belongs in one of three places: SQLite for anything durable, request scope for\n"       \
    "anything per-request, or a `cache` cell whose per-worker semantics are documented.")          \
                                                                                                   \
  X(DIAG_METHOD_NEEDS_SELF, "DT0034", DIAG_ERROR, "a method's first parameter must be `self`",     \
    "A method is declared `fn TypeName.method(self, ...)`, and its first parameter is\n"           \
    "`self`.\n"                                                                                    \
    "\n"                                                                                           \
    "Conversely, `self` is only a parameter of a method. A free function that wants a value\n"     \
    "takes it as an ordinary named parameter.\n"                                                   \
    "\n"                                                                                           \
    "There are no classes and no inheritance (D016): a method is a free function attached\n"       \
    "to a type by name, which is why the receiver is written out rather than implied.")            \
                                                                                                   \
  X(DIAG_SEND_OUTSIDE_STREAM, "DT0035", DIAG_ERROR, "`send` outside a `stream` body",              \
    "`send` writes one server-sent event, so it is only meaningful inside a `stream`\n"            \
    "declaration.\n"                                                                               \
    "\n"                                                                                           \
    "A `route` returns one response; a `stream` sends many. To push updates to a page, add\n"      \
    "a `stream` declaration and have the page reference it with `data-live`.")                     \
                                                                                                   \
  X(DIAG_SELF_OUTSIDE_METHOD, "DT0036", DIAG_ERROR, "`self` outside a method body",                \
    "`self` names a method's receiver, so it has no meaning in a free function, a route,\n"        \
    "or a lambda that is not inside a method.\n"                                                   \
    "\n"                                                                                           \
    "Declare a method with `fn TypeName.method(self)` to have a receiver to refer to.")            \
                                                                                                   \
  X(DIAG_BREAK_OUTSIDE_LOOP, "DT0037", DIAG_ERROR, "`break` outside a loop",                       \
    "`break` leaves the nearest enclosing `for` or `while`, so it needs one.\n"                    \
    "\n"                                                                                           \
    "Note that `match` is not a loop and has no fallthrough, so its arms need no `break`\n"        \
    "(which is one of the reasons doot has `match` and not `switch`).")                            \
                                                                                                   \
  X(DIAG_CONTINUE_OUTSIDE_LOOP, "DT0038", DIAG_ERROR, "`continue` outside a loop",                 \
    "`continue` starts the next iteration of the nearest enclosing `for` or `while`, so it\n"      \
    "needs one.")                                                                                  \
                                                                                                   \
  X(DIAG_COMPARISON_CHAIN, "DT0039", DIAG_ERROR, "comparison operators do not chain",              \
    "`a < b < c` is a syntax error rather than a surprise.\n"                                      \
    "\n"                                                                                           \
    "In most languages it parses as `(a < b) < c`, comparing a boolean against a number,\n"        \
    "which is either a type error or -- worse -- silently meaningful. doot makes the\n"            \
    "comparison operators non-associative so the mistake cannot be written.\n"                     \
    "\n"                                                                                           \
    "Write `a < b and b < c`.")                                                                    \
                                                                                                   \
  X(DIAG_SPAWN_NEEDS_CALL, "DT0040", DIAG_ERROR, "`spawn` takes a function call",                  \
    "`spawn` starts a task running a function, so it needs a call: `spawn send_email(id)`.\n"      \
    "\n"                                                                                           \
    "The arguments are evaluated in the current task and captured immutably, so a data\n"          \
    "race in a spawned closure is not merely unlikely -- it is unrepresentable (D008).")           \
                                                                                                   \
  X(DIAG_DEFER_NEEDS_CALL, "DT0041", DIAG_ERROR, "`defer` takes a function call",                  \
    "`defer` runs a call when the enclosing function returns, so it needs a call:\n"               \
    "`defer file.close()`.\n"                                                                      \
    "\n"                                                                                           \
    "There are no exceptions and no unwinding (D012), so a deferred call runs on the\n"            \
    "normal return path and when a fault terminates the task.")                                    \
                                                                                                   \
  X(DIAG_DUPLICATE_PARAM, "DT0042", DIAG_ERROR, "duplicate parameter name",                        \
    "Two parameters in one signature have the same name, so one of them could never be\n"          \
    "referred to.")                                                                                \
                                                                                                   \
  X(DIAG_DUPLICATE_FIELD, "DT0043", DIAG_ERROR, "duplicate field name",                            \
    "Two fields in one struct have the same name.\n"                                               \
    "\n"                                                                                           \
    "Field names are also how `db` maps a SQL result to a struct (D033), so a duplicate\n"         \
    "would make that mapping ambiguous as well as the field access.")                              \
                                                                                                   \
  X(DIAG_DUPLICATE_VARIANT, "DT0044", DIAG_ERROR, "duplicate enum variant",                        \
    "Two variants of one enum have the same name.")                                                \
                                                                                                   \
  X(DIAG_UNKNOWN_ATTRIBUTE, "DT0045", DIAG_ERROR, "unknown attribute",                             \
    "Attributes are a closed set of twelve: `@len`, `@min`, `@max`, `@one_of`, `@email`,\n"        \
    "`@url`, `@trim`, `@max_size`, `@content_type`, `@before`, `@after`, `@deprecated`.\n"         \
    "\n"                                                                                           \
    "There are no user-defined macros, decorators, or annotations, and there will not be\n"        \
    "any (D043). Metaprogramming is the fastest way to make a codebase unreadable to both\n"       \
    "a newcomer and a static analyzer, and it would put an unbounded surface behind a\n"           \
    "language whose whole premise is a bounded one.\n"                                             \
    "\n"                                                                                           \
    "For validation beyond the declarative cases, call the `validate` module directly.")           \
                                                                                                   \
  X(DIAG_FEATURE_UNAVAILABLE, "DT0046", DIAG_ERROR, "this feature is not available yet",           \
    "The syntax is correct and final, but the feature lands in a later version.\n"                 \
    "\n"                                                                                           \
    "`spawn`, `send`, and `stream` are keywords in v0.1 even though tasks and SSE arrive in\n"     \
    "v0.2. That is deliberate: the keyword list and the grammar are frozen at v0.1 (D042),\n"      \
    "so the grammar never churns and no later addition is a breaking change. The cost is\n"        \
    "this message; the benefit is that code written for 1.0 compiles unchanged on every\n"         \
    "1.x.\n"                                                                                       \
    "\n"                                                                                           \
    "See docs/07-roadmap.md for what lands when.")                                                 \
                                                                                                   \
  X(DIAG_PUB_NOT_ALLOWED, "DT0047", DIAG_ERROR, "`pub` is not allowed here",                       \
    "`pub` exports a function, type, or binding from its file.\n"                                  \
    "\n"                                                                                           \
    "Routes, streams, groups, and tests are not addressed by name from other files, so\n"          \
    "there is nothing for `pub` to export. A route is reachable because the compiler knows\n"      \
    "the whole route table (D024); a test is discovered by `doot test`.")                          \
                                                                                                   \
  X(DIAG_UNCLOSED_ELEMENT, "DT0060", DIAG_ERROR, "unclosed markup element",                        \
    "A markup element was opened and never closed.\n"                                              \
    "\n"                                                                                           \
    "Close it with `</name>`, or with `</>` to close the most recent open element. An\n"           \
    "element with no content may be written self-closing: `<br/>`.\n"                              \
    "\n"                                                                                           \
    "The span points at the opening `<`, which is the element that is still open.")                \
                                                                                                   \
  X(DIAG_MARKUP_TAG_MISMATCH, "DT0061", DIAG_ERROR, "closing tag does not match the open tag",     \
    "The closing tag names a different element than the one that is open.\n"                       \
    "\n"                                                                                           \
    "The diagnostic carries both spans, so the human output shows the opening tag as well\n"       \
    "as the closing one.\n"                                                                        \
    "\n"                                                                                           \
    "`</>` closes the most recent open element without naming it, which avoids the mistake\n"      \
    "entirely in deeply nested markup.")                                                           \
                                                                                                   \
  X(DIAG_MARKUP_VOID_WITH_CLOSE, "DT0063", DIAG_ERROR, "a void element takes no closing tag",      \
    "`br`, `img`, `input`, `hr`, `meta`, `link`, and the other void elements have no\n"            \
    "content, so they have no closing tag.\n"                                                      \
    "\n"                                                                                           \
    "Write `<br>` or `<br/>`; both are accepted and `doot fmt` normalizes to `<br/>`.")            \
                                                                                                   \
  X(DIAG_MALFORMED_TAG_NAME, "DT0064", DIAG_ERROR, "malformed tag name",                           \
    "A tag name must start with a letter or `_`, and may contain letters, digits, `_`,\n"          \
    "and interior hyphens.\n"                                                                      \
    "\n"                                                                                           \
    "`<my-widget>` is a valid tag. `<3>` and `<-x>` are not.")                                     \
                                                                                                   \
  X(DIAG_MALFORMED_ATTR_NAME, "DT0065", DIAG_ERROR, "malformed attribute name",                    \
    "An attribute name must start with a letter or `_`, and may contain letters, digits,\n"        \
    "`_`, and interior hyphens.\n"                                                                 \
    "\n"                                                                                           \
    "`data-live` and `aria-label` are valid. To spread a map of attributes, use `...expr`.")       \
                                                                                                   \
  X(DIAG_MARKUP_BAD_ATTR_VALUE, "DT0066", DIAG_ERROR, "malformed attribute value",                 \
    "An attribute value is a quoted string or an interpolation: `name=\"q\"` or\n"                 \
    "`value=\"${form.email}\"` or `checked=${is_on}`.\n"                                           \
    "\n"                                                                                           \
    "A bare word is not accepted, because it would be ambiguous with the next attribute\n"         \
    "name. An attribute with no value at all is a boolean attribute and is fine:\n"                \
    "`<input required/>`.\n"                                                                       \
    "\n"                                                                                           \
    "Interpolated values are escaped for attribute context automatically, so XSS through\n"        \
    "an attribute is not reachable without `html.raw` (D021).")                                    \
                                                                                                   \
  X(DIAG_MARKUP_DUPLICATE_ATTR, "DT0067", DIAG_ERROR, "duplicate attribute",                       \
    "The same attribute is set twice on one element, so one of the two would silently\n"           \
    "win.\n"                                                                                       \
    "\n"                                                                                           \
    "To build attributes dynamically, spread a map instead: `<div ...attrs/>`.")                   \
                                                                                                   \
  X(DIAG_MARKUP_UNKNOWN_DIRECTIVE, "DT0068", DIAG_ERROR, "unknown markup directive",               \
    "Inside markup, a brace begins a control directive: `{if}`, `{else}`, `{else if}`,\n"          \
    "`{for}`, or `{end}`.\n"                                                                       \
    "\n"                                                                                           \
    "These are the same keywords as statements, so nothing new is learned (D022). To\n"            \
    "interpolate a value rather than branch, write `${expr}`.")                                    \
                                                                                                   \
  X(DIAG_MARKUP_MISSING_END, "DT0069", DIAG_ERROR, "missing `{end}`",                              \
    "A `{if}` or `{for}` inside markup is closed with `{end}`.\n"                                  \
    "\n"                                                                                           \
    "Unlike a statement block, a markup control block has no braces to balance, so the\n"          \
    "terminator is explicit. The span points at the directive that is still open.")                \
                                                                                                   \
  X(DIAG_MARKUP_ELSE_WITHOUT_IF, "DT0070", DIAG_ERROR, "`{else}` with no open `{if}` or `{for}`",  \
    "An `{else}` or `{end}` appeared where no control block was open.\n"                           \
    "\n"                                                                                           \
    "Usually an extra `{end}` earlier closed the block already.")                                  \
                                                                                                   \
  X(DIAG_MARKUP_ELSE_AFTER_ELSE, "DT0071", DIAG_ERROR, "`{else}` is already taken",                \
    "A control block has at most one `{else}`, and `{else if}` cannot follow it.\n"                \
    "\n"                                                                                           \
    "In a `{for}`, the `{else}` arm renders when the collection is empty -- the single most\n"     \
    "common template need, handled without a nested `{if}`.")                                      \
                                                                                                   \
  X(DIAG_UNTERMINATED_MARKUP_COMMENT, "DT0072", DIAG_ERROR, "unterminated markup comment",         \
    "A markup comment opened with `<!--` and was never closed with `-->`.\n"                       \
    "\n"                                                                                           \
    "Markup comments do not nest, so the first `-->` closes the comment.")                         \
                                                                                                   \
  X(DIAG_CANNOT_READ_FILE, "DT1001", DIAG_ERROR, "cannot read file",                               \
    "The file could not be opened or read.\n"                                                      \
    "\n"                                                                                           \
    "Check that the path is spelled correctly, that the file exists, and that you have\n"          \
    "permission to read it.")                                                                      \
                                                                                                   \
  X(DIAG_CANNOT_WRITE_FILE, "DT1002", DIAG_ERROR, "cannot write file",                             \
    "The file could not be replaced.\n"                                                            \
    "\n"                                                                                           \
    "`doot fmt` writes a temporary file beside the target and renames it over the top, so\n"       \
    "a failure here leaves the original file untouched. Check that the directory is\n"             \
    "writable and that the filesystem is not full.")                                               \
                                                                                                   \
  X(DIAG_CANNOT_READ_DIR, "DT1003", DIAG_ERROR, "cannot read directory",                           \
    "The directory could not be listed.\n"                                                         \
    "\n"                                                                                           \
    "Check that the path is spelled correctly and that you have permission to read it.")
