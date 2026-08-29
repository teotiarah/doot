# Decision log

Every locked decision, with the argument that produced it and the alternatives that lost.

**These decisions do not get revisited.** The project's development model is stable-from-day-one: we reason a decision through once, write down why, and live with it. Reversing a decision is far more expensive than spending another day getting it right. A decision is reopened only on new information that invalidates the original argument — not on preference, not on fashion, and not because implementing it turned out to be tedious.

Status values: **locked** (settled, implement as written) · **locked, deferred** (settled, lands in a later version — see [07-roadmap.md](07-roadmap.md)).

---

## Runtime and VM

### D001
**Register-based bytecode VM, interpreted, with computed-goto dispatch.** · locked

32-bit fixed-width instructions, 8-bit opcode with Lua-style A/B/C and Bx/sBx operand forms. Computed goto (`&&label` / GCC-Clang label addresses) with a `switch` fallback for MSVC.

Register-based over stack-based because it produces roughly 30–40% fewer instructions for the same program, and each instruction does more work — which matters more in an interpreter than in a compiler, since dispatch is the dominant cost. Fixed-width 32-bit instructions keep decoding to shifts and masks with no alignment handling.

*Rejected:* a tree-walking interpreter (too slow for goal 4, and its only advantage — implementation speed — is small once a typechecker exists anyway); direct AOT compilation to machine code (forfeits portability, multiplies the maintenance surface by the number of target architectures, and is incompatible with the "one C compiler builds everything" property in [D030](#d030)).

*Consequence:* a JIT remains a purely additive future option ([D035](#d035)), because the bytecode is already the stable interface.

### D002
**No NaN boxing. Untagged 8-byte value slots, with per-function GC frame maps.** · locked

*This reverses an earlier project decision, deliberately.*

NaN boxing exists to fit a *dynamically typed* value into 8 bytes. doot is statically typed, so it does not need the trick, and the trick costs real things:

- It forces a choice between **48-bit integers** or **heap-allocated floats**. Truncated integers are a genuine correctness hazard here: Unix nanoseconds are already ~1.7 × 10¹⁸, past the 2⁶⁰ range, and SQLite rowids and Snowflake-style IDs are full 64-bit. Silent truncation on an ID field is exactly the class of bug that destroys trust in a v1.
- It puts unmask-and-check logic on every arithmetic operation in the interpreter loop.

Because every register's type is known at compile time:

- Registers and locals are **raw untagged 8-byte slots**. Full `int` (i64) and full `float` (f64), both free, neither boxed.
- The collector locates pointers via a **frame map** emitted per function at compile time — exact, table-driven, no scanning heuristics, no conservative false retention.
- Opcodes are **type-specialized** (`ADD_I`, `ADD_F`, `CONCAT_S`, …). The interpreter's hot loop performs **zero type checks and zero unboxing**. This is the real performance dividend of static typing in a bytecode VM, and it is larger than the cache benefit of tight value packing.
- Homogeneous containers are **packed**: `[int]` is a flat i64 array, `{str: int}` has unboxed values, struct fields are laid out C-style with no tags. The "wide values waste memory" objection therefore does not apply, because bulk data carries no tags at all.

Runtime tags exist only *inside* heap objects that genuinely need them: `any` values produced by dynamic JSON parsing, and enum variants.

*Rejected:* NaN boxing (above); 16-byte tagged values as in Lua 5.4 (correct and simple, but pays for tags that static types make unnecessary, and keeps type checks in the dispatch loop).

*Consequence:* the future JIT is much simpler, because monomorphic typed opcodes need no type feedback, no inline caches, and no deoptimization machinery.

### D003
**Checked arithmetic. Integer overflow is a fault, not a wraparound.** · locked

`int` is signed 64-bit. Overflow, division by zero, and modulo by zero raise a fault ([D012](#d012)) rather than producing a wrong number. With type-specialized opcodes the check is a single well-predicted branch on the CPU's overflow flag, which is close to free. `math.wrap_add` and friends provide explicit wrapping for the rare cases (hashes, checksums) that want it.

*Rejected:* silent wraparound (C/Go/Java semantics) — the performance saving is negligible and the failure mode is silent data corruption, which is the worst possible outcome for a language whose users are not going to audit arithmetic.

### D004
**Three-tier memory: task arena, task-local compaction, frozen shared tier. No global GC. No cycle collector.** · locked (tiers 1½ and 2 deferred to v0.2)

The observation that makes this work: **in a web workload, essentially all garbage is request-scoped.**

- **Tier 0 — free.** Ints, floats, bools, and nil live in registers. Strings up to 22 bytes use small-string optimization and never allocate. A large fraction of a typical handler allocates nothing at all.
- **Tier 1 — the task arena.** Every task (and every request is a task) owns a bump allocator built from pooled chunks. Allocation is a pointer increment. When the response is flushed, the entire arena resets in O(1) and its chunks return to the worker's free list. **Deallocation is free, there is no tracing, and locality is near-perfect.**
- **Tier 1½ — task-local compaction.** A long-lived task (an SSE stream open for an hour, a background worker) would grow its arena without bound. When a task's arena crosses a threshold, a semispace copying collector runs **over that one task's arena only.** Roots are that task's registers and frames, found exactly via [D002](#d002) frame maps. Nothing outside the task can point in ([D008](#d008)). This is generational collection where the nursery is a task and the old generation is tier 2 — except a pause is bounded by *one task's* live set, typically kilobytes. Short requests never trigger it.
- **Tier 2 — the frozen tier.** Values that outlive a request are **deep-copied into a compact immutable representation** at the moment they escape. Immutable and acyclic by construction, therefore managed by plain **non-atomic reference counting with no cycle collector, ever.** The same mechanism serves three purposes: cache entries, cross-worker message passing, and long-lived task state.

**There is no stop-the-world pause in doot.** Not a short one — none. That is the property that makes tail latency predictable on a small machine.

*Rejected:* tracing GC over a shared heap (global pauses, and on a 2 GB box the heap headroom a generational tracer wants is a large fraction of the machine); reference counting as the primary strategy (per-object refcount traffic on request-scoped garbage that an arena frees for zero, plus a cycle collector we would then have to build and debug); manual memory management (disqualified by goals 1 and 2).

### D005
**Per-request memory and CPU budgets, enabled by default.** · locked

Defaults: 16 MB of arena per request, 15 s wall clock. Exceeding either raises a `budget_exceeded` fault, which terminates that request with a 500 and a full diagnostic, and touches nothing else.

Because all request allocation flows through one arena ([D004](#d004)), the memory cap is a comparison against the arena's high-water mark — essentially free to enforce. On a single shared box this is the difference between one bad endpoint and total downtime, and it is the kind of safety property that is nearly impossible to retrofit, so it ships in v0.1.

Streams are exempt from the wall-clock budget by nature and instead carry an idle timeout.

### D006
**Green tasks with blocking-style code. No async/await, no OS thread per request.** · locked (user-facing `spawn` deferred to v0.2)

Frames live on a VM-managed heap stack, so suspending a task is a pointer save — no assembly, no stack copying, no platform-specific context switching. This is possible only because of [D029](#d029) (no FFI): the VM owns the entire call stack, so there is never a foreign C frame in the way. An idle SSE connection costs a few KB.

Blocking syscalls — SQLite, filesystem — are offloaded to a small per-worker thread pool so they never stall the event loop.

*Rejected:* **explicit async/await**, which is disqualified by goals 1 and 2 — function coloring doubles the effective API surface, forces every intermediate function to be async, and is one of the most reliable sources of AI-generated bugs. **Thread-per-request**, which is disqualified by goal 8 — 10,000 idle-but-open SSE connections at 8 MB of stack each does not fit on a 2 GB machine.

*Consequence:* an SSE handler is a plain `for` loop over a subscription. That is what "native, not bolted on" means concretely.

### D007
**Multi-worker by shared-nothing isolation: N workers, one per core, each with its own heap and scheduler, sharing the listening socket via `SO_REUSEPORT`.** · locked, deferred to v0.3

No locks on the hot path, non-atomic reference counts, and per-worker collection whose pauses are independent. Chosen over a shared heap with work-stealing because a shared heap forces atomic refcounts and cross-core synchronization throughout, which costs more than the scheduling flexibility is worth at this scale.

Shared state is exactly two things, and no more:

1. **SQLite**, for anything durable — rows, sessions, cache, job queue.
2. **The topic bus**, for realtime fan-out, because a message published on worker 3 must reach subscribers on workers 1, 2, and 4. It moves frozen-tier values ([D004](#d004)) through an internal ring buffer with an eventfd wakeup.

### D008
**No shared mutable state at any level. Top-level mutable bindings are illegal. Function parameters are immutable. `let` is deeply immutable.** · locked

This is the single most valuable constraint in the design, and it resolves several problems at once.

The problem it primarily solves: in a shared-nothing runtime ([D007](#d007)), a module-level mutable global silently becomes per-worker. A counter would give correct answers with one worker in development and wrong answers with sixteen in production. Unacceptable — so the language makes the construct impossible instead of documenting the hazard.

The rules:

- **Top-level bindings are `let` only.** State lives in SQLite, in request scope, or in an explicitly per-worker `cache` cell whose semantics are documented as per-worker.
- **`let` is deeply immutable** — the binding and everything reachable through it. `var` permits mutation.
- **Function parameters are immutable.** A function cannot mutate its caller's data. To modify, bind locally with `var` and return a new value.

Consequences, all good:

- Program semantics are **identical at 1 worker and at 16.** "Works in dev, breaks in prod" is impossible by construction.
- **No aliasing anywhere**, so a closure passed to `spawn` cannot race — data races are not prevented at runtime, they are unrepresentable.
- The frozen tier's promote-by-deep-copy ([D004](#d004)) is sound trivially.
- The compiler knows a `var` local is uniquely owned, so `xs.push(x)` mutates in place. Value semantics do not mean copying.

*Rejected:* mutable parameters with a `mut` annotation in signatures (Rust-style) — it works, but it puts aliasing back in the language and requires users to reason about it; the ergonomic gain does not pay for that at this scale.

### D009
**Structural equality and value semantics for all data.** · locked

`==` compares structs, lists, and maps by content. There is no reference identity operator and no object identity to observe. This follows from [D008](#d008): with no aliasing, identity is not a meaningful concept.

### D010
**HTTP/1.1 only.** · locked

HTTP/2 is not in the v1 scope. The known limitation is on the record: browsers cap roughly 6 connections per origin on HTTP/1.1. The normal case — one SSE stream per tab — is unaffected. A page wanting several concurrent streams would hit the cap, and the answer is to multiplex over one stream by topic, which the `topic` API makes natural.

Written by hand, zero-copy, no dependency. HTTP/2 is re-evaluated at v1.0, not before.

---

## The app/dashboard boundary

### D011
**The doot runtime never listens on TLS. It speaks plain HTTP/1.1 on a TCP port or a Unix domain socket. Certificates, ACME, ports, and TLS termination belong entirely to the dashboard.** · locked

The philosophy rules out nginx and Caddy in the deployment path, which initially looked like it forced TLS into the app runtime. It does not — it puts TLS in the *dashboard*, which is the component that already owns host-header routing and therefore already terminates HTTP.

Locking this now prevents a genuine future conflict. If both the runtime and the dashboard could terminate TLS, every deployment would raise the questions "which one holds the certificate," "which one owns port 443," and "what happens when both try to renew." Answering them once, in advance, in favor of the dashboard, is worth more than the standalone-HTTPS convenience it costs.

**In production, an app listens on a Unix domain socket whose path the dashboard assigns.** This also disposes of port allocation: there are no ports to allocate, no conflicts between co-hosted apps, and no way to accidentally expose an app process to the public internet.

*Consequence:* outbound TLS still exists in the runtime, for the `http` client ([D031](#d031)). Client-side TLS in, server-side TLS never. Full detail in [08-boundaries.md](08-boundaries.md).

---

## Errors

### D012
**Two error classes: recoverable errors are values; bugs are faults that terminate the task.** · locked

**Recoverable errors** are things a correct program must handle: a missing row, a failed insert, a rejected upload, a network timeout. They are values, declared with `!` on the return type, and the compiler forces handling. There are no exceptions, no `throw`, no `try`/`catch`, and no unwinding.

**Faults** are bugs and resource-limit breaches: index out of range, integer overflow, division by zero, budget exceeded ([D005](#d005)). A fault terminates **the current task only** — the request returns 500 with a full diagnostic in the log — and cannot affect the process or any other task. That containment is safe precisely because tasks are isolated with their own arenas ([D004](#d004)) and share no mutable state ([D008](#d008)).

Faults are raised by the runtime and never by user code; there is no `panic` keyword.

*Rejected:* making every fallible operation return a value, including indexing. `xs[i]` returning an optional that must be unwrapped is technically superior and ergonomically miserable; it would put noise on every line of ordinary code. The safe form exists as `xs.get(i) -> T?` for when the index is genuinely untrusted.

### D013
**`!` declares fallible, `!` propagates, `else` handles. Same symbol at declaration and call site.** · locked

```do
fn find(id: int) -> User?          // may be absent
fn create(name: str) -> User!      // may fail

let u = create(name)!                              // propagate; caller must be ! too
let u = create(name) else return page.error(500)   // handle and bail
let u = find(id) else User.guest()                 // supply a default
let n = find(id) else { log.warn("miss"); return http.not_found() }
```

The symmetry is the point: **the declaration and the call site use the same mark**, so a reviewer scanning agent-written code can verify error handling visually without consulting signatures. `else` for coalescing reads as English — "find the user, else return not found" — and reuses an existing keyword, so there is no `orelse` / `??` / `unwrap_or` / `expect` family to learn.

*Rejected:* Go's `if err != nil` (three lines of noise per call, and the compiler cannot force handling); exceptions (invisible control flow, and unwinding interacts badly with arena lifetimes); Zig's prefix `try` (breaks left-to-right reading and loses the declaration/call-site symmetry).

### D014
**One universal `Error` type with a `kind` tag. No generic `Result<T, E>`.** · locked

`Error` carries a `kind` (enum tag), a `message`, a `cause` chain, and a **source location captured automatically**. Handling inspects the kind:

```do
match err.kind {
  .not_found -> return http.not_found()
  .conflict  -> return http.conflict()
  else       -> return http.error(500)
}
```

A user-parameterized error type would drag in generics and variance ([D019](#d019)) for very little benefit at this scale, and would fragment the ecosystem's error handling across incompatible error types — which is precisely the problem a closed stdlib exists to avoid.

*Rejected:* checked-exception-style declared error sets in the signature (`-> User ! NotFound | Conflict`). It sounds appealing for goal 9, but it makes every signature churn whenever an implementation detail changes, and the churn propagates up every call chain.

---

## Type system

### D015
**Statically typed, with inference for locals.** · locked

Declarations are explicit; local bindings infer. Types are written `name: type`, the form most represented in the training data of the models expected to write doot (TypeScript, Python, Rust, Kotlin, Swift).

### D016
**Structs and free functions. No classes, no inheritance, no interfaces.** · locked

```do
type User { id: int, name: str }
fn User.display(self) -> str { ... }
```

Methods attach to a type by name. No subtyping means no variance rules, no vtables, no diamond problems, and no "where is this method actually defined" question — which is a readability property (goal 2) as much as a simplicity one.

*Rejected:* interfaces/traits. They are the right answer for a language with third-party libraries that must interoperate through abstractions. doot has no third-party libraries ([D029](#d029)), so the polymorphism they buy has no consumer.

### D017
**Optional types `T?`, no implicit nil, no optional chaining.** · locked

Only `T?` can hold nil. Unwrapping is via `else` ([D013](#d013)). `?.` chaining is deliberately absent: it encourages long chains that silently produce nil from an unknown link, which is precisely the kind of invisible failure goal 9 is against. Its absence produces a clear compile error with an obvious fix, which is the better outcome.

### D018
**Enums are tag-only in v0.1. No payload-carrying variants.** · locked

`type Status enum { active, banned, pending }`. Sum types with payloads are genuinely useful but require exhaustiveness checking with binding patterns and interact with generics; the main use case in a web app is error classification, which [D014](#d014) already covers. Revisit at v1.0 if real code demands it.

### D019
**No user-defined generics. Built-in generic containers and a small set of stdlib generic slots only.** · locked

`[T]`, `{K: V}`, and stdlib entry points like `db.all[Msg](...)` and `topic.subscribe[Msg](...)` are generic. User code cannot declare a type or function parameter.

Generics are the single largest source of type-system complexity, of incomprehensible error messages, and of long compile times. They earn their cost in a language building reusable libraries for unknown consumers. doot's users build applications with concrete types, and its stdlib is written in C. Skipping them buys a great deal of goal 1 and goal 9.

*Consequence:* the syntax uses `[T]` rather than `<T>` for type application, which removes the `<` ambiguity that would otherwise complicate markup literals ([D022](#d022)).

### D020
**Money is `int` in minor units. There is no decimal type.** · locked

Binary floating point is wrong for money, and a correct decimal type is a large surface (precision, rounding modes, string round-tripping, SQL mapping) for a problem that integer cents solves completely. Documented as convention, with formatting helpers in `str`.

---

## The web model

### D021
**HTML is a distinct type, not a string. Server-rendered HTML is the output primitive.** · locked

`html` is its own type. Every `${...}` interpolated into markup is **escaped automatically**, so **XSS is impossible unless the author explicitly writes `html.raw(s)`.** This soundness is the reason `html` cannot be `str`: if they were the same type, escaping could not be decided by context.

There is no SSR/SSG/CSR distinction to configure, because there is only one rendering model.

### D022
**Native markup literals in the grammar, with `{if}` / `{for}` / `{end}` control flow inside them.** · locked

```do
<ul>
  {for m in msgs}
    <li>${m.body}</li>
  {end}
</ul>
```

Markup is an expression form in the language itself. This gives compile-time checked, auto-escaped, composable templates with no template engine, no second language, and no build step. Control flow inside markup uses **the same keywords as statements**, so nothing new is learned.

Ambiguity with the less-than operator is resolved by requiring `<` to be immediately followed by a tag name with no intervening space, in expression position. Since type application uses `[T]` ([D019](#d019)), there is no second source of `<` ambiguity.

*Rejected:* a separate template file format (a second language to learn, violating goal 1); heredoc strings typed as html (loses structural checking and nesting validation); JSX-style expression-only control flow via `map` and lambdas (correct and composable, but noticeably harder for a non-programmer to read, and goal 2 covers humans too).

### D023
**No component tags. Composition is function calls.** · locked

A layout is a function: `layout("Home", <div>…</div>)`. Since a function returning `html` composes by ordinary call syntax, `<Card user=${u}/>` would add a tag-name resolution rule, a props-versus-attributes distinction, and a children/slot mechanism, to express something already expressible. Fewer rules wins.

### D024
**`route` is a top-level declaration, not a runtime registration.** · locked

Because routes are declarations, **the compiler knows the entire route table.** It detects conflicting and shadowed patterns, verifies that every `:param` in the pattern has a correspondingly typed function parameter, and emits a route map for tooling (`doot routes`). Runtime registration (`app.get("/x", handler)`) forfeits all of that, and makes the set of routes something you must run the program to discover.

### D025
**Typed request binding by reserved parameter name.** · locked

Path parameters bind by name. Exactly three parameter names are reserved and bind automatically:

- **`form`** — the request body (urlencoded or multipart), parsed into the declared struct
- **`query`** — the query string, parsed into the declared struct
- **`json`** — a JSON body, parsed into the declared struct

Binding runs validation from the struct's `@` attributes and short-circuits to 422 before the handler body executes. No annotations are needed, because the URL pattern is in the declaration itself, so the source of every parameter is unambiguous to a reader and to an agent.

### D026
**SSE is a declaration form (`stream`) with a `send` statement, carrying HTML fragments.** · locked, deferred to v0.2

```do
stream GET "/rooms/:room/live" (room: str) {
  for m in topic.subscribe[Msg]("room:" + room) {
    send <li>${m.body}</li>
  }
}
```

Sending **HTML fragments rather than JSON** keeps the server as the only component that knows how to render. The client swaps in what arrives. That is what makes goal 8 coherent with goal 3 instead of in tension with it.

Backpressure resolves elegantly: each subscriber has a bounded buffer (default 64), and on overflow the subscription **closes with a `lagged` error**, ending the loop. The browser's built-in SSE reconnection then re-establishes it and the handler re-reads current state. SSE's own reconnect semantics are the backpressure mechanism.

### D027
**A minimal client runtime, `doot.js`, built into the binary and auto-injected.** · locked, deferred to v0.2

Roughly 4 KB, doing exactly three things: progressive form submission, fragment swapping, and SSE binding via a `data-live` attribute.

Without it, HTML-over-the-wire leaks: every interactive app would hand-write JavaScript, and the no-npm story would be false in practice. With it, the interaction model is complete and requires zero configuration. It is versioned with the runtime, has no build step, and is not extensible — it is a runtime component that happens to execute in the browser, not a JavaScript framework.

### D028
**Automatic CSRF protection for form posts.** · locked

Enabled in v0.1 rather than added later, because a security default that changes behavior is painful to introduce after code exists. Signed tokens via HMAC, injected into `<form>` elements automatically by the markup compiler and verified before handler dispatch.

---

## Modules and packaging

### D029
**No package system, no registry, no FFI. Sharing is by copying source files.** · locked

The finite-surface argument is in [00-vision.md](00-vision.md#the-objection-worth-answering). The structural consequences are what make this more than an ideological position: no FFI is what makes [D006](#d006) cheap, and no registry is what makes [D030](#d030) possible.

A copied `.do` file is ordinary project code: compiled from source, typechecked with everything else, and readable in full by whoever depends on it.

### D030
**No import statements. Stdlib modules are pre-bound global namespaces. User modules are addressed by fully qualified path.** · locked

`models/user.do` exposes `models.user.find(...)`. Within a file, local names are unqualified; everything else is fully qualified, **including files in the same directory.** `pub` marks what is exported.

Slightly more verbose at call sites, and worth it: there is **no import resolution to get wrong**, no ambiguity about which `user` a name refers to, and no aliasing to trace. For agent-written code, unambiguous beats short. Creating a top-level directory whose name collides with a stdlib module is a compile error.

### D031
**The `http` client must be excellent, and it is the only third-party integration mechanism.** · locked, deferred to v0.3

Connection pooling, timeouts, retries with backoff, streaming bodies, outbound TLS, typed JSON in and out. If this module is mediocre, [D029](#d029) fails, because it is the sole path to Stripe, S3, and every other service. Treated as a headline feature with its own quality bar rather than a utility.

---

## Data

### D032
**SQLite, embedded, single file, WAL. No other database, ever.** · locked

Goal 3, directly. On a single box in the 0–50k user range, SQLite in WAL mode is not a compromise: it removes a network hop, a connection pool, a second process to supervise, and an entire category of operational failure.

### D033
**SQL is validated against the real schema at compile time, and result shapes are typechecked against structs.** · locked

```do
let u = db.one[User]("select id, name, email from users where id = ?", id)!
```

A compile error if the table does not exist, a column is misspelled, the placeholder count is wrong, or `User`'s fields do not match the result shape. The compiler builds the schema by replaying migrations into an in-memory SQLite instance, prepares each statement, and reads its column metadata.

This is the decision that makes "no framework" credible, because eliminating the ORM is the hardest part of that claim. SQL stays SQL — no query builder to learn, no codegen step, no drift between schema and code — and it is checked.

### D034
**Migrations are forward-only numbered `.sql` files.** · locked

`migrations/001_init.sql`, applied by `doot migrate`, tracked in a `_doot_migrations` table. **No down migrations:** the rollback that gets exercised in practice is a new forward migration, and maintaining reverse scripts that are never run produces false confidence.

---

## Dependencies

### D035
**Exactly two dependencies at v0.1, both vendored: SQLite and mbedTLS. `cc *.c` builds doot.** · locked

| Dependency | Purpose | Lands |
| --- | --- | --- |
| SQLite (amalgamation) | database; also the compile-time SQL checker ([D033](#d033)) | v0.1 |
| mbedTLS | all crypto primitives; later, outbound TLS | v0.1 |
| libdeflate | gzip | v0.3 |
| argon2 (reference) | password hashing | v0.4 |

**mbedTLS over OpenSSL.** OpenSSL is the largest maintenance liability available: an enormous API surface, frequent CVEs, painful version skew across distributions, and a build system that will fight you. mbedTLS is small, Apache-2.0, TLS 1.3-capable, has a sane API, and — decisively — also provides SHA-256, HMAC, AES, and a CSPRNG, which eliminates a separate crypto dependency. Its compile-time config header lets us enable only what we use. An OpenSSL build flag stays available for distribution packagers who require system TLS, but is not the default.

Corollary rejections: no CMake requirement, no Rust in the bootstrap chain, no code generator, no parser generator. A ten-year maintenance story requires that a plain C99 compiler and nothing else can build the project.

### D036
**No regular expression engine.** · locked

Backtracking regex is a ReDoS vector reachable from untrusted input, and a large surface for something the `validate` module plus `str` matching covers for real cases. If a compelling need appears, the answer is a linear-time non-backtracking (Thompson NFA) engine with no backreferences — never a backtracker.

### D037
**A JIT is not in the v1 scope.** · locked

The typed monomorphic bytecode ([D002](#d002)) makes a copy-and-patch or template JIT straightforward to add later, with no changes to the language, the bytecode, or the memory model. It stays a purely additive option, evaluated at v1.0 against measured need.

---

## Tooling and process

### D038
**Diagnostics are a first-class subsystem designed for machine consumption.** · locked

Every diagnostic has a **stable code** (`DT0142`), an exact byte span, a plain-English explanation, and where possible a suggested fix. `doot check --json` emits the full structured set for piping into an agent. `doot explain DT0142` prints the long form. `doot doc --agent` emits a compact, complete language and stdlib reference sized for an agent's context window — **the language ships its own AI context file**, which is the most direct available attack on goal 2.

Diagnostic codes are permanent once assigned. See [06-tooling.md](06-tooling.md).

### D039
**`doot fmt` is canonical and has no options.** · locked

The gofmt lesson: a single non-negotiable format ends style debate, and — specifically valuable here — makes agent output deterministic and diffs meaningful. Naming is part of the format: modules `lower`, types `PascalCase`, functions and fields `snake_case`.

### D040
**Configuration is doot code. There is no config file format.** · locked

Configuration is a top-level `let` of a known type in `app.do`. No TOML, no YAML, no JSON, no `.env` parsing, no dotfile dialect — and therefore no second syntax to learn, no config parser to maintain, and no class of "config is valid YAML but wrong" errors. Configuration is typechecked like everything else.

### D041
**Single-file mode is supported.** · locked

`doot run app.do` runs a complete application, picking up `schema.sql` beside it if present. The PHP-like path from idea to running page costs nothing to support and matters disproportionately for adoption and for teaching.

### D042
**The keyword list and the reserved-word list are frozen at v0.1.** · locked

All 31 keywords are fixed in v0.1, including those whose features land later (`spawn`, `send`, `stream`), so the grammar never churns. Additionally, 35 words are **reserved but unused** (`async`, `await`, `class`, `import`, `try`, `throw`, `switch`, `const`, `impl`, `trait`, …) so that a programmer or agent reaching for a foreign construct gets a clear, specific error instead of a confusing parse failure — and so no future addition can be a breaking change. Full list in [02-syntax.md](02-syntax.md#reserved-words).

### D043
**Attributes are a closed set. No user-defined macros, decorators, or annotations.** · locked

Twelve built-in attributes in v0.1 ([02-syntax.md](02-syntax.md#attributes)). Metaprogramming is permanently out of scope: it is the fastest way to make a codebase unreadable to both a newcomer and a static analyzer, and it would put an unbounded surface behind a language whose entire premise is a bounded one.

### D044
**Task is the name of the unit of concurrency.** · locked

`task`, `spawn`. Not `job` (collides with the durable queue), not `fiber`, `goroutine`, or `coroutine` (each imports expectations from another runtime).


---

## Implementation and engineering

Practices, argued at the same standard as the language decisions because they are equally hard to change later. Details in [09-engineering.md](09-engineering.md).

### D045
**GNU Make for development; a single amalgamated `.c` file for distribution and as the portability guarantee.** · locked

`tools/amalgamate.sh` emits one `build/doot.c` that includes every translation unit, so `cc -O2 -o doot build/doot.c` builds the entire project. CI enforces this on every commit.

This is what turns [D035](#d035) from an aspiration into a tested property. It is also the packaging format, the fallback when `make` is unavailable, and a source of whole-program optimization without LTO. SQLite proves the model over two decades.

*Rejected:* CMake (a second language to learn and maintain, and a generator whose output differs across versions); autotools (a build system larger than doot will be); a bespoke build tool written in doot (a bootstrap problem for no gain).

### D046
**A narrower subset of C99 than C99 permits.** · locked

No VLAs, no `alloca`, no C11 features, no compiler extensions except computed goto behind `DOOT_HAVE_COMPUTED_GOTO`. Fixed-width integer types everywhere. No global mutable state in the implementation, mirroring [D008](#d008) so that per-worker isolation ([D007](#d007)) is structural rather than audited.

`-Werror` with an aggressive warning set under **both gcc and clang**, including `-Wconversion` and `-Wswitch-enum`. `-Wconversion` demands an explicit cast at every narrowing, which is verbose and is exactly the check that catches the silent integer truncation [D002](#d002) exists to prevent. Enabling it now costs verbosity; enabling it after 40,000 lines exist would cost a week and then get switched off.

### D047
**The implementation dogfoods arenas. There is no `free()` in the compiler.** · locked

One arena per compilation, released whole. This eliminates use-after-free and leaks as *categories*, speeds compilation, and keeps sanitizer output focused on runtime behavior rather than allocator bookkeeping. It also means the arena is exercised by every compile long before it carries a request.

Allocation failure policy is per arena and set at construction: the compiler's arena is **fatal** on exhaustion, a request arena **returns `NULL`** which the VM converts to a `budget_exceeded` fault ([D005](#d005)).

### D048
**Assertions are always on. `NDEBUG` is never defined.** · locked

An assertion failure is an invariant violation in doot itself — not a user error and not a language-level fault ([D012](#d012)). Continuing past one risks corrupting a database or serving wrong data, and for users who cannot debug the runtime, silent corruption is far worse than a loud abort with a stack trace. The cost is a predictable branch outside the interpreter's hot loop.

### D049
**Five test layers, with spec tests as the bulk of the suite.** · locked

Unit tests for internal invariants, **`.do` spec tests as the executable form of the specification**, raw-socket wire tests for HTTP, sanitizers on every suite in CI, and fuzzing on everything that consumes untrusted bytes.

A language is defined by what it accepts and rejects, so the suite's centre of gravity is `tests/spec/` — thousands of small `.do` files with exact expected diagnostics — not unit tests over internal functions, which pin down implementation shape rather than behavior.

Two rules make it load-bearing: **a diagnostic code does not exist until a spec test produces it** (a code without a test fails CI), and **every well-formedness rule in [03-grammar.md](03-grammar.md) has an accepting and a rejecting test.** The runner consumes `doot check --json` and compares structured output exactly, so a changed span or reworded message is a visible diff rather than a silently passing substring match.

Fuzzing exists from the first commit rather than being added when the parser lands, because retrofitting it means retrofitting parse-from-buffer entry points that were never designed to be called that way.

### D050
**The diagnostic registry is one X-macro table: the single source of truth for the compiler, `doot explain`, and the documentation.** · locked

Code, severity, brief message, and long explanation live in one table in `src/base/diag_codes.h`. The compiler, `doot explain`, `--json` output, and the generated documentation all derive from it, so an explanation cannot drift from the code that emits it and a code cannot exist without an explanation.

This is [D038](#d038) made mechanical rather than aspirational. Codes are permanent once assigned: never renumbered, never reused, never repurposed.

### D051
**Eight CI gates, all blocking.** · locked

`build-gcc`, `build-clang`, `unity`, `test`, `sanitize`, `fmt-check`, `tidy`, `fuzz-smoke`, `docs`. Nothing merges with a failing gate and nothing is marked advisory — an advisory gate is a gate that is off.

The `unity` gate specifically protects [D035](#d035), which is otherwise the decision most likely to quietly stop being true.

### D052
**Dependencies are committed to the repository, pinned, checksummed, and unpatched.** · locked

`tools/vendor.sh` fetches a pinned version, verifies SHA-256 against `vendor/MANIFEST`, and writes the tree. A build must work with no network, no package manager, and no upstream still being online — which is what a ten-year horizon actually requires.

**No local patches.** If one becomes unavoidable it lives in `vendor/patches/` as a standalone file applied by the script, with its reason and upstream issue recorded. An edit in place is invisible at the next update, and silently reverting a security fix is the failure this rule exists to prevent.

Vendored code compiles with its own warning flags, is excluded from `clang-format` and `clang-tidy`, and is included in sanitizer runs.

### D053
**`clang-format` is canonical with no local exceptions.** · locked

The same argument as [D039](#d039), applied to the implementation. Disagreements are settled by editing the committed config once, never by an inline `// clang-format off`.

### D054
**No stubs. A command exists only when it fully works.** · locked

No placeholder implementations, no unimplemented branches, no features merged behind a flag that turns them off, and **`TODO` in the source tree is a CI failure.**

The CLI grows one working command at a time. `doot check` does not appear until it genuinely checks, because a command that half-works trains users to distrust the tool and hides the real state of the project from its own author — which, on a project attempted three times, is the specific failure mode most worth engineering against.

Also the eighth item in the definition of done ([09-engineering.md](09-engineering.md#definition-of-done)), which is where it gets enforced per subsystem.


### D055
**Style and analysis tool versions are pinned, verified before use, and installed from the pin in CI.** · locked

`clang-format` and `clang-tidy` versions live in the Makefile, `make tools-check` fails on a mismatch, and CI installs exactly those versions rather than using whatever the runner image ships.

Prompted by a concrete failure: the `tidy` gate passed locally and failed in CI on the first push. The cause was not the code but the environment — `clang-analyzer-valist.Uninitialized` fired on textbook-correct `va_start`/`vfprintf`/`va_end` code under one host's system headers and not another's, alongside several thousand suppressed analyzer warnings from those same headers.

**A gate whose result depends on the machine it runs on is worse than no gate**, because it trains you to treat red as noise, which is precisely how a real failure gets waved through. Pinning restores the property that makes a gate worth having: `make check` locally and CI reach the same verdict. `make check` therefore includes `tidy`, so the two cannot silently diverge again.

Corollary for check selection: **a check that cannot be satisfied by correct code is disabled in `.clang-tidy` with its reasoning written out, not suppressed inline** ([D053](#d053)) and not left failing. Two are currently disabled, each with the argument recorded in that file, including the residual risk accepted and the condition under which it would be re-enabled.

Upgrading a pinned version is a deliberate change: bump the pin, run `make fmt`, review the resulting diff, and commit it as its own change rather than mixed into unrelated work.


---

## Front end

The lexer, the parser, and the AST, argued here and specified in [10-frontend.md](10-frontend.md). These are the decisions that had to be settled before any compiler code could be written, and they are settled in one pass so that the implementation never has to stop and decide something.

### D056
**The lexer is pull-based, with an explicit mode stack. The parser drives it one token at a time.** · locked

`lex_next` / `lex_peek` over a `lexer` context passed explicitly, with a stack of scanning modes (`LEX_NORMAL`, `LEX_STR`, `LEX_MARKUP_TAG`, `LEX_MARKUP_CONTENT`) bounded at 64 entries.

This is forced rather than chosen. Deciding whether `<` opens a markup literal requires knowing whether the parser is in expression position ([D059](#d059)), and a batch lexer that produces a finished token array before parsing begins has no access to that. Once the lexer must interleave with the parser for markup, a single pull-based entry point is simpler than a batch path plus an on-demand path for markup, and it removes the question of which one is authoritative.

Exceeding the mode-stack bound is a diagnostic (`DT0022`), not an assertion. Nesting depth is reachable from untrusted input on any endpoint that compiles source, so it is user input, and [D048](#d048) reserves assertions for invariant violations in doot itself.

*Rejected:* a batch lexer producing a token array, with the parser re-lexing markup regions on demand — two lexer entry points over the same bytes, and the re-lex has to reproduce the mode state the first pass discarded; lexing markup as one opaque token and parsing its interior separately, which is the same problem with an extra representation.

### D057
**A token carries a kind and a span, and nothing else. Literal values are decoded by the parser.** · locked

`token` is `{ token_kind kind; span at; }` — twelve bytes. No decoded integer, no unescaped string.

The lexer therefore **allocates nothing** and touches the arena only to report a diagnostic, which removes allocation failure from every lexer path and makes `fuzz_lex` a pure function of its input. The token stream is reproducible byte-for-byte, which is what allows the spec suite to assert on spans exactly rather than approximately ([D066](#d066)). And source text keeps one representation — a `slice` into `source_text` — instead of two that can disagree.

The re-scan is not free, but decoding happens once per literal either way; the only question is which stage does it.

*Rejected:* a value union in the token (an `int64_t`, a decoded `slice`), which is the conventional design and costs the three properties above to save a second pass over bytes already in cache.

### D058
**String literals lex as a token sequence, not as a single token, and non-interpolated strings use the same shape.** · locked

`STR_START`, `STR_TEXT`, `INTERP_START` … `INTERP_END`, `STR_END`. Interpolation makes a string literal structurally a tree, so a single token cannot represent one without the parser re-lexing its interior — which is [D056](#d056)'s rejected alternative in another place.

`"hello"` gets the full three-token shape rather than a shortcut. Two extra tokens buys a parser with one case instead of two, which is [goal 1](00-vision.md#the-nine-goals) applied to the implementation rather than only to the language.

`STR_TEXT` spans raw bytes with escapes unresolved; the parser resolves them and reports `DT0014`–`DT0016` at the escape's own span rather than at the start of the literal. Raw strings stay a single token, because backticks admit neither escapes nor interpolation and so have no interior.

### D059
**Markup recognition is split at the lexical seam: the lexer decides tag shape, the parser decides expression position. Markup delimiters are distinct token kinds, never operator kinds.** · locked

The `<` rule in [03-grammar.md](03-grammar.md#disambiguation) has three conditions. Conditions 2 and 3 — immediately followed by a letter, `_`, or `/`, forming a valid tag name followed by whitespace, `>`, `/`, or `=` — are decidable from bytes alone, so the lexer checks them and emits `TOK_MARKUP_START` or `TOK_LT`. Condition 1 — expression position — is the parser's, and it resolves it by position: in expression position `TOK_MARKUP_START` opens a literal, in operator position it reads as less-than. The token spans only the `<`, so that reinterpretation needs no re-lexing.

The second half is not cosmetic. `>` is in the statement-continuation set as a comparison operator, so if a markup tag's closing `>` were `TOK_GT`, then `return <p>hi</p>` would end on a continuation token and swallow its own statement terminator. Distinct kinds for `TOK_TAG_END`, `TOK_TAG_SELF_CLOSE`, `TOK_MARKUP_START`, and the rest make the operator entries in that set unambiguous by construction rather than by a special case in the line-structure rule.

*Consequence:* element nesting is tracked by the parser, which is already building the tree, so `</>` closing the most recent open element and the mismatch diagnostics `DT0060`–`DT0062` all sit where the open-element stack lives.

### D060
**Three corrections to the statement-continuation set, and a three-way definition of statement end.** · locked

*This corrects [03-grammar.md](03-grammar.md#line-structure) rather than reversing a decision: the rule's intent is unchanged and the corrections are what the intent requires.*

- **Postfix `!` is removed from the continuation set.** As written it suppressed the newline after `let u = create(name)!` and joined the following line to it — breaking the most common error-handling statement in the language. Prefix `!` does not exist and `!=` is listed separately, so postfix propagation was the only `!` the entry could have meant, and it terminates statements rather than continuing them.
- **`+=`, `-=`, `*=`, `/=`, `%=` are added.** `=` was in the set and the compound forms were not, so a line break after `total =` was legal and after `total +=` was not. Same construct, same treatment.
- **`|` is added**, so a multi-line `match` pattern may break after the alternation bar like every other binary operator.
- **`.` is removed from the *follow* set**, while staying in the continuation set, so a method chain breaks after the dot rather than before it. Leading-dot continuation cannot coexist with `match`, because an arm's pattern begins with a dot:

  ```do
  match status {
    .active -> render()
    .banned -> deny()
  }
  ```

  The newline before `.banned` was suppressed, producing `render().banned` and silently merging two arms into one expression. That is the documented `match` example in [02-syntax.md](02-syntax.md#control-flow), so the rule as written could not parse the language's own reference. An enum pattern has no other spelling, while a chain can break after the dot or inside the parentheses — required syntax wins over optional style. *Found while implementing the parser against this rule.*

Separately, `}` in the *follow* set means the final statement in a block has no `NEWLINE` after it, so `expr NEWLINE` cannot match it. **Statement end** is therefore defined as `NEWLINE` consumed, or a lookahead of `}` or end of input not consumed, everywhere the grammar writes `NEWLINE` in a statement production.

*Consequence:* the emitted `NEWLINE` spans the entire run of consecutive newlines it replaces, so `doot fmt` recovers the author's blank lines by counting `\n` within the span. No extra token field, and the span is accurate rather than nominal.

### D061
**Contextual words lex as identifiers and are matched by text in the parser.** · locked

Two sets: the HTTP methods (`GET`, `POST`, `PUT`, `PATCH`, `DELETE`, `HEAD`, `OPTIONS`), recognized only immediately after `route` or `stream`; and `end`, which closes a markup control block.

The lexer stays context-free, and the parser — the only component that knows the context — produces a specific diagnostic instead of a bare parse failure: "expected an HTTP method after `route`, found `Get`."

Worth stating plainly because the documentation implies otherwise: **`end` is not one of the thirty-one keywords.** [D022](#d022) says markup control flow reuses statement keywords, and `if`, `else`, and `for` do, but `end` has no statement equivalent. It is a contextual word in markup control position, consuming no keyword budget, exactly as type names and stdlib module names are predeclared identifiers rather than keywords.

### D062
**All thirty-five reserved words share one token kind and one diagnostic code, with per-word messages from a table.** · locked

`TOK_RESERVED`, `DT0023`, and an X-macro table mapping each word to the message naming its doot equivalent — `import` to [D030](#d030), `await` to [D006](#d006), `class` to [D016](#d016), and so on.

A distinct token kind per reserved word would add thirty-five enumerators that every `switch` over `token_kind` must list under `-Wswitch-enum`, to distinguish cases that are handled identically: reject with a specific message. One kind plus one table keeps the enum proportional to the grammar and puts the thirty-five messages in one reviewable place, in the same X-macro form as the diagnostic registry ([D050](#d050)).

Reserved words lex as their own kind rather than as `TOK_IDENT`, so they cannot begin a statement or an expression. Two refinements are needed to keep the standard library expressible, and both were found by parsing the documented examples:

- **A reserved word is an ordinary name in a name position** — after `.`, as a field name, as an enum variant. `auth.require`, `uuid.new`, and `chan.new` are all reserved words after a dot, so the strict reading made three documented stdlib entry points unwritable. The reservation exists so that reaching for a *foreign construct* fails clearly, and `x.require` is unambiguously a member access; enforcing it there buys nothing and costs real code.
- **A stdlib module name wins over a keyword or reserved word in expression position.** [02-syntax.md](02-syntax.md#keywords) already declares that "all stdlib module names are predeclared identifiers, not keywords," and exactly two of the thirty-eight collide: `test`, which is keyword #27 and the assertions module (`test.eq(...)`), and `static`, which is reserved and the file-serving module. Neither word can begin an expression any other way, so there is no ambiguity to resolve — only a rule to state.

*The alternative was editing the frozen thirty-five-word list, which [D042](#d042) rules out and which would have been the wrong fix anyway: the collision is not that the words are reserved, it is that reservation was being applied in positions where it has no purpose.*

### D063
**The AST is arena-allocated tagged unions in seven node families, with intrusive singly-linked child lists. Node constructors do not fail.** · locked

Families: `decl`, `stmt`, `expr`, `type_ref`, `pattern`, `markup_node`, `attr`. Separate families rather than one universal node, so a function taking an `expr *` cannot receive a statement and each family's `switch` is exhaustive over only what it can contain.

Children are linked with a `next` pointer and held as `{ first, last, count }`, the shape the base layer already uses for `diag` and `diag_label`. This follows from the allocator: `arena_extend` grows only the arena's most recent allocation, so a contiguous array of children cannot be built while those children are themselves being allocated. A linked list appends in O(1) with no resize and no second pass, every consumer walks it in order, and `count` is there when a count is wanted.

**Constructors return a node, never an error**, because the compilation arena is built with `arena_new_fatal` and aborts on exhaustion ([D047](#d047)). The parser therefore carries no allocation-failure plumbing. This diverges from the base layer, where every allocation is checked, and the divergence is principled: `arena`, `slice`, `buf`, `source`, and `diag` are shared with the runtime, where a request arena returns `NULL` so the VM can raise `budget_exceeded` ([D005](#d005)). The AST is compiler-only and may rely on its arena's policy — which is why [D047](#d047) puts that policy on the arena rather than at the call site.

*Rejected:* index-based nodes in a growable array (compact and cache-friendly, and it forfeits the arena's O(1) whole-tree release for a resize strategy the arena exists to avoid); one universal node type with a single kind enum (fewer types, and every `switch` becomes non-exhaustive over cases that cannot occur).

*Consequence:* **a dotted name is one path node, not a chain of field accesses.** `db.all`, `models.user.find`, and `u.name` all parse as a single `EXPR_IDENT` holding the whole path; `EXPR_FIELD` arises only after something that is not a name, as in `f().name` or `xs[0].y`. This follows from [D030](#d030): with no imports, every non-local name is fully qualified, so the resolver has to decide where a module path ends and field access begins regardless — and it wants the whole path in one node to do that. Splitting it in the parser would only mean reassembling it later.

### D064
**The front-end diagnostic range `DT0001`–`DT0099` is allocated in full, in advance, with sub-ranges.** · locked

`DT0001`–`DT0009` source intake, `DT0010`–`DT0029` lexical, `DT0030`–`DT0059` syntactic, `DT0060`–`DT0079` markup syntax, `DT0080`–`DT0099` held. The complete table is in [10-frontend.md](10-frontend.md#front-end-diagnostics).

Codes are permanent once assigned and never renumbered ([D050](#d050)), which makes the numbering a one-way decision and therefore worth making deliberately rather than incrementally. Allocating the range up front means a code is chosen by where it belongs rather than by what was free that week, and it removes the registry as a coordination point between workstreams that would otherwise both reach for the next integer.

*Consequence:* five of the sixteen well-formedness rules in [03-grammar.md](03-grammar.md#well-formedness-rules) are reassigned from the resolver to the parser, because they need only syntactic context — whether the parser is inside a loop, a method, or a stream body. The parser has the tightest span and already tracks that context, and the resolver then carries no state whose only purpose is producing an error the parser could already see.

### D065
**A diagnostic code enters `diag_codes.h` in the same change as the test that produces it, never earlier.** · locked

[D064](#d064) reserves numbers in the documentation; this decides when a row appears in the registry.

The gate forces it: `tools/check-docs.sh` fails when a registered code is not produced by any test, which is [D049](#d049) made mechanical. Pre-populating forty rows would break the `docs` gate on the first commit — and it would be a stub table, forty codes that `doot explain` describes in full and the compiler never emits, which is what [D054](#d054) exists to prevent.

The reservation is what keeps two workstreams from both taking `DT0031`. The gate is what keeps the registry honest. Both are needed, and they are different mechanisms.

### D066
**The spec runner is a C test binary with its own narrow JSON reader, and it never depends on the `json` stdlib module.** · locked

`tests/spec/spec_runner.c`, built as `doot_spec` beside `doot_test`, outside the amalgamation. It discovers `tests/spec/**/*.do`, reads the directives from [09-engineering.md](09-engineering.md#2-spec-tests--testsspec), invokes the real `doot` binary once per file, and compares structured output exactly — in both directions, so an unexpected diagnostic fails the file just as a missing expected one does.

**Its own JSON reader, roughly 150 lines, understanding only the schema pinned in [06-tooling.md](06-tooling.md#diagnostics).** A test tool that parses its input with the implementation under test cannot fail independently of it: a bug in `json` would make the suite report success. The duplicated effort is small and the independence is the entire value of the suite.

**`system()` with output redirected to a temporary file, not `popen`.** `popen` is POSIX, absent from C99, and spelled `_popen` under MSVC, so it would introduce a platform shim into the test tooling before [v0.5](07-roadmap.md#v05--everywhere) needs one anywhere else. `system()` is standard C, and process startup dominates the runner's cost either way.

### D067
**Front-end implementation order: lexer, then parser, then `doot fmt` with the spec suite. Spec tests arrive with `doot fmt`, not with the lexer.** · locked

*This corrects the sequencing in [09-engineering.md](09-engineering.md#testing).*

A spec test drives a real command; the directives assume `doot check`; and `doot check` cannot ship until it genuinely typechecks ([D054](#d054)). So spec tests cannot arrive "with the lexer" as previously written, and waiting for the typechecker would leave the primary suite until last — the worst of the available orders.

`doot fmt` resolves it. Formatting needs the lexer, the parser, and a printer and nothing else, so it is **complete rather than partial** at the parser milestone. It reports lexical and syntactic diagnostics through the same sink and the same `--json` schema as every other command, `expect-fmt-stable` is already a specified directive, and idempotent formatting is a demanding test of whether the AST and the trivia list preserve everything they must.

*Consequence:* comments are never discarded, because a canonical formatter that deletes comments is not a formatter. The lexer appends each one to an optional caller-supplied list, and `comments == NULL` discards them exactly as `sink == NULL` scans silently.

*This consequence originally said comments are tokens, and that is wrong.* The line-structure rule needs the next significant token across a run of newlines, an unbounded number of comments can sit inside that run, and every token scanned past has to be held for delivery — so inline emission requires a lookahead queue as long as the comment run, which means allocation on the scanning path and contradicts [D057](#d057). A side list costs the same allocation only when a caller wants comments, and loses nothing: they arrive in source order with exact spans, which is all a printer needs. Found while implementing the algorithm against the written rule, which is the point of writing it down first.
