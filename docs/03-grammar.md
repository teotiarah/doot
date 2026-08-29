# Grammar

Normative reference for the doot v0.1 language. Notation: `|` alternation, `?` optional, `*` zero or more, `+` one or more, `( )` grouping, `"x"` literal, `UPPER` lexical token.

---

## Lexical structure

### Line structure

doot is **newline-terminated, not semicolon-terminated**, and whitespace is otherwise insignificant.

A `NEWLINE` token terminates a statement unless the **preceding** token is a continuation token:

```
( [ { , . : -> => = += -= *= /= %= == != < <= > >= + - * / % | and or not
```

or the **following** token is a follow token:

```
) ] }
```

The rule that decides membership of the follow set: **a follow token must not be able to begin a construct.** Suppressing the newline before a token costs the parser the ability to tell "this continues the previous line" from "this starts a new one", so only a token that can never begin anything may be in it.

This means an expression may break across lines after any operator or opening bracket without a continuation marker, and a `} else {` chain stays on one logical statement.

A run of consecutive newlines produces at most one `NEWLINE`, whose span covers the whole run so that `doot fmt` can recover the author's blank lines. Comments are not significant for this rule: they are neither the preceding nor the following token.

**Statement end** is `NEWLINE` consumed, or a lookahead of `}` or end of input, neither consumed. The last statement in a block has no `NEWLINE` after it, because `}` is a follow token — so every `NEWLINE` written in a statement production below is satisfied by any of the three.

Inside markup literals, newlines are content, not terminators. Inside a markup tag they are insignificant whitespace, so attributes may wrap.

Five entries differ from this document's first version, corrected under [D060](01-decisions.md#d060):

- Postfix `!` is **not** a continuation token, because it ended `let u = create(name)!` and swallowed the newline.
- The compound assignments **are** continuation tokens, because `=` is one and they are the same construct.
- `|` is one, so a multi-line `match` pattern breaks like any other binary operator.
- `.` is a continuation token but **not** a follow token, so a method chain breaks *after* the dot. Leading-dot continuation is incompatible with `match`: an arm's pattern begins with a dot, and once the newline is suppressed there is no way to tell `render()` followed by `.banned` from `render().banned`. Required syntax wins over optional style.
- `else` is likewise a continuation token but **not** a follow token, for the same reason one level up: it begins a match arm, so the previous arm's value swallowed it as an `else` coalesce. It needed suppression only for an `else` written on its own line after a `}`, and the parser accepts that form anyway by looking past the newline, with `doot fmt` normalizing it to `} else {`.

