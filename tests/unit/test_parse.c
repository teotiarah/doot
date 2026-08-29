#include "../../src/parse/parse.h"

#include <string.h>

#include "../../src/base/diag.h"
#include "../../src/parse/ast.h"
#include "unit.h"

/* ---- fixture ----------------------------------------------------------- */

typedef struct {
  arena *a;
  diag_sink sink;
  source *src;
  unit_ast *unit;
} fixture;

static void fix_init(fixture *f, const char *text) {
  memset(f, 0, sizeof(*f));
  f->a = arena_new(1u << 16);
  diag_sink_init(&f->sink, f->a, 0u);
  f->src = source_from_memory(f->a, SLICE_LIT("t.do"), slice_from_cstr(text), &f->sink);
  f->unit = parse_unit(f->a, f->src, &f->sink);
}

static void fix_free(fixture *f) {
  arena_destroy(f->a);
}

static bool has_code(const diag_sink *s, diag_code code) {
  const diag *d;

  for (d = s->first; d != NULL; d = d->next) {
    if (d->code == code) {
      return true;
    }
  }
  return false;
}

/* Parses a source that must be clean, reporting the first diagnostic if not. */
static void parse_clean(unit *t, fixture *f, const char *text) {
  fix_init(f, text);
  t->checks++;
  if (diag_error_count(&f->sink) != 0) {
    const diag *d = f->sink.first;
    line_col lc = source_line_col(f->src, d->at.start);

    unit_failf(t, __FILE__, __LINE__,
               "expected a clean parse of:\n    %s\n  got %s at %lu:%lu: %.*s", text,
               diag_code_str(d->code), (unsigned long)lc.line, (unsigned long)lc.col,
               (int)d->message.n, d->message.p);
  }
}

/* Parses an expression by wrapping it in a binding, and returns the value. */
static expr *parse_value(unit *t, fixture *f, const char *expression) {
  static char text[1024];
  const decl *d;

  text[0] = '\0';
  (void)strncat(text, "let x = ", sizeof(text) - 1u);
  (void)strncat(text, expression, sizeof(text) - strlen(text) - 1u);
  parse_clean(t, f, text);
  d = f->unit->decls.first;
  if (d == NULL || d->kind != DECL_LET) {
    return NULL;
  }
  return d->as.let.value;
}

/* ---- declarations ------------------------------------------------------ */

static void a_struct_type_collects_its_fields(unit *t) {
  fixture f;
  const decl *d;

  parse_clean(t, &f, "type User {\n  id: int\n  name: str\n  bio: str?\n}\n");
  d = f.unit->decls.first;
  UNIT_NOT_NULL(t, d);
  if (d != NULL) {
    UNIT_EQ_INT(t, d->kind, DECL_STRUCT);
    UNIT_EQ_SLICE(t, d->as.struct_.name, "User");
    UNIT_EQ_INT(t, d->as.struct_.fields.count, 3);
    UNIT_EQ_SLICE(t, d->as.struct_.fields.first->name, "id");
    UNIT_TRUE(t, d->as.struct_.fields.last->type->optional);
  }
  fix_free(&f);
}

static void a_field_may_carry_a_default_and_attributes(unit *t) {
  fixture f;
  const decl *d;

  parse_clean(t, &f, "type Search {\n  q: str @len(1, 100) @trim\n  page: int = 1\n}\n");
  d = f.unit->decls.first;
  if (d != NULL && d->kind == DECL_STRUCT) {
    const field *q = d->as.struct_.fields.first;
    UNIT_EQ_INT(t, q->attrs.count, 2);
    UNIT_EQ_SLICE(t, q->attrs.first->name, "len");
    UNIT_EQ_INT(t, q->attrs.first->args.count, 2);
    UNIT_NOT_NULL(t, d->as.struct_.fields.last->dflt);
  }
  fix_free(&f);
}

static void an_enum_is_tag_only(unit *t) {
  fixture f;
  const decl *d;

  parse_clean(t, &f, "type Status enum { active, banned, pending }\n");
  d = f.unit->decls.first;
  if (d != NULL) {
    UNIT_EQ_INT(t, d->kind, DECL_ENUM);
    UNIT_EQ_INT(t, d->as.enum_.variants.count, 3);
    UNIT_EQ_SLICE(t, d->as.enum_.variants.first->name, "active");
  }
  fix_free(&f);
}

static void a_method_attaches_to_a_type_by_name(unit *t) {
  fixture f;
  const decl *d;

  parse_clean(t, &f, "fn User.display(self) -> str {\n  return self.name\n}\n");
  d = f.unit->decls.first;
  if (d != NULL) {
    UNIT_EQ_INT(t, d->kind, DECL_FN);
    UNIT_TRUE(t, d->as.fn.has_recv);
    UNIT_EQ_SLICE(t, d->as.fn.recv, "User");
    UNIT_EQ_SLICE(t, d->as.fn.name, "display");
    UNIT_TRUE(t, d->as.fn.params.first->is_self);
  }
  fix_free(&f);
}

static void a_route_records_its_method_and_pattern(unit *t) {
  fixture f;
  const decl *d;

  parse_clean(t, &f, "route GET \"/users/:id\" (id: int) -> html! {\n  return page(id)\n}\n");
  d = f.unit->decls.first;
  if (d != NULL) {
    UNIT_EQ_INT(t, d->kind, DECL_ROUTE);
    UNIT_EQ_SLICE(t, d->as.route.method, "GET");
    UNIT_EQ_SLICE(t, d->as.route.pattern, "/users/:id");
    UNIT_EQ_INT(t, d->as.route.params.count, 1);
    UNIT_TRUE(t, d->as.route.fallible);
  }
  fix_free(&f);
}

static void a_group_carries_a_prefix_and_hooks(unit *t) {
  fixture f;
  const decl *d;

  parse_clean(t, &f,
              "@before(auth.require)\n"
              "group \"/admin\" {\n"
              "  route GET \"/\" () -> html! { return page() }\n"
              "  route GET \"/users\" () -> html! { return users() }\n"
              "}\n");
  d = f.unit->decls.first;
  if (d != NULL) {
    UNIT_EQ_INT(t, d->kind, DECL_GROUP);
    UNIT_EQ_SLICE(t, d->as.group.prefix, "/admin");
    UNIT_EQ_INT(t, d->as.group.items.count, 2);
    UNIT_EQ_INT(t, d->attrs.count, 1);
    UNIT_EQ_SLICE(t, d->attrs.first->name, "before");
  }
  fix_free(&f);
}

