# Roadmap

## The release model

doot goes v0.1 → v1.0, and the version numbers do not mean what they usually mean.

**Stable from day one.** Anything shipped in v0.1 is final. Later versions are **additive**: new modules, new platforms, new capabilities — never revised syntax, never changed semantics, never a renamed stdlib function. The `0.x` prefix communicates *incomplete*, not *unstable*.

This is why the keyword list and the grammar are frozen at v0.1 even for features that land later ([D042](01-decisions.md#d042)). `spawn`, `send`, and `stream` are keywords in v0.1 and parse errors are specific about their availability, so the grammar never churns. Reserved words exist for the same reason: no future addition can be a breaking change.

**Decisions do not get taken back.** A decision is reopened only on new information that invalidates its original argument, and the argument is written down in [01-decisions.md](01-decisions.md) precisely so that "I'd do it differently today" is not sufficient grounds.

The practical consequence for sequencing: **v0.1 must be small enough to finish and correct enough to never revise.** Everything that can be deferred without leaving a hole is deferred. Everything that cannot be retrofitted without changing behavior — the immutability rules, request budgets, CSRF, the error model — ships in v0.1 regardless of cost.

---

## v0.1 — Request in, HTML out

The smallest thing that is a coherent product: a complete language and a real application. The chat example in [02-syntax.md](02-syntax.md) runs, minus its `stream` handler.

**Language, complete and final.** All 31 keywords, the full grammar in [03-grammar.md](03-grammar.md), the typechecker, structs, methods, tag-only enums, `match`, `with`, lambdas, `defer`. The front end — lexer, parser, AST — is specified in [10-frontend.md](10-frontend.md) and lands first, in the order set by [D067](01-decisions.md#d067).

**The error model, complete.** `!` propagation, `else` coalescing, the universal `Error` type, faults with task-level containment ([D012](01-decisions.md#d012)–[D014](01-decisions.md#d014)).

**The immutability model, complete.** No mutable globals, immutable parameters, deep `let` ([D008](01-decisions.md#d008)). Non-negotiable in v0.1: it is what makes the v0.3 multi-worker change semantically invisible.

**VM.** Register bytecode, type-specialized opcodes, untagged slots, frame maps, computed goto. Memory tiers 0 and 1. Per-request budgets ([D005](01-decisions.md#d005)).

**Server.** Single worker, single-threaded event loop on `epoll` plus a blocking pool, HTTP/1.1, TCP and Unix socket listeners, no TLS ever ([D011](01-decisions.md#d011)).

**Web.** `route` declarations, markup literals with `{if}`/`{for}`, typed request binding, file uploads, automatic CSRF, `group` with `@before`/`@after`, `on_error`/`on_not_found`, static file serving.

**Data.** `db` with compile-time checked SQL against the migrated schema ([D033](01-decisions.md#d033)), forward-only migrations.

**Stdlib.** 26 of the 38 modules ([04-stdlib.md](04-stdlib.md)).

**Tooling.** `new run dev check fmt test routes migrate explain doc repl`, `--json` diagnostics with machine-applicable suggestions, `doot doc --agent`, single-file mode.

**Dependencies.** SQLite and mbedTLS, vendored. `cc *.c` builds it.

*Deliberately absent:* `spawn`, `stream`, `topic`, `cache`, `doot.js`, multi-worker, `doot build`, the `http` client, sessions, auth, email.

---

## v0.2 — Realtime

Goal 8, and the point at which doot does something no other single-binary option does comfortably.

- **Tasks, user-facing.** `spawn`, `chan`, `task.sleep`, timeouts.
- **Memory tiers 1½ and 2.** Task-local compaction and the frozen tier ([D004](01-decisions.md#d004)) — required now, because this is the first version with values that outlive a request.
- **`stream` and `send`.** SSE as a declaration form, carrying HTML fragments ([D026](01-decisions.md#d026)).
- **`topic`.** Publish/subscribe with bounded buffers and lag-closes-the-stream backpressure.
- **`cache`.** Per-worker, TTL, `remember`.
- **`doot.js`.** Form submission, fragment swapping, SSE binding ([D027](01-decisions.md#d027)).

The topic bus is built with its cross-worker ring buffer already in place, even though there is still only one worker, so v0.3 turns workers on rather than rewriting fan-out.

---

## v0.3 — Ship it

Everything needed to put an app on a VPS and leave it there.

- **Multi-worker.** N workers, `SO_REUSEPORT`, per-worker heaps ([D007](01-decisions.md#d007)). Semantically invisible thanks to [D008](01-decisions.md#d008) — which is the whole reason this can be a v0.3 change rather than a v0.1 one.
- **`doot build`.** One self-contained executable.
- **The `http` client.** Pooling, retries, timeouts, streaming, outbound TLS ([D031](01-decisions.md#d031)). Unlocks every third-party integration and thereby completes the argument for [D029](01-decisions.md#d029).
- **`jobs` and `cron`.** Durable queue in SQLite, scheduled work.
- **gzip** (`libdeflate`), **`metrics`**, production JSON logging.
- **Graceful reload.** Socket handoff with no dropped connections.
- **`doot bench`.**

---

## v0.4 — Batteries

The things every real app needs and every app gets subtly wrong.

- **`session`.** SQLite-backed, signed cookies, rotation, fixation resistance.
- **`auth`.** argon2id, login/logout, password reset, email verification, rate limiting, `@before(auth.require)` guards, scaffolded by `doot new --auth`.
- **`mail`.** SMTP client with TLS, plus HTML and text bodies from markup.
- **The dev inspector.** Table browsing, ad-hoc queries, route table, log tail, live requests, job queue ([06-tooling.md](06-tooling.md#doot-dev)).
- **`doot lsp`.** Completion, hover, go-to-definition, inline diagnostics, format-on-save.

Auth lands here rather than in v0.1 on purpose: it is the module where a mistake is most expensive, and it benefits from being written against a runtime whose behavior is already settled.

---

## v0.5 — Everywhere

- **macOS** (`kqueue`) and **Windows** (IOCP, `switch` dispatch under MSVC). All three platforms supported before v1.0.
- **`image`.** Decode, resize, re-encode — the file-manipulation half of the upload story.
- **`csv`**, **`jwt`**.
- Documentation, tutorials, and a set of complete reference applications.

---

## v0.6 — The panel

The dashboard, as a separate component with its own binary. Scope and boundary in [08-boundaries.md](08-boundaries.md).

- Host-header routing to app processes over Unix sockets
- TLS termination and ACME certificate management — the *only* place TLS is terminated ([D011](01-decisions.md#d011))
- cgroups-based resource isolation per app
- Git-based deploys, build, health checks, rollback
- Log aggregation and per-app metrics
- Single-user by design: a personal tool for managing several of your own apps, not a hosting platform and not multi-tenant

Deliberately last. It is the most enjoyable part to build and the least load-bearing, and starting it before v0.5 would be the most likely way to leave the language unfinished.

---

## v1.0 — Freeze

- A compatibility promise: source written for 1.0 compiles unchanged on every 1.x
- A full performance pass with published benchmarks against the 2 GB single-box target
- Re-evaluation, against measured need and nothing else, of: **HTTP/2** ([D010](01-decisions.md#d010)), a **JIT** ([D037](01-decisions.md#d037)), **enum payload variants** ([D018](01-decisions.md#d018))
- The maintenance model begins: **one minor release a year, patches and bug fixes in between.** That cadence is the reason for the dependency posture in [D035](01-decisions.md#d035) and for this document's whole approach to deferral.

---

## Sequencing rules

Four rules that exist because they are the ones most likely to be violated under enthusiasm:

1. **Nothing ships before its decision is written down.** Decisions and documentation are one pass; implementation is a separate pass. A design question is never settled inside an implementation diff.
2. **A behavior-changing feature cannot be deferred.** If adding it later would alter the meaning of existing code, it ships in v0.1 — hence budgets, CSRF, and the immutability rules being non-negotiable there.
3. **The abstraction boundary ships before the second implementation.** The event-loop backend is an interface in v0.1 even though only `epoll` exists, and the topic bus is cross-worker-capable in v0.2 even though only one worker exists.
4. **The panel does not start before v0.5.** See above.
