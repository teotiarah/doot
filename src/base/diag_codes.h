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
  X(DIAG_CANNOT_READ_FILE, "DT1001", DIAG_ERROR, "cannot read file",                               \
    "The file could not be opened or read.\n"                                                      \
    "\n"                                                                                           \
    "Check that the path is spelled correctly, that the file exists, and that you have\n"          \
    "permission to read it.")
