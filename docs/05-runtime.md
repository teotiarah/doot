# Runtime and VM

Implementation specification for the compiler, the bytecode VM, the memory system, the scheduler, and the HTTP server. Decisions referenced here are argued in [01-decisions.md](01-decisions.md).

---

## Compiler pipeline

```
source (.do)
  → lexer                    hand-written, one token of lookahead
  → parser                   recursive descent, no generator dependency
  → resolver                 module paths, name binding, mutability rules
  → typechecker              full inference for locals; markup context typing
  → schema checker           replay migrations, prepare every SQL literal (D033)
  → route checker            pattern/parameter agreement, conflict detection (D024)
  → register allocator       linear scan over a per-function window
  → emitter                  bytecode + constant pool + frame maps + debug lines
```

No parser generator, no code generator, no build-time scripting language. The compiler is C, and a C99 compiler plus the two vendored dependencies builds the entire project ([D035](01-decisions.md#d035)).

The first three stages — lexer, parser, and the AST they produce — are specified in detail in [10-frontend.md](10-frontend.md), together with the front-end diagnostic allocation. The suite that tests them is [11-spec-tests.md](11-spec-tests.md).

Compilation is whole-program: there are no separate compilation units to link, because there are no external dependencies to resolve ([D029](01-decisions.md#d029)). This is what makes cross-module inference, exhaustive route checking, and SQL validation possible in one pass, and it is only affordable because project sizes are bounded by the target use case.

---

## Values

**Registers and locals are raw untagged 8-byte slots** ([D002](01-decisions.md#d002)). There is no NaN boxing and no tag word.

| Type | Slot representation |
| --- | --- |
| `int` | i64, full range |
| `float` | f64, full range |
| `bool` | 0 or 1 |
| `str` | pointer, or inline for ≤ 22 bytes |
| `bytes`, `[T]`, `{K: V}`, struct, `html` | pointer to heap object |
| `T?` | pointer with `nil` as null; scalars use a companion presence bit in the frame |
| `any` | pointer to a tagged box; the only tagged value in the language |
| enum | i32 ordinal |

Because the compiler knows each slot's type, **the interpreter performs no type checks and no unboxing.** Arithmetic opcodes are type-specialized (`ADD_I`, `ADD_F`, `CONCAT_S`), so dispatch goes straight to the operation.

### Strings

Immutable, UTF-8, length-prefixed, never NUL-terminated internally. Strings of 22 bytes or fewer are stored **inline in the slot plus a spill word in the frame**, so short strings — overwhelmingly the common case in web code: field names, slugs, statuses, short labels — never allocate.

String building uses a rope-free append buffer in the task arena. Concatenation in a loop is O(n) total, not O(n²), because the buffer grows geometrically inside the arena and the arena never needs to move it.

### Containers are packed

A `[int]` is a flat i64 array. A `{str: int}` has unboxed values. Struct fields are laid out C-style, ordered by the compiler for alignment, with no per-field tags.

This is why the absence of NaN boxing costs nothing in memory: bulk data carries no tags at all, and only the register file holds one type-descriptor table per function rather than one tag per value.

### Frame maps

Each function carries a compile-time table describing which of its slots hold pointers at each safepoint. The collector uses it to find roots **exactly** — no conservative stack scanning, no false retention, no pinning heuristics. Safepoints are allocation sites, calls, and backward branches.

---

## Bytecode

**32-bit fixed-width instructions.** 8-bit opcode plus Lua-style operand forms:

```
  ABC   op:8  a:8  b:8  c:8         register/register/register
  ABx   op:8  a:8  bx:16            register/16-bit unsigned (constant index, jump)
  AsBx  op:8  a:8  sbx:16           register/16-bit signed (relative jump)
  Ax    op:8  ax:24                 24-bit operand
```

Decoding is shifts and masks with no alignment concerns. Dispatch is computed goto via label addresses, with a `switch` fallback compiled for MSVC ([D001](01-decisions.md#d001)).

Approximately 180 opcodes in ten families:

| Family | Examples |
| --- | --- |
| Load / move | `LOAD_K`, `LOAD_NIL`, `MOVE`, `LOAD_TRUE` |
| Integer arithmetic | `ADD_I`, `SUB_I`, `MUL_I`, `DIV_I`, `MOD_I`, `NEG_I` |
| Float arithmetic | `ADD_F`, `SUB_F`, `MUL_F`, `DIV_F`, `NEG_F` |
| Comparison | `EQ_I`, `LT_I`, `LE_I`, `EQ_F`, `EQ_S`, `EQ_STRUCT` |
| String | `CONCAT_S`, `LEN_S`, `SLICE_S`, `INTERN_S` |
| Container | `NEW_LIST`, `GET_IDX_I`, `SET_IDX_I`, `NEW_MAP`, `MAP_GET`, `LEN` |
| Struct | `NEW_STRUCT`, `GET_FIELD`, `SET_FIELD`, `WITH_FIELDS` |
| Control | `JMP`, `JMP_IF`, `JMP_IF_NOT`, `CALL`, `RET`, `MATCH_JMP` |
| Error | `IS_ERR`, `PROPAGATE`, `COALESCE`, `FAULT` |
| Output | `WRITE_K`, `WRITE_ESCAPED`, `WRITE_HTML`, `WRITE_ATTR` |

Arithmetic opcodes include an overflow check that branches to `FAULT` ([D003](01-decisions.md#d003)); on modern hardware the branch is predicted taken-never and costs close to nothing.

Superinstructions for measured hot pairs, and a copy-and-patch JIT, are both **purely additive** later options requiring no change to this format ([D037](01-decisions.md#d037)).

### Markup compilation

Markup does not build intermediate strings ([D022](01-decisions.md#d022)). At compile time, all static text in a markup literal is concatenated and pre-escaped into a **single constant blob**. The literal then compiles to a sequence of output-buffer appends:

```
WRITE_K       blob_offset, length      // static run, already escaped
WRITE_ESCAPED reg                      // dynamic hole, escaped for its context
WRITE_HTML    reg                      // already-html value, spliced directly
```

The output buffer is the socket write buffer, allocated in the task arena. Rendering a page is therefore `memcpy` of static runs plus escaping of the dynamic holes, with no intermediate allocation and no template interpretation. A page with ten interpolations performs eleven appends.

---

## Memory

Three tiers ([D004](01-decisions.md#d004)). **There is no stop-the-world pause in doot** — not a short one, none.

### Tier 0 — no allocation

Scalars in registers; strings up to 22 bytes inline. A typical handler that reads a row, formats two fields, and renders a page allocates only the row, the page buffer, and nothing else.

### Tier 1 — the task arena

Every task owns a bump allocator over chunks drawn from a per-worker free list.

```
allocate:  p = arena->cur; arena->cur += size; return p     (plus a bounds check)
release:   arena->cur = arena->base                          (O(1), whole arena)
```

Chunks are 32 KB, geometrically escalating to 256 KB for tasks that need more, and return to the worker's free list on task completion rather than to the OS — so steady-state request handling performs **zero `malloc` calls**.

Allocation is a pointer increment. Deallocation is free. Locality is near-perfect, because everything a request touches was allocated consecutively.

### Tier 1½ — task-local compaction

A long-lived task — an SSE stream open for hours, a background worker — would grow its arena without bound. When a task's arena crosses a threshold, a **semispace copying collector runs over that one task's arena only.**

- Roots are that task's registers and frames, located exactly via frame maps.
- Nothing outside the task can point in, because there is no shared mutable state ([D008](01-decisions.md#d008)) and escaping values are copied out to tier 2, not referenced.
- A pause is bounded by **one task's live set**, typically kilobytes. Other tasks continue.
- Short requests never trigger it: they finish before reaching the threshold.

Structurally this is generational collection where the nursery is a task and the old generation is tier 2 — but with per-task rather than per-heap scope, which is what removes the global pause.

### Tier 2 — the frozen tier

A value that must outlive its task is **deep-copied into a compact immutable representation** at the escape point. Frozen values are:

- immutable and acyclic **by construction**, therefore
- managed by plain **non-atomic reference counting**, therefore
- requiring **no cycle collector, ever**.

Three consumers, one mechanism: `cache` entries, `topic` messages crossing worker boundaries, and long-lived task state.

Deep copy at the boundary is a real cost, paid deliberately. It buys non-atomic refcounts, lock-free workers, and the absence of a cycle collector — and the boundary is crossed rarely relative to how often ordinary request work happens.

### Budgets

Because all request allocation flows through one arena, the memory cap is a comparison against its high-water mark, and enforcing it is nearly free ([D005](01-decisions.md#d005)). Defaults: 16 MB and 15 s per request. Breaching either raises `budget_exceeded`, which terminates that request with a 500 and touches nothing else.

Upload bodies stream to disk rather than into the arena, so a large upload is not charged against the request's memory budget.

---

## Tasks and scheduling

A **task** is the unit of concurrency ([D044](01-decisions.md#d044)). Every request is a task; `spawn` creates one; every SSE stream is one.

Frames live on a **VM-managed heap stack**, not the C stack. Suspending a task is therefore saving one pointer: no assembly, no stack copying, no `setjmp`, no platform-specific context switching, no split-stack tricks ([D006](01-decisions.md#d006)).

This is possible **only** because no foreign code can be loaded into doot ([D029](01-decisions.md#d029)) — the VM owns 100% of the call stack, so there is never a native C frame in the way of a suspension. A language with an FFI cannot have this at this price, and this is the concrete payoff of the no-registry decision.

An idle task costs its frame stack plus its arena chunk — a few KB. Ten thousand open SSE connections is tens of megabytes, not the tens of gigabytes a thread-per-connection model would need.

Blocking work — SQLite, filesystem, DNS — is dispatched to a small per-worker thread pool (default 4 threads). The calling task suspends; the loop continues.

### Workers

*Multi-worker lands in v0.3; the semantics are identical at one worker and sixteen, by construction.*

N workers, one per core, each with its own heap, arena pools, scheduler, and event loop. All accept on the same listening socket via `SO_REUSEPORT`, so the kernel distributes connections and there is no accept-lock contention ([D007](01-decisions.md#d007)).

No locks on the hot path. Reference counts are non-atomic. Collection is per-worker with independent pauses.

Exactly two channels of shared state exist:

1. **SQLite** — everything durable: rows, sessions, cache-of-record, job queue.
2. **The topic bus** — realtime fan-out, because a message published on worker 3 must reach subscribers on workers 1, 2, and 4. Frozen-tier values move through a per-worker MPSC ring buffer with an `eventfd` wakeup. Publishing is a refcount increment and a ring write; no allocation, no lock held across a copy.

The **reason** program semantics do not change with worker count is [D008](01-decisions.md#d008): there are no mutable globals to be silently per-worker, and no aliasing for a race to exploit. "Works in dev, breaks in prod" is not mitigated here — it is unrepresentable.

---

## I/O and the HTTP server

One event loop per worker over a thin platform abstraction:

| Platform | Mechanism | Lands |
| --- | --- | --- |
| Linux | `epoll` (level-triggered) | v0.1 |
| macOS, BSD | `kqueue` | v0.5 |
| Windows | IOCP | v0.5 |

Linux only through v0.4; the abstraction boundary exists from v0.1 so the later backends are additions rather than surgery.

**HTTP/1.1 only** ([D010](01-decisions.md#d010)). The parser is hand-written and zero-copy: header names and values are slices into the read buffer, and only values that outlive the request get copied. Keep-alive, chunked transfer encoding, and pipelining are supported; `Expect: 100-continue` is honored.

**The runtime never listens on TLS** ([D011](01-decisions.md#d011)). It binds either a TCP port (development) or a **Unix domain socket whose path the supervisor assigns** (production). Outbound TLS exists for the `http` client; inbound TLS does not exist at all. See [08-boundaries.md](08-boundaries.md).

### Request lifecycle

1. Loop reports a readable connection.
2. A task is created; a 32 KB arena chunk comes off the free list.
3. Headers are parsed in place. Route matching uses a compile-time-generated trie over the known route table ([D024](01-decisions.md#d024)) — no runtime pattern list to scan.
4. Path parameters are converted; `form` / `query` / `json` are bound and validated ([D025](01-decisions.md#d025)). A validation failure returns 422 without entering the handler.
5. CSRF is verified for state-changing methods ([D028](01-decisions.md#d028)).
6. `@before` hooks run; an error short-circuits.
7. The handler runs, appending to the output buffer as it renders.
8. Headers are written, the body is flushed, `@after` hooks run.
9. The arena resets in O(1) and its chunks return to the free list.

Steps 2 and 9 are why per-request overhead is measured in microseconds rather than in allocator time.

### Faults

A fault — index out of range, integer overflow, division by zero, budget exceeded — terminates **the current task only** ([D012](01-decisions.md#d012)). The request returns 500, the diagnostic is logged with the full doot stack trace and source spans, and the process, the other tasks, and the worker are untouched.

That containment is sound rather than hopeful: the task's memory is its own arena, and it shares no mutable state with anything ([D008](01-decisions.md#d008)), so there is no partially-mutated structure for another task to observe.

---

## Hot reload

`doot dev` watches the source tree, recompiles on change, and swaps the bytecode image. In-flight requests finish against the old image; new requests get the new one. A compile error keeps the old image serving and displays the diagnostic as an overlay in the browser rather than replacing the page, so a typo does not lose the state of whatever you were looking at.

Because compilation is whole-program and the projects are small, a full rebuild is the reload strategy — there is no incremental-compilation cache to invalidate incorrectly.
