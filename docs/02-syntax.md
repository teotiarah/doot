# Syntax

The design rules, applied throughout: **one way to do each thing**, braces rather than significant indentation, no semicolons, `name: type`, inference for locals, errors as values, no user-defined generics.

Statements are newline-terminated. Comments are `//` to end of line and `/* */` for blocks.

---

## A complete application

This is a full realtime chat app. Runnable as `doot run app.do`. No imports, no framework, no build step, no hand-written JavaScript, no `await`.

```do
type Msg {
  id:   int
  room: str
  body: str
  at:   time.Time
}

fn layout(title: str, body: html) -> html {
  return <html>
    <head><title>${title}</title></head>
    <body>${body}</body>
  </html>
}

route GET "/rooms/:room" (room: str) -> html! {
  let msgs = db.all[Msg](
    "select * from msgs where room = ? order by id desc limit 50", room)!

  return layout(room, <div>
    <ul id="feed" data-live="/rooms/${room}/live">
      {for m in msgs}
        <li>${m.body}</li>
      {end}
    </ul>
    <form method="post" action="/rooms/${room}">
      <input name="body" required/>
      <button>send</button>
    </form>
  </div>)
}

type NewMsg {
  body: str @len(1, 500) @trim
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

Everything below explains a piece of that.

---

## Bindings

```do
let name = "doot"          // immutable, inferred
let count: int = 0         // immutable, explicit
var total = 0              // mutable
total = total + 1
```

`let` is **deeply immutable** — the binding and everything reachable through it. `var` permits mutation, and because a `var` local is uniquely owned, mutation is in place:

```do
var items = ["a", "b"]
items.push("c")            // in place, no copy
```

**Top level allows `let` only.** There are no mutable globals in doot ([D008](01-decisions.md#d008)). State belongs in SQLite, in request scope, or in a `cache` cell.

---

## Types

| Type | Notes |
| --- | --- |
| `int` | signed 64-bit; overflow is a fault, not a wrap |
| `float` | IEEE 754 double |
| `bool` | `true` / `false` |
| `str` | immutable UTF-8 |
| `bytes` | immutable byte sequence |
| `html` | escaped markup; see [Markup](#markup) |
| `any` | dynamic, produced only by untyped JSON parsing |
| `T?` | optional; the only type that can hold `nil` |
| `[T]` | list |
| `{K: V}` | map |
| `fn(A, B) -> C` | function |

Money is `int` in minor units — there is no decimal type ([D020](01-decisions.md#d020)).

Type application uses square brackets, never angle brackets: `db.all[User](...)`.

### Strings

`str` is UTF-8 and is **not indexable by integer**, which removes the classic byte-versus-character bug at the source:

```do
s.len                      // length in bytes
s.char_count               // length in characters
s.slice(0, 5)              // byte offsets, faults if not on a character boundary
s.chars()                  // iterate characters
s.upper()  s.trim()  s.split(",")  s.starts_with("http")
"hello ${name}, you have ${count} messages"
```

Core types carry methods; the `str`, `list`, `map`, and `bytes` modules hold only constructors and statics (`list.range(0, 10)`, `str.from_int(n)`).

### Structs

```do
type User {
  id:    int
  name:  str
  email: str
  bio:   str?
  created: time.Time
}

fn User.display(self) -> str {
  return "${self.name} <${self.email}>"
}
```

No classes, no inheritance, no interfaces ([D016](01-decisions.md#d016)). `==` compares by content ([D009](01-decisions.md#d009)).

Functional update with `with`, which is what makes deep immutability comfortable:

```do
let renamed = user with { name: "Ada", bio: nil }
```

### Enums

Tag-only in v0.1 ([D018](01-decisions.md#d018)). Variants are referenced with a leading dot where the type is known from context:

```do
type Status enum { active, banned, pending }

let s = Status.active
if s == .banned { return http.forbidden() }
```

---

## Functions

```do
pub fn greet(name: str) -> str {
  return "hello ${name}"
}

