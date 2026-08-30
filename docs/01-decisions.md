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
- **`else` is removed from the follow set** as well, and for the same reason one level up: it begins a match arm, so the previous arm's value swallowed it as an `else` coalesce and then found `->` where an expression should be:

  ```do
  match a {
    1 | 2 -> two()
    else -> other()     // parsed as `two() else -> other()`
  }
  ```

  `else` needed suppression only for an `else` written on its own line after a `}`, which is not doot style and appears in no example — and the parser accepts that form regardless by looking past the newline, with `doot fmt` normalizing it to `} else {`. *Found while implementing the printer against the documented examples.*

**The rule that settles membership, arrived at after correcting two of the five entries:**

> A follow token must not be able to begin a construct.

Suppressing the newline *before* a token costs the parser the ability to distinguish "this token continues the previous line" from "this token starts a new one". For `)`, `]`, and `}` that distinction does not exist, because none of them can begin anything — so suppression is safe and is what makes multi-line calls, lists, and blocks work. `.` begins a match pattern and `else` begins a match arm, so both were unsafe. Stating the rule is worth more than the two fixes: it is checkable against any future addition, and it is why the remaining three entries can be trusted.

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


### D068
**`doot fmt` normalizes everything except line structure inside markup literals and argument lists, which it preserves.** · locked

