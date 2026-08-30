# Specification tests

Implementation specification for `tests/spec/` and its runner. Decisions are recorded as [D070](01-decisions.md#d070)–[D080](01-decisions.md#d080).

This suite is **the executable form of the language specification and the primary test layer** ([D049](01-decisions.md#d049)). A language is defined by what it accepts and rejects, so the centre of gravity of doot's testing is a large number of small `.do` files with exact expected output — not unit tests over internal functions, which pin down implementation shape rather than behavior.

It gets its own document rather than a section in [10-frontend.md](10-frontend.md) because it outlives the front end. Every later milestone — the resolver, the typechecker, the VM, the server, the stdlib — adds files here and adds no new mechanism, so the mechanism is settled once, now.

---

## The shape of a spec test

A spec test is a `.do` file whose expectations are declared in a comment block at the top:

```do
// doot-spec: fmt
// expect-error: DT0033 at 1:1 "a top-level binding must be `let`; state belongs in SQLite, in request scope, or in a `cache` cell"

var top = 1
```

The runner discovers `tests/spec/**/*.do`, reads the block as **plain text**, invokes the real `doot` binary once per file, and compares structured output exactly ([D066](01-decisions.md#d066)).

Reading the block as text rather than through `lex_new` and its comment list is deliberate ([D070](01-decisions.md#d070)). It is the same argument that gives the runner its own JSON reader: a test tool that reads its own expectations with the implementation under test cannot fail independently of it, and a lexer bug that dropped or mislocated a comment would make the suite mis-read what it was asked to assert. It is also a hard requirement rather than a preference — a large fraction of these files exist precisely because they *fail* to lex, and the expectations still have to be readable.

### The directive block

The block is the **maximal run of lines from line 1 that begin with `//`**. The first line that does not begin with `//` ends it, blank lines included. Directives appear nowhere else in the file; a `//` comment further down is ordinary source.

```
directive_line := "//" SP directive [SP] EOL
```

Exactly one space after `//`, one directive per line, and directive names are lowercase and matched exactly. Files are **LF-terminated**: the line index is built on `\n` alone and `\r` is horizontal whitespace to the lexer, so the runner strips one trailing `\r` per line and the suite requires LF checkouts. That is a one-line concession now instead of a mystery when Windows arrives in [v0.5](07-roadmap.md#v05--everywhere).

Keeping directives at the top, in a contiguous block, is what makes them findable without a parser, keeps them clear of anywhere `doot fmt` might reposition a comment, and puts a file's whole contract in its first screenful.

---

## Directives

| Directive | Asserts |
| --- | --- |
| `doot-spec: <mode>` | how to run it — `check`, `run`, `fmt`, `routes` |
| `expect-ok` | the command reported no diagnostics at all |
| `expect-error: <CODE> [at <line>:<col>] "<message>"` | that exact code, position, and message |
| `expect-warning: <CODE> [at <line>:<col>] "<message>"` | as above, at warning severity |
| `expect-suggestion: <line>:<col>-<line>:<col> -> "<text>"` | a machine-applicable fix, span and replacement ([D038](01-decisions.md#d038)) |
| `expect-fault: <CODE>` | a runtime fault of that kind |
| `expect-fmt-stable` | `doot fmt` leaves the file byte for byte unchanged |
| `expect-output:` | the command's exact stdout, in the following lines |

`<CODE>` is `DT` followed by four digits. `<line>` and `<col>` are 1-based decimals.

**Every file must declare a mode and must assert something.** A file with no `doot-spec:` line, or with a mode but no expectation directive, is a failure rather than a pass ([D071](01-decisions.md#d071)). So is an unrecognized directive name, a malformed argument, and a JSON key the reader does not know. The runner **fails closed** in every direction, because the one outcome worth engineering against is a test that quietly asserts nothing — which is not hypothetical: the fixture that was supposed to pin `docs/02-syntax.md` against the printer carried a hand-corrected copy of the program and could not detect the drift it existed to catch.

### The mode

The mode is the subcommand the runner invokes. All four are understood from the first commit, since they differ only in argv — but **which modes have files depends on which commands exist**, and no command is driven before it fully works ([D054](01-decisions.md#d054)). At the front-end milestone that is `fmt` alone.

A mode whose command is absent from the binary produces exit code 2, which the runner reports as a **runner failure naming the missing command**, distinct from a test failure. Adding `check` files before `doot check` exists therefore fails loudly and specifically instead of as a wall of unexplained mismatches.

### Positions are character columns

`<line>:<col>` is the position `--json` reports, and that number needs stating precisely because **three different columns exist in this codebase** and only one of them belongs in a directive:

| Number | Where | Counts |
| --- | --- | --- |
| `span.start` / `span.end` | `--json` | **byte** offsets from the start of the file |
| `span.col` | `--json` | **characters**, 1-based, from the start of the line |
| the caret position | human output | **display columns**, with tabs expanded to four |

A directive uses the middle one ([D073](01-decisions.md#d073)). `source_line_col` counts characters by skipping UTF-8 continuation bytes, so a line of CJK text or emoji advances one column per character, and a tab is one column regardless of how it renders. An author reading a column off the human caret output of a tab-indented file will write the wrong number, which is the entire reason this table is here.

**`at <line>:<col>` is omitted when the diagnostic has no position.** The three source-intake codes are reported before the source object exists, so there is no line index to resolve an offset against and `--json` carries neither `file` nor `span` for them — the byte offset is in the message. Those expectations are written without a position:

```do
// doot-spec: fmt
// expect-error: DT0003 "`nul_byte.do` contains a NUL byte at offset 118"
```

The runner matches presence-of-position as part of the tuple, so a positioned expectation never matches an unpositioned diagnostic or the reverse. Note that the path in such a message is a bare basename: the runner sets the working directory to the file's own, which is what keeps these messages identical across machines and build profiles.

`expect-suggestion` is given as a **range**, because a fix is a span plus a replacement and getting the span's end wrong means replacing the wrong number of bytes:

```
// expect-suggestion: 3:36-3:41 -> "email"
```

This refines the start-only shorthand in [09-engineering.md](09-engineering.md#2-spec-tests--testsspec). A pure insertion has an empty range, `3:36-3:36`. Since `--json` reports `suggestion.replace_span` as byte offsets and carries no line and column for it, the runner converts the offsets itself, counting lines on `\n` and columns on non-continuation bytes. That duplicates ten lines of `source_line_col` on purpose, for the independence reason above.

### Output

`expect-output:` asserts the command's **exact stdout with `--json` absent**, and must be the last directive in the block. Every remaining line of the block is expected output, verbatim after the `// ` prefix:

```do
// doot-spec: fmt
// expect-ok
// expect-output:
// 1 file already formatted
```

Two consequences worth being explicit about. It generalizes the run-only form in [09-engineering.md](09-engineering.md#2-spec-tests--testsspec) to any mode, which is what pins `doot fmt`'s human summary — the three-way `reformatted` / `already formatted` / `skipped` report is the primary thing a user sees from the command and was otherwise covered by nothing. And because the runner collects diagnostics from `--json`, a file carrying `expect-output` costs **a second invocation** ([D075](01-decisions.md#d075)); the cost is paid only by the files that use it.

### Formatting

`expect-fmt-stable` asserts that formatting the file changes nothing. `doot fmt` rewrites in place, so the runner **copies the file into a scratch directory and formats the copy** ([D074](01-decisions.md#d074)) — the repository is never written to by its own test suite, by construction rather than by remembering to revert.

Formatting the copy also exercises the real write path, temporary file and `rename` included, which a `--stdout` mode would have bypassed. That is the second reason no new `fmt` flag is introduced for this; the first is that a flag existing only to make testing convenient is CLI surface users pay for and never use.

**`expect-fmt-stable` requires the file to be diagnostic-free**, and combining it with `expect-error` is a directive conflict the runner rejects. The reason is precise: `doot fmt` leaves a file that did not parse byte for byte alone, so a broken file is trivially "unchanged" and would satisfy the directive while proving nothing. `expect-ok` establishes that the printer actually ran.

---

## Matching

Diagnostics are compared as an **exact multiset** of `(code, severity, line, col, message)` tuples ([D072](01-decisions.md#d072)). Comparison runs in both directions: an expected diagnostic that does not appear fails the file, and so does a diagnostic that appears unexpected. Messages are compared **in full**, never by substring, so a reworded message is a visible diff rather than a silent pass — which is the property the suite exists for.

Message text is quoted, with `\"` and `\\` as the only escapes. Nothing else is interpreted.

**Order is not asserted here.** Report order is a real guarantee — `diag_sink` preserves it for determinism — but it is a property of the sink, and pinning it in every spec file would make each one brittle to a legitimate reordering of two independent checks in a single pass. The division is that the spec suite settles *what* is reported and the unit tests over `diag_sink` settle the *ordering and mechanics* of reporting.

Exit codes are asserted rather than ignored, from the contract in [06-tooling.md](06-tooling.md#exit-codes): `expect-ok` requires 0, any expected error requires 1, and 2 is the runner failure described above.

---

## The runner

`tests/spec/spec_runner.c`, built as `build/<profile>/doot_spec` beside `doot_test`, outside the amalgamation — `tools/amalgamate.sh` scans only `src/`, so nothing there changes ([D066](01-decisions.md#d066)).

```
doot_spec <path-to-doot> [filter]
```

**The binary under test is an argument, not a compiled-in path or an environment variable** ([D076](01-decisions.md#d076)). It has to be, because the sanitizer gate must run the ASan `doot_spec` against the ASan `doot`: which binary is under test is a property of the invocation, not of the build. The optional filter selects by path substring, matching `doot_test`'s.

The subprocess runs through ISO C `system()` with output redirected to a file, not `popen`, which is POSIX-only and `_popen` under MSVC ([D066](01-decisions.md#d066)). Process startup dominates the runner's cost either way.

Per-file scratch space lives at `build/<profile>/spec-tmp/<mangled-path>/`, one directory per test so that nothing collides and the runner can be parallelized later without redesign. The tree is cleared at the **start** of a run rather than the end, so a failure leaves its artifacts behind to inspect.

There is **no per-test timeout** ([D077](01-decisions.md#d077)). ISO C `system()` offers no portable way to impose one, and the mitigation is better than a harness timer would be: the modes that have files cannot loop — the lexer always advances and the parser is depth-bounded, both asserted by `fuzz_lex` and `fuzz_parse` — and when `run` mode gets files, a program that hangs is a per-request budget failure ([D005](01-decisions.md#d005)) and therefore a runtime bug to fix rather than something the test harness should paper over.

Output matches the unit harness: one line per directory, detail on failure only. A failure prints the expectations that were not met and the diagnostics that were not expected, as two lists.

The runner is built and run under the sanitizers like every other suite, and it is **not** part of the `tidy` gate. That is deliberate rather than an oversight: `bugprone-command-processor` and `cert-env33-c` correctly flag the `system()` call [D066](01-decisions.md#d066) requires, and disabling them would stop flagging `system()` in `src/`, where it genuinely must never appear. `-Werror` with the project's full warning set, ASan, UBSan, LSan, and the runner's own negative fixtures cover it instead.

---

## Layout

Directories are the reporting unit, so they are organized by what is being specified:

```
tests/spec/
  lex/      strings, numbers, comments, line structure, reserved words
  parse/    declarations, statements, expressions, precedence, recovery
  markup/   markup literals, control blocks, attributes
  fmt/      canonical output and idempotence
  rules/    the well-formedness rules of 03-grammar.md
  docs/     the complete programs in docs/ and README.md, verbatim
```

`sema/`, `types/`, `routes/`, `db/`, and `run/` join as their milestones land. File names are descriptive `snake_case`.

`rules/` is named by convention, because [D049](01-decisions.md#d049) requires **an accepting and a rejecting test for every well-formedness rule** and that obligation should be mechanical rather than aspirational ([D078](01-decisions.md#d078)):

```
tests/spec/rules/rule_01_toplevel_let_ok.do
tests/spec/rules/rule_01_toplevel_let_err.do
```

`tools/check-docs.sh` reads the rule-to-code table in [03-grammar.md](03-grammar.md#well-formedness-rules) and requires both files for every rule whose codes are registered in `src/base/diag_codes.h`. Coverage therefore grows as codes land, and the gate passes today over the five rules the parser discharges without demanding tests for the eleven the semantic pass will own.

---

## The gates

Three mechanical checks, all in `tools/check-docs.sh`.

**A registered code is produced by a spec test.** This tightens the existing rule, which accepts a code named anywhere under `tests/`. The line is drawn by what a code describes ([D079](01-decisions.md#d079)): a code about **source text** is proved by a spec test, and a code about the **driver's environment** is proved by a unit test, because no `.do` file can elicit it. Exactly four are in the second class, and the allowlist is explicit and reasoned in the gate rather than open-ended:

| Code | Why no `.do` file can produce it |
| --- | --- |
| `DT0002` | source file is too large — a 64 MB fixture is not committable |
| `DT1001` | cannot read the file — a spec file is readable by definition |
| `DT1002` | cannot write the file — needs an induced filesystem failure |
| `DT1003` | cannot read the directory — likewise |

Everything else is reachable from source bytes, invalid UTF-8 (`DT0001`) and an embedded NUL (`DT0003`) included, since a spec file is bytes and its leading directive block stays readable regardless of what follows it.

**Every well-formedness rule with a registered code has both tests.** Described above.

**A documented program matches its spec test byte for byte** ([D080](01-decisions.md#d080)). A fenced block that is a complete program carries the spec file it is pinned to in its info string, as `` ```do spec=tests/spec/docs/chat.do ``.

The gate compares the block against that file **with its leading directive block removed**, since the spec file carries directives the documentation does not. Fragments — `s.len`, a bare signature, a single `let` — are unmarked and unchecked; they cannot parse standalone, and inventing surrounding context to make them checkable would test something the documentation does not actually say. GitHub highlights on the first word of an info string and ignores the rest, so the marker costs nothing in rendering.

This closes a hole that was real rather than theoretical. [10-frontend.md](10-frontend.md#idempotence) calls formatting the documentation's own chat application "the strongest single case" and says the assertion is what decides which side is wrong when the printer and the documentation disagree. It could not: the fixture was a hand-corrected copy, it silently omitted two of the program's declarations, and both `db` call sites in the documentation were consequently non-canonical for as long as the test existed.

### The chat application in v0.1

The documented chat program contains a `stream` handler, which is `DT0046` until [v0.2](07-roadmap.md#v02--realtime), and `doot fmt` refuses a file with errors. So in v0.1 the program is pinned two ways and not a third: **byte-equality** against `tests/spec/docs/chat.do` by the gate above, and its **exact diagnostic set** by that file's `expect-error` directives. Its *formatting* stays pinned by the unit test in `tests/unit/test_print.c` over the subset that v0.1 accepts.

When `stream` lands, the whole program becomes valid, the spec file gains `expect-fmt-stable`, and the unit test is deleted as redundant. Recording that here is the point: it is the difference between a known, dated gap and a forgotten one.

---

## What has files today

`fmt` mode only, per the implementation order in [D067](01-decisions.md#d067). Every lexical and syntactic diagnostic the front end emits is reachable through `doot fmt`, which is what makes the suite possible before the typechecker exists — all forty-six codes that describe source text are produced here, and the gate above enforces it.

`check` files arrive with the resolver and the typechecker, `run` with the VM, and `routes` with the route table.

Three directives are implemented and deliberately unexercised, because nothing can reach them yet, and each is named here so the gap is a known one rather than a discovered one:

| Directive | Waiting on |
| --- | --- |
| `expect-warning` | the first warning code — all fifty registered codes are errors |
| `expect-suggestion` | the first machine-applicable fix. `diag_fix` and the JSON `suggestion` field both work and are unit-tested, but no lexer or parser diagnostic calls it; the naming diagnostics in [D069](01-decisions.md#d069) are the first that will |
| `expect-fault` | the VM, since a fault is a runtime event |

That they parse and are rejected when malformed is covered; that they *match* is not, and cannot be until there is something to match. The runner's fail-closed behaviour is what keeps the gap safe in the meantime: a file using one of them today produces a failure, not a pass.