fn find(id: int) -> User?          // may be absent
fn create(name: str) -> User!      // may fail
fn log_it(msg: str)                // returns nothing

fn paginate(page: int = 1, size: int = 20) -> [User]! { ... }
```

**Parameters are immutable.** A function cannot mutate its caller's data ([D008](01-decisions.md#d008)); to modify, bind locally with `var` and return a new value.

`pub` exports. Without it, a function is visible only inside its own file.

Lambdas:

```do
let names = users.map(fn(u: User) => u.name)
let adults = users.filter(fn(u: User) => u.age >= 18)
xs.each(fn(x: int) {
  log.info("saw ${x}")
})
```

---

## Errors

`?` means absent-possible. `!` means failure-possible. **The same mark appears at the declaration and at the call site**, so error handling is verifiable by eye ([D013](01-decisions.md#d013)).

```do
let u = create(name)!                              // propagate; enclosing fn must be ! too
let u = create(name) else return page.error(500)   // handle and bail
let u = find(id) else User.guest()                 // supply a default
let n = find(id) else { log.warn("miss"); return http.not_found() }
```

`Error` is a single type carrying `kind`, `message`, a `cause` chain, and an automatically captured source location ([D014](01-decisions.md#d014)):

```do
let u = db.one[User]("select * from users where id = ?", id) else err {
  match err.kind {
    .not_found -> return http.not_found()
    else       -> return http.error(500)
  }
}
```

`else err { ... }` binds the error for inspection. `else { ... }` without a binding discards it.

There are no exceptions, no `throw`, no `try`/`catch`, and no `panic`. Bugs — index out of range, overflow, budget exceeded — are **faults**: they end the current request with a 500 and a logged diagnostic, and cannot affect the process or any other request ([D012](01-decisions.md#d012)).

For genuinely untrusted indexes, the safe accessor returns an optional:

```do
let first = xs.get(0) else return http.bad_request()
```

---

## Control flow

```do
if count > 10 {
  ...
} else if count > 0 {
  ...
} else {
  ...
}

for u in users { ... }
for i, u in users { ... }              // index, value
for k, v in settings { ... }           // key, value
for i in list.range(0, 10) { ... }

while running {
  if done { break }
  if skip { continue }
}

match status {
  .active  -> render_active()
  .banned  -> http.forbidden()
  else     -> http.error(500)
}

defer file.close()
```

`match` arms use `->` and take an expression or a block. A non-exhaustive `match` requires an `else` arm.

`and`, `or`, `not` are the boolean operators — more readable than `&&`/`||`, and it keeps `!` free for fallibility.

---

## Markup

`html` is a distinct type, and **every interpolation is escaped automatically**, so XSS requires explicitly writing `html.raw(s)` ([D021](01-decisions.md#d021)).

```do
fn card(u: User) -> html {
  return <div class="card">
    <h2>${u.name}</h2>
    {if u.bio != nil}
      <p>${u.bio}</p>
    {end}
    <a href="/users/${u.id}">profile</a>
  </div>
}
```

Control flow inside markup uses the same keywords as statements:

```do
<ul>
  {for u in users}
    <li class="${u.css_class()}">${u.name}</li>
  {else}
    <li class="empty">nobody here</li>
  {end}
</ul>
```

`{for}` accepts an `{else}` branch that renders when the collection is empty — the single most common template need, handled without an extra `if`.

**Composition is function calls, not component tags** ([D023](01-decisions.md#d023)):

```do
return layout("Profile", card(user))
```

Attribute values may be interpolated, and boolean attributes take a `bool`:

```do
<input name="email" value="${form.email}" disabled="${is_locked}"/>
```

`html` values compose freely; a `[html]` list renders as concatenation. `${}` accepts any value with a display form, and `str` is escaped.

---

## Routes

`route` is a declaration, so the compiler knows the whole route table and can check it ([D024](01-decisions.md#d024)).

```do
route GET "/users/:id" (id: int) -> html! {
  let u = db.one[User]("select * from users where id = ?", id)!
  return layout(u.name, card(u))
}
```

Path parameters bind by name and are converted to the declared type; a failed conversion is a 404, not a fault. Three parameter names are reserved and bind automatically ([D025](01-decisions.md#d025)):

```do
type Search { q: str @len(1, 100), page: int = 1 }

