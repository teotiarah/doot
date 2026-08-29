# Standard library

**38 modules, closed set.** All are pre-bound global namespaces — there are no import statements ([D030](01-decisions.md#d030)). Nothing can be added to a project from outside except `.do` source files, so this surface must cover 90–95% of what a web application needs ([D029](01-decisions.md#d029)).

Naming is uniform and enforced by `doot fmt` ([D039](01-decisions.md#d039)): modules `lower`, types `PascalCase`, functions and fields `snake_case`. Fallible functions are marked `!`, optional-returning ones `?`. No abbreviations except the universally understood ones (`id`, `url`, `db`, `json`).

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

```do
db.one[T](sql, args…)   -> T!        // exactly one row; error if zero or many
db.find[T](sql, args…)  -> T?!       // zero or one row
db.all[T](sql, args…)   -> [T]!      // any number of rows
db.count(sql, args…)    -> int!      // single scalar
db.exec(sql, args…)     -> int!      // rows affected
db.tx(fn())             -> ()!       // transaction; rolls back on error
db.batch(sql, [args])   -> ()!       // one prepared statement, many bindings
```

Every SQL string is prepared against the migrated schema at compile time. A misspelled column, a missing table, an arity mismatch between placeholders and arguments, or a result shape that does not match the type argument are all **compile errors with the offending span highlighted**, not runtime surprises.

Result mapping is by column name to field name. A `T?` field accepts `NULL`; a non-optional field receiving `NULL` is a compile error when the schema permits it, so nullability is checked rather than discovered.

Calls run on the worker's blocking pool so they never stall the event loop ([D006](01-decisions.md#d006)). WAL mode, one writer, many readers, `busy_timeout` set by default.

### `http` — the entire third-party integration story

Because there is no package registry, this module is the *only* way to reach Stripe, S3, an OAuth provider, or any other service ([D031](01-decisions.md#d031)). If it is mediocre, [D029](01-decisions.md#d029) fails in practice. It is therefore held to a feature bar, not a utility bar:

```do
http.get(url)                      -> Response!
http.post(url, body)               -> Response!
http.request(Request)              -> Response!
http.json[T](url)                  -> T!            // fetch and decode
```

with connection pooling and keep-alive, per-request and total timeouts, retry with exponential backoff and jitter, streaming request and response bodies, redirect policy, outbound TLS with certificate verification, and gzip transparently. Combined with `crypto.hmac_sha256` and `encode.hex`, this is enough to implement AWS SigV4, Stripe webhook verification, and OAuth 2 in under a hundred lines of doot each — which is the concrete form of the finite-surface argument.

The response-construction half of `http` (`http.see_other`, `http.not_found`, `http.error`, `Response`) ships in v0.1; the client lands in v0.3.

---

## Notes on selected modules

### `html`

```do
html.raw(s)              -> html     // the only escape hatch; audit every use
html.fragment([html])    -> html
html.attrs({str: str})   -> html
html.doc(head, body)     -> html
```

Everything else about HTML is syntax, not library ([D022](01-decisions.md#d022)). `html.raw` is deliberately the single unsafe entry point in the standard library, so `grep raw(` is a complete XSS audit.

### `time`

`time.Time` is a nanosecond instant; `time.Duration` is a signed nanosecond span. Duration literals come from `int` methods: `15.s`, `250.ms`, `2.h`, `7.days`. Storage in SQLite is as an integer, so ordering and range queries work without a format convention.

```do
time.now() time.parse(fmt, s)! time.from_unix(n)
t.format(fmt) t.add(d) t.diff(other) t.before(other) t.in_zone("UTC")
```

### `validate`

Struct attributes ([D043](01-decisions.md#d043)) cover declarative cases and run automatically during request binding ([D025](01-decisions.md#d025)). This module is for the imperative remainder:

```do
validate.check(value, rules) -> ()!
validate.email(s) validate.url(s) validate.in_range(n, lo, hi)
validate.errors(struct) -> {str: str}     // field → message, for re-rendering a form
```

`validate.errors` returning a field-to-message map is deliberate: re-rendering a form with inline errors is the single most common web interaction, and it should require no plumbing.

### `topic`

```do
topic.publish(name, value)      -> ()
topic.subscribe[T](name)        -> Subscription[T]   // iterable; blocks
topic.subscriber_count(name)    -> int
```

Published values are deep-copied into the frozen tier ([D004](01-decisions.md#d004)) so they can cross worker boundaries safely ([D007](01-decisions.md#d007)). A subscription has a bounded buffer (default 64) and closes with a `lagged` error on overflow, at which point the browser's own SSE reconnection re-establishes it ([D026](01-decisions.md#d026)).

### `cache`

Per-worker and in-memory, which is stated plainly rather than hidden: with N workers there are N caches. That is correct for a cache and wrong for a counter, and the language prevents the counter mistake by forbidding mutable globals ([D008](01-decisions.md#d008)). Anything requiring a single source of truth belongs in `db`.

```do
cache.get[T](key)              -> T?
cache.set(key, value, ttl)     -> ()
cache.remember[T](key, ttl, fn() -> T!) -> T!
```

### `jobs`

Durable because it is backed by SQLite, which means a queue with no additional process to run ([D032](01-decisions.md#d032)):

```do
jobs.enqueue(name, payload)              -> ()!
jobs.enqueue_at(name, payload, when)     -> ()!
jobs.handler(name, fn(payload) -> ()!)   -> ()
```

At-least-once delivery, exponential backoff, a dead-letter table, and visibility into all of it from the dev inspector.

### `log`

Structured by default, with automatic request correlation — every line emitted during a request carries its request id, route, and elapsed time without being asked to:

```do
log.info("user created", { user_id: u.id })
log.warn(msg, fields) log.error(msg, fields) log.debug(msg, fields)
```

Human-readable in development, JSON in production, switched by the runtime rather than configured.

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
