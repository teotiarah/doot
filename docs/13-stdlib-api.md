# Standard library API

Normative reference for the **API surface of the 26 standard-library modules that land in v0.1**. Decisions are recorded as [D130](01-decisions.md#d130)–[D156](01-decisions.md#d156).

[04-stdlib.md](04-stdlib.md) is the overview: it decides *which* modules exist, what each is for, and which release it lands in, and it argues the two modules that carry structural weight. **This document decides what every member of every v0.1 module is called, what it takes, what it returns, how it fails, and what it costs.** The overview is unchanged in scope; where the two disagree, this document wins, and every disagreement found while writing it is listed in [Corrections to the overview](#corrections-to-the-overview).

Its immediate consumer is the **module signature table** — the compile-time X-macro the resolver and the typechecker read ([D085](01-decisions.md#d085), [12-semantics.md](12-semantics.md#the-prelude-and-the-module-table)). That table is data, it is on the critical path for every checker, and it cannot be written from an overview. Producing it is one of the three concurrent workstreams in [D102](01-decisions.md#d102), and this document is its input ([D130](01-decisions.md#d130)).

**No implementation.** [Sequencing rule 1](07-roadmap.md#sequencing-rules) is why this exists before any stdlib code: a design question is never settled inside an implementation diff. Nothing here registers a diagnostic code — the numbers below are reservations in the sense of [D065](01-decisions.md#d065), and a row enters `src/base/diag_codes.h` only with the test that produces it.

---

## The 26 modules of v0.1

Counted from the `Lands` column of [04-stdlib.md](04-stdlib.md#overview) and stated here explicitly so the count is checkable rather than asserted:

| Group | Modules | Count |
| --- | --- | --- |
| Core data | `str`, `list`, `map`, `bytes`, `math`, `time` | 6 |
| Serialization | `json`, `encode` | 2 |
| Web | `html`, `http`, `url`, `form`, `cookie`, `mime`, `static` | 7 |
| Data | `db`, `validate` | 2 |
| Crypto | `crypto`, `uuid`, `rand` | 3 |
| System | `fs`, `path`, `env`, `os` | 4 |
| Observability and dev | `log`, `test` | 2 |

**26**, which is the number [07-roadmap.md](07-roadmap.md#v01--request-in-html-out) commits to. Two of them land partially, and both partial lines are in the overview's table already:

- **`http`** — the response-construction half is v0.1; **the outbound client is v0.3** ([D031](01-decisions.md#d031)). Three client entry points are given their signatures below because the module table needs a row per member and the version column is what makes a row honest, but the client's behaviour — pooling, retries, redirect policy, streaming, TLS — is the v0.3 pass and is not specified here.
- **`crypto`** — everything except password hashing is v0.1; **argon2id is v0.4** ([D035](01-decisions.md#d035)).

The remaining twelve modules of the closed set of thirty-eight are **out of scope for this document**: `csv`, `session`, `auth`, `cache`, `task`, `chan`, `topic`, `jobs`, `cron`, `jwt`, `metrics`, and `image`. Reaching a member of any of them, or a member marked v0.3 or v0.4 above, is `DT0046` naming the release — the same code and the same situation as `spawn`, `send`, and `stream` ([D085](01-decisions.md#d085)).

The set is closed at thirty-eight, so **nothing below adds a module.** That constraint did real work: it is why error construction is not an `error` module ([D131](01-decisions.md#d131)).

---

## How to read a signature

A signature in this document is **module-table notation, not doot source** ([D154](01-decisions.md#d154)). A standard-library member has no doot declaration to quote — the compiler knows it from the table and the runtime implements it in C — so a bare `fn` with no body is not what is being shown. The notation maps onto a declaration one-to-one:

```
str.join(parts: [str], separator: str) -> str
```

is the member `join` of module `str`, and its declared form would be `fn join(parts: [str], separator: str) -> str`. Parameter names are part of the signature: they are what a reader of a call site sees named in a diagnostic, and `doot doc --agent` prints them.

| Notation | Meaning |
| --- | --- |
| `-> T` | returns `T`, cannot fail |
| `-> T!` | fallible; the caller writes `!` or `else` ([D013](01-decisions.md#d013)) |
| `-> T?` | may be absent; the caller unwraps with `else` |
| `-> T?!` | both, in that order: `f()!` yields `T?`, `f()! else v` yields `T` ([D094](01-decisions.md#d094)) |
| no `->` | yields nothing and cannot fail |
| `-> ()!` | **fails and yields nothing** ([D134](01-decisions.md#d134)) |
| `member[T](…)` | a type-argument slot ([D019](01-decisions.md#d019), [D135](01-decisions.md#d135)) |
| `args: ...` | a variadic tail, permitted for six members and no others ([D133](01-decisions.md#d133)) |
| `name: type` with no parentheses | a **field**, not a method ([D136](01-decisions.md#d136)) |
| `var` beside a method | it mutates its receiver, so the receiver's root binding must be `var` ([D097](01-decisions.md#d097), [D137](01-decisions.md#d137)) |
| `blocking` beside a member | it runs on the worker's blocking pool ([D006](01-decisions.md#d006), [D149](01-decisions.md#d149)) |

A member of a **module** written without parentheses is a value rather than a function — the stdlib equivalent of a `pub let`, which [D084](01-decisions.md#d084)'s path algorithm already resolves. There are two, `math.pi` and `math.infinity`.

Three notations exceed what [03-grammar.md](03-grammar.md#declarations) can spell, and each is argued rather than assumed: `-> ()!` because `return_type := "->" type fallible?` requires a type and there is no unit type ([D134](01-decisions.md#d134)); `args: ...` because `param` has no variadic form ([D133](01-decisions.md#d133)); and a field with no parentheses because `s.len` in [02-syntax.md](02-syntax.md#strings) is a member access and not a call ([D136](01-decisions.md#d136)).

Every fenced ```do block in this document is, by contrast, **real doot**: it parses under the frozen grammar, and every construct in it means what it appears to mean under the resolver's and typechecker's rules. A name like `User` or `profile` stands for a declaration the surrounding prose supplies; a construct whose *meaning* would change because a name is free does not appear. That distinction is the fix for the defect that produced [D154](01-decisions.md#d154).

**Defaults.** A stdlib parameter may carry a default, under exactly the rules user code obeys: the value must be a constant expression (`DT0229`) and a defaulted parameter may not precede a required one (`DT0232`). A constant expression is a literal or a literal container, so `time.Duration` **cannot** be a default — `0.s` is a member access, not a literal. That is why no member below defaults a duration and why cache-control lifetimes are required arguments rather than optional ones.

---

## Errors

[D014](01-decisions.md#d014) settled that there is one universal `Error` with a `kind`, a `message`, a `cause` chain, and an automatically captured source location. [12-semantics.md](12-semantics.md#originating-an-error) settled the checker-side rule that makes any spelling of construction work — *in a fallible function, `return e` accepts either the declared return type or `Error`* — and left the construction entry point owed by the standard library. This is that entry point.

### Originating an `Error`

**An `Error` is built with a struct literal** ([D131](01-decisions.md#d131)):

```do
fn create(name: str) -> User! {
  if name.trim().is_empty() {
    return Error { kind: .validation, message: "a name is required" }
  }
  return db.one[User]("insert into users (name) values (?) returning *", name)!
}
```

No new syntax is involved, and that is the whole argument. `Error` is a predeclared prelude type ([12-semantics.md](12-semantics.md#the-prelude-and-the-module-table)); a struct literal is how every struct value in doot is built; `return e` in a fallible function already accepts an `Error`; and there is no other value-construction form available, because [D092](01-decisions.md#d092) removed the static method form that `Error.new(…)` would have needed.

A **cause chain** is built by naming the error you caught in the `cause` field:

```do
fn dashboard(user_id: int) -> Dashboard! {
  let widgets = db.all[Widget]("select * from widgets where user_id = ?", user_id) else err {
    return Error {
      kind:    .internal
      message: "loading the dashboard failed"
      cause:   err
    }
  }
  return build(widgets)
}
```

`cause` defaults to `nil`, so a literal that omits it is complete (`DT0221` requires only fields without defaults). Wrapping is therefore one field, chains are ordinary optional-struct recursion — `type Node { next: Node? }` is explicitly legal ([D087](01-decisions.md#d087)) — and there is no `wrap` function whose only job is to set a field.

**The source location is captured from the span of the literal itself.** It is not a field, so there is nothing to write and nothing to forget; the compiler fills it in at the same point it decides the literal's type, exactly as it injects a CSRF token into a `<form>` it can see ([D028](01-decisions.md#d028)). Naming `location` in a literal is `DT0222` — the field does not exist — and reading it is `err.location()`.

`Error` is the **only** prelude type that may be constructed with a literal. `Request`, `Response`, `redirect`, and `Upload` are opaque: each is produced by the members that produce it and refined by the methods it carries, because each is a handle on runtime state that a literal could put into an inconsistent shape. Naming any of them in a struct literal is a reserved diagnostic, `[DT0233]` ([Reserved diagnostics](#reserved-diagnostics)).

### `ErrorKind`

`ErrorKind` is a **tag-only enum of thirteen variants, closed and complete at v0.1** ([D132](01-decisions.md#d132)):

| Variant | Means | Typical producers |
| --- | --- | --- |
| `validation` | a value was rejected by a rule | `validate`, request binding, `json.decode` shape mismatch |
| `not_found` | something required is absent | `db.one` with no rows, `fs.read`, `static.file`, `time.Time.in_zone` |
| `conflict` | a uniqueness or state conflict | a `db` constraint violation, `db.one` with more than one row |
| `permission` | refused for access reasons | `fs` on a path the process may not touch, `static.dir` traversal |
| `timeout` | a deadline passed | `db` busy timeout, the v0.3 client |
| `unavailable` | a resource is temporarily unreachable | `db` locked past its retry, the v0.3 client's connect |
| `parse` | input was not in the expected form | `json`, `time.parse`, `url.parse`, `encode` decoders |
| `range` | a value is outside what the target can hold | `req.body` past the memory budget, `encode.hex_decode` on an odd length |
| `io` | an underlying read or write failed | `fs`, `os.run`, response flush |
| `unsupported` | the operation does not exist in this build or platform | `os.run` where no subprocess exists, `image` in v0.5 |
| `lagged` | a bounded buffer overflowed and the subscription closed | `topic` in v0.2 ([D026](01-decisions.md#d026)) |
| `closed` | the other end has ended | `chan` in v0.2 |
| `internal` | no better classification, including a wrapped lower-layer failure | anything, and every hand-written wrap |

A variant is spelled `snake_case` like every other enum variant ([D039](01-decisions.md#d039)), and is written `.not_found` wherever the expected type is known — which is every position that matters: a `kind` field initializer, a `match` arm on `err.kind`, an argument to a member declared `ErrorKind`.

Three of the thirteen belong to modules that do not land in v0.1, and they are here deliberately. `match err.kind { … }` with no `else` arm is exhaustive against a closed set (`DT0420`), so **adding a variant in v0.2 would turn a compiling program into a non-compiling one** — a breaking change, which [07-roadmap.md](07-roadmap.md#the-release-model) forbids at any version. The set must therefore be complete for every module the closed thirty-eight will ever contain, and it is the one part of the standard library that could not be specified module by module.

### `Error` values

```
kind:    ErrorKind
message: str
cause:   Error?          // default nil

err.location() -> str    // "routes/users.do:14:11", where the Error was built
err.chain() -> [Error]    // this error first, then each cause in turn
err.root() -> Error       // the last element of chain(); this error when cause is nil
```

`location` and `chain` are methods rather than fields because neither is an intrinsic O(1) read of a stored slot ([D136](01-decisions.md#d136)): a location is three values rendered into one string, and a chain is a walk.

`Error` carries no method that formats itself for a user. Rendering an error to a page is `on_error(err: Error) -> html` ([02-syntax.md](02-syntax.md#return-types)) and rendering it to a log is `log.error`, and both are the application's decision about how much to disclose.

### What raises a fault instead

A fault is a bug or a resource breach, it terminates the task, and user code cannot raise one ([D012](01-decisions.md#d012)). Every member below that faults does so for the same reason: the failure is not something a correct program handles, it is something a correct program does not do.

| Member | Faults when | Why not an error |
| --- | --- | --- |
| `str.slice`, `bytes.slice`, `[T].slice`, `[T].insert` | the range is out of bounds, or a `str` bound is not on a character boundary | [D012](01-decisions.md#d012) already argued this: the checked forms are `s.char_at`, `xs.get`, `b.get`, and a comparison against `len`, and making every access optional would put noise on every line |
| `str.split` | the separator is empty | there is no answer to give, and the separator is the caller's own literal |
| `str.from_float_fixed` | `decimals` is negative | same shape |
| `str.repeat`, `bytes.repeat`, `list.repeat` | the count is negative | same shape |
| `list.range_by` | the step is zero | there is no list to produce, and the step is a literal at nearly every call |
| `math.abs` | the operand is the most negative `int`, whose magnitude is not an `int` | it is arithmetic ([D003](01-decisions.md#d003)); `math.checked_sub(0, n)` is the value-returning form |
| `math.pow_int` | the result overflows `int`, or the exponent is negative | likewise, with `math.checked_pow` |
| `map.from_lists` | the two lists have different lengths | a caller that built two lists of different lengths has a bug, and both lengths are its own |
| `bytes.of` | a value is outside `0`–`255` | same shape: the caller supplied the values |
| `rand.int`, `crypto.random_int` | `low` is not below `high` | an inverted range is a bug, not an input |
| `crypto.encrypt`, `crypto.decrypt` | the key is not exactly 32 bytes | a key length is a property of the program's configuration, not of the message |
| `http.status`, `http.error` | the code is outside 100–599, or below 400 for `error` | a status is a literal at every call site |
| `time.Time.format` | the format string holds an unknown `%` directive | a format string is a literal in practice, so this is a typo in the program rather than a value from a request ([D152](01-decisions.md#d152)) |
| every `test` member | called outside a test task ([D151](01-decisions.md#d151)) | there is nothing to record against, and reaching one from a handler is a bug |
| `test.expect_error` | its body succeeds | a test asserting that something fails has failed to test it |

`math.wrap_add`, `math.wrap_sub`, and `math.wrap_mul` are total and appear nowhere above, which is their entire purpose ([D003](01-decisions.md#d003)), and the `checked_*` family is the value-returning complement for every operation that can overflow.

**Nothing in the library faults on data that arrived in a request.** That is the line, and it is worth stating as a property rather than leaving as a pattern: a request body, a file, a decoded document, and a URL all produce an `Error`, and the program's own arithmetic, indexing, literals, and configuration produce faults.

---

## Builtin members on builtin types

The overview says it plainly — "`str` — string statics; methods live on `str` values" — and the same split holds for `[T]`, `{K: V}`, `bytes`, `time.Time`, `time.Duration`, `Error`, `Request`, `Response`, `redirect`, `Upload`, `url.Url`, `cookie.Cookie`, `fs.Info`, and `os.Output`. A module namespace holds what cannot be reached from a value — constructors, statics, and free functions — and everything that has a receiver is a method on the receiver.

This looks like it collides with two locked rules, and it does not ([D136](01-decisions.md#d136)):

- **[Rule 9](03-grammar.md#well-formedness-rules) requires `self` as a method's first parameter**, and `DT0034` is registered and tested.
- **[D092](01-decisions.md#d092) removes the static method form**, so a member access on a type name resolves to an enum variant and to nothing else.

Both are rules about **declarations in a doot source file**. A builtin member has no declaration: it is a row in the module signature table, the compiler resolves a member access against the receiver's type by consulting that table, and the emitter compiles the call to an opcode or a native call rather than to a doot function entry. Rule 9 governs what a user may write; the table governs what already exists. Nothing about `s.upper()` is expressible as a doot declaration, which is exactly [D092](01-decisions.md#d092)'s point in the other direction: **user code may not attach a method to a stdlib type** (`DT0107`), so the method set on `str` is closed, complete, and knowable from this document alone. That is what makes "where is this method defined" answerable without opening the project — the property [D016](01-decisions.md#d016) exists for.

The consequence for a reader: `s.upper()` and `u.display()` look the same and are the same at the call site, and their *declarations* differ because only one of them has a declaration. Nothing about the call site changes, and there is no static form on either side.

**Fields versus methods.** A builtin **field** is an intrinsic value the runtime already has and can return with no work: `s.len`, `s.char_count`, `xs.len`, `m.len`, `b.len`, and `Error`'s three. Everything else is a method, written with parentheses. The rule is worth stating because [02-syntax.md](02-syntax.md#strings) writes `s.len` without parentheses and `s.upper()` with them, and a reader needs to know which way a new member goes. `char_count` is a field despite costing a walk of the string, because a `str` caches it: it is stored, not computed.

---

## Core data

### `str`

`str` is immutable UTF-8 and is not indexable by integer ([02-syntax.md](02-syntax.md#strings)); `s[i]` is `DT0209`. No member mutates, so no member requires a `var` receiver, and `let` bindings of strings are unrestricted.

**Module members.** Constructors and statics only:

```
str.from_int(n: int) -> str
str.from_float(x: float) -> str
str.from_float_fixed(x: float, decimals: int) -> str
str.from_money(minor_units: int, decimals: int) -> str
str.from_bool(value: bool) -> str
str.join(parts: [str], separator: str) -> str
str.repeat(s: str, times: int) -> str
```

`from_float` writes the shortest representation that round-trips; `from_float_fixed` writes exactly `decimals` digits after the point, half away from zero, and faults on a negative `decimals`. `from_money` is the helper [D020](01-decisions.md#d020) owes: `str.from_money(1999, 2)` is `"19.99"` and `str.from_money(-5, 2)` is `"-0.05"`. It emits no currency symbol, no thousands separator, and no locale-dependent decimal mark ([D155](01-decisions.md#d155)).

**Value members.**

```
s.len: int                                   // bytes
s.char_count: int                            // characters

s.is_empty() -> bool
s.upper() -> str
s.lower() -> str
s.trim() -> str
s.trim_start() -> str
s.trim_end() -> str
s.starts_with(prefix: str) -> bool
s.ends_with(suffix: str) -> bool
s.contains(needle: str) -> bool
s.index_of(needle: str) -> int?              // byte offset
s.last_index_of(needle: str) -> int?
s.slice(start: int, end: int) -> str         // byte offsets, half-open; faults off a boundary
s.char_at(index: int) -> str?                // by character, not byte
s.chars() -> [str]
s.lines() -> [str]
s.split(separator: str) -> [str]
s.replace(from: str, to: str) -> str
s.replace_first(from: str, to: str) -> str
s.pad_start(width: int, fill: str) -> str
s.pad_end(width: int, fill: str) -> str
s.to_int() -> int?
s.to_float() -> float?
```

`to_int` and `to_float` return an optional rather than an `Error`, because "this text is not a number" needs no message, no kind, and no cause — it needs a default, and `else` supplies one. A value that overflows `int` is also `nil`; a program that must distinguish the two cases is parsing, and `str.to_int` is not a parser.

`contains` and the `in` operator are the same test, and `in` is the spelling [12-semantics.md](12-semantics.md#operators) gives; `contains` exists so that a method chain does not have to break out of postfix position to ask.

### `list`

**Module members.** Only what a literal cannot express:

```
list.range(start: int, end: int) -> [int]              // half-open
list.range_by(start: int, end: int, step: int) -> [int]
list.repeat[T](value: T, times: int) -> [T]
```

`range_by` yields an empty list when the step points away from `end`, and faults on a zero step. `list.repeat`'s `T` is inferred from `value`.

**Value members.** `[T]` is the one builtin container with mutating methods, because a `var` list is uniquely owned and `xs.push(x)` mutates in place ([D008](01-decisions.md#d008)).

```
xs.len: int

xs.is_empty() -> bool
xs.get(index: int) -> T?
xs.first() -> T?
xs.last() -> T?
xs.slice(start: int, end: int) -> [T]                 // faults out of range
xs.index_of(value: T) -> int?
xs.contains(value: T) -> bool
xs.concat(other: [T]) -> [T]
xs.reversed() -> [T]
xs.sorted() -> [T]                                    // T must be int, float, or str
xs.sorted_by(less: fn(T, T) -> bool) -> [T]
xs.map[U](transform: fn(T) -> U) -> [U]
xs.filter(keep: fn(T) -> bool) -> [T]
xs.find(match: fn(T) -> bool) -> T?
xs.any(match: fn(T) -> bool) -> bool
xs.all(match: fn(T) -> bool) -> bool
xs.each(body: fn(T))

xs.push(value: T)                                     var
xs.pop() -> T?                                        var
xs.insert(index: int, value: T)                       var, faults out of range
xs.remove_at(index: int) -> T?                        var
xs.extend(other: [T])                                 var
xs.clear()                                            var
xs.sort()                                             var, T must be int, float, or str
xs.sort_by(less: fn(T, T) -> bool)                    var
```

`sorted`/`sort` and `reversed` are the pattern the whole library follows: **a past participle returns a new value and a bare imperative mutates** ([D137](01-decisions.md#d137)). Both spellings exist for sorting because both are wanted — a `let` list sorted into a new binding, and a `var` list sorted where it lies — and the naming is the only thing that says which is which. Sorting is stable in both forms, and `sorted`/`sort` on a `T` that is not `int`, `float`, or `str` is the reserved constraint diagnostic `[DT0216]`; `sorted_by` is the form that works for any `T`.

There is no `reduce` and no `fold`. Either would need a second type parameter that no argument determines, so its type argument would always have to be written out, and `for` over a `var` accumulator is both shorter and the idiom [D008](01-decisions.md#d008) already pushes every program toward.

`map`'s `U` is inferred from the function argument: a lambda with declared parameter types and an undeclared return type is inferrable on its own ([D093](01-decisions.md#d093)), so `users.map(fn(u: User) => u.name)` binds `U` to `str` with nothing written ([D135](01-decisions.md#d135)).

### `map`

**Key types.** A map key must be `int`, `bool`, `str`, `bytes`, or an enum ([D139](01-decisions.md#d139)). `float` is excluded because `nan != nan` makes a float key unfindable, and a struct, list, or map key is excluded because hashing a container is a decision with no v0.1 consumer. A key type outside the set is the reserved `[DT0217]`.

**Iteration is insertion order**, for `keys()`, `values()`, and `for k, v in m`. A hash order that varied between runs would make output non-reproducible and would make a spec test's expected bytes depend on the allocator, which is not something a suite may depend on.

**Module members.**

```
map.from_lists[K, V](keys: [K], values: [V]) -> {K: V}   // faults on a length mismatch
map.invert[K, V](m: {K: V}) -> {V: K}                     // V must be a key type
```

Two, both unexpressible with a literal, both with their type arguments inferred from their arguments. A later duplicate key wins in both, which is the same rule a literal follows.

**Value members.**

```
m.len: int

m.is_empty() -> bool
m.get(key: K) -> V?
m.keys() -> [K]
m.values() -> [V]
m.merged(other: {K: V}) -> {K: V}                        // other wins on a collision

m.set(key: K, value: V)                                  var
m.remove(key: K) -> V?                                   var
m.extend(other: {K: V})                                  var
m.clear()                                                var
```

`m[k]` faults on a missing key and `m.get(k)` returns `V?`, which is [D012](01-decisions.md#d012)'s trade restated by [12-semantics.md](12-semantics.md#iteration). Membership is the `in` operator; there is no `has`, because two spellings of one test is the thing [goal 1](00-vision.md#the-nine-goals) rules out.

### `bytes`

Immutable, so no member takes a `var` receiver. `b[i]` yields an `int` in `0`–`255`.

```
bytes.of(values: [int]) -> bytes            // faults on a value outside 0-255
bytes.concat(parts: [bytes]) -> bytes
bytes.repeat(value: int, times: int) -> bytes

b.len: int
b.is_empty() -> bool
b.get(index: int) -> int?
b.slice(start: int, end: int) -> bytes      // faults out of range
b.index_of(needle: bytes) -> int?
b.starts_with(prefix: bytes) -> bool
b.ends_with(suffix: bytes) -> bool
b.contains(needle: bytes) -> bool
```

Conversion to and from `str` is `as`, not a member: `s as bytes` is total and `b as str` yields `str?` ([D089](01-decisions.md#d089)). A `bytes.to_str()` beside it would be a second spelling of a closed table's row.

### Unit suffixes on `int`

`15.s`, `250.ms`, `2.h`, `7.days`, and `16.mb` are in [02-syntax.md](02-syntax.md#configuration) and [02-syntax.md](02-syntax.md#uploads), so they are fixed. They are **fields on `int`**, not calls: `15.s` is `INT` followed by `.` followed by `IDENT` under [03-grammar.md](03-grammar.md#identifiers-and-literals), because `FLOAT` requires a digit after the point. The complete set ([D138](01-decisions.md#d138)):

```
n.ns: time.Duration        n.kb: int
n.us: time.Duration        n.mb: int
n.ms: time.Duration        n.gb: int
n.s: time.Duration
n.min: time.Duration
n.h: time.Duration
n.days: time.Duration
n.weeks: time.Duration
```

`kb`, `mb`, and `gb` are **powers of 1024**, because what they configure is a memory budget ([D005](01-decisions.md#d005)) and a memory budget is counted in binary units. `16.mb` is `16777216`.

`min` rather than `m` for minutes: `m` beside `mb` and `ms` reads as a truncation of either. The mixture of short and long names — `s`, `ms`, `h`, but `days`, `weeks` — is inherited from the documented examples rather than chosen, and rather than renaming `2.h` to `2.hours` this document keeps what the language's reference already shows.

### `math`

Every operator in doot is monomorphic ([12-semantics.md](12-semantics.md#operators)) and there is no overloading, so a `math` member that applies to both `int` and `float` needs two names. **The `int` form takes the unsuffixed name and the `float` form takes a `_float` suffix** ([D141](01-decisions.md#d141)).

```
math.pi: float
math.infinity: float

math.abs(n: int) -> int
math.min(a: int, b: int) -> int
math.max(a: int, b: int) -> int
math.clamp(n: int, low: int, high: int) -> int
math.sign(n: int) -> int
math.pow_int(base: int, exponent: int) -> int

math.checked_add(a: int, b: int) -> int?
math.checked_sub(a: int, b: int) -> int?
math.checked_mul(a: int, b: int) -> int?
math.checked_pow(base: int, exponent: int) -> int?
math.wrap_add(a: int, b: int) -> int
math.wrap_sub(a: int, b: int) -> int
math.wrap_mul(a: int, b: int) -> int

math.abs_float(x: float) -> float
math.min_float(a: float, b: float) -> float
math.max_float(a: float, b: float) -> float
math.clamp_float(x: float, low: float, high: float) -> float
math.floor(x: float) -> float
math.ceil(x: float) -> float
math.round(x: float) -> float
math.truncate(x: float) -> float
math.sqrt(x: float) -> float
math.pow(base: float, exponent: float) -> float
math.exp(x: float) -> float
math.log(x: float) -> float
math.log10(x: float) -> float
math.log2(x: float) -> float
math.sin(x: float) -> float
math.cos(x: float) -> float
math.tan(x: float) -> float
math.atan2(y: float, x: float) -> float
math.is_nan(x: float) -> bool
math.is_infinite(x: float) -> bool
```

The two families that only exist for `int` are the point of the module. **`wrap_*` is what [D003](01-decisions.md#d003) promised**: explicit wraparound for hashes and checksums, total, never faulting. **`checked_*` is its complement**: the same arithmetic as `+`, `-`, `*`, and `math.pow_int`, returning `nil` instead of faulting, so a program that genuinely does not know whether a sum fits can ask rather than crash. Without them, [D003](01-decisions.md#d003)'s checked arithmetic would leave a program with a fault as its only option; with them, `else` handles the case.

`math.round` is half away from zero, stated because "round" names three different functions in three languages. `math.log` is the natural logarithm. `math.sqrt` of a negative yields NaN rather than faulting, because IEEE 754 defines it and `math.is_nan` reads the result; `math.log` of zero yields negative infinity for the same reason. Faulting there would put a value the hardware defines behind an error path.

`math.abs(n)` on the most negative `int` faults, because its magnitude is not an `int` — the one place `abs` is not total, and `math.checked_sub(0, n)` is how a program asks without faulting.

### `time`

`time.Time` is a nanosecond instant and `time.Duration` is a signed nanosecond span ([04-stdlib.md](04-stdlib.md#time)). Both are opaque: neither is literal-constructible, and a `time.Duration` is written with a unit suffix.

```
time.now() -> time.Time
time.monotonic() -> time.Duration                     // since process start; unaffected by clock changes
time.from_unix(seconds: int) -> time.Time
time.from_unix_nanos(nanoseconds: int) -> time.Time
time.parse(format: str, text: str) -> time.Time!
time.parse_rfc3339(text: str) -> time.Time!
time.zone_exists(name: str) -> bool
```

```
t.unix() -> int
t.unix_nanos() -> int
t.format(format: str) -> str
t.format_rfc3339() -> str
t.add(d: time.Duration) -> time.Time
t.diff(other: time.Time) -> time.Duration             // t - other
t.before(other: time.Time) -> bool
t.after(other: time.Time) -> bool
t.in_zone(name: str) -> time.Time!
t.year() -> int
t.month() -> int                                      // 1-12
t.day() -> int                                        // 1-31
t.hour() -> int
t.minute() -> int
t.second() -> int
t.nanosecond() -> int
t.weekday() -> int                                    // 1 is Monday, 7 is Sunday
t.day_of_year() -> int
t.start_of_day() -> time.Time
```

```
d.nanoseconds() -> int
d.milliseconds() -> int
d.seconds() -> float
d.minutes() -> float
d.hours() -> float
d.days() -> float
d.add(other: time.Duration) -> time.Duration
d.diff(other: time.Duration) -> time.Duration
d.negated() -> time.Duration
d.format() -> str                                     // "1h30m", "250ms", "-2s"
```

`d.negated()` exists because unary `-` applies to `int` and `float` only ([12-semantics.md](12-semantics.md#operators)), and a signed span with no way to flip its sign would be a gap. Two `time.Time` values compare with `==` by content ([D009](01-decisions.md#d009)); ordering is `before` and `after` rather than `<`, because `<` is defined for `int`, `float`, and `str` and adding a type to that table is not this document's to do.

**Zones.** `UTC` and `local` always exist. Any other name resolves against the platform's zone database and fails with `not_found` when it is absent, which is why `time.zone_exists` is a member: a program that offers a user a zone list needs to ask before it converts. A `time.Time` is an instant, so `in_zone` changes what `year()`, `hour()`, and `format` report and changes nothing about `unix()`.

**Format directives** are a closed set ([D152](01-decisions.md#d152)). An unknown directive faults, because a format string is a literal in every real program and an unknown directive is therefore a typo in the source rather than a value from a request.

| Directive | Means | Accepted by `time.parse` |
| --- | --- | --- |
| `%Y` | four-digit year | yes |
| `%m` | two-digit month | yes |
| `%d` | two-digit day | yes |
| `%H` | two-digit hour, 00–23 | yes |
| `%M` | two-digit minute | yes |
| `%S` | two-digit second | yes |
| `%L` | three-digit millisecond | yes |
| `%N` | nine-digit nanosecond | yes |
| `%z` | numeric offset, `+0000` | yes |
| `%Z` | zone name | no |
| `%a` | abbreviated weekday, English | yes |
| `%A` | full weekday, English | yes |
| `%b` | abbreviated month, English | yes |
| `%B` | full month, English | yes |
| `%p` | `AM` or `PM` | yes |
| `%%` | a literal `%` | yes |

Weekday and month names are **English only**, and that is a decision rather than an omission: a locale database is a large surface that no other part of doot has, and a page that needs a translated month has a translation table of its own. `%Z` is not accepted by `time.parse` because a zone abbreviation is ambiguous — `CST` names three zones — and guessing would silently shift an instant by hours. `time.parse` fails with `parse` when the text does not match, and with `range` when a matched field is out of range, such as month 13.

Storage in SQLite is INTEGER nanoseconds ([12-semantics.md](12-semantics.md#parameters)), so ordering and range queries work with no format convention.


---

## Serialization

### `json`

```
json.encode[T](value: T) -> str
json.encode_pretty[T](value: T) -> str
json.decode[T](text: str) -> T!
json.parse(text: str) -> any!
```

**What constrains `T`.** One constraint, called **JSON-representable**, used in three places ([D142](01-decisions.md#d142)):

- `int`, `float`, `bool`, `str`;
- an enum, as its variant name in `snake_case`;
- a struct declared in this program, every one of whose fields is JSON-representable;
- `[T]` and `{str: V}` for JSON-representable `T` and `V`;
- `T?`, which encodes `nil` as `null` and accepts `null`;
- `any`, for `json.encode` only.

`bytes` and `html` are **not** representable: `bytes` has no agreed JSON spelling and choosing one silently — base64 by convention — would make `encode` and `decode` disagree with every consumer that chose differently, so a program that wants base64 writes `encode.base64` and gets a `str` field. `html` is excluded because a JSON document is not a page and putting escaped markup into an API response is the mistake the type exists to prevent. A `T` outside the constraint is the reserved `[DT0216]`, whose message names the offending field and the type it holds.

The same constraint governs a route whose return type is a struct, a `[T]`, or a `{K: V}` ([02-syntax.md](02-syntax.md#return-types)) and the response builder `http.json_body`, so there is one rule and one diagnostic rather than three.

**Encoding cannot fail.** The type argument is checked at compile time, so at runtime there is nothing left to refuse; a NaN or infinite `float` encodes as `null`, which is the only representable choice and is stated here rather than discovered. That is why `json.encode` carries no `!` — a fallible encoder would put an `else` on every JSON response for a failure that cannot occur.

**Decoding fails** with `parse` when the text is not JSON and with `validation` when the text is JSON of the wrong shape — a missing field with no default, a string where an `int` belongs, a variant name the enum does not have. The message names the path, `"user.address.zip"`, because a shape mismatch three levels down is unactionable without one. Field matching is by name; a field the struct does not declare is **ignored**, because an API that adds a field must not break a client that has not been recompiled, and that asymmetry with `db`'s `DT0146` is deliberate: a database schema is inside the program and a remote document is not.

`json.parse` is where `any` comes from ([02-syntax.md](02-syntax.md#types)). Navigating the result means casting — `any` supports `as` and nothing else ([D089](01-decisions.md#d089)):

```do
fn first_tag(body: str) -> str? {
  let doc = json.parse(body) else {
    return nil
  }
  let fields = doc as {str: any} else {
    return nil
  }
  let tags = fields.get("tags") else {
    return nil
  }
  let list = tags as [any] else {
    return nil
  }
  let first = list.first() else {
    return nil
  }
  return first as str
}
```

That is verbose, and it is verbose on purpose: every line is a place the document could have been shaped differently, and `json.decode[T]` into a declared struct is the spelling for a document whose shape is known.

### `encode`

```
encode.base64(b: bytes) -> str
encode.base64_decode(text: str) -> bytes!
encode.base64_url(b: bytes) -> str
encode.base64_url_decode(text: str) -> bytes!
encode.hex(b: bytes) -> str
encode.hex_decode(text: str) -> bytes!
encode.url(text: str) -> str
encode.url_decode(text: str) -> str!
```

`base64` emits padding and `base64_url` does not, which is what the two forms are for; both decoders accept padding and its absence. `hex` emits lowercase and `hex_decode` accepts either case, failing with `range` on an odd length and `parse` on a non-hex byte. `encode.url` percent-encodes everything outside the unreserved set, so it is correct for a path segment and for a query value both; `url_decode` fails with `parse` on a truncated or non-hex escape and treats `+` as a space, because the only place a doot program decodes by hand is a form body.

Every decoder is fallible and no encoder is, which is the shape of the whole module: encoding a byte sequence is total, and decoding text that arrived from somewhere else is not.

---

## Web

### `html`

```
html.raw(text: str) -> html
html.fragment(parts: [html]) -> html
html.empty() -> html
html.doc(head: html, body: html) -> html
```

Four members, and **`html.raw` is the only one that takes a `str` and returns `html`** — which is not an observation about this module but a property [D085](01-decisions.md#d085) and [12-semantics.md](12-semantics.md#the-prelude-and-the-module-table) state about the whole signature table, because it is what makes `grep raw(` a complete XSS audit ([D021](01-decisions.md#d021), [D096](01-decisions.md#d096)). **No member added by this document takes a `str` and returns `html`**, in any module, and that is a constraint this document was written under rather than a coincidence.

It cost one member. An escaping counterpart to `raw` — `html.text(str) -> html`, for assembling a `[html]` out of strings — would have been convenient and would have made the stated property false, so it is absent, and the spelling is a markup literal, where text is escaped by construction:

```do
fn crumbs(names: [str]) -> html {
  return <nav>
    {for name in names}
      <span>${name}</span>
    {end}
  </nav>
}
```

`html.doc(head, body)` emits `<!doctype html><html><head>…</head><body>…</body></html>`. It is a convenience over a markup literal and nothing more; the composition mechanism is a function returning `html` ([D023](01-decisions.md#d023)).

**`html.attrs` is not a member** ([D145](01-decisions.md#d145)). The overview listed `html.attrs({str: str}) -> html`, and there is no position in the grammar that accepts an `html` value as an attribute: [03-grammar.md](03-grammar.md#markup) spells that feature `...expr` over a `{str: str}`, and [D096](01-decisions.md#d096) types it. The feature exists; the member was a second, unusable spelling of it.

```do
fn field(extra: {str: str}) -> html {
  return <input name="email" ...extra/>
}
```

### `http`

The response-construction half is v0.1. Three client entry points are listed at the end with their signatures and marked v0.3, because the module table carries a version per member and reaching one before it lands is `DT0046`.

**Status control requires a `Response`** ([D144](01-decisions.md#d144)). A route declared `-> html!` returns `html`, and `html` is not `Response`; there is no widening ([D088](01-decisions.md#d088)). So a handler that can answer 200 with a page *and* 404 with something else is declared `-> Response!`, and a handler that always answers 200 keeps the simpler return type. That is a real constraint on how a handler is written and it is stated rather than papered over:

```do
route GET "/users/:id" (id: int) -> Response! {
  let u = db.find[User]("select * from users where id = ?", id)! else {
    return http.not_found()
  }
  return http.html(profile(u))
}
```

**Constructors.**

```
http.html(body: html) -> Response
http.text(body: str) -> Response
http.bytes(body: bytes, content_type: str) -> Response
http.json_body[T](value: T) -> Response
http.status(code: int) -> Response                 // empty body
http.no_content() -> Response                      // 204

http.bad_request() -> Response                     // 400
http.unauthorized() -> Response                    // 401
http.forbidden() -> Response                       // 403
http.not_found() -> Response                       // 404
http.conflict() -> Response                        // 409
http.unprocessable() -> Response                   // 422
http.too_many_requests() -> Response               // 429
http.error(code: int) -> Response                  // any code of 400 or more

http.see_other(location: str) -> redirect          // 303
http.found(location: str) -> redirect              // 302
http.moved_permanently(location: str) -> redirect  // 301

http.status_text(code: int) -> str
```

`http.json_body[T]`'s `T` is the JSON-representable constraint and is inferred from `value`. Its name is not `http.json` for one reason: [04-stdlib.md](04-stdlib.md#the-two-load-bearing-modules) already gives `http.json[T](url) -> T!` to the client, and one module cannot have two members of one name. Given the collision, naming the v0.1 member around the v0.3 one is the choice that leaves the documented signature alone.

`http.status(code)` accepts any code from 100 to 599 and faults outside that, because a status outside the range is a program error. `http.error(code)` faults below 400 — it exists to name a failure, and `http.error(200)` is a mistake worth catching. The named helpers are the codes a doot application actually returns; every other code is `http.status`.

**`Response` values.** Opaque, immutable, and refined by `with_*` methods that each return a new `Response` ([D156](01-decisions.md#d156)), so a `let` binding of one is unrestricted:

```
r.status: int

r.header(name: str) -> str?
r.with_status(code: int) -> Response
r.with_header(name: str, value: str) -> Response
r.with_content_type(value: str) -> Response
r.with_cookie(c: cookie.Cookie) -> Response
```

Header names are matched case-insensitively and emitted in the case they were given. There is no `Response` body accessor: a response under construction is write-only, and a program that wants the bytes it is about to send has them already.

**`redirect` values.**

```
rd.location: str
rd.status: int

rd.with_cookie(c: cookie.Cookie) -> redirect
```

`redirect` is a distinct type rather than a `Response` because [02-syntax.md](02-syntax.md#return-types) makes it a distinct return type, and because it is the one response a handler produces where the body is never the point. `with_cookie` is on it because setting a cookie and redirecting is one of the two most common things a `POST` handler does.

**`Request` values.** `req` is predeclared in a route body, a stream body, and a hook ([12-semantics.md](12-semantics.md#request-binding)); referring to it anywhere else is `DT0522`.

```
req.id() -> str                         // the correlation id log lines carry
req.method() -> str
req.url() -> str
req.path() -> str
req.host() -> str
req.scheme() -> str                     // as the dashboard forwarded it
req.header(name: str) -> str?
req.headers() -> {str: str}
req.remote_address() -> str
req.content_type() -> str?
req.content_length() -> int?
req.body() -> bytes!                    blocking
req.body_text() -> str!                 blocking
```

`req.body()` is fallible and blocking: the body may still be arriving, the task suspends while it does, and the read fails with `io` on a dropped connection, `timeout` on an idle one, and `range` on a body past the request's memory budget ([D005](01-decisions.md#d005)). It may be read once; a second call fails with `internal`, because the bytes are not retained. A route with a `form`, `query`, or `json` parameter has already had its body consumed by binding, which is the normal path and needs none of this.

`req.scheme()` reports what the dashboard forwarded, because the runtime never terminates TLS ([D011](01-decisions.md#d011)) and therefore never knows on its own.

**The client, v0.3.** Listed for the table's sake; the behaviour is [D031](01-decisions.md#d031)'s pass, not this one.

```
http.get(url: str) -> Response!                    // v0.3
http.post(url: str, body: bytes) -> Response!      // v0.3
http.json[T](url: str) -> T!                       // v0.3
```

`http.json[T]`'s `T` is the same JSON-representable constraint `json.decode[T]` uses, and it must be **written**, because it appears only in the result ([D135](01-decisions.md#d135)). Its failures are the union of the client's — `timeout`, `unavailable`, `io` — and `json.decode`'s.

The overview lists a fourth, `http.request(Request) -> Response!`, and its parameter cannot be the prelude's `Request`: that type is the *inbound* request, it is opaque, and nothing constructs one ([Corrections to the overview](#corrections-to-the-overview)). An outbound request needs a value a program can build — method, URL, headers, body, timeout, retry policy — and deciding its shape is the client's own pass, where the retry and streaming semantics that shape it are also decided. It is named here so the gap is on the record and is not mistaken for the type of the same name.

### `url`

```
url.parse(text: str) -> url.Url!
url.join(base: str, reference: str) -> str!
url.encode_query(params: {str: str}) -> str
url.decode_query(text: str) -> {str: [str]}
```

```
u.scheme: str
u.host: str
u.path: str

u.port() -> int?
u.query() -> {str: [str]}
u.query_value(name: str) -> str?           // the first value
u.fragment() -> str?
u.with_path(path: str) -> url.Url
u.with_query(params: {str: str}) -> url.Url
u.to_str() -> str
```

`url.parse` fails with `parse` on anything it cannot decompose and does not attempt to guess a scheme: `"example.com/x"` is a relative reference and is refused, because a program that guessed would build links to the wrong host. `url.join` implements reference resolution against a base, so `url.join("https://example.com/a/b", "../c")` is `"https://example.com/c"`, and fails with `parse` when the base is not absolute.

`decode_query` returns `{str: [str]}` because a query string may repeat a key and dropping the repeats silently is how a multi-select form loses data. `query_value` is the single-value read for the common case. `encode_query` takes `{str: str}` and emits keys in the map's insertion order ([D139](01-decisions.md#d139)), which makes a built URL byte-stable and therefore cacheable and testable.

`scheme`, `host`, and `path` are fields because a parsed `Url` stores them; `port` is a method because it is optional and derived from the authority.

### `form`

Request binding by reserved parameter name is the normal path and needs no module ([D025](01-decisions.md#d025)). This module is the imperative remainder: a body whose shape is not known until it is read, and uploads reached without declaring a struct.

```
form.values() -> {str: [str]}!             blocking
form.value(name: str) -> str?!             blocking
form.upload(name: str) -> Upload?!         blocking
form.uploads() -> [Upload]!                blocking
```

All four read the request body, so all four are blocking and fallible, and all four fail exactly as `req.body()` does plus `parse` on a body that is neither `application/x-www-form-urlencoded` nor `multipart/form-data`. The body is parsed once and cached for the request, so calling `form.value` three times parses once — which is why these are `form`'s members and not `Request`'s: the cache belongs to the parse, not to the request.

`form.value` returns `str?!`: the read can fail, and the field can be absent. `f()!` yields `str?` and `f()! else ""` yields `str`, and `else err` on the unhandled combination is `DT0406` ([D094](01-decisions.md#d094)).

**`Upload` values.** Produced by binding a struct field of type `Upload` ([02-syntax.md](02-syntax.md#uploads)) or by `form.upload`. A body streams to disk rather than into the request arena, so an upload is not charged against the memory budget ([05-runtime.md](05-runtime.md#budgets)).

```
up.field_name: str
up.file_name: str
up.content_type: str
up.size: int

up.save_to(path: str) -> ()!               blocking
up.bytes() -> bytes!                       blocking
up.text() -> str!                          blocking
```

`file_name` is the client's name with any directory component stripped and is **not** safe to use as a path — it arrived in a request. `save_to` creates parent directories, refuses to overwrite an existing file with `conflict`, fails with `permission` and `io` as `fs` does, and is the reason `uuid.new()` appears in the documented example. `bytes()` reads the whole upload into the arena and is charged against the memory budget, failing with `range` when it does not fit; `text()` adds a UTF-8 check and fails with `parse`.

### `cookie`

Reading is from `req`; writing is onto a `Response` or a `redirect`, because a cookie is a response header and there is no mutable request-scoped bag to put one in ([D008](01-decisions.md#d008)).

```
cookie.get(name: str) -> str?
cookie.all() -> {str: str}
cookie.get_signed(name: str) -> str?
cookie.new(name: str, value: str) -> cookie.Cookie
cookie.signed(name: str, value: str) -> cookie.Cookie
cookie.remove(name: str) -> cookie.Cookie
```

```
c.name: str

c.with_value(value: str) -> cookie.Cookie
c.with_path(path: str) -> cookie.Cookie
c.with_domain(domain: str) -> cookie.Cookie
c.with_max_age(d: time.Duration) -> cookie.Cookie
c.with_secure(on: bool) -> cookie.Cookie
c.with_http_only(on: bool) -> cookie.Cookie
c.with_same_site(value: cookie.SameSite) -> cookie.Cookie
```

`cookie.SameSite` is `enum { strict, lax, none }`.

**Defaults, which are the security posture:** `path` is `/`, `http_only` is `true`, `same_site` is `.lax`, `secure` is `true` unless the process is in development mode, and `max_age` is unset, meaning a session cookie. Every one of them is the safe value, and every one is overridable by a `with_*` method that says so in the source. `with_secure(false)` in production is legal and visible, which is the right trade: a default that cannot be overridden gets worked around invisibly.

`cookie.signed` and `cookie.get_signed` use HMAC over the same key CSRF uses ([D028](01-decisions.md#d028)). **`get_signed` returns `str?` and does not distinguish a missing cookie from a tampered one** ([D153](01-decisions.md#d153)): both are `nil`. An `Error` that said "signature invalid" would be an oracle, telling an attacker that the cookie name is right and only the signature is wrong, and no correct program branches differently on the two cases.

`cookie.remove(name)` returns a cookie with an empty value and a zero max-age, which is how a cookie is cleared; there is no `delete`, because deleting a cookie is setting one.

### `mime`

```
mime.from_extension(extension: str) -> str?
mime.from_path(path: str) -> str?
mime.from_bytes(b: bytes) -> str?
mime.extension_for(content_type: str) -> str?
mime.is_text(content_type: str) -> bool
```

A built-in table covers the types a web application serves and receives; an unknown extension is `nil` rather than `"application/octet-stream"`, so the caller decides what unknown means. `from_bytes` sniffs a magic prefix and recognizes only formats whose prefix is unambiguous — never text, because sniffing text is how a browser gets tricked into executing it. `extension_for` returns the canonical extension without a leading dot.

### `static`

```
static.file(path: str, max_age: time.Duration) -> Response!            blocking
static.dir(root: str, relative: str, max_age: time.Duration) -> Response!   blocking
```

```do
route GET "/static/*rest" (rest: str) -> Response! {
  return static.dir("static", rest, 1.h)!
}
```

`static.dir` is the pair to a wildcard route. It **refuses any `relative` that escapes `root`** — `..` segments, an absolute path, a symbolic link pointing out — with `permission` rather than `not_found`, because the two are different mistakes and only one of them is an attack. A missing file is `not_found`, a directory is `not_found` rather than a listing, and an unreadable file is `permission`.

Both members set `Content-Type` from `mime.from_path`, `Content-Length`, `Last-Modified`, and a strong `ETag` derived from the file's size and modification time, and both answer `304` to a matching `If-None-Match` or `If-Modified-Since` without reading the file. `max_age` becomes `Cache-Control: public, max-age=N`, and `0.s` means `no-cache`, which still permits the validators above. It is a required argument rather than a defaulted one because a default cannot be a duration ([How to read a signature](#how-to-read-a-signature)) and because a caching lifetime is a decision that should appear in the source.

There is no asset pipeline, no fingerprinting, and no bundling ([04-stdlib.md](04-stdlib.md#explicitly-not-in-the-standard-library)). A fingerprinted URL is a name the application chooses and passes to `static.dir` like any other.


---

## Data

### `db`

The compile-time half of this module is [12-semantics.md](12-semantics.md#the-schema-checker) and is not restated here: SQL must be a single literal statement with positional `?` placeholders, result columns map to fields by name in both directions, nullability is decided conservatively, and every failure of any of that is a compile error with a span. What is settled here is the surface.

```
db.one[T](sql: str, args: ...) -> T!             blocking
db.find[T](sql: str, args: ...) -> T?!           blocking
db.all[T](sql: str, args: ...) -> [T]!           blocking
db.count(sql: str, args: ...) -> int!            blocking
db.exec(sql: str, args: ...) -> int!             blocking
db.batch[T](sql: str, rows: [T]) -> int!         blocking
db.tx(body: fn() -> ()!) -> ()!                  blocking
```

Every one runs on the worker's blocking pool, so the calling task suspends and the event loop continues ([D006](01-decisions.md#d006)). WAL mode, one writer and many readers, and `busy_timeout` set by default.

**`T` is a struct declared in this program** for `one`, `find`, `all`, and `batch` — `DT0153` — and must be **written** for the first three, because it appears only in the result. For `db.batch` it is inferred from `rows`.

**The six variadic members** are the closed set [D133](01-decisions.md#d133) permits. An argument's type must be bindable to a SQL parameter, which is `int`, `bool`, `float`, `str`, `bytes`, `time.Time`, an enum, or an optional of any of those (`DT0160`), and the count must match the placeholder count (`DT0144`). Both are compile-time facts, which is what makes a variadic tail acceptable here and nowhere else: the arity and the type of every argument are checked against a prepared statement, so the looseness the notation adds is removed again before the program runs.

**Failure kinds.** `db.one` fails `not_found` with zero rows and `conflict` with more than one — "exactly one" is a uniqueness claim, and the two ways of breaking it are different enough to branch on. A constraint violation is `conflict`; a busy timeout is `timeout`; a locked or corrupt file is `unavailable`; anything else SQLite refuses at execution time is `internal` with SQLite's own message and the statement in the `cause` chain. `db.find` returns `nil` where `db.one` fails `not_found`, and that is the only difference between them.

**`db.count`** is the single-scalar entry point and its column is an integer (`DT0152`). There is no `db.scalar[T]` for a `str` or `float` scalar ([D143](01-decisions.md#d143)): a one-field struct is already how a row of one column is spelled, it names the column at the call site, and it goes through the same result-shape check as every other query. One entry point per shape, not two.

**`db.exec`** returns rows affected and requires a statement with no result columns (`DT0151`). There is no `db.last_insert_id`: SQLite's `returning` clause is a better answer in every case, it is already what the documented insert uses ([02-syntax.md](02-syntax.md#a-complete-application)), and a separate id read after an insert is a second round trip that can disagree with the first.

**`db.batch`** binds each row struct's fields to the placeholders in declaration order (`DT0154`) and returns the total rows affected. It prepares once and binds many times, which is the only reason it exists.

**`db.tx`** runs `body` in a transaction, commits when it returns, and rolls back when it fails or when a fault terminates the task. Its body parameter is `fn() -> ()!`, so **the body must be fallible**, and a body that cannot fail is `DT0207`. That is not a papercut: a transaction whose body cannot fail cannot roll back, so it is a `db.exec` with extra words. Every real body contains a `db` call and therefore a `!`.

A `db.tx` inside a `db.tx` runs as a **savepoint**: the inner one rolls back to its own savepoint on failure and the outer transaction survives to decide what to do. The alternative was a fault on nesting, and it is worse — it makes a function containing `db.tx` uncallable from another function containing `db.tx`, which is a whole-program property that no local reading can establish.

Captures inside the body are immutable like every other lambda's ([D093](01-decisions.md#d093)), so a transaction cannot accumulate into an outer `var`; it returns nothing, and what it wrote is in the database:

```do
route POST "/transfers" (form: Transfer) -> redirect! {
  db.tx(fn() {
    db.exec("update accounts set balance = balance - ? where id = ?", form.amount, form.source)!
    db.exec("update accounts set balance = balance + ? where id = ?", form.amount, form.target)!
  })!
  return http.see_other("/accounts")
}
```

The caller here is a route rather than a helper function, and that is [D134](01-decisions.md#d134) showing through: a helper that wraps a transaction and has nothing to return would be `fn transfer(…) -> ()!`, which the frozen grammar cannot spell. `db.tx(…)!` as a statement is permitted because the expression is a `!` over a call ([D091](01-decisions.md#d091)).

### `validate`

Declarative validation is `@` attributes on a struct's fields, and binding runs it before a handler body and short-circuits to 422 ([D025](01-decisions.md#d025), [D043](01-decisions.md#d043)). This module is the imperative remainder and the re-rendering path.

```
validate.check[T](value: T) -> ()!
validate.errors[T](value: T) -> {str: str}

validate.email(text: str) -> bool
validate.url(text: str) -> bool
validate.in_range(n: int, low: int, high: int) -> bool
validate.in_range_float(x: float, low: float, high: float) -> bool
validate.length(text: str, minimum: int, maximum: int) -> bool
validate.one_of(text: str, options: [str]) -> bool
```

`check` and `errors` run the `@` attributes of `T`'s fields against a value the program has in hand — a struct it built itself, or one it bound in an earlier request and is validating again. `T` is a struct declared in this program and is inferred from `value`; a `T` with no validation attributes returns an empty map, which is not an error.

`validate.errors` returning **field name to message** is the deliberate part, and the overview's argument for it stands: re-rendering a form with inline errors is the single most common web interaction and it should need no plumbing.

```do
route POST "/users" (form: NewUser) -> Response! {
  let problems = validate.errors(form)
  if not problems.is_empty() {
    return http.html(user_form(form, problems)).with_status(422)
  }
  db.exec("insert into users (name, email) values (?, ?)", form.name, form.email)!
  return http.html(thanks())
}
```

`validate.check` is the same check as a failure instead of a map: it fails `validation` with a message naming the first field that failed and the rest in its `cause` chain, one `Error` per field. Both members exist because both shapes are wanted — a map to render, and an error to propagate.

**The overview's `validate.check(value, rules)` loses its second parameter** ([D146](01-decisions.md#d146)). There is no rule value in doot to put in a list: `@len(1, 500)` is an attribute, attributes are a closed set applied to declarations ([D043](01-decisions.md#d043)), and a heterogeneous list of rule objects would need either a payload-carrying enum ([D018](01-decisions.md#d018) says no) or an interface ([D016](01-decisions.md#d016) says no). The predicates above are what an imperative check composes from, with `and`, and the attribute set is what a declarative one composes from.

The predicates return `bool` rather than an `Error` because a predicate's answer is a condition, and a program that wants a message writes it: `if not validate.email(text) { return Error { kind: .validation, message: "that is not an email address" } }`. There is no regex engine to write a seventh predicate with ([D036](01-decisions.md#d036)), and that is the reason the set above is a set rather than a mechanism.

---

## Crypto

### `crypto`

Every primitive comes from vendored mbedTLS ([D035](01-decisions.md#d035)). Nothing here is a construction kit: each member is one whole operation with its parameters fixed, because a v0.1 whose users are not going to audit their own cipher choices should not offer them any.

```
crypto.sha256(b: bytes) -> bytes
crypto.sha512(b: bytes) -> bytes
crypto.sha1(b: bytes) -> bytes
crypto.hmac_sha256(key: bytes, message: bytes) -> bytes
crypto.hmac_sha512(key: bytes, message: bytes) -> bytes
crypto.hmac_sha1(key: bytes, message: bytes) -> bytes
crypto.encrypt(key: bytes, plaintext: bytes) -> bytes!
crypto.decrypt(key: bytes, ciphertext: bytes) -> bytes!
crypto.random_bytes(count: int) -> bytes
crypto.random_int(low: int, high: int) -> int
crypto.constant_time_eq(a: bytes, b: bytes) -> bool
crypto.hash_password(password: str) -> str!                 blocking, v0.4
crypto.verify_password(password: str, hash: str) -> bool     blocking, v0.4
```

**`encrypt` and `decrypt` are AES-256-GCM and nothing else.** The key is exactly 32 bytes or the call faults — a wrong key length is a program error, not an input. A fresh 12-byte nonce comes from the CSPRNG on every call and is prepended to the ciphertext, with the tag appended, so the result is self-contained and `decrypt` needs no second argument. `decrypt` fails `validation` when the tag does not verify, which covers a wrong key, a truncated message, and a tampered one alike, and deliberately does not distinguish them. There is no mode selection, no nonce parameter, and no streaming form: a nonce parameter is the single most reliable way to get AES-GCM wrong.

**`sha1` and `hmac_sha1` are present for verifying other people's signatures**, which is the only reason: some third-party webhooks still sign with HMAC-SHA1, and since the `http` client is the whole third-party integration story ([D031](01-decisions.md#d031)), refusing the primitive would make those integrations impossible rather than more secure. They are not for hashing anything doot originates. `crypto.hmac_sha256` plus `encode.hex` is what [04-stdlib.md](04-stdlib.md#the-two-load-bearing-modules) points at for AWS SigV4 and Stripe webhook verification.

There is no MD5, in any form, because nothing a doot program does needs it.

**`constant_time_eq`** compares in time independent of content and is what a signature comparison uses; `==` on `bytes` short-circuits and is what everything else uses. Both exist because the distinction is real and cannot be inferred from the call site.

**`crypto.random_int(low, high)`** is half-open, faults when `low` is not below `high`, and is uniform — rejection-sampled rather than reduced modulo, because modulo bias in a token generator is invisible and permanent. It is separate from `rand.int` and the separation is the point: one is for tokens and one is for jitter, and a program that reaches for the wrong one should have to say so.

Password hashing is argon2id and lands in v0.4 with `auth` ([D035](01-decisions.md#d035)). `hash_password` returns a self-describing string carrying the algorithm and its parameters, so `verify_password` needs no configuration and a later parameter change can verify an older hash. `verify_password` returns `bool` and is constant-time in the comparison; it does not fail, because "this password is wrong" is an answer and not an error.

### `uuid`

```
uuid.new() -> str
uuid.new_v7() -> str
uuid.is_valid(text: str) -> bool
```

`uuid.new()` is version 4, from the CSPRNG. `uuid.new_v7()` is version 7, time-ordered, which is what a database key wants because it sorts by creation and keeps a B-tree insert local. Both return the canonical lowercase hyphenated form as a `str`.

There is no `Uuid` type. A distinct type would need a parse entry point, a conversion in both directions, a SQL binding rule, and a JSON representation — four decisions and a row in [D089](01-decisions.md#d089)'s closed cast table — to hold sixteen bytes that every consumer wants as text anyway. `str` binds to a TEXT column, encodes to JSON, and renders in markup with nothing added.

`uuid.new` is a reserved word after a dot, which [D062](01-decisions.md#d062) explicitly permits and cites this member as its reason.

### `rand`

Not cryptographic, and named so that the choice is visible: tokens and secrets come from `crypto`.

```
rand.int(low: int, high: int) -> int          // half-open; faults when low >= high
rand.float() -> float                          // [0.0, 1.0)
rand.bool() -> bool
rand.pick[T](xs: [T]) -> T?
rand.shuffled[T](xs: [T]) -> [T]
```

`pick` returns `nil` on an empty list rather than faulting, because an empty collection is data. `shuffled` returns a new list and there is no in-place `shuffle`, which is the one place [D137](01-decisions.md#d137)'s pair is deliberately incomplete: shuffling in place would need a mutating method on `[T]` whose only caller is this module, and the copy is the cost of a shuffle anyway.

**There is no `rand.seed`** ([D148](01-decisions.md#d148)). A seed setter would be a mutable global in everything but name — a value one call changes and every later call observes, per worker, which is exactly the construct [D008](01-decisions.md#d008) makes impossible so that behaviour cannot differ between one worker and sixteen. The generator's state lives in the worker, is not addressable from doot, and is seeded from the CSPRNG at worker start.

The cost is that a test cannot make `rand` deterministic, and the answer is that a function whose result must be reproducible takes the random value as a parameter. That is a testability property rather than a limitation: it moves the nondeterminism to the caller, where a test can supply it.

---

## System

### `fs`

Every member runs on the blocking pool ([D006](01-decisions.md#d006)), and every member is fallible. Paths are `str` and are interpreted by the platform; `path` is the module that manipulates them without touching the filesystem.

```
fs.read(path: str) -> bytes!                              blocking
fs.read_text(path: str) -> str!                           blocking
fs.write(path: str, content: bytes) -> ()!                blocking
fs.write_text(path: str, content: str) -> ()!             blocking
fs.append(path: str, content: bytes) -> ()!               blocking
fs.exists(path: str) -> bool                              blocking
fs.stat(path: str) -> fs.Info!                            blocking
fs.remove(path: str) -> ()!                               blocking
fs.rename(from: str, to: str) -> ()!                      blocking
fs.copy(from: str, to: str) -> ()!                        blocking
fs.make_dir(path: str) -> ()!                             blocking
fs.remove_dir(path: str) -> ()!                           blocking
fs.list(path: str) -> [str]!                              blocking
fs.temp_dir() -> str
```

```
info.size: int
info.is_dir: bool
info.modified: time.Time
```

**Failure kinds** are the same five throughout: `not_found` for a missing path, `permission` for one the process may not touch, `conflict` for a target that already exists where the operation requires it not to, `range` for a read whose size does not fit the request's memory budget, and `io` for everything the platform refuses for its own reasons. `read_text` adds `parse` for content that is not UTF-8.

`fs.exists` returns `bool` and not `bool!`, because a path the process cannot stat is indistinguishable from one that is not there and neither answer helps: the caller is about to open it and the open is where the truth is. It is still blocking, since it is a syscall.

`write` and `write_text` are **atomic**: they write a temporary file beside the target and rename it, so a reader sees the old bytes or the new ones and never a half-written file. That is not an implementation detail to discover later — it is the behaviour a program relies on, so it is specified. `append` is not atomic and cannot be.

`make_dir` creates every missing parent and succeeds on a directory that already exists, because a `make_dir` that failed on an existing directory would be wrapped in an `exists` check at every call site. `remove_dir` removes recursively and fails `not_found` on a missing one; there is no non-recursive form, because a program that wants to fail on a non-empty directory checks `fs.list`.

`fs.list` returns bare names, not paths, sorted bytewise, with `.` and `..` excluded. Sorted because `fs_read_dir`'s order is already a guarantee the compiler depends on ([12-semantics.md](12-semantics.md#what-becomes-visible-and-when)) and a program that lists a directory should get the same answer twice.

### `path`

Pure string manipulation. No member touches the filesystem, no member is fallible, and no member is blocking — which is exactly why it is a module of its own.

```
path.join(parts: [str]) -> str
path.parent(path: str) -> str
path.file_name(path: str) -> str
path.extension(path: str) -> str
path.stem(path: str) -> str
path.is_absolute(path: str) -> bool
path.normalize(path: str) -> str
path.within(root: str, path: str) -> bool
```

`join` takes a list rather than a variadic tail, because the variadic set is closed to the six `db` members ([D133](01-decisions.md#d133)) and `path.join(["a", "b"])` is two characters worse than `path.join("a", "b")` for a member that is usually called with a list anyway.

`extension` includes no leading dot and is `""` when there is none; `stem` is the file name without it. `normalize` resolves `.` and `..` textually and collapses repeated separators, and it does not resolve symbolic links, because that would be a syscall and this module has none.

`path.within(root, path)` answers whether `path`, once normalized, stays under `root`. It is the check `static.dir` performs, exposed because any handler that builds a path from a request parameter needs it, and because writing it by hand is how a traversal bug happens.

### `env`

```
env.get(name: str) -> str?
env.all() -> {str: str}
env.mode() -> env.Mode
env.version() -> str
```

`env.Mode` is `enum { development, production }`.

`env.get` returns `str?` and not `str!`, which is what makes the documented `env.get("NAME") else "default"` work at the top level of a file: `!` needs an enclosing fallible function and a top-level `let` initializer has none (`DT0402`), so an environment read had to be optional rather than fallible or configuration would be unwritable ([D094](01-decisions.md#d094), [02-syntax.md](02-syntax.md#configuration)).

**There is no `env.set`.** The environment is process-wide mutable state, so a setter is a mutable global by another name ([D008](01-decisions.md#d008)), and a program that wants to pass a value to a subprocess passes it to `os.run`.

`env.mode()` reports how the process was started, not an environment variable, and it is what switches `log`'s output format and `cookie`'s `secure` default. `env.version()` is the version of the doot binary that compiled the program, which is what a status page and a bug report want.

### `os`

```
os.cpu_count() -> int
os.pid() -> int
os.hostname() -> str!
os.args() -> [str]
os.run(command: str, args: [str]) -> os.Output!          blocking
os.send_signal(pid: int, signal: os.Signal) -> ()!
```

```
output.status: int
output.stdout: bytes
output.stderr: bytes
```

`os.Signal` is `enum { interrupt, terminate, hangup, kill }`.

`os.cpu_count()` is the member the documented configuration uses for its worker count ([02-syntax.md](02-syntax.md#configuration)). `os.args()` is the argument list the process was started with, first element included.

**`os.run` takes a command and a list, never a command line.** There is no shell, no word splitting, and no quoting, so a request value in `args` cannot become a second command — the injection this shape exists to make unrepresentable. It waits for the process, buffers both streams into the request arena, and fails `not_found` when the command does not exist, `permission` when it may not be executed, `range` when output exceeds the memory budget, `io` on a stream failure, and `unsupported` on a platform with no subprocess mechanism. A non-zero exit is **not** a failure: it is `output.status`, because a program that runs `git` cares about the difference between exit 1 and exit 128.

**Receiving a signal is the runtime's, not the program's** ([D147](01-decisions.md#d147)). `interrupt` and `terminate` begin a graceful shutdown, `hangup` reloads, and none of it is interceptable from doot. A handler-registration entry point would be a runtime registration of a callback — the shape [D024](01-decisions.md#d024) rejected for routes — and it would let a program defeat the graceful shutdown that a deploy depends on. `defer` is how a task cleans up, and it runs on the shutdown path ([12-semantics.md](12-semantics.md#with-lambdas-and-defer)).

What `os` exposes is therefore the **outbound** half: the enum, and `send_signal` for a program that supervises another process it started. `send_signal` fails `not_found` on an unknown pid, `permission` when the process is not the caller's, and `unsupported` where signals do not exist.

There is no `os.exit`. A doot process ends when its server stops or when a fault takes the last task, and an exit call in a request handler would tear down every other in-flight request — the one failure mode [D012](01-decisions.md#d012)'s task containment exists to prevent.

---

## Observability and development

### `log`

```
log.debug(message: str, fields: {str: str} = {})
log.info(message: str, fields: {str: str} = {})
log.warn(message: str, fields: {str: str} = {})
log.error(message: str, fields: {str: str} = {})
```

Four members, one shape, no configuration. Every line emitted during a request carries the request's id, route, and elapsed time without being asked ([04-stdlib.md](04-stdlib.md#log)), because the runtime knows the task it is on; output is human-readable in development and JSON in production, switched by `env.mode()`.

**There is no `log.set_level` and no `log.configure`.** A level set at runtime is a mutable global ([D008](01-decisions.md#d008)) whose value would differ per worker, which is precisely the "correct in development, wrong in production" failure that decision exists to make unrepresentable. The level comes from the process's own configuration, and the format from its mode.

**`fields` is `{str: str}`** ([D140](01-decisions.md#d140)), and a non-string field is written with interpolation:

```do
fn record(u: User) {
  log.info("user created", {"user_id": "${u.id}", "email": u.email})
}
```

A map is homogeneous, so `{str: any}` was the alternative, and it is worse in a way that shows up at every call site: `T` does not widen to `any` implicitly ([D088](01-decisions.md#d088)), so every numeric field would be written `u.id as any`. `"${u.id}"` is shorter, needs no cast, allocates nothing beyond the string, and is the form [D096](01-decisions.md#d096) already types — string interpolation accepts `int`, `float`, and `bool`. The cost is that a JSON log line carries `"user_id": "12"` rather than `"user_id": 12`, which is what most log pipelines index anyway.

The default `{}` is a constant expression and therefore a legal default ([How to read a signature](#how-to-read-a-signature)), so `log.info("started")` needs no second argument.

### `test`

```
test.eq[T](actual: T, expected: T)
test.ne[T](actual: T, expected: T)
test.true(value: bool)
test.false(value: bool)
test.is_nil[T](value: T?)
test.not_nil[T](value: T?)
test.contains(haystack: str, needle: str)
test.fail(message: str)
test.skip(reason: str)
test.expect_error(body: fn() -> ()!) -> Error
test.fixture(name: str) -> bytes!                blocking
test.fixture_text(name: str) -> str!             blocking
```

`T` is inferred from `actual` in `eq` and `ne`, and from `value` in the two nil assertions, which is what makes `test.eq(err.kind, .validation)` check: `T` binds to `ErrorKind` from the first argument and `.validation` then has an expected enum type ([D090](01-decisions.md#d090), [D135](01-decisions.md#d135)). `eq` compares by content ([D009](01-decisions.md#d009)), so it works on structs, lists, and maps with nothing added.

**An assertion records a failure against the enclosing test task and does not fail the call** ([D151](01-decisions.md#d151)). It returns nothing and is not fallible, which is what [02-syntax.md](02-syntax.md#tests) shows and what lets a test make several assertions and report all of them. A test with any recorded failure fails, and its diagnostics carry the assertion's span in the same format as `doot check` ([06-tooling.md](06-tooling.md#testing)).

`test.skip` records the test as skipped and the rest of its body still runs, because a skip that changed control flow would need to diverge and nothing in the language lets a call do that ([D095](01-decisions.md#d095)).

Calling any `test` member outside a test task is a **fault**: there is no test to record against, and reaching one from a request handler is a bug of exactly the kind [D012](01-decisions.md#d012) describes. The alternative — a compile-time rule that a `test` member may appear only lexically inside a `test` block, in the shape of [rules 10 and 11](03-grammar.md#well-formedness-rules) — was rejected because it forbids a helper function that asserts, which is a legitimate and common shape, and detecting "reachable only from a test" is a whole-program analysis for a rule that guards nothing an author would get wrong by accident.

`test.expect_error` runs `body` and returns the `Error` it produced, and **faults when the body succeeds** — a test asserting that something fails has failed to test it, and recording an ordinary failure would let the message say less than a fault's does. Its parameter is `fn() -> ()!`, so a body producing a value discards it, which the effect rule permits for a call ([D091](01-decisions.md#d091)):

```do
test "creating a user rejects a blank name" {
  let err = test.expect_error(fn() {
    create("")!
  })
  test.eq(err.kind, .validation)
}
```

**Fixtures** are files, read from `tests/fixtures/` relative to the project root, which is the whole of what [04-stdlib.md](04-stdlib.md#overview) means by the word. `name` may not escape that directory — the `path.within` check, failing `permission` — and a missing fixture is `not_found`. Data fixtures are not a mechanism: each test already runs in its own task inside a transaction against a temporary database that is rolled back afterwards ([06-tooling.md](06-tooling.md#testing)), so seeding a test is `db.exec` in the test body, and there is nothing for a fixture framework to do.


---

## Immutability across the library

[D008](01-decisions.md#d008) is three rules, and an API can break each of them. This is how the library above does not ([D156](01-decisions.md#d156)).

**No mutable globals.** No module has a setter. There is no `log.set_level`, no `rand.seed`, no `env.set`, no `db.set_busy_timeout`, no signal-handler registration, and no cache of anything a program can observe changing. Every one of those was a plausible member and every one is a value that one call writes and every later call reads — per worker, so a program's behaviour would differ between one worker and sixteen, which is the exact failure [D008](01-decisions.md#d008) exists to make unrepresentable rather than to document. Everything those setters would configure is either the process's own configuration ([D040](01-decisions.md#d040)) or a required argument at the call that needs it, which is why `static.file` takes a `max_age`.

**Immutable parameters.** No member mutates an argument. Every member that produces a modified collection returns a new one, and the two-spelling convention says which is which: `xs.sorted()` returns and `xs.sort()` mutates, and `sort` mutates its **receiver**, never a parameter ([D137](01-decisions.md#d137)).

**Deep `let`.** The mutating column in the module table is what makes this checkable rather than aspirational ([D097](01-decisions.md#d097)), and this document is where its value is decided per member. The complete set of mutating members in v0.1 is:

| Receiver | Mutating members |
| --- | --- |
| `[T]` | `push`, `pop`, `insert`, `remove_at`, `extend`, `clear`, `sort`, `sort_by` |
| `{K: V}` | `set`, `remove`, `extend`, `clear` |

**That is all of them.** `str`, `bytes`, `html`, `time.Time`, `time.Duration`, `Error`, `Request`, `Response`, `redirect`, `Upload`, `url.Url`, `cookie.Cookie`, `fs.Info`, and `os.Output` carry no mutating member at all, so a `let` binding of any of them is unrestricted and `DT0303` cannot fire on one. Two builtin types have mutating methods because two builtin types are containers a handler builds up as it goes, and the argument in [D008](01-decisions.md#d008) — a `var` local is uniquely owned, so mutation is in place and value semantics cost no copy — applies to exactly those two.

Nothing in the library returns a **view**: `m.keys()`, `m.values()`, `req.headers()`, and `form.values()` each return a fresh value, so mutating the result cannot reach back into what produced it. Without that property, `let` would be deeply immutable in the language and shallowly immutable through the standard library, and [D004](01-decisions.md#d004)'s promote-by-deep-copy would be unsound in the same stroke.

## The blocking pool

Blocking work is offloaded to the worker's thread pool, the calling task suspends, and the event loop continues ([D006](01-decisions.md#d006)). The property a reader needs is which calls do that, so it is marked per member above and collected here ([D149](01-decisions.md#d149)):

| Module or type | Blocking members |
| --- | --- |
| `db` | every member |
| `fs` | every member except `temp_dir` |
| `form` | every member |
| `static` | both members |
| `os` | `run` |
| `test` | `fixture`, `fixture_text` |
| `Request` | `body`, `body_text` |
| `Upload` | `save_to`, `bytes`, `text` |
| `crypto` | `hash_password`, `verify_password` (v0.4) |

**Every other member of every other module is non-blocking**, and that is a commitment rather than an observation: it means `time.now()`, `uuid.new()`, `crypto.random_bytes`, `log.info`, and every `str`, `list`, `map`, `math`, `json`, `encode`, `html`, `url`, `cookie`, `mime`, `path`, and `env` member completes on the event loop without a hop. `log` is the one worth stating explicitly, because a logger that wrote synchronously to a file would make every log line a blocking call: log output goes to a buffer the worker flushes, so `log.info` in a hot handler costs an append.

Password hashing is on the pool because argon2id is deliberately slow — tens of milliseconds — and a login that stalled the loop for that long would be a denial of service on every other connection the worker holds.

## Reserved diagnostics

Three numbers, reserved and **not registered** ([D065](01-decisions.md#d065), [D150](01-decisions.md#d150)). Each sits in a held sub-range that [12-semantics.md](12-semantics.md#the-types-range) already allocated, and each is written in brackets in the way [03-grammar.md](03-grammar.md#well-formedness-rules) writes a reservation. A row enters `src/base/diag_codes.h` with the code that emits it and the spec test that proves it, and no earlier.

| Code | Meaning | Sub-range |
| --- | --- | --- |
| [`DT0216`] | a type does not satisfy the constraint on this entry point | `DT0200`–`DT0219`, core type agreement, held at `DT0216` |
| [`DT0217`] | this type cannot be a map key | the same sub-range, held at `DT0217` |
| [`DT0233`] | this type cannot be constructed with a struct literal | `DT0220`–`DT0239`, named types, held at `DT0233` |

`DT0216` is the constraint diagnostic for every type constraint in the library, whether the type was written as an argument or read off a receiver: `json.decode[T]` and `http.json[T]` where `T` must be JSON-representable, `xs.sorted()` where `T` must be `int`, `float`, or `str`, and `map.invert[K, V]` where `V` must be a key type. It is distinct from `DT0215`, which is about the *shape* of a type-argument list — the wrong number of them, or one written where none is declared — and from `DT0153`, which is `db`'s own requirement that a row type be a struct declared in this program. One code for one question, with the message naming the constraint and the type that failed it.

`DT0217` is reported at the offending `{K: V}` type annotation rather than at a use, so it fires once at the declaration.

`DT0233` is reported at a struct literal whose type is a prelude type other than `Error`, and its message names the member that produces a value of that type — `http.html` for a `Response`, `cookie.new` for a `Cookie`. It exists because `Error`'s literal is the one case that works, so the failure is not "this is not a struct" but "this struct is not yours to build".

No other member of any module above implies a compile-time diagnostic that is not already allocated. Argument counts and types are `DT0206` and `DT0207`; a mutating call on a `let` receiver is `DT0303`; a member of an unlanded module or version is `DT0046`; every `db` compile-time check is `DT0140`–`DT0160`; and returning an `Error` from a function that is not fallible is `DT0409`.

## What the module table takes from this document

[12-semantics.md](12-semantics.md#the-prelude-and-the-module-table) lists the six columns the table carries. This is where each column's values come from, so the workstream that writes it has one input and not twelve:

| Column | Source |
| --- | --- |
| module name | [The 26 modules of v0.1](#the-26-modules-of-v01) |
| member name | the signature blocks above, including value members, which are rows keyed by their receiver type |
| signature | the same, with `!`, `?`, defaults, and the three notations in [How to read a signature](#how-to-read-a-signature) |
| type-argument slots | marked `[T]` in the signature, with [D135](01-decisions.md#d135) deciding written versus inferred and the constraint named in the module's own section |
| mutating | the table in [Immutability across the library](#immutability-across-the-library) — twelve members, and no others |
| version | v0.1 for everything above except the four client members and the two password members, which are v0.3 and v0.4 |

Two rows of the **prelude** are settled here rather than in the module table: `Error` gains three fields and three methods, and `ErrorKind` gains its thirteen variants. The rest of the prelude — `int`, `float`, `bool`, `str`, `bytes`, `html`, `any`, `Request`, `Response`, `redirect`, `Upload` — is unchanged in membership; what this document adds is the member set on each of the last four.

The table's `ErrorKind` variant list is load-bearing beyond the library, because it is what makes `match err.kind` decidably exhaustive ([D085](01-decisions.md#d085)). It is therefore the one part of this document that a later release may not extend.

## Corrections to the overview

Every code block in [04-stdlib.md](04-stdlib.md) was read against [03-grammar.md](03-grammar.md), and the overview is updated in this change. The defects, and what each was:

| Where | Defect | Fix |
| --- | --- | --- |
| `log` | `log.info("user created", { user_id: u.id })` — `user_id` is an identifier expression under `map_lit`, so it must resolve to a local, and it does not; the value is also an `int` where the parameter is `{str: str}` | `log.info("user created", {"user_id": "${u.id}"})` |
| `log` | `log.warn(msg, fields) log.error(msg, fields) log.debug(msg, fields)` — three calls juxtaposed on one line, which is not a statement, over unbound identifiers | rewritten as signatures with parameter types, one per line |
| `time` | `time.now() time.parse(fmt, s)! time.from_unix(n)` and `t.format(fmt) t.add(d) t.diff(other) …` — the same juxtaposition, twice | rewritten as signatures with parameter types |
| `validate` | `validate.email(s) validate.url(s) validate.in_range(n, lo, hi)` — juxtaposed again, and `lo`/`hi` are abbreviations [D039](01-decisions.md#d039) forbids | rewritten as signatures, with `low` and `high` |
| `validate` | `validate.check(value, rules) -> ()!` — `rules` has no expressible type ([D146](01-decisions.md#d146)) | `validate.check[T](value: T) -> ()!` |
| `html` | `html.attrs({str: str}) -> html` — no position in the grammar accepts an `html` value as an attribute ([D145](01-decisions.md#d145)) | removed, with a pointer to the `...expr` spread |
| `db`, `http`, `topic`, `cache`, `jobs` | parameter names without types, and `…` for a variadic tail | typed, with `args: ...` as [D133](01-decisions.md#d133) defines it; the deferred modules keep their sketches and are marked as such |

Two further findings are recorded and **not** fixed here, because fixing either belongs to somebody else's change:

- **`http.request(Request)`** cannot take the prelude's `Request`, which is the inbound request type and is not constructible. The client is v0.3 and the shape of an outbound request is its pass ([`http`](#http)).
- **`test.expect_error(fn() => create(""))`** in [02-syntax.md](02-syntax.md#tests) does not check: the lambda's inferred type is `fn() -> User!`, the parameter is `fn() -> ()!`, and a fallible expression in a lambda body needs its `!` like anywhere else ([rule 3](03-grammar.md#well-formedness-rules)). The doot spelling is `test.expect_error(fn() { create("")! })`. It is the same class of defect as `find(id) else User.guest()`, which [D092](01-decisions.md#d092) settled and which is being corrected in [02-syntax.md](02-syntax.md#errors) separately, so it is left to that change rather than split across two.
