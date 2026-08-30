# Semantics

Implementation specification for the four compiler stages between the parser and the register allocator: the **resolver**, the **typechecker**, the **schema checker**, and the **route checker** ([05-runtime.md](05-runtime.md#compiler-pipeline)). Decisions are recorded as [D081](01-decisions.md#d081)–[D102](01-decisions.md#d102). The front end this builds on is [10-frontend.md](10-frontend.md); the suite that tests all of it is [11-spec-tests.md](11-spec-tests.md).

Together these four stages are **the semantic pass**. They consume the AST that [10-frontend.md](10-frontend.md#the-ast) specifies and produce three artifacts the back end consumes: a binding for every name, a type for every expression, and the route table. They add no node kinds to the AST and mutate none of it.

This document also settles eleven of the sixteen well-formedness rules in [03-grammar.md](03-grammar.md#well-formedness-rules) — every rule the parser does not already discharge — and allocates the diagnostic ranges `DT0100`–`DT0199`, `DT0200`–`DT0299`, `DT0300`–`DT0399`, `DT0400`–`DT0499`, `DT0500`–`DT0599`, and `DT0600`–`DT0699` in full and in advance, as [D064](01-decisions.md#d064) did for the front end.

Four reserved diagnostic numbers move as part of that allocation, and [03-grammar.md](03-grammar.md#well-formedness-rules) is updated to match. The argument is in [Ranges are subjects, not stages](#ranges-are-subjects-not-stages).

---

## Stages

```
unit_ast (all files)
  → resolver          scopes, bindings, module paths, naming, mutability
  → typechecker       inference, assignability, fallibility, divergence, markup
  → schema checker    migration replay, statement preparation, result shapes
  → route checker     pattern/parameter agreement, binding, conflicts
```

| Stage | Consumes | Owns | Produces |
| --- | --- | --- | --- |
| resolver | every file's `unit_ast` | scopes, name binding, visibility, naming conventions, mutability, module paths | the **symbol table**, with every *declared* type resolved, and a **binding** for every `EXPR_IDENT`, `EXPR_FIELD`, `EXPR_VARIANT`, `TYPE_PATH`, and lvalue root |
| typechecker | the AST plus the symbol table | the type of every *expression*, assignability, `!` and `else`, exhaustiveness, divergence, markup value typing | a **type** for every expression |
| schema checker | the AST, the bindings, the declared types | the migrated schema, every SQL literal, every result shape | the **schema** and one prepared statement per distinct literal |
| route checker | the AST, the bindings, the declared types | route patterns, request binding, conflicts | the **route table** |

### Why the boundaries fall here

Each boundary is the point at which one kind of fact becomes available and the previous stage's job is finished.

- **Resolver / typechecker.** A name must be bound before its type can be asked for, and the resolver's own rules — visibility, naming, mutability — need only *which declaration a name refers to*, never a type. Mutability is the surprising member of that list and it belongs there for exactly that reason: [D008](01-decisions.md#d008) is a property of a *binding* (`let` versus `var`, parameter versus local), so the check reads the symbol and nothing else.
- **Typechecker / schema checker.** Nothing flows either way, which is worth stating plainly because the pipeline's order suggests otherwise. The typechecker never needs a prepared statement: the result type of `db.all[Msg](...)` is `[Msg]!` from [D019](01-decisions.md#d019)'s explicit type argument. And the schema checker never needs an inferred type: the fields it maps a row into are *declared*, so the resolver already has them, and the one check that needs an argument's type — "this type cannot be bound as a SQL parameter" — is reported by the typechecker, which is visiting every argument anyway.
- **Schema checker / route checker.** Nothing connects them either. The route checker reads patterns, declared parameter types, declared return types, and hook signatures, all of which the resolver produced.

So the last two stages depend on the resolver and not on the typechecker, and the order among the three is [05-runtime.md](05-runtime.md#compiler-pipeline)'s rather than a dependency. What the order buys is the **barrier**: a program whose types do not check never reaches statement preparation, so a mistyped argument produces a type error and not also a SQL complaint about the same argument. Two consequences follow, and both are load-bearing later: the schema checker and the typechecker can be built concurrently ([D102](01-decisions.md#d102)), and `doot routes` can run the resolver and the route checker alone and still be a complete command ([D101](01-decisions.md#d101)).

The parser already discharges the five rules that need only syntactic context ([10-frontend.md](10-frontend.md#which-well-formedness-rules-the-parser-discharges)), so no stage here carries loop, method, or stream state.

### Stage barriers

**A stage runs only if every earlier stage reported no errors** ([D081](01-decisions.md#d081)). Warnings do not stop anything.

Within a stage, checking continues after an error so that a run reports every independent problem, exactly as the parser recovers. Across stages it stops, and that asymmetry is deliberate: a type error derived from a name that did not resolve is not a second finding, it is the same finding restated in a form that names the wrong thing. `diag_sink` collects at most `DIAG_DEFAULT_LIMIT` diagnostics and sets `truncated` past that ([diag.h](../src/base/diag.h)), so a cascade does not merely add noise — it evicts the diagnostic that would have explained it.

This matters more for doot than for a compiler with only human readers. [D038](01-decisions.md#d038) exists so an agent can act on `doot check --json` directly, and an agent handed forty consequences and one cause will fix a consequence.

*Consequence:* a program with a misspelled name needs two `doot check` runs to reveal its type errors. That is the cost, it is paid once per class of mistake, and it buys output in which every diagnostic is about the code the author wrote.

### One walk per stage, not one walk per rule

Each stage is a single recursive walk of the unit, with every check that stage owns performed at the node it applies to. The resolver does not walk once for names and again for mutability; the typechecker does not walk once for types and again for divergence.

The reason is not speed, it is diagnostic ordering. `diag_sink` reports in the order diagnostics arrive and [D072](01-decisions.md#d072) deliberately does not assert that order in spec files — but a reader of human output wants the diagnostics for one function together, and a single walk gives that for free while a per-rule walk gives the opposite: every instance of rule A across the program, then every instance of rule B.

The only pass that is not a walk of statements is the resolver's **collect** step, described next, and the route checker's **conflict** step, which is a comparison over the finished route table rather than a walk of any tree.

---

## The symbol table

Arena-allocated, with intrusive singly-linked lists, following the shape `diag_label` and every AST child list already use ([D063](01-decisions.md#d063), [D082](01-decisions.md#d082)):

```c
typedef struct symbol symbol;

struct symbol {
  symbol_kind kind;
  slice name;
  span at;            /* the declaring occurrence, for a "declared here" label */
  const source *src;  /* which file declared it */
  bool is_pub;
  bool is_mutable;    /* var only */
  bool is_used;       /* for DT0600 and DT0601 */
  sema_type *type;    /* the declared type, resolved from its type_ref;
                       * NULL for a local, whose type the typechecker infers */
  decl *origin;       /* the declaration, or NULL for a prelude symbol */
  symbol *next;
};

typedef struct scope scope;

struct scope {
  scope_kind kind;
  scope *parent;
  symbol *first;
  symbol *last;
  uint32_t count;
  const symbol **index; /* NULL unless the scope has been sealed */
  uint32_t index_mask;
};
```

Lookup walks the scope chain and, within a scope, its list. That is a linear scan, and it is the right one: a function body holds a handful of locals, a scope chain is a handful deep, and a hash table would need a resize strategy the arena deliberately does not have ([D047](01-decisions.md#d047), [10-frontend.md](10-frontend.md#the-ast)).

Two scopes are large enough to matter and both are **built once and then never extended**: the unit scope, which the collect step fills, and a struct's field list. Those are **sealed** — after the last insertion, one arena block holds an open-addressing index sized to the next power of two above `count`, and lookup uses it. Sealing is what makes the linear default honest rather than a shrug: the scopes that grow stay linear because they are small, and the scopes that are big stop growing before they are searched.

`symbol_kind` is `SYM_MODULE`, `SYM_TYPE`, `SYM_FN`, `SYM_METHOD`, `SYM_FIELD`, `SYM_VARIANT`, `SYM_PARAM`, `SYM_SELF`, `SYM_LET`, `SYM_VAR`, `SYM_LOOP_VAR`, `SYM_ERR`, `SYM_REQ`. Distinct kinds rather than a flag word, so that `-Wswitch-enum` forces every consumer to say what it does with each ([D046](01-decisions.md#d046)).

### Scope kinds

| Kind | Holds | Notes |
| --- | --- | --- |
| `SCOPE_UNIT` | stdlib module roots, user module roots, predeclared types, the prelude | sealed after collect; one per compilation |
| `SCOPE_FILE` | every top-level name of one file, `pub` or not | one per file; parent is the unit scope |
| `SCOPE_FN` | parameters, and `self` in a method; `req` in a route, stream, or hook | one per function, method, route, stream, test, and lambda |
| `SCOPE_BLOCK` | the locals of one `{ }` | nests freely |
| `SCOPE_LOOP` | the one or two loop variables of a `for` | the body's block scope nests inside it |
| `SCOPE_COALESCE` | the `err` binding of `else err { ... }` | present only when `binds_err` is set |
| `SCOPE_LAMBDA` | a lambda's parameters | a `SCOPE_FN` variant that also marks a capture boundary |

A `match` arm with a block body gets a `SCOPE_BLOCK` like any other block; there is no arm scope, because an arm binds nothing of its own — the grammar has no binding patterns, since enums are tag-only ([D018](01-decisions.md#d018)).

`{if}` and `{for}` inside a markup literal use `SCOPE_BLOCK` and `SCOPE_LOOP` like their statement forms. A markup control block contains nodes rather than statements, so `break` and `continue` cannot appear inside one and the loop flag `SCOPE_LOOP` carries is never consulted there.

### What becomes visible, and when

Compilation is whole-program and there are no imports ([D029](01-decisions.md#d029), [D030](01-decisions.md#d030)), so a **collect** step runs before any body is resolved:

1. Walk the project's `.do` files in the sorted order `fs_read_dir` guarantees, and derive each file's module path from its path relative to the project root: `/` becomes `.` and the `.do` extension is dropped. `models/user.do` is `models.user`; `app.do` is `app`.
2. Create every module root as a `SYM_MODULE` in the unit scope, and every intermediate segment as a nested namespace.
3. Create a `SCOPE_FILE` per file and insert every top-level declaration into it: functions, methods, types, enums, aliases, and `let` bindings. Routes, streams, groups, and tests are collected into their own lists rather than into a scope, because none of them is addressable by name — which is what [DT0047](10-frontend.md#syntactic) already says with `pub`.
4. Attach each method to its receiver type's method list, and each field and variant to its type.

So **every declaration in the program is visible from every file that may see it, regardless of order.** A function at the bottom of a file may call one at the top and the reverse; `models.user.find` resolves from `routes/index.do` whether or not `models/user.do` was walked first. That is forced: there is no import to establish an order, and no header to declare one.

**Locals are the exception, and they are visible only after their own declaration.** A `let` binds from the statement after it to the end of its scope. There is no hoisting and no use-before-declaration ([DT0103](#the-names-range)). The consequence worth stating: a lambda bound to a local cannot call itself, because its own name is not in scope inside its body. A recursive function is a top-level `fn`, which the collect step made visible everywhere.

### Shadowing and redeclaration

Three rules, argued in [D083](01-decisions.md#d083):

- **Redeclaration in one scope is an error** (`DT0101`), with a `related` label at the first declaration.
- **A binding may not shadow another binding of the same function** (`DT0102`) — a local, a parameter, `self`, a loop variable, or an `err` binding in any enclosing scope up to and including the function's own. Sibling scopes do not shadow each other, so two `for u in ...` loops in one body are fine, and a lambda is *not* a fresh function for this rule: its parameters participate in it, because a parameter spelled like a captured local is the case that reads wrongly.
- **A binding may shadow a predeclared name or a top-level name, and gets a warning** (`DT0603`). [02-syntax.md](02-syntax.md#keywords) declares that type names and stdlib module names are predeclared identifiers that "may be shadowed in a local scope", so this cannot be an error. It is warned because the failure mode is otherwise baffling: after `let time = "12:00"`, `time.now()` is a field access on a `str` and the diagnostic that follows is about `str`, not about the shadowing that caused it.

A name beginning with `_` is **deliberately unused**: it is exempt from `DT0600` and `DT0601`, and the rest of the name must still be `snake_case`. `_` alone is the fully anonymous form — it is the one name that may be bound repeatedly in a single scope, and referring to it is an error (`DT0112`). The grammar admits a leading underscore, so the checker has to give it a meaning or reject it; "deliberately unused" is the meaning that makes `for _, u in users` writable, and without it an unused-binding warning would be unsilenceable.

---

## Name resolution

### Where a module path ends and field access begins

A dotted name is **one** `EXPR_IDENT` holding the whole path ([D063](01-decisions.md#d063)). `db.all`, `models.user.find`, and `u.name` are all that shape, and deciding where the path stops is the resolver's ([D084](01-decisions.md#d084)).

Given segments `s1 … sn`:

1. **Look `s1` up in the lexical scope chain** — locals, parameters, `self`, loop variables, `err`, `req`, then the file scope. If it is found, the binding is `s1` alone and `s2 … sn` are member accesses, resolved left to right by the typechecker against the type of what precedes them. **A lexical binding always wins**, at any depth, over any namespace. This is what makes `u.name` a field access, and it is why shadowing a module warns rather than silently changing meaning.
2. Otherwise `s1` must name a namespace: a stdlib module root, or a user module root. Let `k` be the **largest** index such that `s1 … sk` names a namespace. Then:
   - if `k == n`, the path names a namespace and not a value — `DT0134`, "`models.user` is a module, not a value";
   - otherwise `s1 … s(k+1)` must name a **member** of that namespace — a `pub fn`, `pub let`, `pub type`, or a stdlib entry point — and that is the binding. `s(k+2) … sn` are member accesses. A missing member is `DT0133`; a member that exists but is not `pub` is `DT0110`, which is a different mistake and gets a different message.
3. In type position a `TYPE_PATH` resolves the same way against types: `time.Time` is namespace `time`, member type `Time`, and `int` is a predeclared type symbol in the unit scope.

The algorithm is total and deterministic because of one additional well-formedness requirement: **a path may not be both a namespace and a member.** `models/user.do` and a directory `models/user/` both claim `models.user`, and that is `DT0132`. Two files claiming the same path — which a project cannot normally produce, but a case-insensitive filesystem can — is `DT0131`. With those excluded, "the largest namespace prefix" is unique, so step 2 needs no backtracking and no lookahead.

Two smaller consequences of the same shape:

- `Status.active` takes step 1: `Status` is a type symbol in the file or unit scope, and `active` is a member access on a type. **A member access on a type resolves to an enum variant and to nothing else** — there are no static functions in v0.1, because [rule 9](03-grammar.md#well-formedness-rules) requires a method's first parameter to be `self` and there is no other way to attach a function to a type name ([D092](01-decisions.md#d092)).
- `EXPR_FIELD` arises only after something that is not a name — `f().name`, `xs[0].y` — and is always a member access. It needs none of the above.

### The prelude and the module table

The unit scope is pre-filled from a **compile-time signature table**, one X-macro in the same form as the diagnostic registry and the reserved-word table ([D050](01-decisions.md#d050), [D062](01-decisions.md#d062), [D085](01-decisions.md#d085)). It carries, for every stdlib module and every member of one:

| Column | Why the checker needs it |
| --- | --- |
| module name | the namespace root in the unit scope |
| member name | step 2 of the path algorithm |
| signature | parameter types, return type, `!` and `?` marks |
| type-argument slots | `db.all[T]`, `topic.subscribe[T]` — the only generics in the language ([D019](01-decisions.md#d019)) |
| mutating | whether calling it requires a `var` receiver ([D097](01-decisions.md#d097)) |
| version | the release the entry lands in ([04-stdlib.md](04-stdlib.md#overview)) |

The **prelude** is the same table's unqualified half: the seven predeclared type names from [02-syntax.md](02-syntax.md#keywords) (`int`, `float`, `bool`, `str`, `bytes`, `html`, `any`), plus `Error`, `ErrorKind`, `Request`, `Response`, `redirect`, and `Upload`, which [02-syntax.md](02-syntax.md#errors) and [02-syntax.md](02-syntax.md#return-types) use unqualified. Naming conventions are not applied to prelude names — `redirect` is lowercase because it names a response mode rather than a data structure, and the spelling is fixed by the documentation that uses it.

Two properties of the table are load-bearing rather than incidental:

- **`ErrorKind`'s variant set is closed and known at compile time**, so `match err.kind { ... }` has a decidable exhaustiveness check ([D014](01-decisions.md#d014) makes it one universal type, which is what allows this).
- **`html.raw` is the only member whose result is `html` and whose parameter is `str`.** The type rules close every other route from `str` to `html` ([Markup context typing](#markup-context-typing)), so `grep raw(` stays the complete XSS audit [04-stdlib.md](04-stdlib.md#html) promises.

**A member of a module whose version has not landed reports `DT0046`**, the code the parser already uses for `spawn`, `send`, and `stream` ([10-frontend.md](10-frontend.md#syntactic)). Reusing it is right rather than expedient: the situation is identical — correct, final syntax for a feature that arrives later — and a second code would need a second explanation saying the same thing. So `topic.publish` in v0.1 reports `DT0046` naming v0.2, exactly as `send` does.

The table is not a stub. A module's **compile-time half** — its signatures — is a complete artifact that `doot check` genuinely uses, and it has to exist before the runtime half, because [D033](01-decisions.md#d033) promises SQL is checked at compile time and that promise is unimplementable without `db`'s signatures. What would be a stub is an entry for a module the project has decided against, and there are none: the set is closed at thirty-eight ([04-stdlib.md](04-stdlib.md)).

### Naming rules

Enforced by the checker, not the formatter ([D069](01-decisions.md#d069)), with exact spellings ([D086](01-decisions.md#d086)):

| Applies to | Form | Pattern |
| --- | --- | --- |
| module path segments, i.e. file and directory names | `lower` | `[a-z][a-z0-9_]*` |
| types, enums, aliases | `PascalCase` | one or more `[A-Z][a-z0-9]*` segments |
| functions, methods, fields, variants, bindings, parameters | `snake_case` | `[a-z][a-z0-9]*(_[a-z0-9]+)*`, optionally preceded by one `_` |

`PascalCase` forbids two adjacent capitals, so `UserId` is correct and `UserID` is not, and `Url` is correct and `URL` is not. That is stricter than it needs to be for a human and exactly as strict as it needs to be for a machine: the rule makes **the correct spelling derivable from the wrong one**, which is what lets the diagnostic carry a machine-applicable fix instead of a description of one. The normalization is: split the offending name on underscores and on each transition into a capital or out of a run of capitals, lowercase every segment, then capitalize each segment's first character. `snake_case` normalization is the same split, joined with underscores, all lowercase.

Every naming diagnostic carries the corrected spelling as a `diag_fix` over the offending identifier's own span — the first machine-applicable suggestions in the compiler, and the first producers of `expect-suggestion` ([11-spec-tests.md](11-spec-tests.md#what-has-files-today)).

**A module name is the one exception, and carries no fix.** It lives in the filesystem, so correcting it is a file rename rather than a text edit, and there is no span in any `.do` file to attach an edit to. `DT0120` is therefore reported with no source position at all — the shape [D073](01-decisions.md#d073) already established for the source-intake codes — and names the correct spelling in its message.

Naming violations are **errors**, because [D069](01-decisions.md#d069) says the rules are enforced rather than suggested and [D039](01-decisions.md#d039) makes naming part of the canonical form. The general severity rule this follows: **a warning is for code the language permits and considers redundant or suspicious; anything the language forbids is an error.** A wrong name is forbidden, so it is an error; a redundant `else` arm is permitted, so it is a warning.

---

## Types

### The type lattice

The complete set, from the Values table in [05-runtime.md](05-runtime.md#values):

```c
typedef enum {
  TY_INT, TY_FLOAT, TY_BOOL, TY_STR, TY_BYTES, TY_HTML, TY_ANY,
  TY_LIST, TY_MAP, TY_STRUCT, TY_ENUM, TY_FN, TY_NONE, TY_KIND_COUNT
} sema_type_kind;

typedef struct sema_type sema_type;

struct sema_type {
  sema_type_kind kind;
  bool optional;                /* T? -- the only form that may hold nil */
  union {
    sema_type *elem;            /* [T] */
    struct { sema_type *key; sema_type *val; } map;
    symbol *named;              /* TY_STRUCT, TY_ENUM */
    struct { sema_type **params; uint32_t nparams;
             sema_type *ret; bool fallible; } fn;
  } as;
};
```

Four properties settle most of the rest of this section ([D087](01-decisions.md#d087)):

- **Named types are nominal.** A struct or enum is identified by its symbol, never by its shape, so two structurally identical structs are different types. [D009](01-decisions.md#d009)'s structural equality is about comparing *values* of one type, not about deciding what a type is. Nominal identity also makes `type_eq` terminate on a recursive type without a visited set, because recursion stops at the name.
- **Optionality is a flag, not a wrapper.** `T??` does not exist: applying `?` to an optional yields the same type. This follows the AST, where `type_ref.optional` is already a `bool`, and it removes nested optionals — and therefore the question of how many `else`s unwrap one — from the language entirely.
- **Fallibility is a property of an expression, not of a type.** There is no `T!` type, because [D014](01-decisions.md#d014) has one universal `Error` and no `Result[T, E]`. What the typechecker carries alongside each expression's type is a `fallible` bit, set by a call to a `!`-marked function and cleared by `!` or `else`. A `fn` type carries `fallible` because a signature must record it; a value never does.
- **`TY_NONE` is the absence of a return, not a unit value.** `fn log_it(msg: str)` returns `TY_NONE`; `TY_NONE` cannot be a parameter type, a field type, an element type, or the subject of an operator. There is no `()` value to bind.

`nil` has no type of its own. It is a literal that is checked against an expected `T?` and has no principal type when there is nothing to check it against — so `let x = nil` is `DT0201`, not a mystery type. That is what keeps a bottom type out of the lattice.

Primitives and named types are interned: one canonical `sema_type` each, held in the unit's type table, so equality is pointer equality. Compound types — lists, maps, function types — are compared structurally by `type_eq`, which is finite because it stops at names. Interning them too would need a hash table that resizes, which the arena does not do.

**Recursion must pass through an indirection.** `type Node { next: Node? }` is fine, and so is `type Tree { kids: [Tree] }`, because an optional struct, a list, and a map are all pointers in a slot. `type Node { next: Node }` has no finite layout and is `DT0226`, reported with the cycle named. A type alias cycle is `DT0227`. Both are detected with an explicit colour-marking walk over the declared types after collect, which is also where struct layout order is decided ([05-runtime.md](05-runtime.md#containers-are-packed)).

A **type alias is transparent**: `type Id = int` makes `Id` another spelling of `int`, assignable in both directions. A nominal alias would be a distinct type with no constructor and no conversion, since there are no generics and no newtype ceremony to give it either, so transparency is the only reading that leaves the feature usable.

### Inference and where an annotation is required

Full inference for locals ([D015](01-decisions.md#d015)), implemented as **bidirectional checking** ([D090](01-decisions.md#d090)): `infer(expr) -> type` and `check(expr, expected)`. An expected type flows inward wherever one exists, which is what makes the documented forms work without annotation.

An expected type exists in exactly these positions:

| Position | Expected type from |
| --- | --- |
| the initializer of an annotated `let` | the annotation |
| an argument | the parameter's declared type |
| a field initializer in a struct literal | the field's declared type |
| an element of a list or map literal that already has an expected type | that type's element or value type |
| `return e` | the enclosing function's, method's, route's, or lambda's declared return type |
| the value form of `else` | the type of the expression to its left, with optionality and fallibility removed |
| a markup interpolation or attribute value | the position's renderable set, which is a check against a set rather than against one type |

Six expression forms have **no principal type** and are legal only against an expected type. Without one, they are `DT0201` with a message naming the annotation to write:

`[]`, `{}` (both empty), `nil`, `.variant`, a list or map literal whose elements disagree, and a lambda whose return type is undeclared *and* whose body returns nothing on some path.

Annotations are **mandatory** in every declaration position: parameters, fields, return types, and route parameters. There is no inferred signature anywhere in the language, which is what makes the collect step able to build the whole symbol table with types attached before any body is walked.

### Assignability

`assignable(from, to)` holds when, and only when ([D088](01-decisions.md#d088)):

1. `from` and `to` are the same type, where "same" is nominal for named types, structural for compounds, and alias-transparent; **or**
2. `to` is optional, and `from` is the same type without the optional flag — widening a value into an optional; **or**
3. `from` is `nil` and `to` is optional.

**There is nothing else.** No `int` to `float`, no `float` to `int`, no `str` to `html`, no `T` to `any`, no `any` to `T`, no enum to `int`, no truthiness anywhere. Every conversion doot has is written with `as`, and the set of those is closed. A mismatch that is *only* optionality gets its own code, `DT0404`, rather than the general `DT0200`, because the fix is `else` and naming it is what makes the message actionable.

The direction that is deliberately absent is `T?` to `T`. Unwrapping is `else` ([D017](01-decisions.md#d017)), there is no `?.` and no `!!`, and an optional used where a value is required is the single most common mistake this rule catches.

### `as`

`as` is a **closed, total table** ([D089](01-decisions.md#d089)). Anything not in it is `DT0240`, whose message names the alternative.

| From | To | Result | Behaviour |
| --- | --- | --- | --- |
| `int` | `float` | `float` | exact below 2⁵³, rounded above |
| `float` | `int` | `int` | truncates toward zero; a **fault** on NaN, infinity, or a value outside `int` |
| any type | `any` | `any` | boxes ([05-runtime.md](05-runtime.md#values)) |
| `any` | `T` | `T?` | `nil` when the box does not hold a `T` |
| `str` | `bytes` | `bytes` | the UTF-8 bytes |
| `bytes` | `str` | `str?` | `nil` when the bytes are not valid UTF-8 |
| an enum | `int` | `int` | the variant's ordinal |

Three notes, each of which is the reason a row is shaped that way:

- **`as` never fails as a value.** Where a conversion can be refused, its result is optional, so `let n = v as int else 0` handles it with the mechanism the language already has. The one exception is `float as int`, which faults rather than yielding an optional, because it is arithmetic and [D003](01-decisions.md#d003) makes arithmetic that cannot produce a right answer a fault rather than a value to check.
- **`any` supports `as` and nothing else.** No indexing, no field access, no arithmetic, no comparison. Navigating a decoded JSON document means casting to `{str: any}` or `[any]` first. Letting operations through `any` would put the runtime type check back into the interpreter that [D002](01-decisions.md#d002) removed it from.
- **`str as html` is `DT0241`, not `DT0240`**, and its message names `html.raw`. It is the one impossible cast worth its own code, because it is the one an author reaches for when they are about to create an XSS hole, and the diagnostic is the last place to say so.

A cast whose source and target are the same type is permitted and does nothing, and warns (`DT0606`).

### Operators

Every operator is monomorphic. A mismatch is `DT0202`, naming both operand types.

| Operator | Accepts | Result |
| --- | --- | --- |
| `+` | `int`+`int`, `float`+`float`, `str`+`str` | the operand type |
| `-` `*` `/` | `int`, `float`, matched | the operand type |
| `%` | `int` only | `int` |
| unary `-` | `int`, `float` | the operand type |
| `==` `!=` | two values of one type, or a `T?` against `nil`, or a `T?` against a `T` | `bool` |
| `<` `<=` `>` `>=` | `int`, `float`, `str`, matched | `bool` |
| `in` | `T` in `[T]`, `K` in `{K: V}`, `str` in `str` | `bool` |
| `and` `or` `not` | `bool` only | `bool` |

`%` is integer-only because [05-runtime.md](05-runtime.md#bytecode) has `MOD_I` and no `MOD_F`; the opcode table and the type rules agree by construction rather than by coincidence. `int / int` truncates toward zero and faults on a zero divisor; `float / float` follows IEEE 754. `str` comparison is byte order over the UTF-8 encoding, which is code-point order — stated because "alphabetical" is not what it is, and a locale-aware comparison is not something doot has.

Mixed arithmetic is an error, not a promotion: `1 + 2.0` is `DT0202` and the fix is `1 as float + 2.0`. Silent widening is how a monetary calculation in [D020](01-decisions.md#d020)'s integer cents turns into a float.

**There is no truthiness** ([D091](01-decisions.md#d091)). The condition of `if`, `while`, a markup `{if}`, and both operands of `and`/`or` must be exactly `bool`; anything else is `DT0203`. `if u.bio` is an error and `if u.bio != nil` is the spelling. Comparing two different types is `DT0211` rather than silently `false`, so `id == "3"` is caught rather than always failing at runtime.

**An expression statement must be able to have an effect.** The grammar permits `expr NEWLINE` as a statement, and the typechecker accepts it only when the expression is a call, a `!` over a call, or an `else` over a call. `total + 1` as a statement is `DT0214`. Discarding a call's return value is fine; discarding a *fallible* call's result is not, because rule 3 requires `!` or `else` regardless of whether the value is wanted.

### Structs, methods, and `self`

A struct literal must name a declared struct type; it must initialize every field that has no default (`DT0221`) and may not name a field the type does not have (`DT0222`). Duplicate fields are already `DT0043` in the parser. Field order in a literal is free — it is matched by name — and has no bearing on layout, which the compiler chooses for alignment.

`with` produces the same type as its subject, which must be a non-optional struct (`DT0223`); each named field must exist and its value must be assignable to the field's declared type. That is the whole of `with`, and it is what makes deep immutability comfortable ([02-syntax.md](02-syntax.md#structs)).

Methods, argued in [D092](01-decisions.md#d092):

- A method's receiver must be a **struct or enum declared in this program** (`DT0107`). A method may not be attached to a primitive, to `html`, to `[T]`, to a stdlib type, or to an alias's target. The stdlib is closed ([D029](01-decisions.md#d029)), and if user code could attach to `str`, then reading `s.upper()` would require knowing which files are in the project — which is precisely the "where is this method defined" question [D016](01-decisions.md#d016) exists to answer.
- A method must be declared in the **same file as its type** (`DT0108`). That makes "beside the type, always" the complete answer to the same question, and it makes a type's method list a fact the collect step establishes per file rather than a whole-program search.
- A method's name may not collide with a field of its receiver (`DT0109`), so `u.f` never has to choose between a field of function type and a method.
- A method's visibility is its type's. `pub` on a method is `DT0106` — redundant when the type is `pub` and meaningless when it is not.
- `self` is a parameter, therefore immutable ([D008](01-decisions.md#d008)). A method cannot assign to `self` or to a field through it (`DT0304`); it returns a new value, usually with `with`.
- **There is no static method form in v0.1.** `fn User.guest() -> User` cannot be written, because [rule 9](03-grammar.md#well-formedness-rules) requires `self` first and `DT0034` is registered and tested. A member access on a type name therefore resolves only to an enum variant, and a free function is how a value is constructed. The illustration `User.guest()` in [D013](01-decisions.md#d013) and [02-syntax.md](02-syntax.md#errors) is not declarable under the rule as written; the rule wins, and the doot spelling of that example is a free function.

### Enums and `match`

Enums are tag-only and nominal ([D018](01-decisions.md#d018)). `Status.active` names a variant through its type; `.active` names one where an expected enum type exists, and is `DT0225` where none does.

`match` is a **statement** — the grammar has `match_stmt` and no match expression — so it produces no value, and each arm's `expr` form is subject to the effect rule above.

| Subject type | Exhaustive when |
| --- | --- |
| an enum | every variant appears, or an `else` arm exists |
| `bool` | both `true` and `false` appear, or an `else` arm exists |
| `int`, `str` | never; an `else` arm is required |
| anything else | `DT0423` — the type cannot be matched |

A non-exhaustive `match` with no `else` is `DT0420`, which is [rule 8](03-grammar.md#well-formedness-rules), and the message lists the variants that are missing. A pattern whose type does not match the subject is `DT0421`; a duplicate pattern is `DT0422`, an error because which of two identical arms runs is not observable and the author certainly meant something else; an `else` arm that is not last is `DT0424`. An `else` arm on an already-exhaustive `match` is permitted and warns (`DT0605`), since removing it is safe and keeping it is sometimes deliberate.

Matching an **optional** subject is allowed: `nil` is not a pattern, so an optional enum can only be matched exhaustively through an `else` arm. That is stated rather than left implicit because the alternative — inventing a `nil` pattern — would add pattern syntax the grammar does not have.

### `with`, lambdas, and `defer`

`with` is above. Lambdas ([D093](01-decisions.md#d093)):

- Parameter types are mandatory, which the grammar already guarantees (`param := IDENT ":" type`), so a lambda's parameters are never inferred.
- **An undeclared return type is inferred**, from the expression in the `=> expr` form and from the `return` statements in the block form; all of them must agree, and a block with none returns `TY_NONE`. Fallibility is inferred with it. This is forced by the documentation: `db.tx(fn() { db.exec(...)! })!` writes no return type on the lambda and propagates inside it, so an undeclared lambda that cannot be fallible would make the documented transaction unwritable.
- A **declared** return type must declare `!` if the body propagates; the inference above applies only when the whole return type is omitted.
- `return` inside a lambda returns from the lambda, not from the enclosing function. `!` inside a lambda propagates to the lambda's own signature.
- **A lambda captures immutably, always.** Every name it reads from an enclosing scope is captured by value and is immutable inside the body; assigning to a captured binding is `DT0302`. This is not a `spawn`-specific rule and is not weakened outside `spawn`: it keeps a `var` local's ownership actually unique, which is what lets `xs.push(x)` mutate in place ([D008](01-decisions.md#d008)), and it means moving a loop body into a lambda can never quietly change aliasing. Accumulating into an outer `var` is a `for` loop.

`defer` takes a call ([DT0041](10-frontend.md#syntactic)) and **the call must not be fallible** (`DT0408`). A deferred call runs on the return path and when a fault terminates the task ([D012](01-decisions.md#d012)), and in neither place is there a caller to propagate an error to. `defer` on a fallible operation means doing it explicitly before returning.

A `test` block is checked as a **fallible function returning nothing**, so `!` works inside one and a propagated error fails that test. A `stream` body is likewise checked as fallible and returning nothing, so an error ends the stream.

### Markup context typing

A markup literal has type `html`. `html` is not `str` and there is no implicit conversion in either direction — that is the whole of [D021](01-decisions.md#d021)'s soundness argument, and it survives only if every path between them is closed. There are exactly four places a `str` could become `html`, and all four are shut ([D096](01-decisions.md#d096)):

1. assignability — rule 1 of [Assignability](#assignability) is nominal identity, and `html` is not `str`;
2. `as` — `str as html` is `DT0241`, naming `html.raw`;
3. a text interpolation — escaped, always, never spliced;
4. an attribute interpolation — attribute-escaped, always.

So `html.raw` is the only entry point, which is what [04-stdlib.md](04-stdlib.md#html) claims and this is where it becomes true.

**Text position.** `${e}` in element content accepts a **renderable** type: `str` (HTML-text-escaped), `int`, `float`, `bool`, `html` (spliced, already escaped by construction), `[html]` (concatenated), and the optional of any of those, where `nil` renders as nothing per [03-grammar.md](03-grammar.md#semantics). Anything else — `bytes`, `any`, an enum, a struct, a map, a list of non-`html` — is `DT0260`, and the message names the method to call instead.

**Attribute-value position.** An attribute value is a string literal, possibly with interpolations, or a single interpolation.

| Interpolated type | Effect |
| --- | --- |
| `str`, `int`, `float` | attribute-value escaped and written |
| `bool` | the attribute is emitted or omitted entirely — permitted **only** when the interpolation is the whole value (`DT0263`) |
| `T?` | `nil` omits the attribute; permitted only as the whole value, for the same reason |
| `html` | `DT0262`, always |
| anything else | `DT0261` |

`html` is refused in an attribute rather than escaped because splicing markup into an attribute is an injection with no legitimate use, and because a value that is `html` was escaped for *text*, not for an attribute — two different escapes, and using one for the other is the bug this type system exists to prevent.

A spread attribute `...e` requires `{str: str}` (`DT0264`).

**Control blocks are nodes, not expressions.** `{if}` and `{for}` have no type of their own; the literal containing them is `html` regardless of which branch runs. A `{if}` condition must be `bool` (`DT0265`); a `{for}` subject follows the iterability rules below (`DT0266`) and binds its variables in a `SCOPE_LOOP`. A `{if}` chain with no `{else}` contributes nothing when no branch matches, and a `{for}`'s `{else}` arm renders when the subject is empty ([02-syntax.md](02-syntax.md#markup)).

**Element and attribute names are not validated against an HTML vocabulary.** Only the void-element list is known, and that is the parser's ([DT0063](10-frontend.md#markup-syntax)). HTML is extensible — `data-*`, `aria-*`, custom elements — so a closed vocabulary would reject valid pages, which is a worse failure than accepting a typo in a tag name.

**One policy check lives here rather than in the emitter.** [D028](01-decisions.md#d028) injects a CSRF token into every `<form>` whose method is state-changing, and the compiler can only do that if it can see the method. A `<form>` whose `method` attribute is interpolated is therefore `DT0540`, with a message saying the method must be written literally so that CSRF protection can be applied. Without the check, `<form method="${m}">` would silently produce an unprotected form — a security default that fails quietly, which is exactly what [D028](01-decisions.md#d028) exists to avoid.

**String interpolation is not markup interpolation.** `"${e}"` accepts `str`, `int`, `float`, and `bool`, and refuses `html`, an optional, and everything else. The asymmetry with markup is deliberate: [03-grammar.md](03-grammar.md#semantics) says `nil` renders as nothing *in markup*, and nothing says it anywhere else. Turning an absent value into an empty string is the invisible failure [D017](01-decisions.md#d017) rejects `?.` for, and `html` in a `str` would lose the distinction the type exists to make.

### Iteration

`for x in e` and `{for x in e}` accept:

| Subject | One variable | Two variables |
| --- | --- | --- |
| `[T]` | `x: T` | index `int`, value `T` |
| `{K: V}` | `DT0205` | key `K`, value `V` |
| `Subscription[T]`, `chan[T]` (v0.2) | `x: T` | `DT0205` |

Anything else is `DT0204`. `str` is not iterable — `s.chars()` returns a list, which is the spelling [02-syntax.md](02-syntax.md#strings) uses, and it is what keeps the byte-versus-character distinction visible. A map with one variable is `DT0205` rather than "iterate keys", because there is no reason for a reader to have to remember which of two things a one-variable map loop yields.

Indexing follows the same shape as [D012](01-decisions.md#d012): `xs[i]` needs an `int` and faults out of range, `m[k]` needs a `K` and faults on a missing key, `b[i]` on `bytes` yields an `int`, and `s[i]` on a `str` is `DT0209` with a message naming `s.slice` and `s.chars()`. The safe accessors are `xs.get(i)` and `m.get(k)`, both returning `T?`, which is the trade [D012](01-decisions.md#d012) already argued.

### Attributes on declarations

The closed set of twelve ([D043](01-decisions.md#d043)) is validated for kind and for type. `@len` applies to `str`, `[T]`, and `bytes`; `@min` and `@max` to `int` and `float`; `@one_of` to `str` and `int` with arguments of the field's type; `@email`, `@url`, and `@trim` to `str`; `@max_size` and `@content_type` to `Upload`, and `@content_type` also to a route; `@before` and `@after` to a route or a group; `@deprecated` to any declaration. An attribute in the wrong place or on the wrong type is `DT0230`; wrong arguments are `DT0231`. A reference to a declaration carrying `@deprecated` warns at the use site (`DT0604`), which is what [02-syntax.md](02-syntax.md#attributes) promises.

A parameter or field with a default may not be followed by one without (`DT0232`), since there are no named arguments and a defaulted parameter followed by a required one is unreachable. A default value must be assignable to its declared type (`DT0228`) and must be a constant expression — a literal, a list or map of constants, a struct literal of constants, or a negation of one (`DT0229`). Calls in a default value would run at an unspecified time; the place for computed initialization is the function body.

---

## The error and optional model

This discharges [rules 3, 4, and 16](03-grammar.md#well-formedness-rules).

`?` means absent-possible and `!` means failure-possible, with the same mark at the declaration and at the call site ([D013](01-decisions.md#d013)). The typechecker carries, for every expression, a type — whose `optional` flag answers the first — and a `fallible` bit, which answers the second.

**A fallible expression must be handled immediately.** `!` and `else` are the only two contexts in which one may appear; anywhere else is `DT0403`. That is the teeth in [rule 3](03-grammar.md#well-formedness-rules), and it is what makes error handling visible by eye: a call to a `!` function is followed by a `!` or an `else` on the same line, or the program does not compile.

### `!`

`x!` requires `x` to be fallible (`DT0400`, rule 3) and clears the bit. It leaves optionality alone, so `db.find[User](...)!` is a `User?`, exactly as [02-syntax.md](02-syntax.md#data-access) documents.

`!` also requires the enclosing function to be fallible (`DT0402`), because propagation has to have somewhere to go. "Enclosing function" means the nearest `fn`, method, route, stream, lambda, or `test` — a lambda is a function for this purpose, so `!` inside a lambda makes the *lambda* fallible and says nothing about its caller. At the top level of a file there is no enclosing function at all, so `!` in a top-level `let` initializer is `DT0402` with a message saying so; the spelling that works there is `else` with a value, which is what `env.get("NAME") else "default"` in [02-syntax.md](02-syntax.md#configuration) already is.

### `else`

`else` requires its left operand to be fallible or optional, or both (`DT0401`, rule 4), and **handles everything it finds at once** ([D094](01-decisions.md#d094)): the result is the left type with both the optional flag and the fallible bit cleared.

The two forms are distinguished in the AST by `coalesce_form`, which the parser sets specifically so this check needs no re-derivation from the tree ([ast.h](../src/parse/ast.h)):

- **`COALESCE_VALUE`** — `x else v`. `v` must be assignable to the handled type (`DT0407`).
- **`COALESCE_BLOCK`** — `x else { ... }` or `x else err { ... }`. The block **must diverge on every path** (`DT0440`, rule 16). It supplies no value, which is what keeps blocks from being expressions and leaves the language with one expression syntax.

The `err` binding is where handling both at once needs a rule rather than a convention:

| Left operand | `else v` | `else { }` | `else err { }` |
| --- | --- | --- | --- |
| fallible only | yields `T` | must diverge | `err` is `Error` |
| optional only | yields `T` | must diverge | `DT0405` — there is no error to bind |
| both (`T?!`) | yields `T` | must diverge | `DT0406` — write `!` first |

`DT0406` is the interesting one. On a `T?!` there are two distinct reasons to take the `else` branch and only one of them has an `Error`, so binding `err` would either lie about the nil case or need a nested optional the language does not have. Forcing `db.find(...)! else err { ... }` costs one character and makes the two failures separately handled, which is what the author wanted in the first place. Refusing here rather than inventing a semantics is the same move [D017](01-decisions.md#d017) made against `?.`.

### Divergence and reachability

One analysis serves three checks ([D095](01-decisions.md#d095)): rule 16's `else` block, "this function can finish without returning a value" (`DT0441`), and the unreachable-code warning (`DT0602`).

A statement **diverges** when control cannot continue past it:

- `return`, `break`, and `continue` diverge.
- An `if` diverges when it has an `else` and every branch diverges.
- A `match` diverges when it is exhaustive — including via an `else` arm — and every arm diverges.
- A `while` whose condition is the literal `true` and which contains no `break` targeting it diverges.
- A block diverges when any statement in it diverges.

A statement after a diverging statement in the same block is unreachable and warns once, at the first such statement, with the diverging statement as a related label. A function or method whose return type is not `TY_NONE` and whose body does not diverge is `DT0441`. A `return` with no value in a function that returns something is `DT0213`, and with a value in one that returns nothing is `DT0212`.

Rule 16 says a block form must diverge "or a fault", and no expression in v0.1 is statically known to fault: there is no `never` type and no function that cannot return. So the structural list above is the complete rule, and it is complete in the direction that matters — it never accepts a block that can fall through.

### Originating an error

`return e` in a fallible function accepts **either the declared return type or `Error`**. That is the whole of what the typechecker needs to know, and it is enough for any spelling the standard library eventually gives to error construction. Returning an `Error` from a function that is not fallible is `DT0409`.

How an `Error` value is *built* is standard-library surface and is settled with `Error`'s signatures, not here. What is settled here is that no new syntax is involved: `Error` is a predeclared type, `return` is the existing statement, and the rule above is one line in the return check.

---

## Mutability

This discharges [rules 2 and 15](03-grammar.md#well-formedness-rules), and it is the resolver's work because every question it asks is about a binding rather than a type.

[D008](01-decisions.md#d008) has three rules and the checker enforces each at one place:

**No mutable globals.** A top-level `var` is already `DT0033` in the parser (rule 1). What remains here is that a top-level `let`'s initializer must terminate: initializers run once at startup, in an order derived from the references between them, and a cycle among them is `DT0111`. The dependency order is a topological sort over top-level `let` symbols, computed after collect; it is also what `doot run` needs to emit startup code, so it is an artifact rather than a check that is thrown away.

**Immutable parameters.** A parameter, `self`, a loop variable, an `err` binding, and `req` are all immutable bindings, so assigning through any of them is an error.

**Deep `let`.** A `let` binding and everything reachable through it is immutable. The check has two halves, which is the part worth writing down:

- **The lvalue root.** `lvalue := IDENT ( "." IDENT | "[" expr "]" )*`, so an assignment target is always rooted at one name. That name must resolve to a `SYM_VAR`. `l.f = x` and `l.xs[0] = x` are refused for the same reason `l = x` is, and by the same test — the depth of the path does not matter, which is exactly what "deeply immutable" means. All of it is `DT0300` (rule 2), with the message naming *which* kind of immutable binding it is and a related label at the declaration. Assignment to `self` or through it is `DT0304`, because the message is different enough to be worth its own code: the fix is `with`, not `var`.
- **The mutating method.** `xs.push(x)` mutates in place, so `let xs = ["a"]` followed by `xs.push("b")` must be refused, and no lvalue is involved. The module table therefore records, per member, whether it mutates its receiver ([D097](01-decisions.md#d097)), and calling a mutating member requires the receiver's root binding to be a `SYM_VAR` (`DT0303`). Without that column the deep-`let` guarantee would hold for assignment and leak through every mutating method in the standard library, which would make it not a guarantee.

**Lambdas capture immutably** (`DT0302`), as described under [`with`, lambdas, and `defer`](#with-lambdas-and-defer).

**Rule 15** is then narrow, which is the point: since captures are already immutable everywhere, the only way for `spawn` to reach a mutable binding is a `var` local passed directly as an argument, and that is `DT0301`. `spawn` is `DT0046` until v0.2 and the stage barrier means the resolver never sees a program containing one, so `DT0301` stays a reservation until tasks land — which is why rule 15 is the one pending rule with no spec tests in v0.1.

---

## The schema checker

[D033](01-decisions.md#d033) is the decision that makes "you will never want a framework" credible, and this is the stage that keeps it. It discharges [rule 13](03-grammar.md#well-formedness-rules).

### Building the schema

The schema is derived by **replaying the forward-only migrations into an in-memory SQLite database** ([D034](01-decisions.md#d034), [D098](01-decisions.md#d098)):

1. If a `migrations/` directory exists, read every `NNN_name.sql` in it. A file whose name does not match that shape is `DT0158`; two files with the same number are `DT0157`. Numbers need not be contiguous — gaps are what happens when a branch is abandoned — but they must be unique, because the apply order is the whole contract.
2. Otherwise, if `schema.sql` sits beside the file being checked, use it. This is single-file mode ([D041](01-decisions.md#d041)) and it is the form the specification suite uses.
3. Otherwise there is no schema. That is fine for a program with no `db` call and is `DT0155` at the first one.

Each file is executed in order against a fresh `:memory:` database. A statement that fails is `DT0156`, carrying SQLite's own message and a span computed from `sqlite3_error_offset()` into the `.sql` file's `source` — so a broken migration points at the byte that broke rather than at the file.

While replaying, the checker records **which migration and which byte offset created each table**. That is what lets a later diagnostic attach the `related` label the documented example shows: "table `users` declared here", pointing into `migrations/001_init.sql` ([06-tooling.md](06-tooling.md#diagnostics)).

The replay is **read-only with respect to the real database.** `doot check` never opens the project's database file. Applying migrations is `doot migrate`.

### Locating SQL literals

The checker walks the typed AST for calls whose callee resolves to a `db` entry point with a SQL parameter: `db.one[T]`, `db.find[T]`, `db.all[T]`, `db.count`, `db.exec`, and `db.batch`. `db.tx` takes a lambda and no SQL.

**The SQL argument must be a literal** (`DT0143`): a raw string, or a plain string with no interpolation — one `str_part` whose `value` is `NULL`. A computed or interpolated SQL argument cannot be prepared at compile time, so accepting one would mean silently exempting it from every check in this section, and it is also the only way an injection could be written. The message says to pass values as `?` parameters.

**A literal must contain exactly one statement** (`DT0159`), detected by `sqlite3_prepare_v2` leaving a non-empty tail. Two statements in one literal would run only the first, silently.

Each distinct literal is prepared once, keyed by its bytes, so a statement repeated across files costs one preparation.

### Parameters

Only **positional `?` placeholders** are permitted. A named parameter — `:name`, `@name`, `$name` — is `DT0145`, because arguments are passed positionally and there is no name for the compiler to bind from.

The placeholder count comes from `sqlite3_bind_parameter_count`; a mismatch against the number of arguments after the SQL is `DT0144`, reported at the argument list with both counts.

An argument's type must be bindable (`DT0160`):

| doot type | Stored as |
| --- | --- |
| `int`, `bool` | INTEGER (`bool` as 0 or 1) |
| `float` | REAL |
| `str` | TEXT |
| `bytes` | BLOB |
| `time.Time` | INTEGER, nanoseconds ([04-stdlib.md](04-stdlib.md#time)) |
| an enum | TEXT, the variant's name |
| an optional of any of the above | the same, or NULL |

Anything else is `DT0160`, naming the conversion to write. An enum binds as **text rather than as its ordinal** deliberately: a text column is readable in the database and, decisively, reordering an enum's variants in the source is a refactor that must not silently change what stored rows mean. The ordinal is a runtime representation ([05-runtime.md](05-runtime.md#values)), not a storage format.

`DT0160` is reported by the typechecker rather than the schema checker, because it is a statement about an argument's type and the typechecker is already visiting every argument. It sits in the SQL range because a diagnostic's range is its subject, not its stage — see [Ranges are subjects, not stages](#ranges-are-subjects-not-stages).

### Result shapes

For `db.one[T]`, `db.find[T]`, and `db.all[T]`, `T` must be a struct declared in this program (`DT0153`), and the prepared statement's result columns are matched against its fields **by name, order-independently**:

- a result column with no field of that name is `DT0146`;
- a field with no result column is `DT0147`;
- two result columns with the same name are `DT0150`, since the mapping would be ambiguous;
- a column whose affinity cannot hold the field's type is `DT0148`.

`select *` needs no special handling: the prepared statement's column list is already expanded, so it is checked exactly as an explicit list is. That is the property that makes the documented `db.all[Msg]("select * from msgs ...")` safe rather than a hole.

Affinity comes from `sqlite3_column_decltype` when the column has one — a base-table column — and is treated as unconstrained when it does not, which is the case for `count(*)` and any computed expression. Unconstrained columns accept any storable type, because SQLite will coerce and there is nothing to check against.

**Nullability is checked, and the direction of doubt is stated.** A column is treated as non-nullable **only** when `sqlite3_column_origin_name` identifies a base-table column declared `NOT NULL` *and* the statement contains no outer join. The outer-join test is lexical — a case-insensitive search of the literal for `left`, `right`, `full`, or `outer` outside string literals and quoted identifiers — and when it fires, every column is treated as nullable. A nullable column mapped to a non-optional field is `DT0149`.

That test is conservative rather than exact, and conservative in the right direction: being wrong toward nullable asks the author for a `T?` they did not strictly need, while being wrong toward non-null lets a NULL reach a slot that cannot hold one, which is the failure the check exists to prevent. Parsing SQL to do better would mean owning a SQL parser, which [D032](01-decisions.md#d032) and [D035](01-decisions.md#d035) both argue against.

The remaining entry points:

- `db.count` requires **exactly one** column of INTEGER or unconstrained affinity (`DT0152`).
- `db.exec` requires **zero** result columns (`DT0151`); a `select` or a `returning` clause passed to `db.exec` is a mistake whose message names `db.one` and `db.all`.
- `db.batch(sql, rows)` requires `rows: [T]` for a struct `T` whose fields bind to the placeholders **in declaration order**, and whose field count must equal the placeholder count (`DT0154`). Positional binding from a struct is the minimal rule that makes `db.batch` checkable without tuples, which the language does not have.

### Did-you-mean, and `DT0142`

Two SQLite prepare failures are recognized and given their own codes, because they are the two an author actually produces and both admit a suggestion:

- **no such table** → `DT0141`, listing the tables the schema does have.
- **no such column** → `DT0142`, with the column's own span, a `diag_fix` replacing it with the nearest column name of the table by edit distance, and a `related` label at the migration that created the table.

Everything else SQLite refuses is `DT0140`, which is rule 13's code: SQLite's message, the offset it reports, and no suggestion.

`DT0142` is not an arbitrary number. It is the code the running example in [06-tooling.md](06-tooling.md#diagnostics), [09-engineering.md](09-engineering.md#2-spec-tests--testsspec), and [D038](01-decisions.md#d038) has used since the first commit, down to the message, the suggestion, and the related span. Allocating it to the diagnostic it was always illustrating turns three documents' example into a specification, at the cost of nothing.

---

## The route checker

This discharges [rules 5, 6, and 7](03-grammar.md#well-formedness-rules). Because `route` is a declaration rather than a runtime registration, the compiler knows the entire table ([D024](01-decisions.md#d024)), and this stage is what that buys.

### Patterns

A pattern is a string literal, split on `/`. Each segment is a **literal**, a **parameter** (`:name`), or the **wildcard** (`*name`, permitted only as the last segment). A pattern must begin with `/` and must not end with one unless it is exactly `"/"`; anything else is `DT0503`. A group prefix must begin with `/` and must not end with one, and is `DT0506` otherwise. Group prefixes are concatenated with their routes' patterns before any other check, so the table is flat.

**Rule 5** (`DT0500`): every `:name` in the pattern has a parameter of that name, and every parameter that is not `form`, `query`, or `json` appears in the pattern. Both directions in one code, with the message naming which side is short. A parameter name appearing in both a group prefix and its route's pattern is `DT0509`.

A path parameter's type must be `int`, `float`, or `str` (`DT0504`). A URL segment is text, the useful conversions are numeric, and a failed conversion is a 404 rather than a fault ([02-syntax.md](02-syntax.md#routes)) — so any other type would need a conversion policy that a `str` parameter and one line in the body expresses more clearly. The wildcard binds a `str`. A path parameter may not be optional and may not carry a default (`DT0505`), because the pattern always supplies it.

### Request binding

**Rule 6** (`DT0501`): a `form`, `query`, or `json` parameter must be of struct type, and **at most one of the three may appear per route** — the rule as written in [03-grammar.md](03-grammar.md#well-formedness-rules), and the right rule: two bound structs mean two validation sources and two 422 paths for one request, and the fix is to put the other fields in the path or in the same struct.

`form` and `json` require a method that carries a body — `POST`, `PUT`, or `PATCH` (`DT0508`). `query` is allowed on any method.

Every field of a bound struct must be decodable from a request (`DT0520`): `int`, `float`, `bool`, `str`, `bytes`, an enum, `Upload`, a `[T]` of those, or an optional of any. A struct-valued field, a map, or a function type is `DT0520`, naming that a request body is flat. A field typed `Upload` makes the route multipart and therefore requires `form` rather than `query` or `json` (`DT0523`).

Validation attributes on those fields are typechecked as any other attribute is; binding runs them before the handler body and short-circuits to 422 ([D025](01-decisions.md#d025)).

`req` is a predeclared binding of type `Request`, in scope in a route body, a stream body, and a hook. Referring to it anywhere else is `DT0522`.

Hooks: `@before(f)` and `@after(f)` must name a function that **takes no parameters and returns nothing**, fallible or not (`DT0521`) — an error from a fallible one short-circuits the request ([02-syntax.md](02-syntax.md#groups)). A hook is checked in route context, so `req` is available inside it; threading the request in as a parameter would make every hook's signature restate what `req` already provides, and `@before(auth.require)` is written with no arguments. The well-known hooks `on_error(err: Error) -> html` and `on_not_found() -> html` are looked up across the unit and checked against those signatures with the same code; more than one declaration of either is `DT0113`.

A route's return type must be one of the response types in [02-syntax.md](02-syntax.md#return-types): `html`, `str`, `bytes`, a struct, `[T]`, `{K: V}`, `redirect`, `Response`, or absent, which is a 204. Anything else is `DT0507`. A `send` value in a stream must be `html` or `str`, and the event name in the two-argument form must be `str` (`DT0541`).

### Conflicts

**Rule 7** (`DT0502`) requires that patterns neither conflict nor shadow each other, and the second half is discharged by defining matching precedence rather than by checking anything ([D099](01-decisions.md#d099)).

At each segment position, **a literal beats a parameter, and a parameter beats the wildcard.** That is the natural behaviour of the compile-time trie [05-runtime.md](05-runtime.md#request-lifecycle) already specifies, and stating it as a language rule means `/users/new` and `/users/:id` both remain reachable, as do `/files/:name` and `/files/*rest`. **Shadowing is therefore unrepresentable**, in the same way [D008](01-decisions.md#d008) makes data races unrepresentable: there is no program in which one route hides another.

What remains is genuine ambiguity, and it has exactly one form: **two routes with the same method whose patterns are identical up to parameter renaming.** `/users/:id` and `/users/:slug` under `GET` are the same matcher and the trie cannot choose. That is `DT0502`, reported at the later declaration with a `related` label at the earlier one — which is what the `related` field exists for ([06-tooling.md](06-tooling.md#diagnostics)).

The check is a pairwise comparison over the finished table. It is O(n²) in the number of routes, on a table whose size is bounded by the target use case, and it is exact — which a trie insertion order would not be, because the answer must not depend on the order files were walked in.

### The route table

The stage's artifact is an ordered list of `(method, pattern, segments, params with their sources, hooks, return type, declaring file and line)`. Three consumers: the emitter's matching trie, `doot routes`, and `doot check`, which reports nothing from it but must have run it.

---

## Semantic diagnostics

Six of the nine ranges in [06-tooling.md](06-tooling.md#code-ranges) belong to the semantic pass, and all six are **allocated in full here, now** ([D100](01-decisions.md#d100)) for the reason [D064](01-decisions.md#d064) gave for the front end: a code is permanent once assigned ([D050](01-decisions.md#d050)), so the numbering is a one-way decision, and allocating it up front means a code is chosen by where it belongs rather than by what was free that week. It also removes the registry as a coordination point between workstreams that would otherwise both reach for the next integer — which matters more here than it did for the front end, because [Implementation order](#implementation-order) has several workstreams running at once.

| Range | Subject | Stage that reports it |
| --- | --- | --- |
| `DT0100`–`DT0199` | names, modules, resolution, schema | resolver, schema checker |
| `DT0200`–`DT0299` | types | typechecker |
| `DT0300`–`DT0399` | mutability | resolver |
| `DT0400`–`DT0499` | errors, optionals, exhaustiveness, divergence | typechecker |
| `DT0500`–`DT0599` | routes, request binding, markup policy | route checker |
| `DT0600`–`DT0699` | warnings | resolver, typechecker |

Everything allocated below is a **reservation, not a registration** ([D065](01-decisions.md#d065)). `src/base/diag_codes.h` gains a row only in the change that adds the code's implementation and the spec test that produces it; pre-populating it would break the `docs` gate immediately and would put explanations in `doot explain` for diagnostics the compiler cannot emit.

### Ranges are subjects, not stages

Two things about the range table needed settling before the sub-allocation could be written, and both are visible in the table above.

**A range is a subject, not a pipeline stage.** [05-runtime.md](05-runtime.md#compiler-pipeline) makes the schema checker its own stage while [06-tooling.md](06-tooling.md#code-ranges) puts its codes in a range labelled "names, modules, resolution, SQL/schema", and that looked like an inconsistency. It is not: the four subjects in that label are one subject. A scope, a module namespace, and a database schema are all **things outside the expression that a name must be resolved against**, and a misspelled column is the same kind of mistake as a misspelled module member. The stage boundary and the range boundary are answering different questions — *who checks this* and *what is this about* — and they are allowed to disagree. `DT0160` is the clean case: the typechecker reports it, because only the typechecker knows the argument's type, and it lives in the SQL block, because it is about SQL.

**A reserved rule code sits at the start of its sub-range.** [03-grammar.md](03-grammar.md#well-formedness-rules) reserved `DT0100` for rule 13 and `DT0101` for rule 14, taking the first two numbers of a hundred-code range and leaving the naming rules [D069](01-decisions.md#d069) assigns to that range with no clean start; `DT0402` and `DT0403` did the same one range up, landing two exhaustiveness-and-divergence codes inside what is otherwise a contiguous block about `!` and `else`. Four numbers therefore move:

| Rule | Was | Is |
| --- | --- | --- |
| 13 `sql_against_schema` | `DT0100` | `DT0140` |
| 14 `module_path_collision` | `DT0101` | `DT0130` |
| 8 `match_exhaustive` | `DT0402` | `DT0420` |
| 16 `else_block_diverges` | `DT0403` | `DT0440` |

Rules 2, 3, 4, 5, 6, 7, and 15 keep their numbers, because each already sits at or immediately after the start of the sub-range for its subject.

The move is legitimate and cheap for a specific reason: [D065](01-decisions.md#d065) separates **reservation**, which lives in the documentation, from **registration**, which lives in `diag_codes.h`, and only a registered code is frozen by [D050](01-decisions.md#d050). None of the four is registered. So the cost is one edit to the rule-to-code table in [03-grammar.md](03-grammar.md#well-formedness-rules) and nothing else — and this is the last moment it is available, because the moment any of the four is registered it is permanent. The alternative was carving the sub-ranges around two numbers at the far end of somebody else's block, which would have made every subsequent choice worse in order to protect a number nothing depends on.

### The names range

`DT0100`–`DT0199`, sub-allocated:

| Sub-range | Category |
| --- | --- |
| `DT0100`–`DT0119` | names, scopes, and visibility |
| `DT0120`–`DT0129` | naming conventions |
| `DT0130`–`DT0139` | modules and paths |
| `DT0140`–`DT0169` | SQL and schema |
| `DT0170`–`DT0199` | held |

| Code | Meaning |
| --- | --- |
| `DT0100` | unresolved name |
| `DT0101` | a declaration with this name already exists in this scope |
| `DT0102` | this binding shadows another binding of the same function |
| `DT0103` | a local is used before it is declared |
| `DT0104` | a type name is used where a value is expected |
| `DT0105` | a value is used where a type is expected |
| `DT0106` | `pub` on a method — a method is exported with its type |
| `DT0107` | a method's receiver is not a type declared in this program |
| `DT0108` | a method is declared in a different file from its type |
| `DT0109` | a method's name collides with a field of its receiver type |
| `DT0110` | this declaration is not `pub`, so it is not visible from another file |
| `DT0111` | top-level bindings initialize cyclically |
| `DT0112` | `_` cannot be referenced |
| `DT0113` | a well-known hook is declared more than once |
| `DT0114`–`DT0119` | held |
| `DT0120` | a module name must be lowercase |
| `DT0121` | a type name must be `PascalCase` |
| `DT0122` | a function or method name must be `snake_case` |
| `DT0123` | a field name must be `snake_case` |
| `DT0124` | an enum variant name must be `snake_case` |
| `DT0125` | a binding or parameter name must be `snake_case` |
| `DT0126`–`DT0129` | held |
| `DT0130` | a module path collides with a stdlib module name |
| `DT0131` | two files claim the same module path |
| `DT0132` | a module path is both a namespace and a member |
| `DT0133` | this module has no member with that name |
| `DT0134` | a module is used where a value is expected |
| `DT0135` | a file or directory name is not usable as a module path segment |
| `DT0136`–`DT0139` | held |
| `DT0140` | SQL is not valid against the migrated schema |
| `DT0141` | no such table |
| `DT0142` | no such column |
| `DT0143` | the SQL argument must be a literal |
| `DT0144` | wrong number of SQL parameters |
| `DT0145` | a named SQL parameter is not supported |
| `DT0146` | a result column has no matching field |
| `DT0147` | a field has no matching result column |
| `DT0148` | a result column's affinity does not match its field's type |
| `DT0149` | a nullable result column requires an optional field |
| `DT0150` | duplicate column name in the result |
| `DT0151` | `db.exec` used with a statement that returns rows |
| `DT0152` | `db.count` requires exactly one integer column |
| `DT0153` | a row type argument must be a struct declared in this program |
| `DT0154` | `db.batch`'s row struct does not match the placeholder count |
| `DT0155` | no schema: `migrations/` and `schema.sql` are both absent |
| `DT0156` | a migration failed to apply |
| `DT0157` | duplicate migration number |
| `DT0158` | malformed migration file name |
| `DT0159` | a SQL literal contains more than one statement |
| `DT0160` | this type cannot be bound as a SQL parameter |
| `DT0161`–`DT0169` | held |

Rule 14 is `DT0130`; rule 13 is `DT0140`. `DT0120` is the one naming code with no machine-applicable fix and no source position, for the reason in [Naming rules](#naming-rules).

### The types range

`DT0200`–`DT0299`, sub-allocated:

| Sub-range | Category |
| --- | --- |
| `DT0200`–`DT0219` | core type agreement |
| `DT0220`–`DT0239` | structs, enums, methods, fields, attributes |
| `DT0240`–`DT0259` | casts |
| `DT0260`–`DT0279` | markup value typing |
| `DT0280`–`DT0299` | held |

| Code | Meaning |
| --- | --- |
| `DT0200` | type mismatch |
| `DT0201` | cannot infer a type for this binding |
| `DT0202` | an operator does not apply to these operand types |
| `DT0203` | a condition must be `bool` |
| `DT0204` | this value is not iterable |
| `DT0205` | the wrong number of loop variables |
| `DT0206` | the wrong number of arguments |
| `DT0207` | an argument's type does not match the parameter |
| `DT0208` | this value is not callable |
| `DT0209` | this value cannot be indexed |
| `DT0210` | an index has the wrong type |
| `DT0211` | comparison operands have different types |
| `DT0212` | `return` with a value in a function that returns nothing |
| `DT0213` | `return` with no value in a function that returns a value |
| `DT0214` | this expression statement can have no effect |
| `DT0215` | a type argument list is wrong |
| `DT0216`–`DT0219` | held |
| `DT0220` | no field or method with this name |
| `DT0221` | a struct literal is missing a field |
| `DT0222` | a struct literal names a field this type does not have |
| `DT0223` | `with` requires a struct value |
| `DT0224` | this enum has no such variant |
| `DT0225` | a bare `.variant` has no enum type from context |
| `DT0226` | a type is recursive with no indirection, so it has no size |
| `DT0227` | a type alias is cyclic |
| `DT0228` | a default value's type does not match its field or parameter |
| `DT0229` | a default value must be a constant expression |
| `DT0230` | an attribute does not apply here |
| `DT0231` | an attribute's arguments are wrong |
| `DT0232` | a parameter or field with a default is followed by one without |
| `DT0233`–`DT0239` | held |
| `DT0240` | this cast is not permitted |
| `DT0241` | `str` to `html` must go through `html.raw` |
| `DT0242`–`DT0259` | held |
| `DT0260` | this value cannot be interpolated in markup text |
| `DT0261` | this value cannot be interpolated in an attribute value |
| `DT0262` | `html` may not be interpolated into an attribute value |
| `DT0263` | a `bool` or optional may only be an entire attribute value |
| `DT0264` | an attribute spread must be `{str: str}` |
| `DT0265` | a markup `{if}` condition must be `bool` |
| `DT0266` | a markup `{for}` subject is not iterable |
| `DT0267`–`DT0279` | held |

`DT0200` is the general mismatch and is deliberately not the only one: `DT0404` for a bare optional, `DT0241` for `str` to `html`, and `DT0202` for operands all exist because each has a different fix, and a diagnostic whose message can name the fix is worth a code ([D038](01-decisions.md#d038)).

Markup diagnostics split across two ranges, and the line is stated so nobody has to guess: **a diagnostic about the type of an interpolated value is in the types range; a diagnostic about the document the markup produces is in the routes range.** So a `bytes` in a text position is `DT0260` and a `<form>` with a computed method is `DT0540`. This is the split [10-frontend.md](10-frontend.md#front-end-diagnostics) anticipated when it sent markup semantics to `DT0200`–`DT0299` and `DT0500`–`DT0599`.

### The mutability range

`DT0300`–`DT0399`:

| Code | Meaning |
| --- | --- |
| `DT0300` | assignment to an immutable binding |
| `DT0301` | `spawn` may not reach a mutable binding |
| `DT0302` | a lambda may not capture a mutable binding |
| `DT0303` | a mutating method requires a `var` binding |
| `DT0304` | `self` is immutable |
| `DT0305`–`DT0399` | held |

Rule 2 is `DT0300` and rule 15 is `DT0301`, both unchanged. The range is deliberately sparse: [D008](01-decisions.md#d008) is three rules, and five codes say everything they forbid. The ninety-five held numbers are there because the range exists as a subject, not because the subject is expected to grow — and if `chan` and `spawn` in v0.2 need one, it will be obvious where it goes.

### The error range

`DT0400`–`DT0499`, sub-allocated:

| Sub-range | Category |
| --- | --- |
| `DT0400`–`DT0419` | fallibility and optionality |
| `DT0420`–`DT0439` | `match` and exhaustiveness |
| `DT0440`–`DT0459` | divergence and reachability |
| `DT0460`–`DT0499` | held |

| Code | Meaning |
| --- | --- |
| `DT0400` | `!` applied to an expression that cannot fail |
| `DT0401` | `else` applied to an expression that can neither fail nor be absent |
| `DT0402` | `!` in a function that is not fallible |
| `DT0403` | a fallible expression must be handled with `!` or `else` |
| `DT0404` | an optional value where a non-optional is required |
| `DT0405` | `else err` on an expression that is optional but cannot fail |
| `DT0406` | `else err` on an expression that is both optional and fallible |
| `DT0407` | the `else` value's type does not match |
| `DT0408` | a deferred call must not be fallible |
| `DT0409` | an `Error` returned from a function that is not fallible |
| `DT0410`–`DT0419` | held |
| `DT0420` | a `match` is not exhaustive and has no `else` arm |
| `DT0421` | a pattern's type does not match the value being matched |
| `DT0422` | duplicate pattern |
| `DT0423` | a value of this type cannot be matched |
| `DT0424` | an `else` arm is not the last arm |
| `DT0425`–`DT0439` | held |
| `DT0440` | the block form of `else` must diverge on every path |
| `DT0441` | this function can finish without returning a value |
| `DT0442`–`DT0459` | held |

Rule 3 is `DT0400`, rule 4 is `DT0401`, rule 8 moves to `DT0420`, rule 16 moves to `DT0440`.

### The route range

`DT0500`–`DT0599`, sub-allocated:

| Sub-range | Category |
| --- | --- |
| `DT0500`–`DT0519` | route declarations |
| `DT0520`–`DT0539` | request binding and hooks |
| `DT0540`–`DT0559` | markup policy |
| `DT0560`–`DT0599` | held |

| Code | Meaning |
| --- | --- |
| `DT0500` | a route pattern and its parameters do not agree |
| `DT0501` | a `form`, `query`, or `json` parameter must be a struct, and at most one may appear |
| `DT0502` | two routes conflict |
| `DT0503` | malformed route pattern |
| `DT0504` | a path parameter's type cannot be converted from a URL segment |
| `DT0505` | a path parameter may not be optional or carry a default |
| `DT0506` | malformed group prefix |
| `DT0507` | a route's return type is not a response type |
| `DT0508` | `form` or `json` on a method that carries no body |
| `DT0509` | a parameter name appears in both a group prefix and a route pattern |
| `DT0510`–`DT0519` | held |
| `DT0520` | a bound struct's field type cannot be decoded from a request |
| `DT0521` | a hook does not have the required signature |
| `DT0522` | `req` is not available here |
| `DT0523` | an `Upload` field requires a `form` binding |
| `DT0524`–`DT0539` | held |
| `DT0540` | a `<form>`'s `method` must be written literally |
| `DT0541` | a `send` value must be `html` or `str` |
| `DT0542`–`DT0559` | held |

Rules 5, 6, and 7 are `DT0500`, `DT0501`, and `DT0502`, unchanged: all three are about a route declaration agreeing with itself or with the table, so they share the first sub-range and take its first three numbers in rule order.

`DT0541` stays a reservation until v0.2, for the same reason `DT0301` does: `send` is `DT0046` in v0.1 and the stage barrier means no `send` ever reaches the route checker.

### The warning range

`DT0600`–`DT0699`. These are the first warnings in the compiler — all fifty codes registered today are errors, which is why `expect-warning` has been implemented and unexercised ([11-spec-tests.md](11-spec-tests.md#what-has-files-today)).

| Code | Meaning |
| --- | --- |
| `DT0600` | unused local binding |
| `DT0601` | unused private declaration |
| `DT0602` | unreachable code |
| `DT0603` | this binding shadows a stdlib module or a top-level name |
| `DT0604` | use of a `@deprecated` declaration |
| `DT0605` | redundant `else` arm — the `match` is already exhaustive |
| `DT0606` | this cast has the same source and target type |
| `DT0607`–`DT0699` | held |

`DT0600` and `DT0601` are suppressed by a leading underscore. **Parameters are never warned about**: a signature is a contract, an unused parameter is frequently required by the shape a caller or a route pattern demands, and a warning that fires on correct code is a warning people turn off.

A warning-only run still exits `1`, because `1` means "the command reported diagnostics" ([06-tooling.md](06-tooling.md#exit-codes)) and the spec runner asserts exactly that split.

### Which well-formedness rules land here

The eleven rules the parser does not discharge, with the code each now carries:

| Rule | Slug | Code | Stage |
| --- | --- | --- | --- |
| 2 | `assign_target_var` | `DT0300` | resolver |
| 3 | `propagate_fallible` | `DT0400` | typechecker |
| 4 | `else_optional_fallible` | `DT0401` | typechecker |
| 5 | `route_pattern_params` | `DT0500` | route checker |
| 6 | `request_binding_struct` | `DT0501` | route checker |
| 7 | `route_conflict` | `DT0502` | route checker |
| 8 | `match_exhaustive` | `DT0420` | typechecker |
| 13 | `sql_against_schema` | `DT0140` | schema checker |
| 14 | `module_path_collision` | `DT0130` | resolver |
| 15 | `spawn_captures_immutable` | `DT0301` | resolver |
| 16 | `else_block_diverges` | `DT0440` | typechecker |

Every rule's code is the first code of the sub-range for its subject, except where two or three rules share a subject and take consecutive numbers from its start in rule order — rules 3 and 4, and rules 5, 6, and 7.

---

## `doot check`

The command the semantic pass exists to make work, and — per [D054](01-decisions.md#d054) — one that appears in `src/cli/main.c` only when it fully checks. It follows the shape `cmd_fmt` established: one arena, created with `arena_new_fatal`; one `diag_sink`; `diag_render_json` to stdout under `--json`, `diag_render_human` to stderr plus a summary to stdout otherwise ([main.c](../src/cli/main.c)).

```
doot check [--json] [path]
```

**At most one path** ([D101](01-decisions.md#d101)), because compilation is whole-program and a list of paths does not describe a program. Two or more is exit code `2`, a usage error. `doot fmt` accepts many paths because formatting is per-file; checking is not.

| Argument | Unit |
| --- | --- |
| absent | the project rooted at the working directory |
| a directory | the project rooted there |
| a file | that file alone, plus `schema.sql` beside it if present — single-file mode ([D041](01-decisions.md#d041)) |

A project's unit is every `.do` file under the root, walked in `fs_read_dir`'s sorted order with dotfiles skipped, plus `migrations/` if it exists.

Exit codes are the three in [06-tooling.md](06-tooling.md#exit-codes): `0` with no diagnostics, `1` with any diagnostic including a warning-only run, `2` on misuse.

The human summary is a **pinned interface**, tested exactly by `expect-output` ([D075](01-decisions.md#d075)):

```
checked 12 files, no problems
checked 12 files, 3 errors, 1 warning
checked 1 file, 1 error
```

`checked N file` or `checked N files`, then `, no problems` when there are none, otherwise the non-zero counts in the order errors, warnings, each singular or plural. A unit with no `.do` files in it reports `checked 0 files, no problems` and exits `0`. Under `--json` the summary is absent and stdout carries only the schema in [06-tooling.md](06-tooling.md#diagnostics), unchanged and with nothing added — a command that grew the diagnostic schema would break the spec runner, which is the mechanism [D071](01-decisions.md#d071) intends.

`doot check` is the command the spec runner has been built around since the first commit: `SPEC_MODE_CHECK` is implemented, the directives in [11-spec-tests.md](11-spec-tests.md#directives) were written against it, and the illustration in [09-engineering.md](09-engineering.md#2-spec-tests--testsspec) is a `check` file. No spec file uses the mode today, and a file that did would fail loudly with "the command does not exist in this build" rather than confusingly — which is [D071](01-decisions.md#d071)'s fail-closed behaviour doing what it was designed for.

### `doot routes`

`doot routes [--json]` prints the route table, and it lands **before** `doot check` ([D102](01-decisions.md#d102)). It runs the parser, the resolver, and the route checker — everything the table needs and nothing more — and is therefore complete rather than partial at that milestone, in exactly the way `doot fmt` was complete at the parser milestone ([D067](01-decisions.md#d067)).

Human output, sorted by pattern and then by method so the listing is stable regardless of how files were walked:

```
GET     /rooms/:room          routes/rooms.do:12
POST    /rooms/:room          routes/rooms.do:31
GET     /users/:id            routes/users.do:8
```

Method padded to eight columns, pattern padded to two past the longest, then `file:line`.

Under `--json`, the output carries the diagnostic schema **plus one top-level key of its own**, `routes`, an array of `{method, pattern, file, line, params}` where each parameter is `{name, type, from}` and `from` is `path`, `wildcard`, `form`, `query`, or `json`.

That extra key is a deliberate, single-purpose extension, and it is the general rule ([D101](01-decisions.md#d101)): **a command's `--json` output is the diagnostic schema plus at most one top-level key named after the command.** The spec runner rejects unknown keys by design ([D071](01-decisions.md#d071)), so its reader learns `routes` in the same change that adds the command — visibly, in a diff, which is the direction [D071](01-decisions.md#d071) wanted schema growth to happen in.

---

## Specification tests

The suite and its mechanism are [11-spec-tests.md](11-spec-tests.md); nothing here changes either. What the semantic pass adds is files, in four new directories the layout already anticipates:

```
tests/spec/
  sema/     scopes, shadowing, module paths, naming, visibility, mutability
  types/    inference, assignability, casts, operators, structs, enums, match, markup typing
  routes/   patterns, request binding, conflicts, the `doot routes` listing
  db/       migration replay, statement preparation, parameters, result shapes
```

`db/` needs one thing the other directories do not: a schema. The runner sets the working directory to the file's own before invoking the binary ([11-spec-tests.md](11-spec-tests.md#the-runner)), so a `schema.sql` beside the `.do` files serves every test in the directory, and a case that needs a different schema — a broken migration, a duplicate number, no schema at all — gets a subdirectory of its own with its own `migrations/`. That is what makes every schema diagnostic reachable from a `.do` file, so **the semantic pass adds no entries to the four-code unit-test allowlist** in `tools/check-docs.sh` ([D079](01-decisions.md#d079)).

Each of the eleven pending well-formedness rules becomes a `tests/spec/rules/rule_NN_<slug>_{ok,err}.do` pair, and the gate that requires both files reads the rule-to-code table in [03-grammar.md](03-grammar.md#well-formedness-rules) and fires as soon as a rule's code is registered ([D078](01-decisions.md#d078)). So the pairs arrive with their codes, not before and not after:

| Milestone | Rules | Mode |
| --- | --- | --- |
| A — resolver, route checker, `doot routes` | 2, 5, 6, 7, 14 | `routes` |
| B — typechecker, schema checker, `doot check` | 3, 4, 8, 13, 16 | `check` |
| v0.2 — tasks | 15 | `check` |

**A `rules/` file uses the narrowest mode that reaches its rule**, which is `routes` for the five the resolver and route checker discharge and `check` for the rest. Earlier files are not migrated to `check` when it arrives: a test that pins a rule through the command that first reached it also pins that command's behaviour, and `doot routes` reporting a resolver error is worth keeping pinned.

Rule 15 is the one pending rule with no test in v0.1, for a stated and dated reason: `spawn` is `DT0046` until v0.2, the stage barrier means the resolver never runs on a program containing one, so `DT0301` is unreachable and therefore — correctly — unregistered. It lands with tasks, like `expect-fault` and the `stream` half of the chat program ([11-spec-tests.md](11-spec-tests.md#the-chat-application-in-v01)).

Two directives that exist and have never matched anything get their first producers here, closing both gaps [11-spec-tests.md](11-spec-tests.md#what-has-files-today) records:

| Directive | First produced by |
| --- | --- |
| `expect-warning` | the resolver's warnings `DT0600`, `DT0601`, `DT0603`, and `DT0604`, in milestone A |
| `expect-suggestion` | the naming codes `DT0121`–`DT0125` in milestone A, and `DT0142`'s column suggestion in milestone B |

---

## Implementation order

Two milestones, each finished to the [definition of done](09-engineering.md#definition-of-done) before the next begins ([D102](01-decisions.md#d102)).

The order is forced by two decisions acting together, and the reasoning is the same one [D067](01-decisions.md#d067) worked through for the front end. [D054](01-decisions.md#d054) says a command ships only when it fully works. [D065](01-decisions.md#d065) plus the `docs` gate say a diagnostic code is registered only alongside the spec test that produces it — and a spec test drives a command. So **a stage cannot land before a command that reaches its diagnostics**, and the milestones are the groupings for which such a command exists.

### Milestone A — the resolver, the route checker, and `doot routes`

Everything that needs names but not types, plus the command that reaches it.

1. **The semantic core.** `src/sema/`: the symbol table and scopes, the type representation, the prelude and module table, and the shared walk. No diagnostics of its own.
2. **The resolver.** Collect, scopes, the path algorithm, visibility, naming conventions, mutability. Registers the allocated codes in `DT0100`–`DT0135`, `DT0300` and `DT0302`–`DT0304`, and the warnings `DT0600`, `DT0601`, `DT0603`, and `DT0604`.
3. **The route checker.** Patterns, request binding, hooks, conflicts, the route table. Registers `DT0500`–`DT0509` and `DT0520`–`DT0523`.
4. **`doot routes`, and the suite.** `tests/spec/sema/`, `tests/spec/routes/`, and the rule pairs for 2, 5, 6, 7, and 14.

Steps 1 through 3 land with unit tests only, exactly as the lexer and the parser did. Their codes are registered in step 4, in the same change as the spec tests that produce them — so steps 2 and 3 report diagnostics through codes that arrive at the end of the milestone, and the milestone is one pull request rather than four. Roughly fifty codes in total.

### Milestone B — the typechecker, the schema checker, and `doot check`

1. **The typechecker core.** Inference, assignability, operators, casts, calls, structs, enums, `match`, `with`, lambdas.
2. **Markup value typing**, including the escaping-soundness rules and `DT0540`'s form-method check.
3. **The error and optional model**, and the divergence analysis that rule 16, `DT0441`, and `DT0602` share.
4. **The schema checker.** SQLite enters the build here: migration replay, statement location, parameters, result shapes.
5. **`doot check`, and the suite.** `tests/spec/types/`, `tests/spec/db/`, and the rule pairs for 3, 4, 8, 13, and 16.

This is the largest single landing in the project — roughly eighty codes and their tests, being the allocated codes in `DT0140`–`DT0160`, `DT0200`–`DT0266`, and `DT0400`–`DT0441`, plus `DT0540` and the warnings `DT0602`, `DT0605`, and `DT0606` — and the size is a consequence of the two decisions above rather than a choice: `doot check` is the only command that reaches the typechecker, and it cannot ship without the schema checker, because a `doot check` that silently ignored a SQL literal would be the half-working command [D054](01-decisions.md#d054) exists to prevent, on the one promise ([D033](01-decisions.md#d033)) that is hardest to make credible.

What keeps it reviewable is that the five steps are separate commits on one branch, each complete on its own terms, with the codes and their spec tests attached to whichever step implements them and the command added last.

### What can be built concurrently

Four workstreams, with what each is gated on:

| Workstream | Gated on | Independent of |
| --- | --- | --- |
| the module and prelude table | the table's format, fixed by A1 | every checker; it is data |
| the schema checker | A's symbol table and declared types | the typechecker entirely |
| markup value typing | B1's type representation | the schema checker, the route checker |
| the spec suite per directory | the codes for that directory | the other directories |

The schema checker is the significant one. It needs the resolver's symbol table and the field types of declared structs, both of which milestone A produces, and it needs nothing at all from the typechecker — the one check that would have needed an inferred type, `DT0160`, is the typechecker's, because it is visiting every argument anyway. So the schema checker and the typechecker can be written at the same time by different people, land in the same pull request, and touch no common file except the ones listed below.

The module table is the other one worth planning for. It is 26 modules of signatures for v0.1 ([04-stdlib.md](04-stdlib.md#overview)) — mechanical, large, and reviewable in isolation — and it is on the critical path for everything else, because no checker can be tested against a program that calls a module the table does not describe. Its format is therefore the first thing milestone A fixes.

### The shared chokepoints

Six files serialize work no matter how the stages are divided, and each is named here so that two workstreams can agree in advance rather than discover it in a merge:

| File | What touches it | How to keep it cheap |
| --- | --- | --- |
| `Makefile`, `LAYERS` | adding `sema` | one edit, in A1, before anything else |
| `tools/amalgamate.sh`, `units` | **every** new translation unit under `src/` | the file layout is settled in A1 and units are appended in layer order; the script fails loudly when a `.c` is missing, so a forgotten edit cannot pass CI |
| `src/base/diag_codes.h` | every code-registering change | the sub-allocation above is what makes the *content* conflict-free; the table stays ordered by code, so each workstream inserts only inside its own contiguous sub-range and a textual conflict is mechanical |
| `docs/06-tooling.md`, the range table | nothing further | allocated once, by this document |
| `src/cli/main.c`, the dispatch chain | `routes` in A4, `check` in B5 | two edits, one per milestone, in different milestones by construction |
| `tests/unit/main.c`, the suite array | one line per new suite | the array is ordered by layer with a comment saying so; new suites append after `suite_print` |

The vendored SQLite is the seventh, and it is a one-way step rather than a recurring cost. It enters the build in B4, which means `tools/vendor.sh` installs the `sqlite/` tree for the first time and the `unity` gate's command grows a second translation unit: `cc -O2 -o doot build/doot.c vendor/sqlite/sqlite3.c`. That does not weaken [D045](01-decisions.md#d045) — the amalgamation is one file of *doot*, and SQLite's amalgamation is already one file of SQLite, which is the form [09-engineering.md](09-engineering.md#vendoring) pins it in. Vendored code keeps its own warning flags, so it cannot be `#include`d into `build/doot.c` under `-Wconversion`, and it is not.

The schema checker needs `SQLITE_ENABLE_COLUMN_METADATA` for `sqlite3_column_origin_name` and `sqlite3_table_column_metadata`, without which nullability cannot be checked at all. That is a **compile-time define**, not a source edit, so it satisfies [D052](01-decisions.md#d052)'s no-patches rule without qualification.

### Where the code lives

```
src/sema/
  sym.c/h        symbols, scopes, sealing, lookup
  prelude.c/h    the module and prelude signature table
  type.c/h       sema_type, interning, type_eq, assignability, layout cycles
  resolve.c/h    collect, the path algorithm, visibility, naming, mutability
  check.c/h      the typechecker, including markup typing and divergence
  schema.c/h     migration replay and statement preparation (links vendored SQLite)
  route.c/h      the route table and its checks
```

`src/sema/schema.c` is the **compile-time** half of `db` and shares nothing with the runtime driver that lands in `src/db/` with the VM except the vendored library itself. Keeping them apart matters: one runs in the compiler's fatal arena against an in-memory database and the other runs in a request arena against the project's file, and they have opposite failure policies ([D047](01-decisions.md#d047)).

### Fuzzing

No new target. `fuzz_parse` is extended to continue into the resolver and the typechecker when the parse produced no errors, keeping the target list in [09-engineering.md](09-engineering.md#5-fuzzing--fuzz) accurate and the seed corpus the one that already exists. A separate semantic target would take source bytes as its input either way, so it would only duplicate `fuzz_parse`'s generation.

The schema checker is not fuzzed through it: its input is a `.sql` file on disk and a database handle, neither of which a byte-buffer entry point supplies. Its untrusted-input surface is SQLite's own SQL parser, which is fuzzed continuously upstream and is the reason [D033](01-decisions.md#d033) uses a real prepare rather than a hand-written SQL parser in the first place.

The invariant is [09-engineering.md](09-engineering.md#5-fuzzing--fuzz)'s: arbitrary input yields a diagnostic, never a crash, a hang, or unbounded memory. Termination is not assumed anywhere. Every recursive walk here is bounded either by the parser's depth bound or by explicit cycle detection, and there are exactly three of the latter — type alias cycles (`DT0227`), struct layout cycles (`DT0226`), and top-level initializer cycles (`DT0111`). Named together because they are the only places in the semantic pass where a graph rather than a tree is walked.
