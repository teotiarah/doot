# Tooling

One binary. No toolchain to install, no package manager, no asset pipeline, no separate test runner, no language server to download.

---

## CLI

| Command | Purpose | Lands |
| --- | --- | --- |
| `doot new <name>` | scaffold a project | v0.1 |
| `doot run [file]` | run the project, or a single `.do` file | v0.1 |
| `doot dev` | run with hot reload and in-browser diagnostics | v0.1 |
| `doot check` | typecheck without running | v0.1 |
| `doot fmt` | format, canonically and without options | v0.1 |
| `doot test [filter]` | run `test` blocks | v0.1 |
| `doot routes` | print the route table | v0.1 |
| `doot migrate` | apply pending migrations | v0.1 |
| `doot explain <code>` | long-form explanation of a diagnostic | v0.1 |
| `doot doc [--agent]` | language and stdlib reference | v0.1 |
| `doot repl` | interactive evaluation | v0.1 |
| `doot build` | single self-contained executable | v0.3 |
| `doot bench` | benchmark handlers under load | v0.3 |
| `doot lsp` | language server over stdio | v0.4 |

Every command accepts `--json` for machine-readable output.

### Exit codes

Three, the same for every command:

| Code | Meaning |
| --- | --- |
| `0` | success, and no diagnostics were reported |
| `1` | the command ran and reported diagnostics |
| `2` | the command was used wrongly — unknown command, unknown option, bad arguments |

