# Engineering

How the implementation is built, tested, and kept honest. Decisions are recorded as [D045](01-decisions.md#d045)–[D054](01-decisions.md#d054).

The governing constraint: **doot must still build and work in ten years**, from a checkout, with a C compiler and nothing else ([D035](01-decisions.md#d035)). Every choice here follows from that, and most of them cost a little now to avoid costing a lot later.

---

## Build

**GNU Make for development, a single amalgamated `.c` for distribution.** No CMake, no autotools, no Meson, no build-script language ([D045](01-decisions.md#d045)).

```sh
make                  # debug build                    -> build/debug/doot
make release          # optimized                      -> build/release/doot
make test             # unit tests   (FILTER=name to narrow)
make test-asan        # unit tests under ASan + UBSan + LSan
make unity            # amalgamate, then build it with $(CC) alone
make fuzz             # build libFuzzer targets
make fuzz-smoke       # short fuzz run + every committed regression
make fmt / fmt-check  # clang-format
make tidy             # clang-tidy
make tools-check      # verify the pinned clang-format/clang-tidy versions
make docs             # cross-references, diagnostic codes, no TODO markers
make check            # everything CI runs, ordered to fail fastest
```

The **unity build** is the load-bearing one. `tools/amalgamate.sh` emits a single `build/doot.c` that `#include`s every translation unit in order, so the entire compiler and runtime build with one literal command:

```sh
cc -O2 -o doot build/doot.c
```

CI enforces that this works on every commit. It is what makes [D035](01-decisions.md#d035) a tested property rather than an aspiration, it is how doot will be packaged, and it is the fallback if `make` is ever unavailable. It also gives whole-program optimization without needing LTO.

### The C subset

`-std=c99 -pedantic`, with a deliberately narrower subset than C99 permits ([D046](01-decisions.md#d046)):

- **No VLAs, no `alloca`.** Stack depth must be statically bounded; MSVC does not support VLAs anyway.
- **No compiler extensions**, except computed goto, which is behind `DOOT_HAVE_COMPUTED_GOTO` with a `switch` fallback ([D001](01-decisions.md#d001)).
- **No C11.** No `_Generic`, no anonymous unions, no `stdatomic.h`. Atomics arrive in v0.3 for the topic bus, behind a shim in `src/base/plat.h`.
- **Fixed-width types everywhere** — `int32_t`, `uint64_t`, `size_t`. Bare `int` only for C library interop and small local counters.
- **No global mutable state**, mirroring [D008](01-decisions.md#d008) in the implementation. Every subsystem takes its context explicitly, which is what makes per-worker isolation ([D007](01-decisions.md#d007)) possible without an audit.

Warnings are errors, and the set is aggressive on purpose:

```
-Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wstrict-prototypes
-Wmissing-prototypes -Wold-style-definition -Wvla -Wcast-qual -Wwrite-strings
-Wpointer-arith -Wredundant-decls -Wswitch-enum -Wundef -Wdouble-promotion
-Wformat=2 -Wstrict-overflow=2
```

`-Wconversion` is the expensive one. It requires an explicit cast at every narrowing, which is verbose — and it is exactly the check that catches silent integer truncation, the hazard [D002](01-decisions.md#d002) exists to avoid. Enabling it now costs a little verbosity; enabling it after 40,000 lines exist would cost a week and get switched off.

`-Wswitch-enum` requires every enum case to be listed explicitly, so adding an opcode or a token kind produces a compile error at every site that must handle it. That is the mechanism that keeps a growing VM consistent.

Two compilers, always: **gcc and clang both build with `-Werror`.** They disagree about enough to be worth it.

---

## Memory discipline in the implementation

**The implementation dogfoods arenas** ([D047](01-decisions.md#d047)). `src/base/arena.c` is the allocator described in [05-runtime.md](05-runtime.md#tier-1--the-task-arena), and it is used by the compiler as well as the runtime.

**There is no `free()` in the compiler.** One arena per compilation, released whole at the end. This eliminates use-after-free and leaks as categories rather than as bugs to be found, makes compilation faster, and means AddressSanitizer and LeakSanitizer report on the runtime's behavior rather than on allocator bookkeeping.

Allocation failure policy is explicit per arena:

| Arena | On limit or OOM |
| --- | --- |
| compiler | fatal — abort with a diagnostic; a compiler that cannot allocate cannot proceed |
| request | returns `NULL`, which the VM converts to a `budget_exceeded` fault ([D005](01-decisions.md#d005)) |

The distinction is set with `arena_set_fatal_on_oom` at construction, so no call site has to remember which kind it holds.

## Assertions are always on

`DOOT_ASSERT` is **never compiled out**, and the build never defines `NDEBUG` ([D048](01-decisions.md#d048)).

An assertion failure is an invariant violation in doot itself, not a user error and not a language-level fault ([D012](01-decisions.md#d012)). Continuing past one risks corrupting a database or serving wrong data, and for a language whose users cannot debug it, silent corruption is far worse than a loud crash with a stack trace and a bug report. The cost is a branch on paths that are not the interpreter's hot loop.

Three forms: `DOOT_ASSERT(cond)`, `DOOT_ASSERTF(cond, fmt, ...)`, and `DOOT_UNREACHABLE()`. Failure prints file, line, function, and message to stderr and calls `abort()`.

---

## Testing

Five layers, each catching what the others cannot ([D049](01-decisions.md#d049)). The ratio that matters: **spec tests are the bulk of the suite**, because a language is defined by what it accepts and rejects, not by the shape of its internal functions.

### 1. Unit tests — `tests/unit/`

A hand-written harness, no dependency, ~150 lines. Suites register explicitly in a table rather than through constructor attributes, which keeps it portable to MSVC in v0.5.

```c
static void test_arena_alignment(unit *t) {
  arena *a = arena_new(1024);
  UNIT_TRUE(t, ((uintptr_t)arena_alloc(a, 1, 8) % 8) == 0);
  arena_destroy(a);
}
```

`make test` runs all of them; `./build/debug/doot_test arena` filters by suite or test name. Output is one line per suite plus detail on failure only.

Scope: the base layer (arena, buffers, slices, source mapping, diagnostics), and later the data structures the VM depends on. Not compiler behavior — that is layer 2.

### 2. Spec tests — `tests/spec/`

**The executable form of the specification, and the primary suite.** A `.do` file, its expectations declared in comments, and a runner that drives the real `doot` binary.

```do
// doot-spec: check
// expect-error: DT0142 at 3:36 "column `emial` does not exist on table `users`"
// expect-suggestion: 3:36 -> "email"

route GET "/" () -> html! {
  let u = db.one[User]("select id, emial from users", 1)!
  return <p>${u.name}</p>
}
```

Directives, all of which the runner verifies exactly rather than by substring:

| Directive | Asserts |
| --- | --- |
| `doot-spec: <mode>` | how to run it — `check`, `run`, `fmt`, `routes` |
| `expect-ok` | compiles with no diagnostics |
| `expect-error: <CODE> at <line>:<col> "<message>"` | that exact code, span, and message |
| `expect-warning: …` | as above |
| `expect-suggestion: <line>:<col> -> "<text>"` | a machine-applicable fix ([D038](01-decisions.md#d038)) |
| `expect-output:` | stdout of `doot run`, as a following comment block |
| `expect-fault: <CODE>` | a runtime fault of that kind |
| `expect-fmt-stable` | `doot fmt` is idempotent on this file |

Two rules make this suite load-bearing rather than decorative:

1. **A diagnostic code does not exist until a spec test produces it.** A code in the registry with no test is a CI failure. This is what keeps [D038](01-decisions.md#d038)'s promise of exact spans and applicable suggestions true as the compiler grows.
2. **Every well-formedness rule in [03-grammar.md](03-grammar.md#well-formedness-rules) has at least one accepting and one rejecting test.** The grammar document and the suite are checked against each other.

The runner compares full structured output — it consumes `doot check --json`, so a changed span or a reworded message is a visible diff rather than a silently passing substring match.

### 3. Wire tests — `tests/wire/`

HTTP is a byte protocol, so it is tested at the byte level: the runner opens a raw socket, writes exact request bytes, and asserts on exact response bytes. No HTTP client library, which means no library's opinions between the test and the protocol.

This is the only way to test what actually breaks in servers: keep-alive framing, chunked encoding, pipelining, `Expect: 100-continue`, header folding, oversized requests, slow-loris timeouts, and truncated bodies. Malformed input is a first-class case, not an afterthought.

Arrives with the server in v0.1.

### 4. Sanitizers

`make asan` builds with AddressSanitizer, UndefinedBehaviorSanitizer, and LeakSanitizer. **Every unit, spec, and wire test runs under it in CI on every commit** — sanitizers are a gate, not a periodic audit.

ThreadSanitizer joins in v0.3 with multi-worker. It should have very little to find, since [D008](01-decisions.md#d008) removes shared mutable state, which is precisely the claim worth verifying mechanically rather than trusting.

### 5. Fuzzing — `fuzz/`

libFuzzer targets on every component that consumes untrusted bytes. This is not optional for software that parses network input and is meant to be stable for years, and it must exist from the start, because retrofitting it means retrofitting parse-from-buffer entry points that were never designed to be called that way.

| Target | Consumes | From |
| --- | --- | --- |
| `fuzz_source` | arbitrary bytes as source: UTF-8 validation, line indexing, span rendering | now |
| `fuzz_lex` | token stream | lexer |
| `fuzz_parse` | full source → AST, must not crash on any input | parser |
| `fuzz_http` | request bytes | server |
| `fuzz_json` | JSON documents | `json` |
| `fuzz_form` | urlencoded and multipart bodies | `form` |

**Hand-written seeds** are committed to `fuzz/corpus/<target>/`, so the committed corpus documents what a target is meant to see rather than accumulating thousands of generated blobs. The working corpus libFuzzer grows lives in `build/corpus/` and is cached by CI between runs, so coverage compounds without any of it entering git.

**Every crash-triggering input is committed to `fuzz/regressions/<target>/` permanently** and runs as an ordinary test from then on, never deleted even after the code it exercised is rewritten. CI runs a 60-second smoke per target on each commit and a 30-minute run nightly. Details in [fuzz/README.md](../fuzz/README.md).

The invariant every target asserts: **arbitrary input produces a diagnostic, never a crash, a hang, or unbounded memory growth.**

---

## CI gates

Every one of these must pass before merge ([D051](01-decisions.md#d051)):

| Gate | Catches |
| --- | --- |
| `build-gcc`, `build-clang` | `-Werror` under both compilers |
| `unity` | the `cc build/doot.c` property, i.e. [D035](01-decisions.md#d035) |
| `test` | unit, spec, and wire suites |
| `sanitize` | ASan + UBSan + LSan across all suites |
| `fmt-check` | `clang-format` clean |
| `tidy` | curated `clang-tidy` checks |
| `fuzz-smoke` | 60 s per target, plus all committed regressions |
| `docs` | no broken cross-references; every diagnostic code documented and tested; no vendored tree drifted |

Linux only through v0.4, then macOS and Windows runners join at v0.5 ([07-roadmap.md](07-roadmap.md#v05--everywhere)).

### The tools are pinned

`clang-format` and `clang-tidy` versions live in the Makefile; `make tools-check` verifies them and CI installs exactly those versions rather than using whatever the runner image ships ([D055](01-decisions.md#d055)).

```sh
pip install clang-format==22.1.8 clang-tidy==22.1.8
```

This exists because of a real failure: the `tidy` gate passed locally and failed in CI on the first push, and the cause was the host's system headers rather than the code. **A gate whose result depends on the machine it runs on is worse than no gate**, because it teaches you to read red as noise — which is how a genuine failure eventually gets waved through. `make check` includes `tidy` for the same reason: a local run and a CI run must reach the same verdict.

Two `clang-tidy` checks are disabled, both in `.clang-tidy` with their reasoning written out beside them, never suppressed inline ([D053](01-decisions.md#d053)). The rule applied when deciding: **a check that correct code cannot satisfy is not a check.** Each disabled entry records the residual risk accepted and the condition under which it comes back.

---

## Style

**`clang-format` is canonical and has no local exceptions** ([D053](01-decisions.md#d053)) — the same reasoning as `doot fmt` for doot code ([D039](01-decisions.md#d039)). The config is committed; disagreements are resolved by editing it once, never by an inline `// clang-format off`.

Conventions:

| Kind | Form | Example |
| --- | --- | --- |
| Types | `snake_case` | `arena`, `diag_sink`, `token_kind` |
| Functions | `module_verb_noun` | `arena_alloc`, `diag_report`, `source_line_col` |
| Macros | `DOOT_UPPER` | `DOOT_ASSERT`, `DOOT_LIKELY` |
| Enum members | `MODULE_UPPER` | `DIAG_ERROR`, `TOK_IDENT` |
| Files | `snake_case.c` / `.h` | `arena.c`, `diag.h` |

Every `.c` has a matching `.h` declaring exactly its public surface; everything else is `static`. `-Wmissing-prototypes` enforces this mechanically, so a function that should be private but isn't produces a build error rather than a review comment.

## Layout

```
Makefile  .clang-format  .clang-tidy  .github/workflows/ci.yml
src/
  base/               plat, assert, arena, slice, buf, source, diag
  cli/                command dispatch
  (lex, parse, sema, vm, http, db … as they land)
tests/
  unit/               C unit tests + harness
  spec/               .do specification tests        (with the lexer)
  wire/               raw-socket HTTP tests          (with the server)
fuzz/
  fuzz_source.c  corpus/  regressions/
tools/
  amalgamate.sh  vendor.sh  check-docs.sh
vendor/
  MANIFEST            pinned versions and checksums
  sqlite/  mbedtls/   committed trees, installed by tools/vendor.sh
docs/
```

## Vendoring

Dependencies are **committed to the repository**, not fetched at build time ([D052](01-decisions.md#d052)). A build must work with no network, on a machine with no package manager, in ten years.

`tools/vendor.sh` downloads a pinned version, verifies its SHA-256 against `vendor/MANIFEST`, and writes the tree. `--verify` detects drift between an installed tree and its pin, which is the CI gate. Rules:

- **A pinned version and a recorded checksum**, always.
- **No local patches.** If one becomes unavoidable it lives in `vendor/patches/` as a standalone file applied by the script, with the reason and the upstream issue recorded — never an edit in place, because an edit in place is invisible at the next update.
- Vendored code compiles with **its own warning flags**, not ours. We do not own its style and will not fork it to satisfy `-Wconversion`.
- Vendored code is **excluded from `clang-format` and `clang-tidy`**, and **included in sanitizer runs**.

Currently pinned: **SQLite 3.53.0400** (the amalgamation — one `.c` and one `.h`, which is the form the compile-time SQL checker links against) and **mbedTLS 3.6.7**. mbedTLS is pinned to the **3.6 LTS branch deliberately, not 4.x**: 4.x moved to a PSA-only crypto API split across a second repository, while 3.6 is long-term support with a stable API in a single tree. That is the conservative choice for a project promising one minor release a year. Each tree is installed when the version that needs it is built.

## Definition of done

A subsystem is finished when all of the following hold. This list exists because "done" otherwise means "the happy path works," and the difference surfaces two releases later ([D054](01-decisions.md#d054)):

1. It builds clean under gcc and clang with `-Werror`.
2. It has unit tests for its own invariants and spec tests for its user-visible behavior.
3. Every diagnostic it emits has a code, a spec test, and an `explain` entry.
4. It has a fuzz target if it consumes untrusted input, with its corpus committed.
5. It is clean under ASan, UBSan, and LSan.
6. It allocates from an arena, and its ownership and lifetime rules are stated in its header.
7. The documentation it affects is updated in the same pull request.
8. **No stubs, no `TODO`, no unimplemented branches.** A CLI command exists only when it fully works; a partially implemented feature is not merged behind a flag. `TODO` in the source tree is a CI failure.