`<` and `>` are unambiguous here because markup delimiters are distinct token kinds and never the comparison operators ([D059](01-decisions.md#d059)).

### Comments

```
COMMENT      := "//" <any except NEWLINE>*
BLOCK_COMMENT := "/*" <any> "*/"          // nests
```

### Identifiers and literals

```
IDENT        := (LETTER | "_") (LETTER | DIGIT | "_")*
INT          := DIGIT (DIGIT | "_")*
               | "0x" HEXDIGIT (HEXDIGIT | "_")*
               | "0b" ("0" | "1" | "_")+
FLOAT        := DIGIT (DIGIT | "_")* "." DIGIT (DIGIT | "_")* EXPONENT?
               | DIGIT (DIGIT | "_")* EXPONENT
EXPONENT     := ("e" | "E") ("+" | "-")? DIGIT+
STR          := '"' (CHAR | ESCAPE | INTERP)* '"'
RAW_STR      := "`" <any except "`">* "`"
ESCAPE       := "\\" ("n" | "t" | "r" | "\\" | '"' | "$" | "u{" HEXDIGIT+ "}")
INTERP       := "${" expr "}"
BOOL         := "true" | "false"
NIL          := "nil"
METHOD       := "GET" | "POST" | "PUT" | "PATCH" | "DELETE" | "HEAD" | "OPTIONS"
```

`STR` is UTF-8 and supports interpolation. `RAW_STR` (backticks) supports neither escapes nor interpolation, and exists for SQL and other embedded text.

`METHOD` tokens are contextual: they are recognized as HTTP methods only immediately after `route` or `stream`, and are ordinary identifiers elsewhere. They lex as identifiers and are matched by text in the parser ([D061](01-decisions.md#d061)), as is `end` in a markup control block — which is likewise not a keyword.

### Keywords

The thirty-one keywords and the reserved-word list are in [02-syntax.md](02-syntax.md#keywords). Reserved words lex as keywords and are rejected by the parser with a specific diagnostic.

Two refinements, both required to make the documented standard library expressible ([D062](01-decisions.md#d062)):

- A reserved word is an **ordinary name in a name position** — after `.`, as a field name, as an enum variant. `auth.require`, `uuid.new`, and `chan.new` all depend on this, and nobody writing `x.require` is reaching for a foreign construct, which is the only thing the reservation exists to catch.
- Where a **stdlib module name** collides with a keyword or a reserved word, the module wins in expression position, because [02-syntax.md](02-syntax.md#keywords) already declares module names to be predeclared identifiers. Exactly two of the thirty-eight collide: `test` (keyword, and the assertions module used as `test.eq`) and `static` (reserved, and the file-serving module).

---

## Program

```
program      := item*
item         := NEWLINE
              | attribute* declaration

declaration  := "pub"? ( fn_decl
                       | type_decl
                       | let_decl
                       | route_decl
                       | stream_decl
                       | group_decl
                       | test_decl )

attribute    := "@" IDENT ( "(" arg_list? ")" )? NEWLINE?
```

A top-level `let_decl` may not be `var` — there are no mutable globals ([D008](01-decisions.md#d008)).

---

## Declarations

```
fn_decl      := "fn" ( IDENT "." )? IDENT "(" param_list? ")" return_type? block

param_list   := param ( "," param )* ","?
param        := "self"
              | IDENT ":" type ( "=" expr )?

return_type  := "->" type fallible?
fallible     := "!"                     // may fail; see D013

type_decl    := "type" IDENT ( struct_body | "enum" enum_body | "=" type )

struct_body  := "{" NEWLINE? field* "}"
field        := IDENT ":" type ( "=" expr )? attribute* NEWLINE

enum_body    := "{" NEWLINE? IDENT ( "," NEWLINE? IDENT )* ","? NEWLINE? "}"

let_decl     := ( "let" | "var" ) IDENT ( ":" type )? "=" expr

route_decl   := "route" METHOD STR "(" param_list? ")" return_type? block
stream_decl  := "stream" METHOD STR "(" param_list? ")" block
group_decl   := "group" STR "{" NEWLINE? ( attribute* ( route_decl | stream_decl ) NEWLINE )* "}"
test_decl    := "test" STR block
```

Route and stream path patterns are string literals containing `:name` segments and an optional trailing `*rest` wildcard. Every `:name` must have a correspondingly named parameter; the parameter names `form`, `query`, and `json` are reserved for request binding ([D025](01-decisions.md#d025)) and may not appear in a pattern.

`fn IDENT "." IDENT` declares a method on the named type; its first parameter must be `self`.

---

## Types

```
type         := base_type "?"?
base_type    := path
              | "[" type "]"
              | "{" type ":" type "}"
              | "fn" "(" type_list? ")" return_type?
              | "(" type ")"
path         := IDENT ( "." IDENT )*
type_list    := type ( "," type )* ","?
type_args    := "[" type ( "," type )* "]"
```

Type application uses square brackets (`db.all[User]`), never angle brackets — which is what keeps `<` unambiguous for markup ([D019](01-decisions.md#d019), [D022](01-decisions.md#d022)). User code cannot introduce type parameters; `type_args` appear only at stdlib entry points that declare them.

---

## Statements

```
block        := "{" NEWLINE? statement* "}"

statement    := NEWLINE
              | let_decl NEWLINE
              | assign_stmt NEWLINE
              | if_stmt
              | for_stmt
              | while_stmt
              | match_stmt
              | return_stmt NEWLINE
              | send_stmt NEWLINE
              | spawn_stmt NEWLINE
              | defer_stmt NEWLINE
              | "break" NEWLINE
              | "continue" NEWLINE
              | expr NEWLINE

assign_stmt  := lvalue assign_op expr
lvalue       := IDENT ( "." IDENT | "[" expr "]" )*
assign_op    := "=" | "+=" | "-=" | "*=" | "/=" | "%="

if_stmt      := "if" expr block ( "else" ( if_stmt | block ) )?
for_stmt     := "for" IDENT ( "," IDENT )? "in" expr block
while_stmt   := "while" expr block
return_stmt  := "return" expr?
send_stmt    := "send" ( expr "," )? expr
spawn_stmt   := "spawn" call_expr
defer_stmt   := "defer" call_expr

match_stmt   := "match" expr "{" NEWLINE? match_arm+ "}"
match_arm    := ( pattern | "else" ) "->" ( expr | block ) NEWLINE
pattern      := "." IDENT              // enum variant
              | INT | STR | BOOL
              | pattern ( "|" pattern )+
```

`assign_stmt` requires its target to resolve to a `var` binding; assigning through a `let` binding or a parameter is a compile error ([D008](01-decisions.md#d008)). A `match` whose arms are not exhaustive requires an `else` arm.

---

## Expressions

```
expr         := coalesce_expr

coalesce_expr := or_expr ( "else" coalesce_tail )?
coalesce_tail := IDENT? block            // else err { ... } | else { ... }
               | return_stmt             // else return ...
               | or_expr                 // else <default value>

or_expr      := and_expr ( "or" and_expr )*
and_expr     := not_expr ( "and" not_expr )*
not_expr     := "not" not_expr | cmp_expr
cmp_expr     := add_expr ( ( "==" | "!=" | "<" | "<=" | ">" | ">=" | "in" ) add_expr )?
add_expr     := mul_expr ( ( "+" | "-" ) mul_expr )*
mul_expr     := cast_expr ( ( "*" | "/" | "%" ) cast_expr )*
cast_expr    := unary_expr ( "as" type )?
unary_expr   := "-" unary_expr | postfix_expr

postfix_expr := primary postfix*
postfix      := "." IDENT                            // field or method
              | type_args? "(" arg_list? ")"         // call
              | "[" expr "]"                         // index
              | "!"                                  // propagate; see D013
              | "with" "{" field_init_list "}"        // functional update

primary      := INT | FLOAT | STR | RAW_STR | BOOL | NIL
              | IDENT
              | "self"
              | "." IDENT                            // inferred enum variant
              | list_lit | map_lit | struct_lit
              | lambda
              | markup
              | "(" expr ")"

list_lit     := "[" ( expr ( "," expr )* ","? )? "]"
map_lit      := "{" ( expr ":" expr ( "," expr ":" expr )* ","? )? "}"
struct_lit   := path "{" NEWLINE? field_init_list "}"
field_init_list := field_init ( ( "," | NEWLINE ) NEWLINE? field_init )* ","? NEWLINE?
field_init   := IDENT ":" expr

lambda       := "fn" "(" param_list? ")" return_type? ( "=>" expr | block )
arg_list     := expr ( "," expr )* ","?
```

### Precedence

Loosest to tightest:

| Level | Operators | Associativity |
| --- | --- | --- |
| 1 | `else` (coalesce) | right |
| 2 | `or` | left |
| 3 | `and` | left |
| 4 | `not` | prefix |
| 5 | `==` `!=` `<` `<=` `>` `>=` `in` | non-associative |
| 6 | `+` `-` | left |
| 7 | `*` `/` `%` | left |
| 8 | `as` | left |
| 9 | unary `-` | prefix |
| 10 | `.` `(...)` `[...]` `!` `with` | left |

`cmp_expr` is non-associative: `a < b < c` is a syntax error rather than a surprise.

There is no prefix `!` for negation — `not` is the boolean operator, which leaves postfix `!` unambiguously meaning error propagation. The lexer distinguishes `!` from `!=` by lookahead.

There is no ternary operator, no `?.`, no `??`, and no `?:`. `else` covers all of it ([D017](01-decisions.md#d017)).

---

## Markup

```
markup       := "<" tag attr* ( "/>" | ">" node* "</" tag? ">" )
tag          := IDENT ( "-" IDENT )*
attr         := IDENT ( "-" IDENT )* ( "=" attr_value )?
              | "..." expr                          // spread a {str: str}
attr_value   := STR | interp
node         := TEXT | interp | markup | markup_ctrl | comment_node
interp       := "${" expr "}"
markup_ctrl  := "{" "if" expr "}"   node*
                ( "{" "else" "if" expr "}" node* )*
                ( "{" "else" "}" node* )?
                "{" "end" "}"
              | "{" "for" IDENT ( "," IDENT )? "in" expr "}" node*
                ( "{" "else" "}" node* )?
                "{" "end" "}"
comment_node := "<!--" <any> "-->"
```

### Disambiguation

A `<` begins a markup literal if and only if all of the following hold:

1. It appears in **expression position** (not after an operand, where it would be a comparison).
2. It is **immediately followed** by a letter, `_`, or `/`, with no intervening whitespace.
3. The token sequence forms a valid tag name followed by whitespace, `>`, `/`, or `=`.

Because type application uses `[T]` ([D019](01-decisions.md#d019)), condition 1 is the only case requiring lookahead, and one token of it suffices.

`</>` closes the most recent open tag when the name is omitted. Void elements (`br`, `img`, `input`, `hr`, `meta`, `link`, …) may be written `<br>` or `<br/>`; both are accepted and `doot fmt` normalizes to `<br/>`.

### Semantics

Interpolations in text and attribute positions are **escaped for their context** — HTML text escaping in text position, attribute-value escaping in attribute position ([D021](01-decisions.md#d021)). `html.raw(s)` is the only way to emit unescaped content.

An `interp` whose value is `html` or `[html]` is spliced without escaping, because it is already escaped by construction. `nil` renders as nothing. `bool` in attribute position controls whether the attribute is emitted at all.

Static text is concatenated and pre-escaped at compile time into a single constant blob, and markup compiles to output-buffer append opcodes rather than string construction ([05-runtime.md](05-runtime.md#markup-compilation)).

A `<form method="post">` element gains a hidden CSRF token automatically ([D028](01-decisions.md#d028)).

---

## Well-formedness rules

Each has an assigned diagnostic code. Rules **1, 9, 10, 11, and 12** need only syntactic context and are enforced by the parser, with codes allocated in [10-frontend.md](10-frontend.md#which-well-formedness-rules-the-parser-discharges); the rest need names, types, the route table, or control-flow analysis and are enforced after parsing, in the resolver and typechecker ([D064](01-decisions.md#d064)).

1. A top-level binding must be `let`, never `var`.
2. An assignment target must resolve to a `var` local; parameters and `let` bindings are immutable, transitively.
3. `!` may only be applied to an expression of fallible type, and only inside a function whose return type is fallible or which handles it with `else`.
4. `else` may only be applied to an expression of optional or fallible type.
5. Every `:name` in a route pattern must have a matching parameter, and every non-reserved parameter must appear in the pattern.
6. `form`, `query`, and `json` parameters must be of struct type, and at most one may appear per route.
7. Route patterns must not conflict or shadow each other within the program.
8. A `match` must be exhaustive or have an `else` arm.
9. A method's first parameter must be `self`.
10. `send` may only appear inside a `stream` body.
11. `self` may only appear inside a method body.
12. `break` and `continue` may only appear inside a loop.
13. A SQL string literal passed to a `db` entry point must be valid against the migrated schema, and its result shape must match the declared type argument ([D033](01-decisions.md#d033)).
14. A user module path may not collide with a stdlib module name.
15. A closure passed to `spawn` may capture only immutable bindings.
16. The block form of `else` must diverge on every path — `return`, `break`, `continue`, or a fault. To supply a replacement value instead, use the expression form (`else default_value`). This keeps blocks from being value-producing expressions, so there is exactly one expression syntax in the language.