The distinction between 1 and 2 is the one that matters to a caller: `1` means doot has something to say about your code, `2` means doot did not understand what you asked for. A script can therefore treat `2` as its own bug and `1` as a finding. The spec runner relies on exactly that split — it reports exit code `2` as a failure of the suite rather than of the file under test ([11-spec-tests.md](11-spec-tests.md#the-mode)).

`doot fmt` returns `1` when it reports diagnostics even though it also formatted whatever it could, because a run that skipped a file did not fully succeed. Its stdout summary distinguishes the three outcomes — reformatted, already formatted, and skipped — and that summary is a pinned interface, tested exactly ([D075](01-decisions.md#d075)).

### `doot new`

```
myapp/
  app.do                  entry point: config and error hooks
  routes/
    index.do
  models/
  views/
    layout.do
  static/
    app.css
  migrations/
    001_init.sql
```

No lockfile, no manifest, no `node_modules`, no virtualenv, no `Dockerfile`, no CI config, no build script. Configuration is a `let` in `app.do` ([D040](01-decisions.md#d040)).

### `doot build`

Produces one executable containing the bytecode image, the runtime, the static assets, and `doot.js`. Deployment is copying that file to a machine and running it. No runtime installation on the target, no shared libraries beyond libc, no interpreter version to match.

---

## Diagnostics

Diagnostics are a **first-class subsystem designed for machine consumption as much as human** ([D038](01-decisions.md#d038)). This is goal 9, and it is treated as a feature with a specification rather than as error-message polish.

Every diagnostic carries:

- a **stable code** — `DT0142`, permanent once assigned, never renumbered or reused
- an **exact byte span**, with the source line and a caret range
- a **plain-English explanation** that names the rule being violated
- a **suggested fix** where one is determinable
- **related spans** — the declaration a call site disagrees with, the earlier route that conflicts, the migration a column is missing from

Human output. The gutter width comes from the primary span's line number, and every related span is rendered as its own snippet at the same gutter:

```
error[DT0142]: column `emial` does not exist on table `users`
  --> routes/users.do:14:36
   |
14 |   let u = db.one[User]("select id, emial from users where id = ?", id)!
   |                                    ^^^^^
  --> migrations/001_init.sql:3:3
   |
 3 |   email text not null,
   |   ^^^^^ table `users` declared here
  help: replace with `email`
  help: run `doot explain DT0142` for more
```

Tabs in a source line are expanded to four columns when rendering, and the caret is placed by display column, so a snippet stays aligned regardless of how the file is indented.

Machine output — `doot check --json`, shown formatted here and emitted as one line:

```json
{
  "diagnostics": [{
    "code": "DT0142",
    "severity": "error",
    "message": "column `emial` does not exist on table `users`",
    "file": "routes/users.do",
    "span": { "start": 312, "end": 317, "line": 14, "col": 36 },
    "suggestion": { "replace_span": [312, 317], "with": "email" },
    "related": [{
      "file": "migrations/001_init.sql",
      "span": { "start": 48, "end": 53, "line": 3, "col": 3 },
      "message": "table `users` declared here"
    }]
  }],
  "summary": { "errors": 1, "warnings": 0, "truncated": false }
}
```

`span` carries byte offsets *and* line/column: byte offsets are what an editing tool needs to apply an edit, line and column are what a human needs to navigate. `truncated` reports whether collection stopped at the diagnostic limit, so a consumer never mistakes a truncated list for a complete one.

The `suggestion` field is a machine-applicable edit — a span and a replacement — so an agent can apply the fix without re-parsing prose. That is the difference between diagnostics an agent can *read* and diagnostics an agent can *act on*.

### Code ranges

| Range | Category |
| --- | --- |
| `DT0001`–`DT0099` | lexical and syntactic |
| `DT0100`–`DT0199` | names, modules, resolution, SQL/schema |
| `DT0200`–`DT0299` | types |
| `DT0300`–`DT0399` | mutability and immutability rules |
| `DT0400`–`DT0499` | errors, optionals, exhaustiveness |
| `DT0500`–`DT0599` | routes, request binding, markup |
| `DT0600`–`DT0699` | warnings and deprecations |
| `DT0900`–`DT0999` | runtime faults |
| `DT1000`–`DT1099` | driver, CLI, and I/O |

A range is a **subject**, not a pipeline stage, so a code lives with what it is about rather than with whatever stage happens to notice it. Each range is sub-allocated in full, in advance, in the document that owns it: `DT0001`–`DT0099` in [10-frontend.md](10-frontend.md#front-end-diagnostics) ([D064](01-decisions.md#d064)), and `DT0100`–`DT0699` in [12-semantics.md](12-semantics.md#semantic-diagnostics) ([D100](01-decisions.md#d100)).

### `doot doc --agent`

Emits a compact, complete language and stdlib reference sized to drop into an agent's context window: every keyword, every construct with one example, every stdlib signature, and the twenty most common mistakes with their corrections.

**The language ships its own AI context file**, versioned with the binary and generated from the same source of truth as the compiler. It is the most direct available attack on goal 2: rather than hoping models were trained on enough doot, we hand them an authoritative reference at the moment they need it, and it can never drift from the implementation.

---

## `doot fmt`

**Canonical, with no options** ([D039](01-decisions.md#d039)). The gofmt lesson: one non-negotiable format ends style debate, and — specifically valuable here — makes agent output deterministic and diffs meaningful.

Fixed choices: two-space indentation; no semicolons; no trailing whitespace; one blank line maximum between declarations; struct fields aligned on the colon; markup indented as markup with attributes wrapped past 100 columns; imports do not exist, so there is nothing to sort.

Naming is enforced, not suggested: modules `lower`, types `PascalCase`, functions and fields `snake_case`, enum variants `snake_case`, constants `snake_case` (there is no separate constant case, because there are no globals to distinguish).

It is enforced by **`doot check`, not by `doot fmt`** ([D069](01-decisions.md#d069)). A formatter cannot rename: renaming changes what the code means and requires rewriting every use site, which is refactoring rather than formatting. `doot fmt` therefore reports nothing about names, and the checker reports a violation with the correct spelling as a machine-applicable suggestion. The exact patterns, and the one violation that cannot carry a suggestion because it names a file rather than a span, are in [12-semantics.md](12-semantics.md#naming-rules) ([D086](01-decisions.md#d086)).

**Line structure inside markup and argument lists is preserved, not decided** ([D068](01-decisions.md#d068)). Everything else is canonical — indentation, spacing, parentheses, void elements, blank lines, field alignment — but the printer never adds or removes a line break inside a markup literal or an argument list, because whitespace between elements is rendered content and because a formatter with no wrapping rule must not join a line the author broke.

---

## `doot dev`

- Recompiles on save; in-flight requests finish against the old image ([05-runtime.md](05-runtime.md#hot-reload))
- A compile error displays as an overlay **without replacing the page**, so a typo does not discard the state you were looking at
- A fault renders the full doot stack trace with source context in the browser
- Request timing, SQL statements with their timings, and log lines for the current request appear inline
- `doot.js` is served unminified with source context in development

The **dev inspector** (v0.4) adds a browser UI for browsing tables, running ad-hoc queries, viewing the route table, tailing structured logs, and watching live requests and job queue state. It is part of the runtime rather than a separate tool, and is unavailable when the process is started in production mode.

---

## `doot.js`

The client runtime ([D027](01-decisions.md#d027)). Roughly 4 KB, built into the binary, auto-injected, versioned with the runtime, with no build step and no extensibility surface.

It does exactly three things:

1. **Progressive form submission** — a `<form>` posts without a full page reload and swaps in the returned fragment. Works normally with JavaScript disabled, because the server returns real HTML either way.
2. **Fragment swapping** — a link or form with a `data-target` selector replaces that element with the response.
3. **SSE binding** — `data-live="/path"` opens a stream and applies arriving HTML fragments to the element, reconnecting automatically.

```html
<ul id="feed" data-live="/rooms/general/live">…</ul>
<form method="post" action="/rooms/general" data-target="#feed">…</form>
```

Without this, HTML-over-the-wire leaks: every interactive app would hand-write JavaScript, and the no-npm claim would be false in practice. With it, the interaction model is complete at zero configuration cost.

It is **not a JavaScript framework** and will not grow into one. It is a runtime component that happens to execute in the browser. Anything beyond these three behaviors is the application's own JavaScript, served as a static file.

---

## Testing

`test` blocks live beside the code they test; `doot test` discovers them ([02-syntax.md](02-syntax.md#tests)).

Each test runs **in its own task, inside a transaction against a temporary database, rolled back afterward** — so isolation is structural rather than a convention that a forgotten cleanup can break. There is no fixture framework, no mocking library, and no dependency injection to enable testing, because there is nothing to inject.

`doot test --json` emits per-test results with timings and failure spans in the same diagnostic format as `doot check`.