static void pub_marks_a_declaration_exported(unit *t) {
  fixture f;

  parse_clean(t, &f, "pub fn greet(name: str) -> str {\n  return name\n}\n");
  UNIT_TRUE(t, f.unit->decls.first->is_pub);
  fix_free(&f);
}

static void a_test_block_is_a_declaration(unit *t) {
  fixture f;
  const decl *d;

  parse_clean(t, &f,
              "test \"greet formats a name\" {\n  test.eq(greet(\"Ada\"), \"hello Ada\")\n}\n");
  d = f.unit->decls.first;
  if (d != NULL) {
    UNIT_EQ_INT(t, d->kind, DECL_TEST);
    UNIT_EQ_SLICE(t, d->as.test.name, "greet formats a name");
    UNIT_EQ_INT(t, d->as.test.body.count, 1);
  }
  fix_free(&f);
}

/* ---- statements -------------------------------------------------------- */

static void statements_are_newline_terminated(unit *t) {
  fixture f;
  const decl *d;

  parse_clean(t, &f, "fn f() {\n  let a = 1\n  var b = 2\n  b = a + b\n  b += 1\n}\n");
  d = f.unit->decls.first;
  if (d != NULL) {
    UNIT_EQ_INT(t, d->as.fn.body.count, 4);
    UNIT_EQ_INT(t, d->as.fn.body.first->kind, STMT_LET);
    UNIT_EQ_INT(t, d->as.fn.body.last->kind, STMT_ASSIGN);
    UNIT_EQ_INT(t, d->as.fn.body.last->as.assign.op, ASSIGN_ADD);
  }
  fix_free(&f);
}

static void the_last_statement_in_a_block_needs_no_newline(unit *t) {
  /* `}` is a follow token, so the lexer suppressed the newline before it; D060's
   * three-way statement end is what makes this parse. */
  fixture f;

  parse_clean(t, &f, "fn f() { let a = 1 }\n");
  UNIT_EQ_INT(t, f.unit->decls.first->as.fn.body.count, 1);
  fix_free(&f);

  parse_clean(t, &f, "fn f() {\n  let a = 1\n}\n");
  UNIT_EQ_INT(t, f.unit->decls.first->as.fn.body.count, 1);
  fix_free(&f);
}

static void if_else_chains_nest(unit *t) {
  fixture f;
  const stmt *s;

  parse_clean(t, &f,
              "fn f() {\n"
              "  if a > 10 {\n    x()\n  } else if a > 0 {\n    y()\n  } else {\n    z()\n  }\n"
              "}\n");
  s = f.unit->decls.first->as.fn.body.first;
  UNIT_EQ_INT(t, s->kind, STMT_IF);
  UNIT_TRUE(t, s->as.if_.has_else);
  UNIT_NOT_NULL(t, s->as.if_.else_if);
  if (s->as.if_.else_if != NULL) {
    UNIT_TRUE(t, s->as.if_.else_if->as.if_.has_else);
    UNIT_EQ_INT(t, s->as.if_.else_if->as.if_.else_body.count, 1);
  }
  fix_free(&f);
}

static void a_condition_is_not_a_struct_literal(unit *t) {
  /* `if x { ... }` -- the brace opens the body, so struct-literal detection has to
   * be suppressed in a condition. */
  fixture f;
  const stmt *s;

  parse_clean(t, &f, "fn f() {\n  if ready {\n    run()\n  }\n}\n");
  s = f.unit->decls.first->as.fn.body.first;
  UNIT_EQ_INT(t, s->kind, STMT_IF);
  UNIT_EQ_INT(t, s->as.if_.cond->kind, EXPR_IDENT);
  UNIT_EQ_INT(t, s->as.if_.then_body.count, 1);
  fix_free(&f);

  /* Outside a condition the same shape is a struct literal. */
  parse_clean(t, &f, "let u = User { id: 1, name: \"a\" }\n");
  UNIT_EQ_INT(t, f.unit->decls.first->as.let.value->kind, EXPR_STRUCT);
  UNIT_EQ_INT(t, f.unit->decls.first->as.let.value->as.struct_lit.fields.count, 2);
  fix_free(&f);
}

static void for_loops_take_one_or_two_variables(unit *t) {
  fixture f;
  const stmt *s;

  parse_clean(t, &f, "fn f() {\n  for u in users {\n    x(u)\n  }\n}\n");
  s = f.unit->decls.first->as.fn.body.first;
  UNIT_EQ_INT(t, s->kind, STMT_FOR);
  UNIT_FALSE(t, s->as.for_.has_second);
  UNIT_EQ_SLICE(t, s->as.for_.first_name, "u");
  fix_free(&f);

  parse_clean(t, &f, "fn f() {\n  for i, u in users {\n    x(i, u)\n  }\n}\n");
  s = f.unit->decls.first->as.fn.body.first;
  UNIT_TRUE(t, s->as.for_.has_second);
  UNIT_EQ_SLICE(t, s->as.for_.second_name, "u");
  fix_free(&f);
}

static void match_arms_take_an_expression_or_a_block(unit *t) {
  fixture f;
  const stmt *s;

  parse_clean(t, &f,
              "fn f() {\n"
              "  match status {\n"
              "    .active -> render()\n"
              "    .banned | .pending -> {\n      log.warn(\"denied\")\n      deny()\n    }\n"
              "    else -> error()\n"
              "  }\n"
              "}\n");
  s = f.unit->decls.first->as.fn.body.first;
  UNIT_EQ_INT(t, s->kind, STMT_MATCH);
  UNIT_EQ_INT(t, s->as.match.arms.count, 3);
  UNIT_EQ_INT(t, s->as.match.arms.first->pat->kind, PAT_VARIANT);
  UNIT_EQ_INT(t, s->as.match.arms.first->next->pat->kind, PAT_ALT);
  UNIT_EQ_INT(t, s->as.match.arms.first->next->pat->as.alts.count, 2);
  UNIT_TRUE(t, s->as.match.arms.first->next->has_block);
  UNIT_NULL(t, s->as.match.arms.last->pat);
  fix_free(&f);
}

