# Standard library

**38 modules, closed set.** All are pre-bound global namespaces — there are no import statements ([D030](01-decisions.md#d030)). Nothing can be added to a project from outside except `.do` source files, so this surface must cover 90–95% of what a web application needs ([D029](01-decisions.md#d029)).

Naming is uniform and enforced by `doot fmt` ([D039](01-decisions.md#d039)): modules `lower`, types `PascalCase`, functions and fields `snake_case`. Fallible functions are marked `!`, optional-returning ones `?`. No abbreviations except the universally understood ones (`id`, `url`, `db`, `json`).

**This document is the overview.** It decides which modules exist, what each is for, and when each lands. The **complete, exact signature of every member of every v0.1 module** — parameter names and types, return types, failure kinds, which calls block, which methods mutate, and how an `Error` is constructed — is [13-stdlib-api.md](13-stdlib-api.md), which is normative and which the compiler's module signature table is built from ([D130](01-decisions.md#d130)). Signatures shown here are abbreviated illustrations; where the two differ, [13-stdlib-api.md](13-stdlib-api.md) wins. Signatures for modules that land after v0.1 are sketches with their parameter types omitted, and each is settled in the pass that lands its module.

---

## Overview

| Module | Purpose | Lands |
| --- | --- | --- |
| **Core data** | | |
| `str` | string statics; methods live on `str` values | v0.1 |
| `list` | list statics; methods live on `[T]` values | v0.1 |
| `map` | map statics; methods live on `{K: V}` values | v0.1 |
| `bytes` | byte sequence construction and conversion | v0.1 |
| `math` | arithmetic, rounding, min/max, wrapping ops | v0.1 |
| `time` | instants, durations, formatting, parsing, zones | v0.1 |
| **Serialization** | | |
| `json` | typed and dynamic encode/decode | v0.1 |
| `encode` | base64, base64url, hex, url-encoding | v0.1 |
| `csv` | read and write delimited data | v0.5 |
| **Web** | | |
| `html` | escaping, `raw`, fragments, document helpers | v0.1 |
| `http` | status/response constructors; outbound client | v0.1 / client v0.3 |
| `url` | parse, build, join, query strings | v0.1 |
| `form` | request body decoding, uploads | v0.1 |
| `cookie` | read and write, signed cookies | v0.1 |
| `mime` | type detection and lookup | v0.1 |
| `static` | static file serving with caching headers | v0.1 |
| `session` | server-side sessions backed by SQLite | v0.4 |
| `auth` | password hashing, login flows, guards | v0.4 |
| **Data** | | |
| `db` | SQLite with compile-time checked SQL | v0.1 |
| `validate` | field and struct validation | v0.1 |
| `cache` | per-worker in-memory cache with TTL | v0.2 |
| **Realtime and background** | | |
| `task` | task introspection, sleep, timeouts | v0.2 |
| `chan` | typed channels between tasks | v0.2 |
| `topic` | publish/subscribe, cross-worker fan-out | v0.2 |
| `jobs` | durable background queue in SQLite | v0.3 |
| `cron` | scheduled work | v0.3 |
| **Crypto** | | |
| `crypto` | hashes, HMAC, AES, CSPRNG, password hashing | v0.1 / argon2 v0.4 |
| `uuid` | v4 and v7 identifiers | v0.1 |
| `jwt` | sign and verify tokens | v0.5 |
| `rand` | non-cryptographic randomness | v0.1 |
| **System** | | |
| `fs` | files and directories | v0.1 |
| `path` | path manipulation | v0.1 |
| `env` | environment variables, build info | v0.1 |
| `os` | signals, subprocess, CPU count, hostname | v0.1 |
| **Observability and dev** | | |
| `log` | structured logging with request correlation | v0.1 |
| `metrics` | counters, gauges, histograms, `/metrics` | v0.3 |
| `test` | assertions and fixtures | v0.1 |
| **Media** | | |
| `image` | decode, resize, re-encode | v0.5 |

---

## The two load-bearing modules

Most of this library is ordinary utility surface. Two modules carry structural weight, and a quality failure in either invalidates a project-level decision.

### `db` — compile-time checked SQL

This is what makes "you will never want a framework" credible, because eliminating the ORM is the hardest part of that claim ([D033](01-decisions.md#d033)).

```
db.one[T](sql: str, args: ...) -> T!        // exactly one row; fails if zero or many
db.find[T](sql: str, args: ...) -> T?!      // zero or one row
db.all[T](sql: str, args: ...) -> [T]!      // any number of rows
db.count(sql: str, args: ...) -> int!       // a single integer scalar
db.exec(sql: str, args: ...) -> int!        // rows affected
db.batch[T](sql: str, rows: [T]) -> int!    // one prepared statement, many bindings
db.tx(body: fn() -> ()!) -> ()!             // transaction; rolls back on error
```

Every SQL string is prepared against the migrated schema at compile time. A misspelled column, a missing table, an arity mismatch between placeholders and arguments, or a result shape that does not match the type argument are all **compile errors with the offending span highlighted**, not runtime surprises.

Result mapping is by column name to field name. A `T?` field accepts `NULL`; a non-optional field receiving `NULL` is a compile error when the schema permits it, so nullability is checked rather than discovered.

Calls run on the worker's blocking pool so they never stall the event loop ([D006](01-decisions.md#d006)). WAL mode, one writer, many readers, `busy_timeout` set by default.

### `http` — the entire third-party integration story

Because there is no package registry, this module is the *only* way to reach Stripe, S3, an OAuth provider, or any other service ([D031](01-decisions.md#d031)). If it is mediocre, [D029](01-decisions.md#d029) fails in practice. It is therefore held to a feature bar, not a utility bar:

```
http.get(url: str) -> Response!
http.post(url: str, body: bytes) -> Response!
http.json[T](url: str) -> T!                 // fetch and decode
```

with connection pooling and keep-alive, per-request and total timeouts, retry with exponential backoff and jitter, streaming request and response bodies, redirect policy, outbound TLS with certificate verification, and gzip transparently. Combined with `crypto.hmac_sha256` and `encode.hex`, this is enough to implement AWS SigV4, Stripe webhook verification, and OAuth 2 in under a hundred lines of doot each — which is the concrete form of the finite-surface argument.

The response-construction half of `http` (`http.see_other`, `http.not_found`, `http.error`, `Response`) ships in v0.1 and is specified in full in [13-stdlib-api.md](13-stdlib-api.md#http); the client lands in v0.3. A fourth client entry point taking a whole request value is wanted and is not listed above, because the value it takes is not the prelude's `Request` — that type is the inbound request and nothing constructs one — and deciding the shape of an outbound request belongs with the retry and streaming semantics that shape it, in v0.3.

---

## Notes on selected modules

### `html`

```
html.raw(text: str) -> html            // the only escape hatch; audit every use
html.fragment(parts: [html]) -> html
html.empty() -> html
html.doc(head: html, body: html) -> html
```

Everything else about HTML is syntax, not library ([D022](01-decisions.md#d022)). `html.raw` is deliberately the single unsafe entry point in the standard library, so `grep raw(` is a complete XSS audit.

There is no `html.attrs`, and there was no way for there to be one: no position in the grammar accepts an `html` value as an attribute, and the feature it was reaching for is the `...expr` spread over a `{str: str}` that [03-grammar.md](03-grammar.md#markup) already has ([D145](01-decisions.md#d145)).

### `time`

`time.Time` is a nanosecond instant; `time.Duration` is a signed nanosecond span. Duration literals come from `int` methods: `15.s`, `250.ms`, `2.h`, `7.days`. Storage in SQLite is as an integer, so ordering and range queries work without a format convention.

```
time.now() -> time.Time
time.from_unix(seconds: int) -> time.Time
time.parse(format: str, text: str) -> time.Time!

t.format(format: str) -> str
t.add(d: time.Duration) -> time.Time
t.diff(other: time.Time) -> time.Duration
t.before(other: time.Time) -> bool
t.in_zone(name: str) -> time.Time!
```

The full member set of `time`, `time.Time`, and `time.Duration`, and the closed set of `%` format directives, are in [13-stdlib-api.md](13-stdlib-api.md#time).

### `validate`

Struct attributes ([D043](01-decisions.md#d043)) cover declarative cases and run automatically during request binding ([D025](01-decisions.md#d025)). This module is for the imperative remainder:

```
validate.check[T](value: T) -> ()!
validate.errors[T](value: T) -> {str: str}   // field → message, for re-rendering a form
validate.email(text: str) -> bool
validate.url(text: str) -> bool
validate.in_range(n: int, low: int, high: int) -> bool
```

`validate.errors` returning a field-to-message map is deliberate: re-rendering a form with inline errors is the single most common web interaction, and it should require no plumbing.

`validate.check` takes the value alone. A second `rules` parameter has no expressible type — there is no rule value in doot to put in a list, since a payload-carrying enum ([D018](01-decisions.md#d018)) and an interface ([D016](01-decisions.md#d016)) are both permanently absent — so the declarative rules are `@` attributes and the imperative ones are the predicates above, composed with `and` ([D146](01-decisions.md#d146)).

### `topic`

```
topic.publish(name, value)      -> ()
topic.subscribe[T](name)        -> Subscription[T]   // iterable; blocks
topic.subscriber_count(name)    -> int
```

Published values are deep-copied into the frozen tier ([D004](01-decisions.md#d004)) so they can cross worker boundaries safely ([D007](01-decisions.md#d007)). A subscription has a bounded buffer (default 64) and closes with a `lagged` error on overflow, at which point the browser's own SSE reconnection re-establishes it ([D026](01-decisions.md#d026)).

### `cache`

Per-worker and in-memory, which is stated plainly rather than hidden: with N workers there are N caches. That is correct for a cache and wrong for a counter, and the language prevents the counter mistake by forbidding mutable globals ([D008](01-decisions.md#d008)). Anything requiring a single source of truth belongs in `db`.

```
cache.get[T](key)              -> T?
cache.set(key, value, ttl)     -> ()
cache.remember[T](key, ttl, fn() -> T!) -> T!
```

### `jobs`

Durable because it is backed by SQLite, which means a queue with no additional process to run ([D032](01-decisions.md#d032)):

```
jobs.enqueue(name, payload)              -> ()!
jobs.enqueue_at(name, payload, when)     -> ()!
jobs.handler(name, fn(payload) -> ()!)   -> ()
```

At-least-once delivery, exponential backoff, a dead-letter table, and visibility into all of it from the dev inspector.

### `log`

Structured by default, with automatic request correlation — every line emitted during a request carries its request id, route, and elapsed time without being asked to:

```
log.debug(message: str, fields: {str: str} = {})
log.info(message: str, fields: {str: str} = {})
log.warn(message: str, fields: {str: str} = {})
log.error(message: str, fields: {str: str} = {})
```

```do
log.info("user created", {"user_id": "${u.id}"})
```

A map is homogeneous, so a field map is `{str: str}` and a non-string field is written with interpolation ([D140](01-decisions.md#d140)). A map key is an expression, so it is a quoted string and not a bare name.

Human-readable in development, JSON in production, switched by the runtime rather than configured. There is no `log.set_level`: a level set at runtime is a mutable global, which [D008](01-decisions.md#d008) makes impossible.

---

## Explicitly not in the standard library

Each of these is a deliberate exclusion, not a gap awaiting a contribution:

- **A regex engine** — [D036](01-decisions.md#d036)
- **An ORM or query builder** — `db` with checked SQL replaces both
- **A template engine** — markup literals replace it
- **WebSockets** — SSE only, per goal 8
- **A second database driver** — [D032](01-decisions.md#d032)
- **GraphQL, gRPC, protobuf** — return HTML, or JSON where a client needs it
- **A dependency injection container** — no interfaces, no registry, nothing to inject
- **A migration DSL** — migrations are `.sql` files ([D034](01-decisions.md#d034))
- **A CSS or JavaScript pipeline** — `static` serves files; there is no asset build step
- **A PDF, spreadsheet, or archive library** — genuinely outside the "request in, HTML out" thesis
