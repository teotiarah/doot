# Fuzzing

Every component that consumes untrusted bytes has a libFuzzer target ([D049](../docs/01-decisions.md#d049)). The invariant each one asserts is the same:

> **Arbitrary input produces a diagnostic, never a crash, a hang, or unbounded memory growth.**

```sh
make fuzz                          # build every target
make fuzz-smoke                    # 60 s each, plus all regressions
FUZZ_SECONDS=600 make fuzz-smoke   # longer run
build/fuzz/fuzz_source build/corpus/fuzz_source -jobs=8
```

## Layout

| Path | Contents | Committed? |
| --- | --- | --- |
| `fuzz/fuzz_*.c` | the targets | yes |
| `fuzz/corpus/<target>/` | **hand-written seeds** — small, meaningful, readable | yes |
| `fuzz/regressions/<target>/` | inputs that once caused a crash | yes, permanently |
| `build/corpus/<target>/` | the working corpus libFuzzer grows | no |

Seeds are hand-written and stay small, so the committed corpus documents *what a target is meant to see* rather than accumulating thousands of generated blobs. The working corpus is a build artifact; CI caches it between runs so coverage compounds without any of it entering git.

## When a target finds something

1. libFuzzer writes `crash-<sha1>` to the working directory.
2. Commit it to `fuzz/regressions/<target>/` with a name describing the bug, e.g. `line_index_off_by_one`.
3. Fix the bug.
4. The regression input runs on every `make fuzz-smoke` from then on, forever.

A regression input is never deleted, even after the code it exercised is rewritten.

## Targets

| Target | Input | Status |
| --- | --- | --- |
| `fuzz_source` | arbitrary bytes as source: UTF-8 validation, line indexing, span rendering | active |
| `fuzz_lex` | token stream | with the lexer |
| `fuzz_parse` | source to AST | with the parser |
| `fuzz_http` | request bytes | with the server |
| `fuzz_json` | JSON documents | with `json` |
| `fuzz_form` | urlencoded and multipart bodies | with `form` |