route GET "/search" (query: Search) -> html! { ... }
route POST "/users" (form: NewUser) -> redirect! { ... }
route POST "/api/users" (json: NewUser) -> User! { ... }
```

Validation from `@` attributes runs before the handler body, and a failure returns 422 without executing it. `req` is implicitly available for headers and cookies.

### Return types

| Return type | Response |
| --- | --- |
| `html` | 200, `text/html` |
| `str` | 200, `text/plain` |
| `bytes` | 200, `application/octet-stream` unless `@content_type` |
| a struct, `[T]`, or `{K: V}` | 200, `application/json` |
| `redirect` | 303 via `http.see_other(...)`, or 301/302 helpers |
| `Response` | full control over status, headers, and body |

An error propagated out of a route becomes a 500 with a logged diagnostic. Two well-known hooks customize the failure pages:

```do
fn on_error(err: Error) -> html { ... }
fn on_not_found() -> html { ... }
```

### Groups

Groups add a path prefix and shared before/after behavior, expressed with attributes rather than extra keywords:

```do
@before(auth.require)
group "/admin" {
  route GET "/"      ()         -> html! { ... }
  route GET "/users" (query: P) -> html! { ... }
}
```

An `@before` function returning an error short-circuits the request.

### Uploads

A struct field typed `Upload` makes the form multipart-aware:

```do
type AvatarForm {
  caption: str  @len(0, 200)
  file:    Upload @max_size(5.mb) @content_type("image/png", "image/jpeg")
}

route POST "/avatar" (form: AvatarForm) -> redirect! {
  form.file.save_to("uploads/${uuid.new()}.png")!
  return http.see_other("/profile")
}
```

Bodies stream to disk rather than buffering in the request arena, so a large upload does not consume the request memory budget.

---

## Tasks and realtime

*Lands in v0.2 ([07-roadmap.md](07-roadmap.md)); the syntax is locked now.*

```do
stream GET "/rooms/:room/live" (room: str) {
  for m in topic.subscribe[Msg]("room:" + room) {
    send <li>${m.body}</li>
  }
}
```

No `await` anywhere: a task blocks, and the runtime schedules around it ([D006](01-decisions.md#d006)). `send` writes an SSE event; `send "name", value` sets the event name.

```do
spawn send_welcome(user.id)                  // fire-and-forget task
let ch = chan.new[int](16)
spawn producer(ch)
for n in ch { ... }
```

Closures passed to `spawn` capture immutably, so a data race is not merely unlikely — it is unrepresentable ([D008](01-decisions.md#d008)).

---

## Data access

SQL is validated against the real schema at compile time, and result shapes are checked against the struct ([D033](01-decisions.md#d033)):

```do
let u  = db.one[User]("select * from users where id = ?", id)!     // exactly one; error if none
let mu = db.find[User]("select * from users where email = ?", e)!  // User? — nil if none
let us = db.all[User]("select * from users order by name")!        // [User]
let n  = db.count("select count(*) from users")!
db.exec("update users set name = ? where id = ?", name, id)!

db.tx(fn() {
  db.exec("insert into ledger (amount) values (?)", 100)!
  db.exec("update accounts set balance = balance - ?", 100)!
})!
```

A misspelled column, a missing table, a wrong placeholder count, or a result shape that does not match `User` are all **compile errors**.

---

## Modules

No import statements exist ([D030](01-decisions.md#d030)). Stdlib modules are pre-bound. User modules are addressed by path, with `/` becoming `.`:

```
myapp/
  app.do              → app.*
  routes/users.do     → routes.users.*
  models/user.do      → models.user.*
  views/layout.do     → views.layout.*