static void defer_takes_a_call(unit *t) {
  fixture f;

  parse_clean(t, &f, "fn f() {\n  defer file.close()\n}\n");
  UNIT_EQ_INT(t, f.unit->decls.first->as.fn.body.first->kind, STMT_DEFER);
  fix_free(&f);

  fix_init(&f, "fn f() {\n  defer x\n}\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_DEFER_NEEDS_CALL));
  fix_free(&f);
}

/* ---- expressions ------------------------------------------------------- */

static void precedence_follows_the_grammar(unit *t) {
  fixture f;
  const expr *e;

  /* `*` binds tighter than `+`. */
  e = parse_value(t, &f, "1 + 2 * 3");
  UNIT_EQ_INT(t, e->kind, EXPR_BINARY);
  UNIT_EQ_INT(t, e->as.binary.op, BINOP_ADD);
  UNIT_EQ_INT(t, e->as.binary.rhs->as.binary.op, BINOP_MUL);
  fix_free(&f);

  /* `and` binds tighter than `or`. */
  e = parse_value(t, &f, "a or b and c");
  UNIT_EQ_INT(t, e->as.binary.op, BINOP_OR);
  UNIT_EQ_INT(t, e->as.binary.rhs->as.binary.op, BINOP_AND);
  fix_free(&f);

  /* `not` is looser than comparison, so it negates the whole comparison. */
  e = parse_value(t, &f, "not a == b");
  UNIT_EQ_INT(t, e->kind, EXPR_UNARY);
  UNIT_EQ_INT(t, e->as.unary.op, UNOP_NOT);
  UNIT_EQ_INT(t, e->as.unary.operand->as.binary.op, BINOP_EQ);
  fix_free(&f);
}

static void arithmetic_is_left_associative(unit *t) {
  fixture f;
  const expr *e = parse_value(t, &f, "1 - 2 - 3");

  UNIT_EQ_INT(t, e->as.binary.op, BINOP_SUB);
  UNIT_EQ_INT(t, e->as.binary.lhs->kind, EXPR_BINARY);
  UNIT_EQ_INT(t, e->as.binary.rhs->kind, EXPR_INT);
  fix_free(&f);
}

static void comparison_does_not_chain(unit *t) {
  fixture f;

  fix_init(&f, "let x = a < b < c\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_COMPARISON_CHAIN));
  fix_free(&f);

  parse_clean(t, &f, "let x = a < b and b < c\n");
  fix_free(&f);
}

static void a_dotted_name_is_one_path(unit *t) {
  /* `db.all` is a single qualified name, not a field access on `db`. D030 makes
   * every non-local name fully qualified, so the resolver is what decides whether a
   * path names a module member or a field on a local -- and it wants the whole path
   * in one node. EXPR_FIELD arises only after something that is not a name. */
  fixture f;
  const expr *e = parse_value(t, &f, "db.all(q)!");

  UNIT_EQ_INT(t, e->kind, EXPR_PROPAGATE);
  UNIT_EQ_INT(t, e->as.propagate->kind, EXPR_CALL);
  UNIT_EQ_INT(t, e->as.propagate->as.call.callee->kind, EXPR_IDENT);
  UNIT_EQ_INT(t, e->as.propagate->as.call.callee->as.ident.count, 2);
  UNIT_EQ_SLICE(t, e->as.propagate->as.call.callee->as.ident.last->name, "all");
  fix_free(&f);

  /* After a call, a `.` is a field access. */
  e = parse_value(t, &f, "f().name");
  UNIT_EQ_INT(t, e->kind, EXPR_FIELD);
  UNIT_EQ_INT(t, e->as.field.target->kind, EXPR_CALL);
  fix_free(&f);
}

static void a_type_argument_is_distinguished_from_an_index(unit *t) {
  /* `db.all[Msg](...)` is a call with a type argument; `xs[i]` is an index. The
   * difference is decided by the `(` that follows, with no backtracking. */
  fixture f;
  const expr *e;

  e = parse_value(t, &f, "db.all[Msg](sql, room)!");
  UNIT_EQ_INT(t, e->kind, EXPR_PROPAGATE);
  if (e->as.propagate->kind == EXPR_CALL) {
    UNIT_EQ_INT(t, e->as.propagate->as.call.type_args.count, 1);
    UNIT_EQ_INT(t, e->as.propagate->as.call.args.count, 2);
    UNIT_EQ_SLICE(t, e->as.propagate->as.call.type_args.first->as.p.segs.first->name, "Msg");
  }
  fix_free(&f);

  e = parse_value(t, &f, "xs[i]");
  UNIT_EQ_INT(t, e->kind, EXPR_INDEX);
  fix_free(&f);
}

static void else_is_right_associative_and_loosest(unit *t) {
  fixture f;
  const expr *e = parse_value(t, &f, "a else b else c");

  UNIT_EQ_INT(t, e->kind, EXPR_COALESCE);
  UNIT_EQ_INT(t, e->as.coalesce.form, COALESCE_VALUE);
  UNIT_EQ_INT(t, e->as.coalesce.fallback->kind, EXPR_COALESCE);
  fix_free(&f);
}

static void else_has_a_value_form_and_a_block_form(unit *t) {
  fixture f;
  const decl *d;

  parse_clean(t, &f, "fn f() -> int! {\n  let u = find(1) else return 0\n  return 1\n}\n");
  d = f.unit->decls.first;
  UNIT_EQ_INT(t, d->as.fn.body.first->as.let.value->as.coalesce.form, COALESCE_BLOCK);
  fix_free(&f);

  parse_clean(t, &f,
              "fn f() -> int! {\n  let u = find(1) else err {\n    return 0\n  }\n  return 1\n}\n");
  d = f.unit->decls.first;
  UNIT_TRUE(t, d->as.fn.body.first->as.let.value->as.coalesce.binds_err);
  UNIT_EQ_SLICE(t, d->as.fn.body.first->as.let.value->as.coalesce.err_name, "err");
  fix_free(&f);

  parse_clean(t, &f, "let u = find(1) else User.guest()\n");
  UNIT_EQ_INT(t, f.unit->decls.first->as.let.value->as.coalesce.form, COALESCE_VALUE);
  fix_free(&f);
}

static void with_updates_fields_functionally(unit *t) {
  fixture f;
  const expr *e = parse_value(t, &f, "user with { name: \"Ada\", bio: nil }");

  UNIT_EQ_INT(t, e->kind, EXPR_WITH);
  UNIT_EQ_INT(t, e->as.with.fields.count, 2);
  fix_free(&f);
}

static void lambdas_take_an_expression_or_a_block_body(unit *t) {
  fixture f;
  const expr *e;

  e = parse_value(t, &f, "users.map(fn(u: User) => u.name)");
  UNIT_EQ_INT(t, e->kind, EXPR_CALL);
  UNIT_EQ_INT(t, e->as.call.args.first->kind, EXPR_LAMBDA);
  UNIT_FALSE(t, e->as.call.args.first->as.lambda.has_block);
  fix_free(&f);

  e = parse_value(t, &f, "xs.each(fn(x: int) {\n  log(x)\n})");
  UNIT_TRUE(t, e->as.call.args.first->as.lambda.has_block);
  fix_free(&f);
}

static void collections_and_literals(unit *t) {
  fixture f;
  const expr *e;

  e = parse_value(t, &f, "[1, 2, 3]");
  UNIT_EQ_INT(t, e->kind, EXPR_LIST);
  UNIT_EQ_INT(t, e->as.list.count, 3);
  fix_free(&f);

  e = parse_value(t, &f, "{\"a\": 1, \"b\": 2}");
  UNIT_EQ_INT(t, e->kind, EXPR_MAP);
  UNIT_EQ_INT(t, e->as.map.count, 2);
  fix_free(&f);

  e = parse_value(t, &f, ".active");
  UNIT_EQ_INT(t, e->kind, EXPR_VARIANT);
  UNIT_EQ_SLICE(t, e->as.variant, "active");
  fix_free(&f);
}

/* ---- literal decoding -------------------------------------------------- */

static void integer_literals_decode_in_every_radix(unit *t) {
  fixture f;

  UNIT_EQ_INT(t, parse_value(t, &f, "42")->as.int_value, 42);
  fix_free(&f);
  UNIT_EQ_INT(t, parse_value(t, &f, "0xff")->as.int_value, 255);
  fix_free(&f);
  UNIT_EQ_INT(t, parse_value(t, &f, "0b1011")->as.int_value, 11);
  fix_free(&f);
  UNIT_EQ_INT(t, parse_value(t, &f, "1_000_000")->as.int_value, 1000000);
  fix_free(&f);
}

static void a_negative_literal_folds_its_sign(unit *t) {
  /* Folding is what makes the most negative int expressible: its magnitude does
   * not fit on its own, so it must not be parsed on its own. */
  fixture f;
  const expr *e;

  e = parse_value(t, &f, "-42");
  UNIT_EQ_INT(t, e->kind, EXPR_INT);
  UNIT_EQ_INT(t, e->as.int_value, -42);
  fix_free(&f);

  e = parse_value(t, &f, "-9223372036854775808");
  UNIT_EQ_INT(t, e->kind, EXPR_INT);
  UNIT_TRUE(t, e->as.int_value == (-9223372036854775807LL - 1LL));
  fix_free(&f);
}

static void an_integer_literal_out_of_range_is_reported(unit *t) {
  fixture f;

  fix_init(&f, "let x = 9223372036854775808\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_INT_LITERAL_RANGE));
  fix_free(&f);

  fix_init(&f, "let x = 99999999999999999999999\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_INT_LITERAL_RANGE));
  fix_free(&f);
}

static void a_float_literal_out_of_range_is_reported(unit *t) {
  fixture f;

  fix_init(&f, "let x = 1e400\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_FLOAT_LITERAL_RANGE));
  fix_free(&f);
}

static void escapes_are_resolved(unit *t) {
  fixture f;
  const expr *e = parse_value(t, &f, "\"a\\nb\\tc\\\\d\\\"e\"");

  UNIT_EQ_INT(t, e->kind, EXPR_STR);
  UNIT_EQ_INT(t, e->as.str.count, 1);
  UNIT_EQ_SLICE(t, e->as.str.first->text, "a\nb\tc\\d\"e");
  fix_free(&f);
}

static void a_unicode_escape_becomes_utf8(unit *t) {
  fixture f;
  const expr *e = parse_value(t, &f, "\"\\u{41}\\u{e9}\\u{1f600}\"");

  /* A, e-acute, and an astral character, encoded as 1 + 2 + 4 bytes. */
  UNIT_EQ_SLICE(t, e->as.str.first->text, "A\xc3\xa9\xf0\x9f\x98\x80");
  fix_free(&f);
}

static void bad_escapes_are_reported_at_the_escape(unit *t) {
  fixture f;
  const diag *d;

  fix_init(&f, "let x = \"a\\qb\"\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_UNKNOWN_ESCAPE));
  for (d = f.sink.first; d != NULL; d = d->next) {
    if (d->code == DIAG_UNKNOWN_ESCAPE) {
      /* The backslash, not the opening quote at offset 8. */
      UNIT_EQ_INT(t, d->at.start, 10);
      break;
    }
  }
  fix_free(&f);

  fix_init(&f, "let x = \"\\u41\"\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_MALFORMED_UNICODE_ESCAPE));
  fix_free(&f);

  fix_init(&f, "let x = \"\\u{}\"\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_MALFORMED_UNICODE_ESCAPE));
  fix_free(&f);

  fix_init(&f, "let x = \"\\u{110000}\"\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_UNICODE_ESCAPE_RANGE));
  fix_free(&f);

  /* A surrogate is not a scalar value, so it cannot appear in a UTF-8 string. */
  fix_init(&f, "let x = \"\\u{d800}\"\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_UNICODE_ESCAPE_RANGE));
  fix_free(&f);
}

static void interpolation_builds_a_part_list(unit *t) {
  fixture f;
  const expr *e = parse_value(t, &f, "\"hello ${name}, you have ${count} messages\"");

  UNIT_EQ_INT(t, e->kind, EXPR_STR);
  UNIT_EQ_INT(t, e->as.str.count, 5);
  UNIT_EQ_SLICE(t, e->as.str.first->text, "hello ");
  UNIT_NOT_NULL(t, e->as.str.first->next->value);
  fix_free(&f);
}

static void a_raw_string_keeps_its_bytes(unit *t) {
  fixture f;
  const expr *e = parse_value(t, &f, "`select * from t where a = ?`");

  UNIT_EQ_INT(t, e->kind, EXPR_RAW_STR);
  UNIT_EQ_SLICE(t, e->as.raw_str, "select * from t where a = ?");
  fix_free(&f);
}

/* ---- types ------------------------------------------------------------- */

static void type_forms_parse(unit *t) {
  fixture f;
  const decl *d;

  parse_clean(t, &f,
              "fn f(a: int, b: [str], c: {str: int}, d: fn(int) -> str, e: time.Time?) {\n}\n");
  d = f.unit->decls.first;
  UNIT_EQ_INT(t, d->as.fn.params.count, 5);
  UNIT_EQ_INT(t, d->as.fn.params.first->type->kind, TYPE_PATH);
  UNIT_EQ_INT(t, d->as.fn.params.first->next->type->kind, TYPE_LIST);
  UNIT_EQ_INT(t, d->as.fn.params.first->next->next->type->kind, TYPE_MAP);
  UNIT_EQ_INT(t, d->as.fn.params.first->next->next->next->type->kind, TYPE_FN);
  UNIT_TRUE(t, d->as.fn.params.last->type->optional);
  UNIT_EQ_INT(t, d->as.fn.params.last->type->as.p.segs.count, 2);
  fix_free(&f);
}

/* ---- the rules the parser discharges ----------------------------------- */

static void a_top_level_binding_must_be_let(unit *t) {
  fixture f;

  fix_init(&f, "var total = 0\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_TOPLEVEL_MUST_BE_LET));
  fix_free(&f);

  /* Inside a function `var` is fine. */
  parse_clean(t, &f, "fn f() {\n  var total = 0\n  total += 1\n}\n");
  fix_free(&f);
}

static void a_method_needs_self_and_a_function_must_not_have_it(unit *t) {
  fixture f;

  fix_init(&f, "fn User.display(name: str) -> str {\n  return name\n}\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_METHOD_NEEDS_SELF));
  fix_free(&f);

  fix_init(&f, "fn display(self) -> str {\n  return \"x\"\n}\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_METHOD_NEEDS_SELF));
  fix_free(&f);
}

static void self_break_and_continue_need_their_context(unit *t) {
  fixture f;

  fix_init(&f, "fn f() {\n  return self.x\n}\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_SELF_OUTSIDE_METHOD));
  fix_free(&f);

  fix_init(&f, "fn f() {\n  break\n}\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_BREAK_OUTSIDE_LOOP));
  fix_free(&f);

  fix_init(&f, "fn f() {\n  continue\n}\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_CONTINUE_OUTSIDE_LOOP));
  fix_free(&f);

  /* Inside a loop both are fine, including nested in an if. */
  parse_clean(t, &f, "fn f() {\n  while a {\n    if b { break }\n    continue\n  }\n}\n");
  fix_free(&f);
}

static void send_needs_a_stream(unit *t) {
  fixture f;

  fix_init(&f, "fn f() {\n  send x\n}\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_SEND_OUTSIDE_STREAM));
  fix_free(&f);
}

static void duplicate_names_are_reported(unit *t) {
  fixture f;

  fix_init(&f, "fn f(a: int, a: str) {\n}\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_DUPLICATE_PARAM));
  fix_free(&f);

  fix_init(&f, "type T {\n  a: int\n  a: str\n}\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_DUPLICATE_FIELD));
  fix_free(&f);

  fix_init(&f, "type E enum { a, b, a }\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_DUPLICATE_VARIANT));
  fix_free(&f);
}

static void an_unknown_attribute_is_reported(unit *t) {
  fixture f;

  fix_init(&f, "type T {\n  a: int @nonsense\n}\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_UNKNOWN_ATTRIBUTE));
  fix_free(&f);

  parse_clean(t, &f, "type T {\n  a: str @len(1, 5) @email @trim\n}\n");
  fix_free(&f);
}

static void pub_is_rejected_where_it_means_nothing(unit *t) {
  fixture f;

  fix_init(&f, "pub route GET \"/\" () -> html! { return page() }\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_PUB_NOT_ALLOWED));
  fix_free(&f);
}

static void a_reserved_word_names_the_doot_construct(unit *t) {
  fixture f;
  const diag *d;

  fix_init(&f, "import x\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_RESERVED_WORD));
  for (d = f.sink.first; d != NULL; d = d->next) {
    if (d->code == DIAG_RESERVED_WORD) {
      UNIT_TRUE(t, slice_contains_byte(d->message, '`'));
      break;
    }
  }
  fix_free(&f);
}

static void deferred_features_parse_but_are_unavailable(unit *t) {
  /* The grammar is frozen at v0.1 even for features that land later (D042), so
   * these must parse and then say when they arrive. */
  fixture f;

  fix_init(&f, "stream GET \"/live\" (room: str) {\n  send x\n}\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_FEATURE_UNAVAILABLE));
  UNIT_EQ_INT(t, f.unit->decls.first->kind, DECL_STREAM);
  fix_free(&f);

  fix_init(&f, "fn f() {\n  spawn work(1)\n}\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_FEATURE_UNAVAILABLE));
  fix_free(&f);
}

/* ---- markup ------------------------------------------------------------ */

static void a_markup_literal_becomes_an_element_tree(unit *t) {
  fixture f;
  const expr *e = parse_value(t, &f, "<div class=\"card\"><h2>hi</h2></div>");
  const markup_node *m;

  UNIT_EQ_INT(t, e->kind, EXPR_MARKUP);
  m = e->as.markup;
  UNIT_EQ_INT(t, m->kind, MARKUP_ELEMENT);
  UNIT_EQ_SLICE(t, m->as.element.tag, "div");
  UNIT_EQ_INT(t, m->as.element.attrs.count, 1);
  UNIT_EQ_SLICE(t, m->as.element.attrs.first->name, "class");
  UNIT_EQ_INT(t, m->as.element.children.count, 1);
  UNIT_EQ_SLICE(t, m->as.element.children.first->as.element.tag, "h2");
  fix_free(&f);
}

static void a_void_element_self_closes(unit *t) {
  fixture f;
  const expr *e = parse_value(t, &f, "<input name=\"q\" required/>");

  UNIT_TRUE(t, e->as.markup->as.element.self_closing);
  UNIT_EQ_INT(t, e->as.markup->as.element.attrs.count, 2);
  UNIT_NULL(t, e->as.markup->as.element.attrs.last->value);
  fix_free(&f);
}

static void a_void_element_written_without_a_slash_still_self_closes(unit *t) {
  /* `<br>` and `<br/>` are the same element. Without this the lexer keeps the
   * frame in content mode and everything after `<br>` becomes its children,
   * swallowing the rest of the literal. */
  fixture f;
  const markup_node *div;

  div = parse_value(t, &f, "<div><br>after<hr>more</div>")->as.markup;
  UNIT_EQ_SLICE(t, div->as.element.tag, "div");
  UNIT_EQ_INT(t, div->as.element.children.count, 4);
  UNIT_EQ_INT(t, div->as.element.children.first->kind, MARKUP_ELEMENT);
  UNIT_EQ_SLICE(t, div->as.element.children.first->as.element.tag, "br");
  UNIT_TRUE(t, div->as.element.children.first->as.element.self_closing);
  UNIT_EQ_INT(t, div->as.element.children.first->next->kind, MARKUP_TEXT);
  fix_free(&f);
}

static void interpolation_in_text_and_attributes(unit *t) {
  fixture f;
  const expr *e = parse_value(t, &f, "<a href=\"/users/${u.id}\">${u.name}</a>");
  const markup_node *m = e->as.markup;

  UNIT_EQ_INT(t, m->as.element.attrs.count, 1);
  UNIT_EQ_INT(t, m->as.element.attrs.first->value->kind, EXPR_STR);
  UNIT_EQ_INT(t, m->as.element.children.count, 1);
  UNIT_EQ_INT(t, m->as.element.children.first->kind, MARKUP_INTERP);
  fix_free(&f);
}

static void markup_control_flow_uses_statement_keywords(unit *t) {
  fixture f;
  const markup_node *ul;
  const markup_node *loop;

  ul = parse_value(t, &f, "<ul>{for m in msgs}<li>${m.body}</li>{else}<li>none</li>{end}</ul>")
           ->as.markup;
  UNIT_EQ_INT(t, ul->as.element.children.count, 1);
  loop = ul->as.element.children.first;
  UNIT_EQ_INT(t, loop->kind, MARKUP_FOR);
  UNIT_EQ_SLICE(t, loop->as.for_.first_name, "m");
  UNIT_EQ_INT(t, loop->as.for_.body.count, 1);
  UNIT_TRUE(t, loop->as.for_.has_empty);
  UNIT_EQ_INT(t, loop->as.for_.empty.count, 1);
  fix_free(&f);
}

static void markup_if_chains_collect_branches(unit *t) {
  fixture f;
  const markup_node *node;

  node = parse_value(t, &f, "<p>{if a}<b>1</b>{else if c}<b>2</b>{else}<b>3</b>{end}</p>")
             ->as.markup->as.element.children.first;
  UNIT_EQ_INT(t, node->kind, MARKUP_IF);
  UNIT_EQ_INT(t, node->as.if_.branches.count, 3);
  UNIT_NOT_NULL(t, node->as.if_.branches.first->cond);
  UNIT_NULL(t, node->as.if_.branches.last->cond);
  fix_free(&f);
}

static void an_attribute_spread_parses(unit *t) {
  fixture f;
  const expr *e;

  e = parse_value(t, &f, "<div ...attrs/>");
  UNIT_TRUE(t, e->as.markup->as.element.attrs.first->is_spread);
  UNIT_EQ_INT(t, e->as.markup->as.element.attrs.first->value->kind, EXPR_IDENT);
  fix_free(&f);

  /* Anything beyond a bare name goes through an interpolation, which the lexer
   * already knows how to open inside a tag. */
  e = parse_value(t, &f, "<div ...${html.attrs(m)}/>");
  UNIT_TRUE(t, e->as.markup->as.element.attrs.first->is_spread);
  UNIT_EQ_INT(t, e->as.markup->as.element.attrs.first->value->kind, EXPR_CALL);
  fix_free(&f);
}

static void a_mismatched_closing_tag_carries_both_spans(unit *t) {
  fixture f;
  const diag *d;

  fix_init(&f, "let x = <div><p>hi</b></div>\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_MARKUP_TAG_MISMATCH));
  for (d = f.sink.first; d != NULL; d = d->next) {
    if (d->code == DIAG_MARKUP_TAG_MISMATCH) {
      /* The opening tag is attached as a related span, which is what makes the
       * human output show both ends of the mistake. */
      UNIT_NOT_NULL(t, d->labels);
      break;
    }
  }
  fix_free(&f);
}

static void markup_errors_are_reported(unit *t) {
  fixture f;

  /* A void element self-closes at its `>`, so its closing tag never matches an
   * open element and gets the specific message rather than the generic mismatch. */
  fix_init(&f, "let x = <div><br>text</br></div>\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_MARKUP_VOID_WITH_CLOSE));
  fix_free(&f);

  fix_init(&f, "let x = <div a=1></div>\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_MARKUP_BAD_ATTR_VALUE));
  fix_free(&f);

  fix_init(&f, "let x = <div a=\"1\" a=\"2\"></div>\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_MARKUP_DUPLICATE_ATTR));
  fix_free(&f);

  fix_init(&f, "let x = <p>{nonsense}</p>\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_MARKUP_UNKNOWN_DIRECTIVE));
  fix_free(&f);

  fix_init(&f, "let x = <p>{if a}x</p>\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_MARKUP_MISSING_END));
  fix_free(&f);

  fix_init(&f, "let x = <p>{if a}x{else}y{else}z{end}</p>\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_MARKUP_ELSE_AFTER_ELSE));
  fix_free(&f);

  fix_init(&f, "let x = <p>{end}</p>\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_MARKUP_ELSE_WITHOUT_IF));
  fix_free(&f);
}

/* ---- recovery ---------------------------------------------------------- */

static void a_syntax_error_does_not_cascade(unit *t) {
  fixture f;

  fix_init(&f, "let a = ;\nlet b = 2\nlet c = 3\n");
  UNIT_TRUE(t, diag_error_count(&f.sink) > 0);
  /* Recovery keeps the later declarations. */
  UNIT_TRUE(t, f.unit->decls.count >= 2);
  fix_free(&f);
}

static void unterminated_input_terminates(unit *t) {
  fixture f;

  fix_init(&f, "fn f() {\n  let a = (((\n");
  UNIT_TRUE(t, diag_error_count(&f.sink) > 0);
  fix_free(&f);

  fix_init(&f, "type T {\n");
  UNIT_TRUE(t, diag_error_count(&f.sink) > 0);
  fix_free(&f);
}

static void the_generic_syntax_diagnostics_are_reachable(unit *t) {
  fixture f;

  /* An unexpected token names what was expected instead. */
  fix_init(&f, "type 9 {}\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_UNEXPECTED_TOKEN));
  fix_free(&f);

  /* Running out of input mid-construct is its own code, so the message can say
   * that the file ended rather than naming a token that is not there. */
  fix_init(&f, "fn f(");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_UNEXPECTED_EOF));
  fix_free(&f);

  /* Two statements on one line: there is no separator to write, because doot is
   * newline-terminated and has no `;`. */
  fix_init(&f, "fn f() { let a = 1 let b = 2 }\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_EXPECTED_STMT_END));
  fix_free(&f);

  fix_init(&f, "fn f() {\n  spawn x\n}\n");
  UNIT_TRUE(t, has_code(&f.sink, DIAG_SPAWN_NEEDS_CALL));
  fix_free(&f);
}

static void recovery_always_makes_progress(unit *t) {
  /* A token that both fails to parse and is a synchronization boundary used to
   * leave the cursor where it was, so the enclosing loop spun forever. Found by
   * fuzz_parse on its first run; these are the shapes that reproduced it. */
  fixture f;

  fix_init(&f, "}");
  UNIT_TRUE(t, diag_error_count(&f.sink) > 0);
  fix_free(&f);

  fix_init(&f, "}}}}\n}\n");
  UNIT_TRUE(t, diag_error_count(&f.sink) > 0);
  fix_free(&f);

  fix_init(&f, "fn f() {\n  }}}\n}\n");
  UNIT_TRUE(t, diag_error_count(&f.sink) > 0);
  fix_free(&f);

  fix_init(&f, "group \"/x\" {\n  }}}\n}\n");
  UNIT_TRUE(t, diag_error_count(&f.sink) > 0);
  fix_free(&f);
}

static void deep_nesting_is_a_diagnostic_not_a_stack_overflow(unit *t) {
  fixture f;
  char text[1024];
  size_t i;

  (void)memcpy(text, "let x = ", 8u);
  for (i = 0; i < 400u; i++) {
    text[8u + i] = '(';
  }
  text[408] = '\0';

  fix_init(&f, text);
  UNIT_TRUE(t, has_code(&f.sink, DIAG_NESTING_TOO_DEEP));
  fix_free(&f);
}

/* ---- the documented application ---------------------------------------- */

static void the_chat_application_parses_clean(unit *t) {
  /* The complete program from docs/02-syntax.md. Its `stream` handler reports
   * DT0046 because SSE lands in v0.2, so that declaration is checked separately;
   * everything else must parse with no diagnostics at all. */
  static const char *src =
      "type Msg {\n"
      "  id:   int\n"
      "  room: str\n"
      "  body: str\n"
      "  at:   time.Time\n"
      "}\n"
      "\n"
      "fn layout(title: str, body: html) -> html {\n"
      "  return <html>\n"
      "    <head><title>${title}</title></head>\n"
      "    <body>${body}</body>\n"
      "  </html>\n"
      "}\n"
      "\n"
      "route GET \"/rooms/:room\" (room: str) -> html! {\n"
      "  let msgs = db.all[Msg](\n"
      "    \"select * from msgs where room = ? order by id desc limit 50\", room)!\n"
      "\n"
      "  return layout(room, <div>\n"
      "    <ul id=\"feed\" data-live=\"/rooms/${room}/live\">\n"
      "      {for m in msgs}\n"
      "        <li>${m.body}</li>\n"
      "      {end}\n"
      "    </ul>\n"
      "    <form method=\"post\" action=\"/rooms/${room}\">\n"
      "      <input name=\"body\" required/>\n"
      "      <button>send</button>\n"
      "    </form>\n"
      "  </div>)\n"
      "}\n"
      "\n"
      "type NewMsg {\n"
      "  body: str @len(1, 500) @trim\n"
      "}\n"
      "\n"
      "route POST \"/rooms/:room\" (room: str, form: NewMsg) -> redirect! {\n"
      "  let m = db.one[Msg](\n"
      "    \"insert into msgs (room, body, at) values (?, ?, ?) returning *\",\n"
      "    room, form.body, time.now())!\n"
      "\n"
      "  topic.publish(\"room:\" + room, m)\n"
      "  return http.see_other(\"/rooms/\" + room)\n"
      "}\n";
  fixture f;

  parse_clean(t, &f, src);
  UNIT_EQ_INT(t, f.unit->decls.count, 5);
  UNIT_EQ_INT(t, f.unit->decls.first->kind, DECL_STRUCT);
  UNIT_EQ_INT(t, f.unit->decls.last->kind, DECL_ROUTE);
  fix_free(&f);
}

static void the_stream_handler_parses_with_only_the_availability_error(unit *t) {
  static const char *src = "stream GET \"/rooms/:room/live\" (room: str) {\n"
                           "  for m in topic.subscribe[Msg](\"room:\" + room) {\n"
                           "    send <li>${m.body}</li>\n"
                           "  }\n"
                           "}\n";
  fixture f;
  const diag *d;

  fix_init(&f, src);
  UNIT_EQ_INT(t, f.unit->decls.count, 1);
  UNIT_EQ_INT(t, f.unit->decls.first->kind, DECL_STREAM);
  /* Every diagnostic must be the availability notice: the syntax is final. */
  for (d = f.sink.first; d != NULL; d = d->next) {
    UNIT_EQ_INT(t, d->code, DIAG_FEATURE_UNAVAILABLE);
  }
  fix_free(&f);
}

static void the_configuration_example_parses(unit *t) {
  /* Configuration is doot code, not a file format (D040). */
  fixture f;

  parse_clean(t, &f,
              "let config = Config {\n"
              "  listen:          \":8080\"\n"
              "  database:        \"data/app.db\"\n"
              "  workers:         os.cpu_count()\n"
              "  request_memory:  16.mb\n"
              "  request_timeout: 15.s\n"
              "  static_dir:      \"static\"\n"
              "}\n");
  UNIT_EQ_INT(t, f.unit->decls.first->as.let.value->kind, EXPR_STRUCT);
  UNIT_EQ_INT(t, f.unit->decls.first->as.let.value->as.struct_lit.fields.count, 6);
  fix_free(&f);
}

static void comments_survive_parsing(unit *t) {
  /* The formatter depends on this: the lexer records them, the unit carries them
   * in source order (D067). */
  fixture f;

  parse_clean(t, &f, "// leading\nfn f() {\n  // inside\n  let a = 1 // trailing\n}\n");
  UNIT_EQ_INT(t, f.unit->comments.count, 3);
  fix_free(&f);
}

static const unit_case cases[] = {
    {"a_struct_type_collects_its_fields", a_struct_type_collects_its_fields},
    {"a_field_may_carry_a_default_and_attributes", a_field_may_carry_a_default_and_attributes},
    {"an_enum_is_tag_only", an_enum_is_tag_only},
    {"a_method_attaches_to_a_type_by_name", a_method_attaches_to_a_type_by_name},
    {"a_route_records_its_method_and_pattern", a_route_records_its_method_and_pattern},
    {"a_group_carries_a_prefix_and_hooks", a_group_carries_a_prefix_and_hooks},
    {"pub_marks_a_declaration_exported", pub_marks_a_declaration_exported},
    {"a_test_block_is_a_declaration", a_test_block_is_a_declaration},
    {"statements_are_newline_terminated", statements_are_newline_terminated},
    {"the_last_statement_in_a_block_needs_no_newline",
     the_last_statement_in_a_block_needs_no_newline},
    {"if_else_chains_nest", if_else_chains_nest},
    {"a_condition_is_not_a_struct_literal", a_condition_is_not_a_struct_literal},
    {"for_loops_take_one_or_two_variables", for_loops_take_one_or_two_variables},
    {"match_arms_take_an_expression_or_a_block", match_arms_take_an_expression_or_a_block},
    {"defer_takes_a_call", defer_takes_a_call},
    {"precedence_follows_the_grammar", precedence_follows_the_grammar},
    {"arithmetic_is_left_associative", arithmetic_is_left_associative},
    {"comparison_does_not_chain", comparison_does_not_chain},
    {"a_dotted_name_is_one_path", a_dotted_name_is_one_path},
    {"a_type_argument_is_distinguished_from_an_index",
     a_type_argument_is_distinguished_from_an_index},
    {"else_is_right_associative_and_loosest", else_is_right_associative_and_loosest},
    {"else_has_a_value_form_and_a_block_form", else_has_a_value_form_and_a_block_form},
    {"with_updates_fields_functionally", with_updates_fields_functionally},
    {"lambdas_take_an_expression_or_a_block_body", lambdas_take_an_expression_or_a_block_body},
    {"collections_and_literals", collections_and_literals},
    {"integer_literals_decode_in_every_radix", integer_literals_decode_in_every_radix},
    {"a_negative_literal_folds_its_sign", a_negative_literal_folds_its_sign},
    {"an_integer_literal_out_of_range_is_reported", an_integer_literal_out_of_range_is_reported},
    {"a_float_literal_out_of_range_is_reported", a_float_literal_out_of_range_is_reported},
    {"escapes_are_resolved", escapes_are_resolved},
    {"a_unicode_escape_becomes_utf8", a_unicode_escape_becomes_utf8},
    {"bad_escapes_are_reported_at_the_escape", bad_escapes_are_reported_at_the_escape},
    {"interpolation_builds_a_part_list", interpolation_builds_a_part_list},
    {"a_raw_string_keeps_its_bytes", a_raw_string_keeps_its_bytes},
    {"type_forms_parse", type_forms_parse},
    {"a_top_level_binding_must_be_let", a_top_level_binding_must_be_let},
    {"a_method_needs_self_and_a_function_must_not_have_it",
     a_method_needs_self_and_a_function_must_not_have_it},
    {"self_break_and_continue_need_their_context", self_break_and_continue_need_their_context},
    {"send_needs_a_stream", send_needs_a_stream},
    {"duplicate_names_are_reported", duplicate_names_are_reported},
    {"an_unknown_attribute_is_reported", an_unknown_attribute_is_reported},
    {"pub_is_rejected_where_it_means_nothing", pub_is_rejected_where_it_means_nothing},
    {"a_reserved_word_names_the_doot_construct", a_reserved_word_names_the_doot_construct},
    {"deferred_features_parse_but_are_unavailable", deferred_features_parse_but_are_unavailable},
    {"a_markup_literal_becomes_an_element_tree", a_markup_literal_becomes_an_element_tree},
    {"a_void_element_self_closes", a_void_element_self_closes},
    {"a_void_element_written_without_a_slash_still_self_closes",
     a_void_element_written_without_a_slash_still_self_closes},
    {"interpolation_in_text_and_attributes", interpolation_in_text_and_attributes},
    {"markup_control_flow_uses_statement_keywords", markup_control_flow_uses_statement_keywords},
    {"markup_if_chains_collect_branches", markup_if_chains_collect_branches},
    {"an_attribute_spread_parses", an_attribute_spread_parses},
    {"a_mismatched_closing_tag_carries_both_spans", a_mismatched_closing_tag_carries_both_spans},
    {"markup_errors_are_reported", markup_errors_are_reported},
    {"a_syntax_error_does_not_cascade", a_syntax_error_does_not_cascade},
    {"unterminated_input_terminates", unterminated_input_terminates},
    {"the_generic_syntax_diagnostics_are_reachable", the_generic_syntax_diagnostics_are_reachable},
    {"recovery_always_makes_progress", recovery_always_makes_progress},
    {"deep_nesting_is_a_diagnostic_not_a_stack_overflow",
     deep_nesting_is_a_diagnostic_not_a_stack_overflow},
    {"the_chat_application_parses_clean", the_chat_application_parses_clean},
    {"the_stream_handler_parses_with_only_the_availability_error",
     the_stream_handler_parses_with_only_the_availability_error},
    {"the_configuration_example_parses", the_configuration_example_parses},
    {"comments_survive_parsing", comments_survive_parsing},
};

UNIT_SUITE(suite_parse, "parse", cases);
