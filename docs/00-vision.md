# Vision

## The problem

Building a small-to-mid-size web application requires assembling a stack. Not designing one — assembling one, from parts that were never meant to fit together, at a cost that has nothing to do with the application being built.

Two paths exist today, and both charge a toll.

**The interpreted path** (Python, Ruby, PHP) gets you a working app quickly and then bills you forever in resources. A few hundred requests per second on a 2 GB machine, and optimizing past that means learning the runtime's internals. The deployment story is worse: either a PaaS that still requires assembling a dozen services, or a self-hosted control plane — Coolify, Dokku, and friends — where you now own a container orchestrator, a reverse proxy, a database, a language runtime, and the interactions between all four.

**The JavaScript path** charges the toll up front, in decisions. Pick a runtime (Node, Deno, Bun). Pick a framework. Pick packages, and inherit their transitive dependency trees. Then discover that the deployment target constrains all of it — Cloudflare's V8 isolates support some packages and not others, Vercel pulls toward Node and Next-specific APIs, and the rendering model (SSR, SSG, ISR, islands, client components) is now something you must have an opinion about before you can return a page.

None of this is complexity the *application* needed. A CRUD app with a database, some forms, and a live-updating page does not require a distributed system. It requires a request to come in and HTML to go out.

## The thesis

**A request comes in. HTML goes out. One process, one machine, one file of data. The language ships everything needed to do that, and nothing else.**

PHP owned this era, and it's worth being precise about why. The unit of work was a file. The unit of composition was a template. You could go from an idea to a page on the internet without learning an architecture. That was not a limitation people tolerated; it was the product.

PHP lost it for reasons that are all fixable:

| PHP's problem | doot's answer |
| --- | --- |
| No persistent process — so no realtime, no in-process cache | Long-lived process with cheap concurrent tasks |
| No static types | Statically typed, checked end to end, including SQL |
| Incoherent stdlib accreted over decades | ~35 modules, designed once, coherent naming |
| Deployment needs nginx + php-fpm + a process manager | One binary, one Unix socket |
| Templates as string concatenation, XSS by default | HTML as a checked type, escaping by default |

doot keeps what PHP got right and fixes what it got wrong.

## The shape that follows

doot is best understood as **a runtime that happens to have a language attached**, not a general-purpose language that happens to be usable for web work. The language exists to make the runtime's job expressible.

This ordering is what makes the scope decidable. When a feature is proposed, the question is not "is this a good language feature" — most of them are — but "does an application that takes a request and returns HTML on a single machine need this." Almost nothing passes. That is the point.

The target is explicitly **0 to 50,000 users**: the range where a single box with SQLite is not a compromise but the correct architecture, and where every distributed-systems concept you import is pure cost.

## The nine goals

These are the project's fixed points. Every decision in [01-decisions.md](01-decisions.md) traces to at least one.

1. **Simplicity at the core.** One way to do each thing. No dialects, no config formats, no sugar duplication.
2. **The cleanest syntax an AI agent can write and a human can read.** Ambiguity is the enemy; unambiguous beats terse.
3. **The monolith, deliberately.** SQLite, single box, single process. No distributed systems ambitions, ever.
4. **Efficient to run and to maintain.** Thousands of concurrent connections and predictable latency on a 2 GB machine.
5. **Modern ergonomics.** A real CLI, a formatter, a language server, hot reload, structured diagnostics.
6. **A purpose-built stdlib of ~35 modules** so that reaching for a web framework never occurs to the user.
7. **No package system and no central registry.** The surface a web app needs is finite; cover 90–95% of it and let people write the rest.
8. **Realtime (SSE only) that is native, not bolted on.**
9. **Static types and a diagnostics system built for machine consumption as much as human.**

## The constraints are the advantages

The three decisions that look most limiting are the source of doot's largest structural wins. This is worth internalizing, because these are the decisions that will come under pressure later, and the pressure will be wrong.

**No registry → no user FFI → cheap concurrency.** The hardest part of a goroutine-style concurrency model — which is what thousands of open SSE connections demand — is that native C frames on the call stack cannot be suspended. Because no foreign code can ever be loaded into doot, the VM owns 100% of the call stack, and suspending a task becomes a pointer save: no assembly, no stack copying, no per-platform context switching. A language with FFI cannot have this at this price.

**No registry → no import statements at all.** With a closed stdlib, every module is pre-bound as a global namespace. `db.all(...)`, `time.now()`, `topic.publish(...)`. This deletes an entire class of agent error — wrong import path, missing import, wrong package name — which is goal 2 directly.

**Static types → no value tagging, and a faster interpreter.** The compiler knows what every register holds, so registers carry raw untagged values, arithmetic opcodes are type-specialized, and the interpreter's hot loop performs zero type checks. Static typing pays for itself in speed here, not just in correctness. See [D002](01-decisions.md#d002).

## The objection worth answering

"No packages" sounds naive until you notice that **every modern integration is HTTPS plus JSON.** Stripe, S3, OAuth providers, Postmark, SES, OpenAI — there is no native library to bind, only an HTTP call to make and a payload to sign. So the finite-surface claim holds, on one condition: the `http` client and `crypto` module must be genuinely excellent, because they are the *entire* third-party integration story. That is treated as a hard requirement, not a nice-to-have.

Sharing works by copying source. If someone writes a good `stripe.do`, you paste the file into your project and the compiler treats it as ordinary project code. No versions, no lockfile, no transitive dependencies, no supply chain, and you can read every line you depend on.

## Non-goals, permanently

Not "not yet" — these are out, and proposals to add them should be closed by pointing here.

- **Distributed anything.** No clustering across machines, no service discovery, no distributed tracing, no eventual consistency.
- **A package registry or dependency resolver.**
- **A foreign function interface.** It would forfeit the concurrency model (see above), the safety guarantees, and the "read every line you depend on" property.
- **TLS termination in the app runtime.** See [08-boundaries.md](08-boundaries.md).
- **Docker, Kubernetes, or a reverse proxy in the deployment path.**
- **WebSockets.** SSE only, per goal 8. SSE plus form posts covers the interaction model; WebSockets would double the realtime surface for the remainder.
- **An ORM, a query builder, or a template engine.** Checked SQL and markup literals make all three unnecessary.
- **Client-side rendering as a first-class model.** The server renders HTML. See [D021](01-decisions.md#d021).
- **A general-purpose systems language.** doot will never be a good choice for a compiler, a game, or a CLI tool that isn't `doot` itself.
