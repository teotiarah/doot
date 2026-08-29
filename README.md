# doot

**A programming language purpose-built for the web. One binary. One machine. SQLite. HTML.**

A request comes in. HTML goes out. That is the whole model.

doot exists because building a small-to-mid-size web application currently requires assembling a stack — a runtime, a framework, a package tree, a database, a reverse proxy, a container, a deployment platform — none of which the application needed. For the 0 to 50,000 user range, a single box with SQLite is not a compromise. It is the correct architecture, and every distributed-systems concept imported into it is pure cost.

> **Status: early implementation.** The base layer, build, and CI gates are in place; the compiler is not started. The documents below are the specification, settled before the code that implements it. Decisions in [`docs/01-decisions.md`](docs/01-decisions.md) are locked and do not get revisited.

```do
type Msg {
  id:   int
  room: str
  body: str
  at:   time.Time
}

route GET "/rooms/:room" (room: str) -> html! {
  let msgs = db.all[Msg](
    "select * from msgs where room = ? order by id desc limit 50", room)!

  return layout(room, <ul id="feed" data-live="/rooms/${room}/live">
    {for m in msgs}
      <li>${m.body}</li>
    {end}
  </ul>)
}

route POST "/rooms/:room" (room: str, form: NewMsg) -> redirect! {
  let m = db.one[Msg](
    "insert into msgs (room, body, at) values (?, ?, ?) returning *",
    room, form.body, time.now())!

  topic.publish("room:" + room, m)
  return http.see_other("/rooms/" + room)
}

stream GET "/rooms/:room/live" (room: str) {
  for m in topic.subscribe[Msg]("room:" + room) {
    send <li>${m.body}</li>
  }
}
```

That is a complete realtime chat application. No imports, no framework, no package manager, no build step, no hand-written JavaScript, no `await`, no Dockerfile. The SQL is checked against the schema at compile time — a misspelled column is a compile error, not a 500.

## What it is

| | |
| --- | --- |
| **Statically typed** | including SQL, request bodies, and templates |
| **HTML as a type** | escaped by default, so XSS requires explicitly asking for it |
| **SQLite only** | one file, WAL, no connection pool, no second process |
| **SSE built into the grammar** | a stream handler is a `for` loop, not a callback |
| **~38 stdlib modules, closed** | no registry, no packages, no lockfile, no supply chain |
| **No import statements** | stdlib modules are pre-bound namespaces |
| **No stop-the-world pause** | request-scoped arenas; nothing global to collect |
| **Two dependencies** | SQLite and mbedTLS, vendored. `cc *.c` builds it |
| **One binary** | `doot build` emits a self-contained executable |

Written in C, with the mindset that goes with it: ship a stable v1, one minor release a year, patches in between, and make it still work in a decade.

## Building

Requires a C99 compiler and nothing else.

```sh
make            # -> build/debug/doot
make test       # unit tests
make check      # everything CI runs
make help       # every target
```

Or without `make` at all, which is the property [D035](docs/01-decisions.md#d035) exists to guarantee and CI enforces on every commit:

```sh
tools/amalgamate.sh build/doot.c
cc -O2 -o doot build/doot.c
```

The binary currently supports `doot fmt`, `doot codes`, and `doot explain <code>`. Commands appear only when they fully work ([D054](docs/01-decisions.md#d054)) — the set grows as subsystems land. See [09-engineering.md](docs/09-engineering.md) for the build, test, and CI setup.

## Documentation

| | |
| --- | --- |
| [00-vision.md](docs/00-vision.md) | the problem, the thesis, the nine goals, permanent non-goals |
| [01-decisions.md](docs/01-decisions.md) | every locked decision with its full argument |
| [02-syntax.md](docs/02-syntax.md) | the language, by example |
| [03-grammar.md](docs/03-grammar.md) | normative EBNF, precedence, well-formedness rules |
| [04-stdlib.md](docs/04-stdlib.md) | the 38 modules and what is deliberately absent |
| [05-runtime.md](docs/05-runtime.md) | compiler pipeline, bytecode, memory tiers, scheduler, server |
| [06-tooling.md](docs/06-tooling.md) | CLI, machine-readable diagnostics, formatter, `doot.js` |
| [07-roadmap.md](docs/07-roadmap.md) | v0.1 through v1.0, and the release model |
| [08-boundaries.md](docs/08-boundaries.md) | where the runtime ends and the panel begins |
| [09-engineering.md](docs/09-engineering.md) | build, C subset, testing strategy, fuzzing, CI gates, vendoring |
| [10-frontend.md](docs/10-frontend.md) | lexer, parser, AST, front-end diagnostics, spec-test runner |

**Start with [00-vision.md](docs/00-vision.md), then [02-syntax.md](docs/02-syntax.md).**

## The constraints are the point

The three decisions that look most limiting are where the leverage is:

**No package registry → no FFI → cheap concurrency.** The hard part of a goroutine-style model — which thousands of open SSE connections demand — is that native C frames cannot be suspended. Since no foreign code can ever be loaded, the VM owns the entire call stack, and suspending a task is a pointer save. No assembly, no stack copying, no platform-specific context switching. A language with an FFI cannot have this at this price.

**No registry → no imports at all.** With a closed stdlib, every module is pre-bound. This deletes a whole category of AI-agent error: wrong import path, missing import, wrong package name.

**Static types → no value tagging, and a faster interpreter.** The compiler knows what every register holds, so registers carry raw untagged values, arithmetic opcodes are type-specialized, and the interpreter's hot loop performs zero type checks. Static typing pays for itself in speed, not only in correctness.

## Not goals, permanently

Distributed anything. A package registry. An FFI. TLS termination in the app runtime. Docker or Kubernetes. WebSockets. An ORM. A template engine. Client-side rendering as a first-class model. Being a good general-purpose language.

See [00-vision.md](docs/00-vision.md#non-goals-permanently).