```

```do
// in routes/users.do
let u = models.user.find(id) else return http.not_found()
return views.layout.page("User", views.user.card(u))
```

Within a file, local names are unqualified. Everything else is fully qualified, **including files in the same directory** — there is no import resolution to get wrong and no aliasing to trace.

---

## Configuration

Configuration is doot code, not a file format ([D040](01-decisions.md#d040)):

```do
// app.do
let config = Config {
  listen:         ":8080"
  database:       "data/app.db"
  workers:        os.cpu_count()
  request_memory: 16.mb
  request_timeout: 15.s
  static_dir:     "static"
}
```

Typechecked like everything else. Values that must come from the environment use `env.get("NAME") else "default"`.

Note the suffix literals: `16.mb`, `15.s`, `250.ms` are ordinary method calls on `int` producing sized and duration values.

---

## Keywords

Thirty-one, frozen at v0.1 ([D042](01-decisions.md#d042)) including those whose features land later:

```
and       as        break     continue  defer     else      enum
false     fn        for       group     if        in        let
match     nil       not       or        pub       return    route
self      send      spawn     stream    test      true      type
var       while     with
```

What is **absent** is as deliberate as what is present:

- No `import` / `use` / `require` / `package` / `module` — [D030](01-decisions.md#d030)
- No `async` / `await` / `go` / `promise` — [D006](01-decisions.md#d006)
- No `try` / `catch` / `throw` / `finally` / `panic` — [D012](01-decisions.md#d012)
- No `class` / `interface` / `extends` / `implements` / `new` / `this` — [D016](01-decisions.md#d016)
- No `const` / `static` / `global` — [D008](01-decisions.md#d008)
- No `switch` — `match` subsumes it
- No `null` alongside `nil` — one absent value, one spelling

Type names (`int`, `float`, `str`, `bool`, `bytes`, `html`, `any`) and all stdlib module names are **predeclared identifiers, not keywords**, so they may be shadowed in a local scope and consume no keyword budget.

### Reserved words

Reserved, unused, and a compile error with a specific message pointing at the doot equivalent. This makes reaching for a foreign construct fail clearly instead of confusingly, and keeps every future addition non-breaking:

```
async     await     class     const     do        extends   finally
foreign   go        impl      import    interface loop      macro
module    mut       new       null      package   panic     private
protected require   select    static    switch    this      throw
trait     try       typeof    unsafe    use       where     yield
```

---

## Attributes

A closed set of twelve ([D043](01-decisions.md#d043)). No user-defined macros, decorators, or annotations, ever.

| Attribute | Applies to | Effect |
| --- | --- | --- |
| `@len(min, max)` | `str`, `[T]`, `bytes` | length bounds |
| `@min(n)` / `@max(n)` | `int`, `float` | value bounds |
| `@one_of(a, b, …)` | `str`, `int` | membership |
| `@email` | `str` | email validation |
| `@url` | `str` | URL validation |
| `@trim` | `str` | strip surrounding whitespace before validating |
| `@max_size(n)` | `Upload` | reject larger bodies |
| `@content_type(…)` | `Upload`, route | restrict or set content type |
| `@before(fn)` / `@after(fn)` | `route`, `group` | request hooks |
| `@deprecated("msg")` | any declaration | compile warning at use sites |

---

## Tests

Built in; no framework, no separate runner:

```do
test "greet formats a name" {
  test.eq(greet("Ada"), "hello Ada")
}

test "creating a user rejects a blank name" {
  let err = test.expect_error(fn() => create(""))
  test.eq(err.kind, .validation)
}
```

`doot test` discovers every `test` block in the project. Each test runs in its own task with a fresh transaction against a temporary database, rolled back afterward, so tests are isolated by construction rather than by convention.
