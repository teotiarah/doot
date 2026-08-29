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
  X(DIAG_UNCLOSED_ELEMENT, "DT0060", DIAG_ERROR, "unclosed markup element",                        \
    "A markup element was opened and never closed.\n"                                              \
    "\n"                                                                                           \
    "Close it with `</name>`, or with `</>` to close the most recent open element. An\n"           \
    "element with no content may be written self-closing: `<br/>`.\n"                              \
    "\n"                                                                                           \
    "The span points at the opening `<`, which is the element that is still open.")                \
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
  X(DIAG_UNTERMINATED_MARKUP_COMMENT, "DT0072", DIAG_ERROR, "unterminated markup comment",         \
    "A markup comment opened with `<!--` and was never closed with `-->`.\n"                       \
    "\n"                                                                                           \
    "Markup comments do not nest, so the first `-->` closes the comment.")                         \
                                                                                                   \
  X(DIAG_CANNOT_READ_FILE, "DT1001", DIAG_ERROR, "cannot read file",                               \
    "The file could not be opened or read.\n"                                                      \
    "\n"                                                                                           \
    "Check that the path is spelled correctly, that the file exists, and that you have\n"          \
    "permission to read it.")