Canonical, as [D039](#d039) requires: indentation, spacing, blank lines capped at one, struct fields aligned on the colon, void elements normalized to `<br/>`, parentheses re-derived from precedence, one spelling per numeric value. What the printer does *not* do is decide where a line breaks inside a markup literal or an argument list.

Two independent reasons, either of which is sufficient.

**The printer has no wrapping rule, so it must not join.** [06-tooling.md](06-tooling.md#doot-fmt) specifies a wrap column for markup attributes and for nothing else. A formatter that cannot break a long line must not merge one either, or a line the author split deliberately becomes unsplittable — the formatter would fight the only tool the author has left. So `f(\n  a,\n  b\n)` stays broken and `f(a, b)` stays joined, and the multi-line form is normalized to one argument per line with a trailing comma.

**In markup, a line break is content.** Whitespace between elements collapses to a single space in inline context, so *changing the amount* of indentation is invisible to the rendered page while *adding or removing a break* is not. Deciding breaks correctly needs an inline-versus-block element table and an understanding of CSS `white-space` — a large surface, and being wrong in it silently changes what a user's page looks like rather than producing an error. `pre` and `textarea` are emitted verbatim for the same reason, one step further.

*Consequence:* a text run that already spans lines is re-indented line by line, which is safe under the same collapsing argument and is what allows nested markup to be indented at all. A text run with no newline is emitted byte for byte, because a single space between two inline elements is meaningful.

*Rejected:* full re-layout of markup, as Prettier does for HTML — it needs the element and CSS model above, and its failure mode is a page that renders differently, which is exactly the class of silent behavior change this project refuses elsewhere. *Also rejected:* preserving nothing and emitting every construct on one line — simple, canonical, and it makes long lines unbreakable.

### D069
**Naming rules are enforced by the checker, not by the formatter.** · locked

*This corrects [06-tooling.md](06-tooling.md#doot-fmt), which said naming "is part of the format".*

A formatter cannot rename. Renaming changes what the code means and requires rewriting every use site, which is refactoring; formatting rewrites only how the same meaning is spelled. `doot fmt` therefore reports nothing about names.

The rules are unchanged and still enforced rather than suggested — modules `lower`, types `PascalCase`, functions and fields `snake_case`, enum variants `snake_case` — but they are the checker's, alongside the rest of name resolution, and their codes are allocated in the `DT0100`–`DT0199` names range in the semantic pass rather than here.

*Consequence:* the diagnostic carries the correct spelling as a machine-applicable suggestion ([D038](#d038)), so an agent or an editor can apply the rename even though `doot fmt` will not.


---

## Specification tests

### D070
**The spec runner reads its directives as plain text and links nothing from `src/`.** · locked

The runner finds the leading `//` block by scanning bytes, not by calling `lex_new` and walking the comment list it already collects for `doot fmt`.

Reusing the lexer is the obvious move and it is wrong twice. It is the argument from [D066](#d066) applied one level out: a test tool that reads its own expectations through the implementation under test cannot fail independently of it, and a lexer bug that dropped a comment or misplaced its span would make the suite mis-read what it was asked to assert — reporting success while the thing it guards is broken. It is also simply impossible in the general case, because a large share of these files exist precisely because they *fail* to lex, and their expectations still have to be readable.

*Rejected:* a `--emit-directives` mode on the `doot` binary, which moves the same circularity behind a flag and adds CLI surface for the test suite's benefit; parsing directives from a sidecar `.json` or `.toml` file per test, which doubles the file count, separates a test from its expectations, and needs a parser for a second format nobody asked for.

*Consequence:* the runner is a pure function of the bytes of the `.do` file and the bytes the subprocess wrote, which is what lets it be trusted to arbitrate between the compiler and the documentation.

### D071
**The runner fails closed: an unknown directive, an unknown JSON key, a missing mode, or a file that asserts nothing is a failure.** · locked

A spec test that silently asserts nothing is worse than a missing test, because it reports as coverage. So every ambiguity resolves to failure: a directive name that is not in the table, a malformed argument, a `.do` file with no `doot-spec:` line, a file with a mode and no expectation, a JSON object carrying a key the reader does not know, and a mode whose command the binary does not implement.

This is not a hypothetical risk being priced in. The unit fixture that was supposed to pin `docs/02-syntax.md` against the printer embedded a hand-corrected copy of the program rather than the documentation's bytes, and silently omitted two of its declarations; it passed for as long as it existed while the documentation it named was non-canonical. That is exactly this failure mode, in the codebase, found by reading rather than by a gate.

*Consequence:* adding a field to the `--json` diagnostic schema is a deliberate change that breaks the runner until the reader is updated. That is the desired direction: the schema is pinned in [06-tooling.md](06-tooling.md#diagnostics), and a reader that tolerated unknown keys would let the schema drift from its specification silently.

### D072
**Diagnostics are compared as an exact multiset of `(code, severity, line, col, message)`, in both directions, with messages matched in full.** · locked

An expected diagnostic that does not appear fails the file, and an unexpected one that appears fails it too. The symmetry is the point: a substring match on stderr would let a reworded message pass unnoticed, and the suite exists to make that a visible diff.

**Order is deliberately not asserted.** Report order is a genuine guarantee that `diag_sink` provides for determinism, but it belongs to the sink, and requiring every spec file to mirror it would make each one brittle to a legitimate reordering of two independent checks inside one pass — churn in hundreds of files to restate a property one unit test already pins. The division of labour is that the spec suite settles *what* is reported and the unit tests over `diag_sink` settle the *ordering and mechanics* of reporting.

*Rejected:* substring or regex matching on messages, which is what makes most compiler suites decorative; matching on code and position while ignoring the message, which would let the explanation rot while the tests stayed green, and the explanation is half of what [D038](#d038) promises.

### D073
**Positions in directives are the 1-based character columns that `--json` reports, and `expect-suggestion` carries a full range.** · locked

Three different columns exist in this codebase and only one belongs in a directive: `span.start`/`span.end` are byte offsets, `span.col` is a character column counted by skipping UTF-8 continuation bytes, and the human caret is a display column with tabs expanded to four. Directives use `span.col`. Writing that down is worth a decision because an author who reads a column off the caret in a tab-indented file will produce a wrong number that looks right.

`expect-suggestion` is a range — `3:36-3:41 -> "email"` — refining the start-only shorthand first sketched in [09-engineering.md](09-engineering.md#2-spec-tests--testsspec). A fix is a span plus a replacement, and an end that is wrong by a byte replaces the wrong text; a directive that cannot express the end cannot test what [D038](#d038) actually promises. An insertion is an empty range.

*This decision originally required `at <line>:<col>` on every diagnostic expectation, and that is wrong: not every diagnostic has a position.* The source-intake codes — `DT0001`, `DT0002`, `DT0003` — are reported by `source_from_file` **before** the source object exists, so there is no line index to resolve an offset against, and `diag_render_json` correctly omits `file` and `span` for them; the offset appears in the message instead. `at <line>:<col>` is therefore **optional**, and `expect-error: DT0003 "<message>"` is the form for a diagnostic that carries no span. Requiring `at` would have made three registered codes inexpressible, and spelling them `at 0:0` would have asserted a position that does not exist. Found while writing the spec tests for those codes, which is the earliest point at which it could have been found.

*Consequence:* the runner converts `suggestion.replace_span` from byte offsets to line and column itself, duplicating about ten lines of `source_line_col`. That duplication is intended, for the reason in [D070](#d070); it is also the one place the runner reimplements compiler logic, so it is stated here rather than discovered in the diff.

### D074
**`expect-fmt-stable` formats a copy in a scratch directory, and requires the file to be diagnostic-free.** · locked

`doot fmt` writes in place, so the runner copies the file into `build/<profile>/spec-tmp/` and formats the copy. **The test suite never writes to the repository**, and that holds by construction rather than by a cleanup step that a failing run can skip.

Formatting a real file on disk also keeps the write path under test — the temporary plus `rename` in `fs_write_file`, which is what users actually hit.

The second half is the subtle half. `doot fmt` leaves a file that did not parse byte for byte alone, so a file full of errors is trivially unchanged and would satisfy `expect-fmt-stable` while proving nothing at all. Pairing it with `expect-ok` is therefore required, and combining it with `expect-error` is a conflict the runner rejects: only a file with no diagnostics is known to have reached the printer.

*Rejected:* `doot fmt --stdout`, which bypasses the write path the suite should be covering, and `doot fmt --check`, which reports a boolean where the suite wants the bytes. Both add permanent CLI surface whose only consumer is the test harness, and [D039](#d039)'s no-options posture is not a reason to be careless about the operational flags that do exist.

### D075
**`expect-output` asserts exact stdout with `--json` absent, in any mode, at the cost of a second invocation.** · locked

Generalizes the run-only form in [09-engineering.md](09-engineering.md#2-spec-tests--testsspec). The runner collects diagnostics from `--json`, which occupies stdout, so a file that also pins human output needs a second run of the same command. The cost is paid only by files that ask for it, and process startup already dominates.

The reason to want it beyond `doot run` is `doot fmt`'s summary. Its three outcomes — reformatted, already formatted, skipped — are the primary thing a user sees from the command, they were reported wrongly until recently, and nothing in the suite covered them: `--json` carries diagnostics only, so the summary is invisible to a runner that reads only JSON. A directive that pins stdout exactly is the cheapest honest fix.

*Consequence:* human output becomes a tested interface for the commands that choose to pin it. That is a commitment, not an accident — it means rewording a summary line is a visible, deliberate diff.

### D076
**The binary under test is passed to the runner as an argument.** · locked

`doot_spec <path-to-doot> [filter]`, supplied by the Makefile.

It cannot be a compile-time path, because the sanitizer gate has to run the ASan `doot_spec` against the ASan `doot` while the ordinary `test` gate runs the debug pair — which binary is under test is a property of the invocation, not of the build. An environment variable would work and is worse: an invisible input that changes what the suite tested without changing the command that ran it. The filter argument mirrors `doot_test`'s.

### D077
**No per-test timeout. A hanging test is a budget bug.** · locked

ISO C `system()` provides no portable way to bound a subprocess, and adding one would mean a platform shim in the test tooling before [v0.5](07-roadmap.md#v05--everywhere) needs one anywhere else — the same reasoning that chose `system()` over `popen` in [D066](#d066).

The exposure is small and shrinking rather than accepted blindly. The modes that have files cannot loop: the lexer always advances and the parser is depth-bounded, both asserted continuously by `fuzz_lex` and `fuzz_parse`. When `run` mode gets files, a program that fails to terminate is a per-request budget failure ([D005](#d005)) — a runtime bug with a diagnostic, which is the mechanism that should catch it. A harness timer would convert that bug into a flaky test and hide it.

*Consequence:* the CI job timeout is the only backstop until the VM lands, and that is sufficient for a suite whose commands are provably terminating.

### D078
**`tests/spec/` is organized by subject, and the accepting-and-rejecting pair for every well-formedness rule is mechanically enforced.** · locked

Directories are the runner's reporting unit, so they name what is being specified — `lex/`, `parse/`, `markup/`, `fmt/`, `rules/`, `docs/` — rather than diagnostic number ranges, which would scatter one feature across several directories and tell a reader nothing.

[D049](#d049) requires an accepting and a rejecting test for each of the sixteen well-formedness rules in [03-grammar.md](03-grammar.md#well-formedness-rules), and that has been an aspiration with no mechanism. Files in `rules/` are named `rule_NN_<slug>_ok.do` and `rule_NN_<slug>_err.do`, and `tools/check-docs.sh` reads the rule-to-code table in the grammar and requires both files for every rule whose codes are registered. Coverage grows as codes land, and the gate is honest today: it demands tests for the five rules the parser discharges and nothing for the eleven the semantic pass will own.

*Consequence:* the grammar document gains an explicit rule-to-code column, because a gate cannot read a mapping that exists only as prose.

### D079
**A code that describes source text is proved by a spec test; a code that describes the driver's environment is proved by a unit test.** · locked

This replaces the interim rule, which accepted a code named anywhere under `tests/` — correct while spec tests did not exist, and too loose afterwards, since it would let every future code satisfy [D049](#d049) with a unit test over an internal function.

The line follows from what a code is *about*. Four codes cannot be elicited by any `.do` file and are allowlisted in the gate with a reason each: `DT0002` (source too large — a 64 MB fixture is not committable), `DT1001` (cannot read the file — a spec file is readable by definition), `DT1002` (cannot write it), and `DT1003` (cannot read the directory). Everything else is reachable from bytes, including invalid UTF-8 (`DT0001`) and an embedded NUL (`DT0003`), because a spec file is bytes and its leading directive block stays readable no matter what follows.

*Rejected:* an open-ended "unit test also counts" escape, which is the version of this rule that decays; and moving the four driver codes out of the registry to dodge the question, which would leave `doot explain` unable to describe errors the CLI genuinely emits.

### D080
**A documented complete program is pinned to a spec test byte for byte, declared in the code fence.** · locked

A fenced block that is a whole program names the file it is pinned to — ```` ```do spec=tests/spec/docs/chat.do ```` — and `tools/check-docs.sh` compares the block against that file with its leading directive block stripped. GitHub highlights on the first word of an info string and ignores the rest, so the marker is free.

The hole this closes was real. [10-frontend.md](10-frontend.md#idempotence) calls formatting the documented chat application "the strongest single case" and says the assertion decides which side is wrong when the printer and the documentation disagree. It could not: the fixture held a hand-corrected copy rather than the documentation's bytes and silently dropped two declarations, so both `db` call sites in the documentation stayed non-canonical for as long as the test existed. A claim that a document and an implementation agree has to be checked against the document.

Fragments stay unmarked and unchecked. A bare signature or a single expression cannot parse standalone, and inventing the surrounding context needed to make it checkable would test something the documentation does not say.

*Consequence:* in v0.1 the chat program is pinned for byte-equality and for its exact diagnostic set — three `DT0046`s for `stream` and `send` — but not for formatting, since `doot fmt` refuses a file with errors. Its formatting stays covered by the unit test over the subset v0.1 accepts, and when `stream` lands in [v0.2](07-roadmap.md#v02--realtime) the spec file gains `expect-fmt-stable` and that unit test is deleted. A dated gap, recorded, rather than a forgotten one.


---

## Semantic pass

The resolver, the typechecker, the schema checker, and the route checker, argued here and specified in [12-semantics.md](12-semantics.md). These are the decisions that had to be settled before any semantic-pass code could be written, and they are settled in one pass so that the implementation never has to stop and decide something.

### D081
**Four sequential stages — resolver, typechecker, schema checker, route checker — with a barrier between each: a stage runs only if every earlier stage reported no errors.** · locked

The stage list and its order are [05-runtime.md](05-runtime.md#compiler-pipeline)'s. What is decided here is that they are genuinely separate walks rather than one fused pass, and that an error in one stops the next.

The boundaries are not arbitrary. A name must be bound before its type can be asked for, and every rule the resolver owns — visibility, naming, mutability — needs only *which declaration a name refers to*. Mutability is the surprising member of that list and belongs there for exactly that reason: [D008](#d008) is a property of a binding, not of a type. The last two stages, however, depend on the resolver and **not** on the typechecker, which the pipeline's order does not suggest and which is worth stating: the typechecker never needs a prepared statement, because [D019](#d019)'s explicit type argument already gives `db.all[Msg](...)` the type `[Msg]!`, and the schema checker never needs an inferred type, because the fields it maps a row into are declared and the one check that would need one is reported by the typechecker, which is visiting every argument anyway. So the order among the last three is [05-runtime.md](05-runtime.md#compiler-pipeline)'s rather than a dependency, and what it buys is the barrier below — plus two things that turn out to be load-bearing: the schema checker and the typechecker can be built concurrently ([D102](#d102)), and `doot routes` can run the resolver and the route checker alone and still be a complete command ([D101](#d101)).

The barrier is the part worth arguing. Within a stage, checking continues after an error, so a run reports every independent problem — the same reason the parser recovers. Across stages it stops, because a type error derived from a name that did not resolve is not a second finding but the same finding restated about the wrong thing. `diag_sink` truncates past its limit, so a cascade does not merely add noise, it evicts the diagnostic that explains it. And [D038](#d038) exists so that an agent can act on `doot check --json` directly; an agent handed forty consequences and one cause will fix a consequence.

*Rejected:* fusing the resolver and typechecker into one walk, which is the conventional design and is cheaper by one traversal. It costs the barrier — once types are computed while names are still being bound, there is no point at which "all names resolved" is true — and it makes the two stages one workstream instead of two.

*Consequence:* a program with a misspelled name needs a second `doot check` run to reveal its type errors. Paid once per class of mistake, in exchange for output in which every diagnostic is about code the author wrote.

*Consequence:* each stage is one walk of the unit with every check it owns performed at the node it applies to, not one walk per rule. That is for diagnostic ordering rather than speed: a single walk reports a function's problems together, and a per-rule walk reports every instance of rule A across the program and then every instance of rule B.

### D082
**The symbol table is arena-allocated symbols in intrusive scope lists, with linear lookup, and the two scopes large enough to matter are sealed and indexed after they stop growing.** · locked

`symbol` carries a kind, a name, its declaring span and file, `pub`, mutability, a used flag, a type slot — its declared type, resolved from its `type_ref`, or `NULL` for a local whose type the typechecker infers — and a `next` pointer; `scope` holds `{ first, last, count }` and a parent. This is the shape [D063](#d063) chose for every AST child list and the base layer already uses for `diag_label`, and it is chosen for the same reason: `arena_extend` grows only the arena's most recent allocation, so a contiguous array cannot be built while its elements are themselves being allocated.

Lookup is a linear walk of the chain and of each scope's list, which is correct for the sizes involved — a function body holds a handful of locals and a chain is a handful deep — and a hash table would need the resize strategy the arena deliberately does not have ([D047](#d047)).

Two scopes are big: the unit scope, and a struct's field list. Both are **built once and then never extended**, so each is **sealed** after its last insertion — one arena block holding an open-addressing index sized to the next power of two above `count`. Sealing is what makes the linear default honest rather than a shrug: the scopes that keep growing stay linear because they are small, and the scopes that are large stop growing before anything searches them.

*Rejected:* a hash table for every scope (a resize inside an arena means either leaking the old table, which is free but unbounded, or a second allocator); interning every name into a global string table and comparing pointers (a real win in a compiler with many compilation units, and doot has exactly one, so `slice_eq` over short names is already what the comparison costs).

### D083
**Declarations are visible everywhere in the unit; locals are visible only after their own declaration. Shadowing within a function is an error, shadowing a predeclared or top-level name is a warning, and a leading underscore marks a binding as deliberately unused.** · locked

Compilation is whole-program with no imports ([D029](#d029), [D030](#d030)), so a **collect** step runs before any body is resolved: it derives every file's module path from its filesystem path, creates the module namespaces, fills a file scope with every top-level declaration, and attaches methods, fields, and variants to their types. Forward references across the unit therefore work in every direction, which is forced rather than chosen — there is no import to establish an order and no header to declare one.

Locals are the exception and bind from the statement after their `let`. There is no hoisting, and the consequence is worth stating: a lambda bound to a local cannot call itself, because its own name is not yet in scope. A recursive function is a top-level `fn`, which collect made visible everywhere.

The three shadowing rules:

- **Redeclaration in one scope** is an error, with a label at the first declaration.
- **Shadowing another binding of the same function** — a local, parameter, `self`, loop variable, or `err` binding in any enclosing scope up to the function's own — is an error. Sibling scopes do not shadow, so two `for u in ...` loops in one body are fine. A lambda is *not* a fresh function for this rule: its parameters participate, because a parameter spelled like a captured local is precisely the case that reads wrongly.
- **Shadowing a predeclared name or a top-level name** is permitted with a warning. It cannot be an error, because [02-syntax.md](02-syntax.md#keywords) declares type names and stdlib module names to be predeclared identifiers that "may be shadowed in a local scope". It is warned because the failure mode is otherwise baffling: after `let time = "12:00"`, `time.now()` is a field access on a `str` and every diagnostic that follows is about `str`.

A name beginning with `_` is exempt from the unused-binding warnings, and `_` alone is the anonymous form — the one name that may be bound repeatedly in a scope, and referring to it is an error. The grammar admits a leading underscore, so the checker has to give it a meaning or reject it, and "deliberately unused" is the meaning that makes `for _, u in users` writable. Without it, an unused-binding warning would be unsilenceable, which is how a warning gets switched off.

*Rejected:* permitting shadowing within a function silently, as most languages do. Goal 2 is that unambiguous beats terse, and a name that means one thing everywhere in a function is a property a reader can rely on without tracking which block they are in; the fix is a rename, which is mechanical. *Also rejected:* forbidding shadowing of a stdlib module, which [02-syntax.md](02-syntax.md#keywords) has already promised is allowed.

### D084
**A dotted name resolves by longest namespace prefix, a lexical binding always wins, and a path may not be both a namespace and a member.** · locked

[D063](#d063) makes a dotted name one `EXPR_IDENT` holding the whole path and leaves the split to the resolver. The algorithm, over segments `s1 … sn`:

1. If `s1` is found in the lexical chain — locals, parameters, `self`, loop variables, `err`, `req`, then the file scope — the binding is `s1` alone and everything after it is a member access. **A lexical binding wins at any depth over any namespace**, which is what makes `u.name` a field access and what makes [D083](#d083)'s shadowing warning necessary rather than decorative.
2. Otherwise take the **largest** `k` such that `s1 … sk` names a namespace. If `k == n`, the path is a module and not a value, which is its own diagnostic. Otherwise `s1 … s(k+1)` must name a member of that namespace and is the binding, with the rest member accesses. A missing member and a member that exists but is not `pub` are different mistakes and get different messages.
3. In type position the same walk runs against types, so `time.Time` is namespace `time`, member type `Time`.

Step 2 is unique and needs no backtracking only because of one added well-formedness requirement: **a path may not be both a namespace and a member.** A file `models/user.do` and a directory `models/user/` both claim `models.user`, and that is an error rather than a precedence question. Deciding it by precedence would mean a reader has to know which files exist to know what `models.user.find` means, which is exactly what [D030](#d030) removed by deleting imports.

*Rejected:* resolving the shortest prefix that names anything and treating the rest as members, which fails on `models.user.find` because `models` alone names a namespace and has no member `user` in the value sense; *also rejected:* requiring a marker to separate module path from member access, which is an import statement wearing a different hat.

*Consequence:* `Status.active` takes step 1, since `Status` is a type symbol, and a member access on a type resolves to an enum variant and nothing else. See [D092](#d092).

### D085
**The standard library and the prelude are one compile-time signature table, and a member of a module whose version has not landed reports `DT0046`.** · locked

One X-macro, in the same form as the diagnostic registry ([D050](#d050)) and the reserved-word table ([D062](#d062)), carrying per member: the module, the name, the signature with its `!` and `?` marks, any type-argument slots, whether it mutates its receiver ([D097](#d097)), and the release it lands in. The prelude is the same table's unqualified half — the seven predeclared type names from [02-syntax.md](02-syntax.md#keywords) plus `Error`, `ErrorKind`, `Request`, `Response`, `redirect`, and `Upload`, all of which the documentation already uses unqualified.

Two of its properties are load-bearing rather than incidental. `ErrorKind`'s variant set is **closed and known at compile time**, which is what makes `match err.kind` decidably exhaustive — a benefit of [D014](#d014)'s single universal error type that only shows up here. And `html.raw` is **the only member taking `str` and returning `html`**, which is what makes `grep raw(` the complete XSS audit [04-stdlib.md](04-stdlib.md#html) promises; the type rules close every other route between the two.

**Reaching a module whose version has not landed reports `DT0046`**, the code the parser already uses for `spawn`, `send`, and `stream`. The situation is identical — correct, final syntax for a feature that arrives later — and a second code would need a second explanation saying the same thing. So `topic.publish` in v0.1 reports `DT0046` naming v0.2, exactly as `send` does.

*This is not a stub, and the distinction matters under [D054](#d054).* A module's compile-time half is a complete artifact that `doot check` genuinely uses, and it has to exist before the runtime half, because [D033](#d033) promises SQL is checked at compile time and that is unimplementable without `db`'s signatures. A stub would be an entry for a module the project has not decided on, and there are none: the set is closed at thirty-eight.

*Consequence:* the table is on the critical path for every checker, because no checker can be tested against a program that calls a module the table does not describe. Its format is therefore the first thing the semantic pass fixes, and filling it in is a workstream of its own ([D102](#d102)).

### D086
**Naming rules are checked with exact patterns whose corrected spelling is derivable, and every violation carries a machine-applicable rename — except a module name, which has no span.** · locked

[D069](#d069) moved the rules from the formatter to the checker and left their spellings as the four words in [D039](#d039). The patterns: module path segments `[a-z][a-z0-9_]*`; types one or more `[A-Z][a-z0-9]*` segments; everything else `[a-z][a-z0-9]*(_[a-z0-9]+)*` with an optional single leading `_`.

`PascalCase` therefore forbids two adjacent capitals: `UserId`, not `UserID`; `Url`, not `URL`. That is stricter than a human needs and exactly as strict as a machine needs, because it makes **the correct spelling derivable from the wrong one** — split on underscores and on each transition into a capital or out of a run of capitals, then recase. A rule that cannot derive the fix can only describe it, and [D069](#d069) promised a machine-applicable suggestion.

**A module name is the exception and carries no fix.** It lives in the filesystem, so correcting it is a file rename rather than a text edit, and there is no span in any `.do` file to attach an edit to; the diagnostic is reported with no position at all — the shape [D073](#d073) established for the source-intake codes — and names the correct spelling in its message.

Violations are **errors**, following the general severity rule this pass adopts: **a warning is for code the language permits and considers redundant or suspicious; anything the language forbids is an error.** [D069](#d069) says the rules are enforced rather than suggested, so they are errors, and a redundant `else` arm is a warning.

*Consequence:* the naming codes are the first machine-applicable fixes in the compiler and therefore the first producers of `expect-suggestion`, which has been implemented and unexercised since the spec suite landed ([D073](#d073)).

### D087
**Named types are nominal; optionality is a flag on a type, not a wrapper; fallibility is a property of an expression, not a type; and recursion must pass through an indirection.** · locked

Four properties of the type representation, each of which settles a question that would otherwise recur.

**Nominal.** A struct or enum is identified by its symbol, never by its shape, so two structurally identical structs are different types. [D009](#d009)'s structural equality is about comparing values of one type, not about deciding what a type is. Nominal identity also makes type equality terminate on a recursive type with no visited set, because the recursion stops at the name.

**Optionality is a flag.** `T??` does not exist — applying `?` to an optional yields the same type. This follows the AST, where `type_ref.optional` is already a `bool`, and it deletes nested optionals and therefore the question of how many `else`s unwrap one.

**Fallibility is not a type.** There is no `T!`, because [D014](#d014) has one universal `Error` and no `Result[T, E]`. What the typechecker carries beside each expression's type is a `fallible` bit, set by a call to a `!` function and cleared by `!` or `else`. A function *type* records it, because a signature must; a value never does.

**Recursion needs an indirection.** `type Node { next: Node? }` and `type Tree { kids: [Tree] }` are fine, because an optional struct, a list, and a map are all pointers in a slot ([05-runtime.md](05-runtime.md#values)). `type Node { next: Node }` has no finite layout and is an error naming the cycle.

Additionally, **a type alias is transparent**: `type Id = int` is another spelling of `int`. A nominal alias would be a distinct type with no constructor and no conversion, since there are no generics and no newtype ceremony to give it either, so transparency is the only reading that leaves the feature usable.

*Rejected:* structural typing for structs, which would make `User` and `Admin` interchangeable whenever their fields happened to line up — a silent hazard in exactly the code that maps database rows; *also rejected:* a `nil` type or a bottom type, which would let `let x = nil` acquire a type nobody wrote. `nil` is a literal with no principal type, and using it without an expected type is a diagnostic naming the annotation to add.

### D088
**Assignability is nominal identity plus widening into an optional, and nothing else. There are no implicit conversions.** · locked

`assignable(from, to)` holds when they are the same type, or when `to` is optional and `from` is that type without the flag, or when `from` is `nil` and `to` is optional.

No `int` to `float`, no `float` to `int`, no `str` to `html`, no `T` to `any`, no `any` to `T`, no enum to `int`. Every conversion doot has is written with `as` and the set of those is closed ([D089](#d089)). Mixed arithmetic is an error with a fix, not a promotion: silent widening is how a monetary calculation in [D020](#d020)'s integer cents becomes a float.

The direction deliberately absent is `T?` to `T`. Unwrapping is `else` ([D017](#d017)), and an optional used where a value is required gets its own code rather than the general mismatch, because the fix is `else` and naming it is what makes the message actionable rather than descriptive.

*Rejected:* implicit `int` to `float` widening, which every C-family language has and which is the one conversion users will miss. It is also the one that turns [D003](#d003)'s checked integer arithmetic into unchecked float arithmetic without anything appearing in the source.

### D089
**`as` is a closed, total table, and where a conversion can be refused its result is optional rather than fallible.** · locked

The whole table: `int` to `float`; `float` to `int`, truncating toward zero and faulting on NaN, infinity, or an out-of-range value; any type to `any`, boxing; `any` to `T`, yielding `T?`; `str` to `bytes`; `bytes` to `str`, yielding `str?`; an enum to `int`, its ordinal. Anything else is a diagnostic naming the alternative.

Making the refusable conversions **optional-returning** rather than fallible or faulting is what keeps `as` out of the error model: `let n = v as int else 0` handles a failed cast with the mechanism the language already has, and no cast needs a `!`. `float as int` is the exception and faults, because it is arithmetic, and [D003](#d003) already decided that arithmetic which cannot produce a right answer is a fault rather than a value to check.

**`any` supports `as` and nothing else** — no indexing, no field access, no arithmetic, no comparison. Navigating a decoded JSON document means casting to `{str: any}` or `[any]` first. Letting operations through `any` would put back into the interpreter the runtime type check [D002](#d002) removed from it, on the one type that carries a tag.

**`str as html` gets its own code**, whose message names `html.raw`. It is the one impossible cast worth a code of its own, because it is what an author reaches for immediately before creating an XSS hole, and the diagnostic is the last place to say so.

*Rejected:* `as` as a general formatting operator (`n as str`), which duplicates `str.from_int` and would make the table open-ended; *also rejected:* a fallible `as` marked `!`, which would put casts into the error model and make every JSON field access a propagation site.

### D090
**Checking is bidirectional, and six expression forms are legal only against an expected type.** · locked

`infer(expr)` and `check(expr, expected)`, with an expected type flowing inward from an annotation, a parameter, a field, an enclosing collection's element type, a return type, or the left side of a value-form `else`.

That is what makes the documented forms work with no annotation: `let xs: [int] = []`, `let u: User? = nil`, `let s: Status = .active`, `f(.active)`. The six forms with no principal type — empty list, empty map, `nil`, a bare `.variant`, a literal whose elements disagree, and a lambda with no declared return type that returns nothing on some path — are diagnostics naming the annotation to write when no expected type exists.

**Annotations are mandatory in every declaration position**: parameters, fields, return types, route parameters. There is no inferred signature anywhere in the language, which is what lets the collect step build the entire symbol table with types attached before any body is walked — and therefore what makes whole-program forward references cost one pass rather than a fixed point.

*Rejected:* full Hindley–Milner inference over the unit, which would infer signatures too and is a poor fit for a language whose goal 2 is that code be readable without consulting an inference engine; *also rejected:* requiring an annotation on every local, which [D015](#d015) already decided against.

### D091
**There is no truthiness, and an expression statement must be able to have an effect.** · locked

The condition of `if`, `while`, and a markup `{if}`, and both operands of `and` and `or`, must be exactly `bool`. `if u.bio` is an error and `if u.bio != nil` is the spelling. Comparing two different types is a diagnostic rather than a constant `false`, so `id == "3"` is caught rather than always failing at runtime.

The grammar permits `expr NEWLINE` as a statement, and the typechecker accepts it only when the expression is a call, or a `!` or `else` over one. `total + 1` as a statement is an error. Discarding a call's return value is fine; discarding a *fallible* call's result is not, because [D013](#d013) requires `!` or `else` whether or not the value is wanted.

Both rules exist for the same reason: they turn a class of agent-written mistake — a condition that is a value, a line that was meant to be an assignment — from silent behaviour into a diagnostic with an obvious fix. Truthiness in particular is a rule a reader has to memorize per type, and goal 1 does not allow one.

### D092
**A method attaches only to a type this program declares, is declared in the same file as its type, and has no static form.** · locked

Four rules and one consequence.

- **Only a struct or enum declared in this program** may receive a method: not a primitive, not `html`, not `[T]`, not a stdlib type, not an alias's target. The stdlib is closed ([D029](#d029)), and if user code could attach to `str`, then reading `s.upper()` would require knowing which files are in the project — which is exactly the "where is this method actually defined" question [D016](#d016) exists to answer.
- **In the same file as its type**, which makes "beside the type, always" the complete answer to that question, and makes a type's method list a per-file fact the collect step establishes rather than a whole-program search.
- **A method's name may not collide with a field** of its receiver, so a member access never has to choose between a field of function type and a method.
- **`pub` on a method is an error.** A method's visibility is its type's — redundant when the type is `pub`, meaningless when it is not.

**There is no static method form in v0.1.** `fn User.guest() -> User` cannot be written, because [03-grammar.md](03-grammar.md#well-formedness-rules) rule 9 requires `self` as the first parameter and `DT0034` is registered and tested. A member access on a type name therefore resolves to an enum variant and to nothing else, and a value is constructed by a free function.

*This corrects an illustration rather than a decision.* [D013](#d013) and [02-syntax.md](02-syntax.md#errors) both write `find(id) else User.guest()`, which rule 9 makes undeclarable. The rule as written wins — it is registered, tested, and load-bearing for [D016](#d016)'s no-classes claim — and the doot spelling of that example is a free function returning a `User`. Found while specifying path resolution against the documented examples, which is the point of specifying it first.

*Rejected:* permitting a receiver-typed function with no `self` as a static constructor, which would reverse rule 9; *also rejected:* allowing methods on stdlib types, which is the extension method feature, and whose cost is that the meaning of a call depends on which files are present.

### D093
**A lambda captures immutably, always, and infers its return type and its fallibility when both are omitted.** · locked

Every name a lambda reads from an enclosing scope is captured by value and is immutable inside the body; assigning to a captured binding is an error. This is not a `spawn`-specific rule and is not relaxed away from `spawn`: it keeps a `var` local's ownership genuinely unique, which is what lets `xs.push(x)` mutate in place ([D008](#d008)), and it means moving a loop body into a lambda can never quietly introduce aliasing. Accumulating into an outer `var` is a `for` loop.

A lambda with **no declared return type infers both the type and the fallibility** from its body — from the expression in the `=> expr` form, and from the agreeing `return` statements in the block form. This is forced by the documentation: `db.tx(fn() { db.exec(...)! })!` declares no return type on the lambda and propagates inside it, so a lambda that could not become fallible by inference would make the documented transaction unwritable. A lambda that *does* declare a return type must declare `!` as well if its body propagates.

`return` inside a lambda returns from the lambda; `!` inside one propagates to the lambda's own signature and says nothing about the enclosing function.

*Consequence:* [03-grammar.md](03-grammar.md#well-formedness-rules) rule 15 becomes narrow rather than a separate analysis. Since captures are already immutable everywhere, the only way for `spawn` to reach a mutable binding is a `var` local passed directly as an argument, and that is what rule 15's code reports.

*Rejected:* mutable capture with a `var` closure, which is what most languages do and which reintroduces the aliasing [D008](#d008) removed, at the one place — a closure handed to another task — where it is least visible.

### D094
**`else` handles fallibility and optionality together, and `else err` is refused when both are present.** · locked

`else` requires its left operand to be fallible or optional or both, and clears everything it finds: the result is the left type with the optional flag and the fallible bit both cleared. `!` clears only fallibility, so `db.find[User](...)!` is a `User?`, exactly as [02-syntax.md](02-syntax.md#data-access) documents.

The binding form is where handling both at once needs a rule. On a fallible operand, `else err` binds an `Error`. On an operand that is only optional, `else err` is an error, because there is no error to bind. On an operand that is **both** — `T?!`, which `db.find` returns — `else err` is an error naming the fix: write `!` first, then `else`.

That last case is the whole reason this is a decision. There are two distinct reasons to take the branch and only one of them has an `Error`, so binding `err` would either lie about the nil case or need a nested optional [D087](#d087) does not have. Forcing `db.find(...)! else err { ... }` costs one character and separates two failures the author wanted handled separately. Refusing rather than inventing a semantics is the move [D017](#d017) made against `?.`.

Separately, **a fallible expression must be handled immediately**: `!` and `else` are the only two contexts one may appear in. That is the teeth in rule 3, and it is what makes error handling verifiable by eye — a call to a `!` function is followed by a `!` or an `else` on the same line, or the program does not compile.

*Consequence:* `!` requires the enclosing function to be fallible, and at the top level of a file there is no enclosing function, so `!` in a top-level `let` initializer is an error. The spelling that works there is `else` with a value, which is what `env.get("NAME") else "default"` in [02-syntax.md](02-syntax.md#configuration) already is.

*Consequence:* `coalesce_form` in the AST is read directly rather than re-derived, which is what it was added for ([ast.h](../src/parse/ast.h)).

### D095
**One divergence-and-reachability analysis serves rule 16, the missing-return check, and the unreachable-code warning.** · locked

A statement diverges when control cannot continue past it: `return`, `break`, and `continue`; an `if` with an `else` whose every branch diverges; a `match` that is exhaustive and whose every arm diverges; a `while true` containing no `break` for it; a block containing any diverging statement.

That single definition answers three questions that would otherwise be three analyses: whether the block form of `else` diverges on every path ([03-grammar.md](03-grammar.md#well-formedness-rules) rule 16), whether a function with a return type can finish without returning, and whether a statement following a diverging one is reachable. Writing it once means the three cannot disagree, which they would eventually, in the `match` case.

Rule 16 also says "or a fault", and no expression in v0.1 is statically known to fault: there is no `never` type and no function that cannot return. The structural list is therefore complete, and complete in the direction that matters — it never accepts a block that can fall through.

*Consequence:* unreachable code is a warning rather than an error, reported once at the first unreachable statement with the diverging statement as a related label. It is code the language permits and considers pointless, which is what [D086](#d086)'s severity rule makes a warning.

### D096
**Markup value typing closes every path from `str` to `html` except `html.raw`, refuses `html` in an attribute value, and requires a `<form>`'s method to be written literally.** · locked

[D021](#d021)'s soundness claim — XSS is impossible unless the author writes `html.raw(s)` — holds only if every path between the two types is shut. There are exactly four, and the type rules close all four: assignability is nominal identity ([D088](#d088)); `as` refuses it with a dedicated diagnostic ([D089](#d089)); a text interpolation is always escaped; an attribute interpolation is always attribute-escaped. So `html.raw` is the only entry point, which is what makes `grep raw(` the complete audit [04-stdlib.md](04-stdlib.md#html) promises.

Text position accepts a renderable type — `str`, `int`, `float`, `bool`, `html`, `[html]`, and their optionals, with `nil` rendering as nothing per [03-grammar.md](03-grammar.md#semantics). Attribute-value position accepts `str`, `int`, and `float`; a `bool` or an optional only as the *entire* value, where it controls whether the attribute is emitted at all; and **`html` never**. Refusing `html` in an attribute is not conservatism: a value of type `html` was escaped for *text*, and using a text escape in an attribute is the bug this type exists to prevent.

**A `<form>` whose `method` attribute is interpolated is an error.** [D028](#d028) injects a CSRF token into every state-changing form, and the compiler can only do that if it can see the method. Without the check, `<form method="${m}">` would silently produce an unprotected form — a security default failing quietly, which is the outcome [D028](#d028) shipped in v0.1 to avoid.

Element and attribute *names* are not validated against an HTML vocabulary. HTML is extensible — `data-*`, `aria-*`, custom elements — so a closed vocabulary would reject valid pages, which is worse than accepting a typo in a tag name. Only the void-element list is known, and it is already the parser's.

*Consequence:* string interpolation is deliberately narrower than markup interpolation. `"${e}"` accepts `str`, `int`, `float`, and `bool`, and refuses `html` and optionals. [03-grammar.md](03-grammar.md#semantics) says `nil` renders as nothing *in markup* and nothing says it anywhere else; turning an absent value into an empty string is the invisible failure [D017](#d017) rejects `?.` for.

### D097
**Mutating standard-library members are declared in the module table, so deep `let` is checkable rather than aspirational.** · locked

[D008](#d008) makes `let` deeply immutable, and the assignment half of that is easy: an lvalue is always rooted at one name ([03-grammar.md](03-grammar.md#statements)), so the root must resolve to a `var` local and the depth of the path is irrelevant — which is exactly what "deeply immutable" means.

The other half has no lvalue at all. `xs.push(x)` mutates in place, so `let xs = ["a"]` followed by `xs.push("b")` must be refused, and nothing in the syntax says `push` mutates. The module table ([D085](#d085)) therefore carries a **mutating** column, and calling a mutating member requires the receiver's root binding to be a `var` local.

Without that column, the deep-`let` guarantee would hold for assignment and leak through every mutating method in the standard library — which would make it not a guarantee, and would make [D004](#d004)'s frozen-tier promotion and [D008](#d008)'s "no aliasing anywhere" unsound in the same stroke.

*Consequence:* `self` is a parameter and therefore immutable, so a method cannot assign to `self` or through it. Its own code, because the fix is `with` rather than `var`.

### D098
**The schema is derived by replaying migrations into an in-memory database, a SQL argument must be a single literal statement with positional placeholders, and nullability is decided conservatively.** · locked

[D033](#d033) says the compiler builds the schema by replaying migrations, prepares each statement, and reads its column metadata. What is decided here is everything that leaves unspecified.

- **Source of the schema:** `migrations/NNN_name.sql` in numeric order if the directory exists, otherwise `schema.sql` beside the file being checked ([D041](#d041)), otherwise none — and none is an error at the first `db` call rather than at startup. Numbers need not be contiguous, because gaps are what abandoned branches leave, but they must be unique, because the order is the whole contract. A failing migration reports SQLite's own message at a span computed from `sqlite3_error_offset()`, so it points at the byte that broke.
- **The replay records which migration created each table**, which is what lets a later diagnostic attach the "table `users` declared here" label the documented example in [06-tooling.md](06-tooling.md#diagnostics) shows. `doot check` never opens the project's real database; applying migrations is `doot migrate`.
- **A SQL argument must be a literal** — a raw string, or a plain string with no interpolation — and must contain **exactly one statement**. A computed argument cannot be prepared, so accepting one would silently exempt it from every check in this section, and it is also the only way an injection could be written. Two statements in one literal would silently run the first only.
- **Only positional `?` placeholders.** Arguments are passed positionally and a named placeholder has no name to bind from.
- **An enum binds as text, not as its ordinal.** A text column is readable in the database and, decisively, reordering an enum's variants is a source refactor that must not change what stored rows mean. The ordinal is a runtime representation ([D002](#d002)), not a storage format.
- **Result columns map to fields by name, order-independently**, in both directions, so `select *` needs no special case: the prepared statement's column list is already expanded.
- **Nullability errs toward nullable.** A column is non-nullable only when `sqlite3_column_origin_name` identifies a `NOT NULL` base-table column *and* the statement contains no outer join, tested lexically. Being wrong toward nullable asks for a `T?` that was not strictly needed; being wrong toward non-null lets a NULL reach a slot that cannot hold one, which is the failure the check exists to prevent. Doing better means owning a SQL parser, which [D035](#d035) argues against for larger reasons.
- **`db.batch(sql, rows)` binds a row struct's fields to placeholders in declaration order.** It is the minimal rule that makes `db.batch` checkable without tuples, which the language does not have.

*Consequence:* two SQLite prepare failures get codes of their own, because both admit a suggestion: "no such table", listing the tables that exist, and "no such column", with the column's span, the nearest name by edit distance as a machine-applicable fix, and a related label at the migration that created the table. The second is allocated `DT0142` — the number [D038](#d038), [06-tooling.md](06-tooling.md#diagnostics), and [09-engineering.md](09-engineering.md#2-spec-tests--testsspec) have used as their running example since the first commit, down to the message and the suggestion. Allocating it to the diagnostic it was always illustrating turns three documents' example into a specification at no cost.

*Consequence:* the schema checker requires `SQLITE_ENABLE_COLUMN_METADATA`, which is a compile-time define rather than a source edit and therefore satisfies [D052](#d052) without qualification.

### D099
**Route matching precedence is literal, then parameter, then wildcard, per segment — which makes shadowing unrepresentable, so rule 7 is duplication only.** · locked

[03-grammar.md](03-grammar.md#well-formedness-rules) rule 7 forbids patterns that "conflict or shadow each other", and the second half turns out not to be a check.

At each segment position, a literal beats a parameter and a parameter beats the wildcard. That is already the behaviour of the compile-time trie [05-runtime.md](05-runtime.md#request-lifecycle) specifies; stating it as a language rule means `/users/new` and `/users/:id` are both reachable, and so are `/files/:name` and `/files/*rest`. **No program exists in which one route hides another** — shadowing is unrepresentable, in the same way [D008](#d008) makes data races unrepresentable rather than merely unlikely.

What remains is genuine ambiguity, and it has one form: two routes with the same method whose patterns are identical up to parameter renaming. `GET /users/:id` and `GET /users/:slug` are the same matcher and the trie cannot choose. That is rule 7's diagnostic, reported at the later declaration with a related label at the earlier.

The check is a pairwise comparison over the finished, group-flattened table — quadratic in the number of routes, on a table bounded by the target use case, and exact. Detecting conflicts during trie insertion would be cheaper and would make the answer depend on the order files were walked in, which is not something a diagnostic may depend on.

*Consequence:* a path parameter's type is restricted to `int`, `float`, and `str`. A URL segment is text, a failed conversion is a 404 rather than a fault ([02-syntax.md](02-syntax.md#routes)), and any other type would need a conversion policy that a `str` parameter and one line in the body expresses more clearly.

### D100
**The six semantic diagnostic ranges are allocated in full and in advance; a range is a subject rather than a stage; and four reserved rule numbers move to the start of their sub-range.** · locked

This is [D064](#d064) applied to `DT0100`–`DT0699`. Codes are permanent once assigned ([D050](#d050)), so numbering is a one-way decision worth making deliberately, and allocating up front means a code is chosen by where it belongs rather than by what was free that week — which matters more here than for the front end, because [D102](#d102) has several workstreams running at once. The full sub-allocation is in [12-semantics.md](12-semantics.md#semantic-diagnostics).

Two conflicts had to be resolved to write it.

**A range is a subject, not a pipeline stage.** [05-runtime.md](05-runtime.md#compiler-pipeline) makes the schema checker its own stage while [06-tooling.md](06-tooling.md#code-ranges) puts its codes in a range labelled "names, modules, resolution, SQL/schema". That is not an inconsistency: the four subjects in that label are one subject, because a scope, a module namespace, and a database schema are all things outside the expression that a name must be resolved against, and a misspelled column is the same kind of mistake as a misspelled module member. The stage boundary answers *who checks this* and the range boundary answers *what is this about*, and they are allowed to disagree — the clean case being the "cannot be bound as a SQL parameter" code, which the typechecker reports because only it knows the argument's type, and which lives in the SQL block because it is about SQL.

**A reserved rule code sits at the start of its sub-range.** [03-grammar.md](03-grammar.md#well-formedness-rules) reserved `DT0100` for rule 13 and `DT0101` for rule 14, taking the first two numbers of a hundred-code range and leaving the naming rules [D069](#d069) assigns to that range with no clean start; `DT0402` and `DT0403` did the same one range up, dropping two exhaustiveness-and-divergence codes into an otherwise contiguous block about `!` and `else`. So four numbers move: rule 13 to `DT0140`, rule 14 to `DT0130`, rule 8 to `DT0420`, rule 16 to `DT0440`. Rules 2, 3, 4, 5, 6, 7, and 15 keep theirs, each already sitting at or immediately after its sub-range's start.

The move is legitimate and cheap for one specific reason: [D065](#d065) separates **reservation**, which lives in the documentation, from **registration**, which lives in `diag_codes.h`, and only a registered code is frozen by [D050](#d050). None of the four is registered, so the cost is one edit to the rule-to-code table in [03-grammar.md](03-grammar.md#well-formedness-rules) and nothing else — and this is the last moment it is available, because the instant any of them is registered it is permanent.

*Rejected:* carving the sub-ranges around the two numbers where they sat, which protects numbers nothing depends on and makes every subsequent choice worse; *also rejected:* giving the schema checker a new top-level range at `DT0700`, which would contradict the "subject, not stage" reading above and would require editing the range table in [06-tooling.md](06-tooling.md#code-ranges) to say something less true than what it already says.

*Consequence:* the range table in [06-tooling.md](06-tooling.md#code-ranges) is unchanged. Both sub-allocations — the front end's and this one — live in the documents that own their stages, which is where [D064](#d064) put the first one.

### D101
**`doot check` takes at most one path, and a command's `--json` output is the diagnostic schema plus at most one top-level key named after the command.** · locked

`doot check [--json] [path]`, where the path is a directory (that project), a file (single-file mode, with `schema.sql` beside it), or absent (the working directory). **Two or more paths is a usage error**, because compilation is whole-program and a list of paths does not describe a program. `doot fmt` accepts many paths because formatting is per-file; checking is not, and quietly checking each path as its own program would be a different command wearing the same name.

Exit codes are the three in [06-tooling.md](06-tooling.md#exit-codes), with one worth stating because it is easy to get wrong: a **warning-only** run exits `1`, since `1` means "reported diagnostics", and the spec runner asserts exactly that split. The human summary is pinned exactly by `expect-output` ([D075](#d075)).

The second half concerns `doot routes`, which needs to emit a route table and cannot do it inside a schema that carries only diagnostics. The rule: a command's `--json` output is the diagnostic schema plus **at most one** top-level key, named after the command. `doot routes` adds `routes`; `doot check` adds nothing.

The spec runner rejects unknown JSON keys by design ([D071](#d071)), so its reader learns `routes` in the same change that adds the command — visibly, in a diff. That is not a cost being absorbed, it is the mechanism [D071](#d071) chose: schema growth is meant to be a deliberate, breaking, reviewed edit rather than something a tolerant reader absorbs silently.

*Rejected:* a separate `doot routes --format=json` or a sidecar file, both of which add surface to avoid a one-key schema extension; *also rejected:* nesting the route table inside `summary`, which would overload a field whose meaning is pinned.

### D102
**Two milestones: the resolver, the route checker, and `doot routes` first; then the typechecker, the schema checker, and `doot check`.** · locked

The order is forced rather than preferred, by two decisions acting together. [D054](#d054) says a command ships only when it fully works. [D065](#d065) and the `docs` gate say a diagnostic code is registered only alongside the spec test that produces it — and a spec test drives a command. So **a stage cannot land before a command that reaches its diagnostics**, and the milestones are the groupings for which such a command exists.

`doot routes` is what makes a first milestone possible, and it is [D067](#d067)'s insight in a second place. It needs the parser, the resolver, and the route checker and nothing else, so it is **complete rather than partial** at that point — exactly as `doot fmt` was complete at the parser milestone. That milestone therefore lands the symbol table, the module table, the type representation, name resolution, naming, mutability, the route table, and rules 2, 5, 6, 7, and 14, with spec tests in `routes` mode.

The second milestone lands the typechecker, the schema checker, `doot check`, and rules 3, 4, 8, 13, and 16. It is the largest single landing in the project — roughly eighty codes and their tests, against the first milestone's fifty — and the size is a consequence rather than a choice: `doot check` is the only command that reaches the typechecker, and it cannot ship without the schema checker, because a `doot check` that silently ignored a SQL literal is the half-working command [D054](#d054) exists to prevent, on the one promise ([D033](#d033)) that is hardest to make credible. What keeps it reviewable is five complete internal steps as five commits on one branch, with the command added last.

Three workstreams run concurrently inside it: the **schema checker**, which needs only the first milestone's symbol table and declared types and is independent of the typechecker except for one code the typechecker reports anyway; **markup value typing**, which needs only the type representation; and the **module signature table**, which is data rather than logic, is 26 modules for v0.1, and is on the critical path for everything because no checker can be tested against a program calling a module the table does not describe.

The chokepoints that serialize work regardless of how the stages are divided are named in [12-semantics.md](12-semantics.md#the-shared-chokepoints): `LAYERS` in the Makefile, the `units` list in `tools/amalgamate.sh`, `src/base/diag_codes.h`, the range table in [06-tooling.md](06-tooling.md#code-ranges), the dispatch chain in `src/cli/main.c`, and the suite array in `tests/unit/main.c`. The sub-allocation in [D100](#d100) is what makes the third of those conflict-free in content; the rest are one or two edits each, placed deliberately at the start of a milestone.

*Consequence:* the vendored SQLite enters the build in the second milestone, which is where `tools/vendor.sh` installs its tree for the first time and the `unity` gate's command grows a second translation unit: `cc -O2 -o doot build/doot.c vendor/sqlite/sqlite3.c`. That does not weaken [D045](#d045) — the amalgamation is one file of *doot*, and SQLite's amalgamation is already one file of SQLite — and it cannot be folded in, because vendored code keeps its own warning flags ([D052](#d052)).

*Consequence:* rule 15 is the one pending well-formedness rule with no spec tests in v0.1. `spawn` is `DT0046` until v0.2, and [D081](#d081)'s barrier means the resolver never runs on a program containing one, so its code is unreachable and therefore correctly unregistered until tasks land. A dated gap, recorded, rather than a forgotten one.


---

## Standard library API

The API surface of the 26 modules that land in v0.1, argued here and specified in [13-stdlib-api.md](13-stdlib-api.md). These are the decisions that had to be settled before any standard-library code could be written — and before the compiler's module signature table could be filled in, which [D102](#d102) puts on the critical path for every checker.

### D130
**[13-stdlib-api.md](13-stdlib-api.md) is the normative standard-library reference; [04-stdlib.md](04-stdlib.md) remains the overview.** · locked

The overview decides *which* modules exist, what each is for, and which release each lands in, and it argues the two that carry structural weight. It does not and should not carry a signature per member: the v0.1 surface is several hundred members, and a table of that size inside a document whose job is the shape of the library would bury the shape.

Two consumers force the split. [D085](#d085) makes the standard library a compile-time signature table with a column per property — parameters, marks, type-argument slots, mutating, version — and that table cannot be generated from prose with parameter names and no types. And [D102](#d102) makes filling the table a workstream of its own, on the critical path for the resolver, the typechecker, and the schema checker alike, because no checker can be tested against a program that calls a module the table does not describe. A workstream needs one input document.

*Rejected:* expanding [04-stdlib.md](04-stdlib.md) in place, which would make one document both the argument for a closed library and its reference manual, and would make every future signature change a diff against the prose that justifies the module's existence; *also rejected:* one document per module, which multiplies the anchor surface the `docs` gate has to check and puts the cross-cutting decisions — error origination, the fields-versus-methods rule, the blocking set — in no document at all.

*Consequence:* the overview's abbreviated signatures are illustrations and are marked as such. Where the two documents disagree, the reference wins, and every disagreement found while writing it is listed in [13-stdlib-api.md](13-stdlib-api.md#corrections-to-the-overview) rather than silently corrected.

### D131
**An `Error` is constructed with a struct literal, and `Error` is the only prelude type that may be.** · locked

```do
return Error { kind: .validation, message: "a name is required" }
return Error { kind: .internal, message: "loading failed", cause: err }
```

This is the entry point [12-semantics.md](12-semantics.md#originating-an-error) left owed. The checker-side rule is already settled — in a fallible function, `return e` accepts either the declared return type or `Error` — and what was missing was how the value comes into existence.

**The answer was already determined; it had not been written down.** [D014](#d014) says `Error` carries a `kind`, a `message`, and a `cause` chain, which is a struct with three fields. A struct literal is how every struct value in doot is built. And the alternative spelling a reader would reach for first, `Error.new(…)`, is not available, because [D092](#d092) removed the static method form and made a member access on a type name resolve to an enum variant and to nothing else. So a literal is not merely the most convenient construction form, it is the only one the language has.

Three details follow rather than being chosen. **`cause` is a field with a default of `nil`**, so wrapping is one field and a chain is ordinary optional-struct recursion, which [D087](#d087) explicitly permits. **The source location is captured from the span of the literal**, so it is not a field, there is nothing to write and nothing to forget, and the compiler fills it at the same point it decides the literal's type — the same move [D028](#d028) makes when it injects a CSRF token into a `<form>` it can see. **A literal naming `location` is `DT0222`**, an ordinary "this type has no such field".

`Error` is the only literal-constructible prelude type. `Request`, `Response`, `redirect`, and `Upload` are handles on runtime state that a literal could put into an inconsistent shape, so each is produced by the members that produce it and refined by the methods it carries.

*Rejected:* an `error` module with `error.new(kind, message)`. The module set is closed at thirty-eight ([04-stdlib.md](04-stdlib.md)), [D085](#d085) states that closure as a property the checker relies on, and a thirty-ninth module for one constructor would spend the closure on the smallest possible thing. *Also rejected:* an unqualified prelude function, `fail(kind, message)`. The prelude is types plus `redirect`; adding the language's only unqualified callable, shadowable by any local, to save eight characters over a literal is a poor trade against [D030](#d030)'s "everything else is fully qualified". *Also rejected:* a `wrap(err, kind, message)` member, which is a function whose entire body sets one field.

*Consequence:* `DT0409` — an `Error` returned from a function that is not fallible — becomes reachable from user code in v0.1, which it would not have been if construction had waited.

*Consequence:* a struct literal of a prelude type other than `Error` needs a diagnostic of its own, reserved as [`DT0233`] ([D150](#d150)).

### D132
**`ErrorKind` is a tag-only enum of thirteen variants, closed and complete at v0.1, including the variants only deferred modules produce.** · locked

`validation`, `not_found`, `conflict`, `permission`, `timeout`, `unavailable`, `parse`, `range`, `io`, `unsupported`, `lagged`, `closed`, `internal`.

The set has to be complete now, and that is forced rather than tidy. [D085](#d085) makes `ErrorKind`'s variant set closed and known at compile time, which is what makes `match err.kind { … }` with no `else` arm decidably exhaustive (`DT0420`). **So adding a variant in a later release would turn a compiling program into a non-compiling one** — a breaking change, which [07-roadmap.md](07-roadmap.md#the-release-model) forbids at every version. A closed enum that the standard library grows into is not additive.

Three variants therefore exist for modules that do not land in v0.1: `lagged` for `topic`'s bounded-buffer overflow ([D026](#d026)), `closed` for `chan`, and `unsupported` for the platform and format refusals that `os` and `image` produce. This is the one part of the standard library that could not be specified module by module.

The set is thirteen because each variant has to earn a `match` arm someone would actually write. `not_found` and `conflict` are in [D014](#d014)'s own example. `validation` is in [02-syntax.md](02-syntax.md#tests). `parse` and `range` are distinct because "this text is not a number" and "this number does not fit" have different fixes. `permission`, `timeout`, `unavailable`, and `io` are the four ways an operating system or a network refuses, and collapsing them would make a retry policy unwritable. `internal` is the catch-all that lets every other variant stay specific.

*Rejected:* a `kind` that is a `str`, which needs no closed set and gives up exhaustiveness, the whole benefit [D014](#d014)'s single error type buys; *also rejected:* a per-module kind set, which is [D014](#d014)'s rejected `Result[T, E]` fragmentation wearing an enum; *also rejected:* fewer variants with detail pushed into `message`, which makes handling a string comparison.

*Consequence:* `errors` in a module's failure table are stated per member in [13-stdlib-api.md](13-stdlib-api.md), because a variant with no producer is as much a defect as a producer with no variant.

### D133
**Six `db` members take a variadic tail. No other standard-library member does, and no user function can.** · locked

`db.one`, `db.find`, `db.all`, `db.count`, `db.exec`, and — with a list rather than a tail — `db.batch` are the closed set. [03-grammar.md](03-grammar.md#declarations) has no variadic `param` form, so this is a property of the module signature table and of nothing else.

The reason it is acceptable in exactly this place is that **the looseness is removed before the program runs.** A `db` call's SQL argument is a literal prepared at compile time ([D098](#d098)), so the placeholder count is known and an arity mismatch is `DT0144`, and every argument's type is checked for bindability as `DT0160`. A variadic tail elsewhere would be genuinely unchecked arity.

The alternative was a list — `db.one[User](sql, [id, name])` — and it fails on types before it fails on taste: a list is homogeneous, so a query binding an `int` and a `str` would need `[any]` and a cast per argument, which is a cast on the most common call in a doot program and would defeat `DT0160` by erasing the types it checks.

*Rejected:* a general variadic parameter form in the grammar, which is frozen ([D042](#d042)) and which would put unchecked arity into user code for the sake of six members; *also rejected:* fixed-arity overloads, `db.one1`, `db.one2`, which is what a language without variadics looks like when it pretends otherwise.

*Consequence:* `path.join` takes `[str]` and `str.join` takes `[str]`, because the variadic set is closed and they are not in it. Both are usually called with a list anyway.

### D134
**A member that fails and yields nothing is written `-> ()!`, a notation of the module table rather than a type. The combination is not spellable in a doot declaration, and that gap is recorded rather than closed.** · locked

`db.tx`, `fs.write`, and `Upload.save_to` all fail and have nothing to return. [03-grammar.md](03-grammar.md#declarations) says `return_type := "->" type fallible?`, the type is not optional, and there is no unit type — [D087](#d087) makes `TY_NONE` the *absence* of a return and explicitly not a value. So `fn f() -> ()!` does not parse and neither does `fn f() !`.

The type system nevertheless has the combination and reaches it three ways: a lambda with no declared return type infers both its type and its fallibility ([D093](#d093)), and a `test` body and a `stream` body are both checked as fallible functions returning nothing ([12-semantics.md](12-semantics.md#with-lambdas-and-defer)). `db.tx(fn() { db.exec(…)! })!` — the transaction in [02-syntax.md](02-syntax.md#data-access) — is exactly that combination on both sides of the call. So the notation names something the checker already represents; what is missing is a written spelling in a declaration.

**The gap is left open deliberately.** Closing it means editing frozen grammar, and the cost of the gap is small and bounded: a user function that fails and has nothing to return either returns something it has — the id it inserted, the count it wrote — or is restructured. It is recorded here so that it is a known limitation with a stated workaround rather than a discovery during implementation, and so that if v1.0 ever revisits the grammar it is on the list.

*Rejected:* adding a unit type `()`, which puts a value into the language whose only purpose is to be returned and which [D087](#d087) argued against on its own terms; *also rejected:* making these members return `bool` or `int` so that the notation is unnecessary, which is a lie in the signature to satisfy a notation.

*Consequence:* `-> ()!` appears in the module table and in [13-stdlib-api.md](13-stdlib-api.md), and never in a `.do` file.

### D135
**A type argument is written when it appears only in the result, and inferred from one designated argument otherwise.** · locked

`db.all[User](sql, …)` and `json.decode[Config](text)` must be written, because nothing in the argument list determines `T`. `json.encode(u)`, `validate.errors(form)`, `list.repeat(0, 4)`, `test.eq(err.kind, .validation)`, and `xs.map(fn(u: User) => u.name)` are written without one, because in each case exactly one argument determines it, and the table records which.

This is what makes the documented calls work as documented. [02-syntax.md](02-syntax.md#functions) writes `users.map(fn(u: User) => u.name)` with no type argument, and it checks because a lambda with declared parameter types and an undeclared return type is inferrable on its own ([D093](#d093)) — so `U` is read off the lambda's inferred return type rather than solved for. **No unification is introduced:** the designated argument is inferred first, the slot is bound from its type, and every remaining argument is then checked against a signature with no unknowns. That is one extra step in the call check and no new machinery, which matters because [D090](#d090) chose bidirectional checking specifically to avoid an inference engine.

*Rejected:* requiring every type argument to be written, which would spell `users.map[str](…)` and contradict the documented example; *also rejected:* general inference over all arguments, which needs a solver and would let two arguments disagree about a slot in a way whose diagnostic names neither.

*Consequence:* every type-argument slot in [13-stdlib-api.md](13-stdlib-api.md) states whether it is written or inferred and, if inferred, from which parameter. A slot whose designated argument does not satisfy the slot's constraint is [`DT0216`] ([D150](#d150)).

### D136
**A builtin member is a row in the module table, not a declaration, so rule 9 and [D092](#d092) do not reach it. A builtin field is a stored value; everything else is a method.** · locked

[04-stdlib.md](04-stdlib.md#overview) says "`str` — string statics; methods live on `str` values", and the same split holds for `[T]`, `{K: V}`, `bytes`, `time.Time`, `time.Duration`, and every stdlib and prelude type with a receiver. That looks like it collides with two locked rules: [rule 9](03-grammar.md#well-formedness-rules) requires `self` as a method's first parameter, and [D092](#d092) says there is no static method form.

There is no collision, because **both rules are about declarations in a `.do` file.** A builtin member has no declaration: the compiler resolves the member access against the receiver's type by consulting the table, and the emitter compiles the call to an opcode or a native call rather than to a doot function entry. Rule 9 governs what a user may write, and the table governs what already exists. The other direction of [D092](#d092) is what makes this safe rather than a loophole: **user code may not attach a method to a stdlib type** (`DT0107`), so the method set on `str` is closed, complete, and knowable from one document — which is [D016](#d016)'s "where is this method defined" answered without opening the project.

The field-versus-method rule needs stating because [02-syntax.md](02-syntax.md#strings) writes `s.len` without parentheses and `s.upper()` with them, and a reader needs to know which way a new member goes. A **field** is a value the receiver stores and can return with no work: `s.len`, `s.char_count`, `xs.len`, `m.len`, `b.len`, `Error`'s three, and the handful on `url.Url`, `fs.Info`, `os.Output`, `Upload`, `Response`, and `redirect`. `char_count` is a field despite costing a walk to compute, because a `str` caches it — it is stored, not computed. Everything else takes parentheses.

*Rejected:* declaring the builtin methods as doot source in a bundled prelude file, which would need `self` and would make the standard library shadowable, redeclarable, and visible to the resolver as ordinary user code; *also rejected:* making every builtin member a module function — `str.upper(s)` — which contradicts [04-stdlib.md](04-stdlib.md#overview)'s split and turns `s.trim().lower()` into `str.lower(str.trim(s))`.

*Consequence:* the module table's rows are keyed by module *or* by receiver type, and the workstream that fills it needs both halves. That is stated in [13-stdlib-api.md](13-stdlib-api.md#what-the-module-table-takes-from-this-document) so it is a known shape rather than a mid-implementation discovery.

### D137
**An imperative name mutates the receiver in place and requires a `var` binding; a past participle returns a new value. Twelve members mutate, and they are all on `[T]` and `{K: V}`.** · locked

`xs.sort()` mutates and `xs.sorted()` returns; `xs.reversed()` returns and there is no `xs.reverse()`, because nothing wanted it. The mutating set is `push`, `pop`, `insert`, `remove_at`, `extend`, `clear`, `sort`, and `sort_by` on `[T]`, and `set`, `remove`, `extend`, and `clear` on `{K: V}`.

[D097](#d097) put a mutating column in the module table so that deep `let` is checkable rather than aspirational, and left its values to be decided per member. This is that decision, and the naming convention is what makes it readable at the call site: `let` plus an imperative name is `DT0303`, and the diagnostic can name the non-mutating spelling because the convention guarantees one exists where one is wanted.

Two builtin types have mutating methods because two are containers a handler builds up as it goes, and [D008](#d008)'s argument applies to exactly those: a `var` local is uniquely owned, so `xs.push(x)` mutates in place and value semantics cost no copy. `str`, `bytes`, `html`, and every stdlib and prelude type carry no mutating member at all, so a `let` binding of any of them is unrestricted.

*Rejected:* only the returning forms, which would make building a list in a loop allocate a new list per iteration and would waste the in-place mutation [D008](#d008) went out of its way to make sound; *also rejected:* only the mutating forms, which would force `var` on a binding that is never reassigned and train a reader to ignore the distinction; *also rejected:* a `!` or `_mut` suffix marking mutation, which is a third convention where English already has one.

*Consequence:* `rand.shuffled` has no in-place pair, and that is the one deliberate incompleteness: an in-place shuffle would need a mutating `[T]` method whose only caller is one module, and a shuffle copies anyway.

### D138
**Unit suffixes are fields on `int`. Durations are `ns us ms s min h days weeks`; byte sizes are `kb mb gb`, in powers of 1024.** · locked

`15.s`, `250.ms`, `2.h`, `7.days`, and `16.mb` are in [02-syntax.md](02-syntax.md#configuration) and [02-syntax.md](02-syntax.md#uploads), so the spellings are fixed and what remains is the complete set and the semantics.

They are **fields, not calls**, which [02-syntax.md](02-syntax.md#configuration) gets slightly wrong when it calls them "ordinary method calls on `int`": `15.s` is `INT` `.` `IDENT` under [03-grammar.md](03-grammar.md#identifiers-and-literals), because `FLOAT` requires a digit after the point, and there are no parentheses. Under [D136](#d136)'s rule a field is a stored value, and a duration suffix computes a multiplication — so this is the one exception, and it is the right one: `15.s()` is worse to read, and every documented example is written without parentheses.

**Byte sizes are powers of 1024**, because what they configure is a memory budget ([D005](#d005)) and a budget is counted in binary units; `16.mb` is `16777216`. **`min` rather than `m`** for minutes, because `m` beside `mb` and `ms` reads as a truncation of either. The mixture of short and long names is inherited from the documented examples rather than chosen, and keeping `2.h` was preferred to renaming it for consistency with `7.days`.

*Rejected:* a `time.seconds(15)` family, which the documented examples already rule out; *also rejected:* decimal byte sizes, which would make `16.mb` a number no allocator uses.

### D139
**A map key is `int`, `bool`, `str`, `bytes`, or an enum. Map iteration is insertion order.** · locked

`float` is excluded because `nan != nan` makes a float key unfindable and a rounded float key is a silent bug. A struct, list, or map key is excluded because hashing a container is a decision with no v0.1 consumer, and [D009](#d009)'s structural equality means the question would otherwise have to be answered for every user type. A key type outside the set is [`DT0217`] ([D150](#d150)), reported at the annotation.

**Insertion order** for `keys()`, `values()`, and `for k, v in m`. A hash order that varied between runs would make a rendered page's output depend on the allocator, which makes a spec test's expected bytes unpinnable and makes a built query string uncacheable — and [D072](#d072) already pays a price to keep the compiler's output order deterministic. Insertion order costs a link per entry and buys reproducibility everywhere.

*Rejected:* sorted iteration order, which imposes a comparison on every key type and is not what an author who built a map in a meaningful order wants; *also rejected:* unspecified order, which is what most languages do and which is how a program comes to depend on one accidentally.

### D140
**`log`'s field map is `{str: str}`, and a non-string field is written with interpolation.** · locked

```do
log.info("user created", {"user_id": "${u.id}"})
```

A map is homogeneous, so the alternative was `{str: any}`, and it is worse at exactly the call site that matters most. [D088](#d088) has no implicit widening to `any`, so every numeric field would be written `u.id as any` — a cast on the most common logging call in the language, on the type `any` that [02-syntax.md](02-syntax.md#types) describes as coming only from untyped JSON. `"${u.id}"` needs no cast, is the form [D096](#d096) already types, and produces the text both output formats were going to carry anyway.

The cost is stated rather than hidden: a JSON log line carries `"user_id": "12"` and not `"user_id": 12`. That is what most log pipelines index, and a program that needs a numeric field in a structured sink is writing to `db` or to `metrics`, not to a log.

*Rejected:* `{str: any}`, above; *also rejected:* a variadic key-value tail, which [D133](#d133) closes to `db`; *also rejected:* one member per arity, `log.info1`, `log.info2`, which is that rejection's usual consequence.

*Consequence:* `log` has no configuration entry point at all — no `set_level`, no `configure` — because a level set at runtime is a mutable global whose value differs per worker, which is the failure [D008](#d008) exists to make unrepresentable. The level is the process's configuration ([D040](#d040)) and the format is `env.mode()`.

### D141
**In `math`, the `int` form takes the unsuffixed name and the `float` form takes a `_float` suffix. Every checked integer operation has a `checked_*` and a `wrap_*` companion.** · locked

Every operator in doot is monomorphic ([12-semantics.md](12-semantics.md#operators)) and there is no overloading, so `abs`, `min`, `max`, and `clamp` need two names each. `int` takes the plain one because [D020](#d020) makes money an `int` and therefore makes `int` the common case in a web application. The choice between `abs_float` and `float_abs` is arbitrary; the suffix form was chosen because it sorts the two spellings of one operation together in a reference.

The two integer-only families are the point of the module. **`wrap_add` and friends are what [D003](#d003) promised**: explicit wraparound for hashes and checksums, total, never faulting. **`checked_add` and friends are its complement**, returning `int?` where `+` would fault. Without them, [D003](#d003)'s checked arithmetic leaves a program that genuinely does not know whether a sum fits with a fault as its only outcome; with them, `else` handles it, and the fault stays where it belongs — on arithmetic the author believed could not overflow.

*Rejected:* overloading `math.abs` on argument type, which is one special case in a monomorphic type system and would be the only one; *also rejected:* float-only members with `as` at the call site, which routes integer arithmetic through binary floating point and is exactly what [D088](#d088) refuses to do implicitly and [D020](#d020) refuses to do at all.

*Consequence:* `math.abs` faults on the most negative `int`, because its magnitude is not an `int`. `math.checked_sub(0, n)` is how a program asks without faulting, which is the pattern the `checked_*` family exists for.

### D142
**JSON representability is one constraint, used by `json`, by a route's return type, and by `http.json_body`. Encoding is total and therefore not fallible.** · locked

Representable: `int`, `float`, `bool`, `str`, an enum as its variant name, a struct declared in this program whose every field is representable, `[T]` and `{str: V}` of representable elements, `T?` as `null`, and `any` for encoding only. `bytes` and `html` are not.

One constraint rather than three, because [02-syntax.md](02-syntax.md#return-types) already makes a struct return type a JSON response and `json.encode` already has to answer the same question; three separately-worded rules would drift, and the diagnostic would name a different reason depending on which one noticed.

**`bytes` is excluded** because it has no agreed JSON spelling: choosing base64 silently would make `encode` and `decode` disagree with every consumer that chose hex, and a program that wants base64 writes `encode.base64` into a `str` field where the choice is visible. **`html` is excluded** because a JSON document is not a page, and putting escaped markup into an API response is the confusion the type exists to prevent.

**Encoding cannot fail**, because the type argument is checked at compile time and nothing is left to refuse at runtime; a NaN or infinite `float` encodes as `null`, which is the only representable choice and is stated rather than discovered. A fallible encoder would put an `else` on every JSON response for a failure that cannot happen.

Decoding is fallible in two ways that get different kinds: `parse` when the text is not JSON, `validation` when it is JSON of the wrong shape. **A field the struct does not declare is ignored**, which is deliberately the opposite of `db`'s `DT0146`: a database schema is inside the program and a remote document is not, so an API that adds a field must not break a client.

*Rejected:* base64 `bytes` by convention, above; *also rejected:* a fallible `json.encode`, above; *also rejected:* refusing unknown fields on decode, which makes every upstream addition a breaking change for the doot side.

### D143
**`db` has one entry point per result shape: no `db.scalar`, no `db.last_insert_id`, a fallible `db.tx` body, and a nested `db.tx` is a savepoint.** · locked

Four settlements, each removing a member or a case somebody would otherwise add later.

**No `db.scalar[T]`.** `db.count` is the single-scalar entry point and its column is an integer (`DT0152`). A `str` or `float` scalar is a one-field struct, which names the column at the call site and goes through the same result-shape check as every other query. Two entry points for one shape is what [goal 1](00-vision.md#the-nine-goals) rules out.

**No `db.last_insert_id`.** SQLite's `returning` clause is better in every case, it is already what the documented insert uses ([02-syntax.md](02-syntax.md#a-complete-application)), and a separate id read is a second round trip that can disagree with the first under concurrency.

**`db.tx`'s body must be fallible** — its parameter is `fn() -> ()!`, and a body that cannot fail is `DT0207`. Function types compare structurally including their fallibility, so no widening is available ([D088](#d088)), and this is the right side of that: a transaction whose body cannot fail cannot roll back, so it is a `db.exec` with extra words. Every real body contains a `db` call and therefore a `!`.

**A nested `db.tx` runs as a savepoint.** The inner one rolls back to its own savepoint and the outer transaction survives to decide what to do. The alternative — a fault on nesting — makes a function containing `db.tx` uncallable from another function containing `db.tx`, which is a whole-program property no local reading can establish and exactly the kind of hazard that only shows up in production.

*Rejected:* each of the above's alternative in place; *also rejected:* a `db.tx` that takes an isolation level, which SQLite in WAL mode with one writer does not have a second choice for ([D032](#d032)).

### D144
**A handler that controls its status is declared `-> Response!`. `Request`, `Response`, `redirect`, and `Upload` are opaque, and a `Response` is refined by `with_*` methods that each return a new one.** · locked

`html` is not `Response` and there is no widening ([D088](#d088)), so `route … -> html!` cannot `return http.not_found()`. That is a real constraint on how a handler is written, and stating it is better than the two ways of hiding it. A handler that always answers 200 keeps the simple return type; a handler that can answer 404 is declared `-> Response!` and wraps its page in `http.html(…)`.

The four types are opaque because each is a handle on runtime state that a literal could put into an inconsistent shape — a `Response` with a body and a 204, an `Upload` with a size that does not match its bytes. [D131](#d131) makes `Error` the single exception, for the single reason that `Error` is a data structure with three fields and nothing else.

`with_status`, `with_header`, `with_content_type`, and `with_cookie` each return a **new** `Response`, so a `let` binding of one is unrestricted and none of them appears in [D137](#d137)'s mutating set. That also makes them chainable, which is what a handler wants: `http.html(page).with_status(201).with_cookie(c)`.

*Rejected:* implicit widening from `html` to `Response`, which is a row in [D088](#d088)'s closed table and would be the first implicit conversion in the language; *also rejected:* a mutable response object that a handler configures, which is a mutable binding threaded through every call and is what [D008](#d008) removed; *also rejected:* a `Response` struct literal, which would let a program build a response the server cannot send.

*Consequence:* the response-side JSON builder is `http.json_body[T]` and not `http.json`, because [04-stdlib.md](04-stdlib.md#the-two-load-bearing-modules) gives `http.json[T](url)` to the v0.3 client and one module cannot have two members of one name. Given the collision, naming the new member around the documented one is what leaves the documentation alone.

### D145
**`html.attrs` is not a member. The `...expr` spread is that feature.** · locked

[04-stdlib.md](04-stdlib.md#html) listed `html.attrs({str: str}) -> html`, and there is no position in the grammar that accepts an `html` value as an attribute. [03-grammar.md](03-grammar.md#markup) spells the feature `attr := … | "..." expr`, [D096](#d096) types it as `{str: str}` with `DT0264`, and the value it takes is the map — not an `html` wrapper around the map.

So the member was a second spelling of an existing feature, and the second spelling was the unusable one. Removing it is not a reduction in capability: `<input name="email" ...extra/>` does everything `html.attrs` was listed for.

*Rejected:* adding an attribute position that accepts `html`, which would need a value of type `html` to be spliced into a tag without escaping — the one place [D021](#d021)'s soundness argument has no story, since an `html` value was escaped for text and not for an attribute ([D096](#d096) refuses it explicitly).

*Consequence:* found by reading every code block in [04-stdlib.md](04-stdlib.md) against the frozen grammar, which is the audit [D154](#d154) makes standing practice.

### D146
**`validate.check` takes the value alone. There is no rule value.** · locked

[04-stdlib.md](04-stdlib.md#validate) listed `validate.check(value, rules) -> ()!`, and `rules` has no expressible type. A list of heterogeneous rules needs either payload-carrying enum variants, which [D018](#d018) defers past v1.0, or an interface, which [D016](#d016) rules out permanently. A `{str: str}` of rule names and arguments would be a string-typed DSL inside a statically typed language, checked at runtime, which is the opposite of what [D043](#d043) bought by making attributes a closed set.

So validation has exactly two shapes and both are typeable: **declaratively**, `@` attributes on a struct's fields, run automatically during request binding ([D025](#d025)); and **imperatively**, `bool` predicates composed with `and`. `validate.check[T](value: T) -> ()!` and `validate.errors[T](value: T) -> {str: str}` are the same run of a type's attributes, one failing and one reporting, and the pair exists because both shapes are wanted — an error to propagate, and a map to re-render a form with.

*Rejected:* a rule value, above; *also rejected:* only `errors`, which makes propagating a validation failure a hand-written `if` at every call; *also rejected:* only `check`, which loses the field-to-message map that [04-stdlib.md](04-stdlib.md#validate) argues is the whole point.

### D147
**Receiving a signal is the runtime's, not the program's. `os` exposes the `Signal` enum and `send_signal`, and nothing else.** · locked

`interrupt` and `terminate` begin a graceful shutdown, `hangup` reloads, and none of it is interceptable from doot.

A handler-registration entry point — `os.on_signal(sig, handler)` — is a runtime registration of a callback, which is the shape [D024](#d024) rejected for routes, and it is worse here: it stores a mutable per-worker binding ([D008](#d008)), and it lets a program defeat the graceful shutdown that a deploy and a hot reload both depend on ([05-runtime.md](05-runtime.md#hot-reload)). A signal is also process-wide while doot's unit of everything is a task, so there is no task for a handler to belong to.

What a program legitimately needs on the shutdown path is cleanup, and `defer` already runs there ([12-semantics.md](12-semantics.md#with-lambdas-and-defer)). What it legitimately needs outbound is the ability to signal a process it started, which is `os.send_signal` beside `os.run`.

*Rejected:* signal handler registration, above; *also rejected:* a third well-known hook, `fn on_shutdown()`, which would add language surface — a name the resolver must know and the route checker must typecheck — for a callback that `defer` already covers.

*Consequence:* [04-stdlib.md](04-stdlib.md#overview)'s "signals" for `os` means the enum and the outbound half, which is stated in [13-stdlib-api.md](13-stdlib-api.md#os) so the table's row is not read as promising a handler.

### D148
**`rand` has no seed. Its generator state is the worker's and is not addressable from doot.** · locked

A seed setter is a mutable global in everything but name: a value one call writes and every later call reads, per worker, so a program's behaviour would differ between one worker and sixteen — the exact construct [D008](#d008) makes impossible rather than documenting. The generator is seeded from the CSPRNG at worker start.

The cost is that a test cannot make `rand` deterministic, and the answer is that a function whose result must be reproducible takes the random value as a parameter. That is a testability property rather than a limitation: it moves the nondeterminism to the caller, where a test supplies it and a handler calls `rand`.

`crypto.random_int` is a separate member from `rand.int` for the same reason `crypto` is a separate module: one is for tokens and one is for jitter, and a program reaching for the wrong one should have to say so. `crypto.random_int` is rejection-sampled rather than reduced modulo, because modulo bias in a token generator is invisible and permanent.

*Rejected:* `rand.seed(n)`, above; *also rejected:* an explicit generator value threaded through calls, which is correct, is what a pure language would do, and costs a parameter on every function in the call chain for a feature whose only consumer is a test.

### D149
**The blocking set is enumerated per member, and every member not in it is non-blocking as a commitment.** · locked

`db` entirely; `fs` except `temp_dir`; `form` entirely; both `static` members; `os.run`; `test.fixture` and `test.fixture_text`; `Request.body` and `body_text`; `Upload.save_to`, `bytes`, and `text`; and `crypto`'s two password members in v0.4. Each is marked at its signature and all of them are collected in one table.

[D006](#d006) says blocking work is offloaded so it never stalls the event loop, and that guarantee is only usable if a reader can tell which calls are which. Marking it per member — rather than per module, which would be wrong for `os` and `test` — makes the answer local to the call.

The complement is the part that is a commitment rather than an observation: `time.now()`, `uuid.new()`, `crypto.random_bytes`, `log.info`, and every `str`, `list`, `map`, `math`, `json`, `encode`, `html`, `url`, `cookie`, `mime`, `path`, and `env` member completes on the loop with no hop. **`log` is the one worth naming**, because a logger that wrote synchronously to a file would make every log line a blocking call: log output goes to a worker buffer that the worker flushes, so `log.info` in a hot handler costs an append.

*Rejected:* marking blocking per module, which is imprecise in both directions and would put `os.cpu_count` and `path.join` behind a pool hop in a reader's mind; *also rejected:* leaving it to the implementation, which is how a synchronous write ends up in a hot path and is not discovered until a benchmark.

### D150
**Three diagnostic numbers are reserved for the standard library's compile-time constraints: [`DT0216`], [`DT0217`], and [`DT0233`]. None is registered.** · locked

| Code | Meaning |
| --- | --- |
| [`DT0216`] | a type does not satisfy the constraint on this entry point |
| [`DT0217`] | this type cannot be a map key |
| [`DT0233`] | this type cannot be constructed with a struct literal |

All three sit in held sub-ranges [D100](#d100) already allocated — `DT0216` and `DT0217` in core type agreement, `DT0233` in named types — so the reservation costs nothing and collides with nothing. They are written in brackets, the form [03-grammar.md](03-grammar.md#well-formedness-rules) uses, and a row enters `src/base/diag_codes.h` only with the code that emits it and the spec test that proves it ([D065](#d065)).

Three is the whole list, which is the interesting part: the module surface of 26 modules implies exactly three compile-time checks that are not already allocated. Argument counts and types are `DT0206` and `DT0207`; a mutating call on a `let` receiver is `DT0303`; an unlanded module or version is `DT0046`; every `db` check is `DT0140`–`DT0160`; an `Error` from a non-fallible function is `DT0409`. That is the dividend of specifying the library against a semantic pass that was allocated in full and in advance.

`DT0216` is deliberately distinct from `DT0215`, which is about the *shape* of a type-argument list, and from `DT0153`, which is `db`'s own requirement that a row type be a struct declared in this program. One code per question, with the message naming the constraint and the type that failed it.

*Rejected:* reusing `DT0200` for all three, which is the general type mismatch and whose message could not name a fix ([D038](#d038)); *also rejected:* registering them now, which would break the `docs` gate immediately ([D065](#d065)).

### D151
**A `test` assertion records a failure against the enclosing test task and returns nothing. Calling one outside a test is a fault.** · locked

[02-syntax.md](02-syntax.md#tests) writes `test.eq(greet("Ada"), "hello Ada")` with no `!` and no `else`, so assertions are neither fallible nor optional-returning, and a test with several assertions reports all of their failures rather than stopping at the first.

Outside a test task there is nothing to record against, and reaching an assertion from a request handler is a bug of exactly the kind [D012](#d012) describes — so it is a fault, and it needs no new diagnostic.

The alternative was a compile-time rule that a `test` member may appear only lexically inside a `test` block, in the shape of [rules 10 and 11](03-grammar.md#well-formedness-rules), and a compile error is normally better than a runtime fault. It is rejected because it forbids a helper function that asserts — a legitimate and common shape — and because "reachable only from a test" is a whole-program analysis for a rule guarding nothing an author would get wrong by accident.

*Rejected:* fallible assertions, `test.eq(…)!`, which would make every assertion propagate and stop a test at its first failure, and which the documented example rules out anyway; *also rejected:* the lexical rule, above.

*Consequence:* `test.expect_error` **faults when its body succeeds**, rather than recording an ordinary failure: a test asserting that something fails has failed to test it, and a fault's diagnostic says more than an assertion's would.

### D152
**Time formatting is a closed set of sixteen `%` directives, English-only, and an unknown directive is a fault.** · locked

`%Y %m %d %H %M %S %L %N %z %Z %a %A %b %B %p %%`, with `%Z` accepted by `format` and not by `parse`.

A closed set rather than a pattern language, for the same reason [D043](#d043) closes the attribute set: the surface is bounded, a reader can hold it, and `doot doc --agent` can print all of it. **An unknown directive faults** because a format string is a literal in every real program, so an unknown directive is a typo in the source rather than a value from a request — and `format` therefore returns `str` rather than `str!`, which is what [04-stdlib.md](04-stdlib.md#time) already shows.

**English only**, and that is a decision rather than an omission: a locale database is a large surface no other part of doot has, and a page that needs a translated month has a translation table of its own. **`%Z` is not accepted by `parse`** because a zone abbreviation is ambiguous — `CST` names three zones — and guessing would silently shift an instant by hours.

Zones beyond `UTC` and `local` resolve against the platform's database and fail `not_found` when absent, which is why `time.zone_exists` is a member: a program that offers a user a list of zones needs to ask before it converts.

*Rejected:* a Go-style reference-time layout (`2006-01-02`), which is memorable to people who already know it and inscrutable otherwise; *also rejected:* a fallible `format`, which puts an `else` on every rendered timestamp for a typo that a single test catches.

### D153
**`cookie.get_signed` returns `str?` and does not distinguish a tampered cookie from a missing one.** · locked

Both are `nil`. An `Error` saying "signature invalid" would be an oracle: it tells an attacker that the name is right and only the signature is wrong, which is information a correct program never needs and an attacker always wants. No correct program branches differently on the two cases, because both mean "there is no trustworthy value here".

The same posture applies to `crypto.decrypt`, which fails `validation` for a wrong key, a truncated message, and a tampered one alike.

*Rejected:* `str?!` with a `validation` failure on a bad signature, above.

*Consequence:* `cookie`'s defaults are the safe values — `path` `/`, `http_only` true, `same_site` `.lax`, `secure` true outside development — and each is overridable by a `with_*` method that says so in the source. A default that cannot be overridden gets worked around invisibly.

### D154
**A fenced ```do block is real doot; a signature listing is not marked `do`.** · locked

*This is a practice rather than a language rule, and it exists because its absence produced a defect that stood in the documentation until the standard library was specified against the grammar.*

[04-stdlib.md](04-stdlib.md#log) wrote `log.info("user created", { user_id: u.id })` inside a ```do block. It parses — `map_lit` is `"{" expr ":" expr … "}"`, so `user_id` is an identifier expression — and it means something other than what it appears to mean: a lookup of a local named `user_id`, which does not exist. The same block continued `log.warn(msg, fields) log.error(msg, fields)`, three calls juxtaposed on one line, which is not a statement at all. Two more blocks in the same document did the same thing.

The rule, in the form that catches those:

> A block marked `do` parses under [03-grammar.md](03-grammar.md), and every construct in it means what it appears to mean under the resolver's and typechecker's rules. A free identifier standing for a value the surrounding prose supplies is permitted; a construct whose *meaning changes* because a name is free is not.

That is what separates `card(u)` in a fragment, which is fine, from `{ user_id: u.id }`, which is not. A listing of signatures is not doot in the first place — a standard-library member has no declaration to quote — so it goes in an unmarked fence, which also stops a reader from believing that `args: ...` and `-> ()!` are things they may write.

*Rejected:* marking every block `do` and relying on review, which is what was in place; *also rejected:* pinning every block to a spec test, which [D080](#d080) already argues is wrong for fragments — inventing the surrounding context needed to make a signature checkable would test something the documentation does not say.

*Consequence:* every code block in [04-stdlib.md](04-stdlib.md) was audited against the frozen grammar in this change, and the seven defects found are listed in [13-stdlib-api.md](13-stdlib-api.md#corrections-to-the-overview).

### D155
**`str.from_money(minor_units, decimals)` formats money, with no locale, no grouping, and no currency symbol.** · locked

`str.from_money(1999, 2)` is `"19.99"` and `str.from_money(-5, 2)` is `"-0.05"`.

[D020](#d020) makes money an `int` in minor units and promises "formatting helpers in `str`", and this is that helper. It takes the number of decimals rather than assuming two, because minor units are not always hundredths.

What it deliberately does not do is the locale-dependent part: a thousands separator, a decimal comma, a currency symbol, and symbol placement are four decisions that vary by locale and by currency independently, and a wrong answer is a wrong price on a page. An application that needs them has a template of its own, and the number is already correct.

*Rejected:* a locale parameter, which needs a locale database ([D152](#d152) rejects one for the same reason); *also rejected:* leaving money formatting to `str.from_float` and division, which reintroduces binary floating point at the last step of a calculation [D020](#d020) exists to keep out of it.

### D156
**No member of the standard library exposes a setter or returns a mutable view.** · locked

The setters that were each individually plausible and are each individually absent: `log.set_level`, `rand.seed`, `env.set`, `db.set_busy_timeout`, and signal-handler registration. Every one is a value that one call writes and every later call reads, per worker, so a program's behaviour would differ between one worker and sixteen — which [D008](#d008) exists to make unrepresentable rather than to document. Everything they would configure is either the process's own configuration ([D040](#d040)) or a required argument at the call that needs it, which is why `static.file` takes a `max_age` rather than reading a module-level default.

The second half is quieter and equally load-bearing. `m.keys()`, `m.values()`, `req.headers()`, and `form.values()` each return a **fresh** value, so mutating the result cannot reach back into what produced it. Without that, `let` would be deeply immutable in the language and shallowly immutable through the library, and both [D004](#d004)'s promote-by-deep-copy and [D008](#d008)'s "no aliasing anywhere" would be unsound in the same stroke — the guarantee would hold for assignment, be checked for mutating methods by [D097](#d097)'s column, and leak through every accessor that handed out a reference.

*Rejected:* a per-worker configuration cell that reads as immutable, which is [D008](#d008)'s "explicitly per-worker `cache` cell" applied to something that is not a cache — a level or a timeout has one correct value for the process, so making it per-worker is wrong rather than merely surprising; *also rejected:* views for performance, which is a real cost paid in a language whose containers are packed ([05-runtime.md](05-runtime.md#containers-are-packed)) and copying a key list is a `memcpy`.

*Consequence:* the mutating column in the module table has exactly twelve `true` rows ([D137](#d137)), and every one of them is a method on `[T]` or `{K: V}`.
