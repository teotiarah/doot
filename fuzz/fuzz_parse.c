/* fuzz_parse.c -- arbitrary bytes through the whole front end.
 *
 * The broadest target in the project: source to AST, including markup
 * tokenization, which no other target reaches because entering markup is the
 * parser's decision (D059).
 *
 * Beyond "does not crash", this asserts the contract parse_unit publishes: it
 * always returns a unit, a malformed input yields a populated sink rather than a
 * NULL tree, and every span in the resulting tree lies inside the source. The last
 * one matters because a bad span turns a diagnostic into a crash in the renderer,
 * a long way from the code that produced it.
 *
 * The invariant, as for every doot fuzz target: arbitrary input produces a
 * diagnostic, never a crash, a hang, or unbounded memory growth.
 */
#include <stdint.h>
#include <stdlib.h>

#include "../src/base/arena.h"
#include "../src/base/buf.h"
#include "../src/base/diag.h"
#include "../src/base/source.h"
#include "../src/parse/ast.h"
#include "../src/parse/parse.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static uint32_t g_limit;
static int g_bad;

static void check_span(span at) {
  if (span_is_none(at)) {
    return; /* a node may legitimately carry no position */
  }
  if (at.start > at.end || at.end > g_limit) {
    g_bad = 1;
  }
}

/* Walks the tree far enough to touch every span-bearing node reachable without
 * duplicating the whole grammar here: declarations, their bodies, and expressions
 * to a bounded depth. */
static void walk_expr(const expr *e, uint32_t depth);

static void walk_stmts(const stmt_list *l, uint32_t depth);

static void walk_markup(const markup_node *m, uint32_t depth) {
  const markup_node *n;

  if (depth > 64u) {
    return;
  }
  for (n = m; n != NULL; n = n->next) {
    check_span(n->at);
    if (n->kind == MARKUP_ELEMENT) {
      const markup_attr *a;

      for (a = n->as.element.attrs.first; a != NULL; a = a->next) {
        check_span(a->at);
        walk_expr(a->value, depth + 1u);
      }
      walk_markup(n->as.element.children.first, depth + 1u);
    } else if (n->kind == MARKUP_INTERP) {
      walk_expr(n->as.interp, depth + 1u);
    } else if (n->kind == MARKUP_IF) {
      const markup_branch *b;

      for (b = n->as.if_.branches.first; b != NULL; b = b->next) {
        check_span(b->at);
        walk_expr(b->cond, depth + 1u);
        walk_markup(b->body.first, depth + 1u);
      }
    } else if (n->kind == MARKUP_FOR) {
      walk_expr(n->as.for_.iter, depth + 1u);
      walk_markup(n->as.for_.body.first, depth + 1u);
      walk_markup(n->as.for_.empty.first, depth + 1u);
    }
  }
}

static void walk_expr(const expr *e, uint32_t depth) {
  if (e == NULL || depth > 64u) {
    return;
  }
  check_span(e->at);
  switch (e->kind) {
  case EXPR_STR: {
    const str_part *part;

    for (part = e->as.str.first; part != NULL; part = part->next) {
      check_span(part->at);
      walk_expr(part->value, depth + 1u);
    }
    break;
  }
  case EXPR_LIST: {
    const expr *item;

    for (item = e->as.list.first; item != NULL; item = item->next) {
      walk_expr(item, depth + 1u);
    }
    break;
  }
  case EXPR_MAP: {
    const map_entry *entry;

    for (entry = e->as.map.first; entry != NULL; entry = entry->next) {
      check_span(entry->at);
      walk_expr(entry->key, depth + 1u);
      walk_expr(entry->value, depth + 1u);
    }
    break;
  }
  case EXPR_STRUCT:
  case EXPR_WITH: {
    const field_init *init;
    const field_init_list *fields =
        e->kind == EXPR_STRUCT ? &e->as.struct_lit.fields : &e->as.with.fields;

    if (e->kind == EXPR_WITH) {
      walk_expr(e->as.with.value, depth + 1u);
    }
    for (init = fields->first; init != NULL; init = init->next) {
      check_span(init->at);
      walk_expr(init->value, depth + 1u);
    }
    break;
  }
  case EXPR_LAMBDA:
    walk_expr(e->as.lambda.body_expr, depth + 1u);
    walk_stmts(&e->as.lambda.body, depth + 1u);
    break;
  case EXPR_MARKUP:
    walk_markup(e->as.markup, depth + 1u);
    break;
  case EXPR_UNARY:
    walk_expr(e->as.unary.operand, depth + 1u);
    break;
  case EXPR_BINARY:
    walk_expr(e->as.binary.lhs, depth + 1u);
    walk_expr(e->as.binary.rhs, depth + 1u);
    break;
  case EXPR_CAST:
    walk_expr(e->as.cast.value, depth + 1u);
    break;
  case EXPR_CALL: {
    const expr *arg;

    walk_expr(e->as.call.callee, depth + 1u);
    for (arg = e->as.call.args.first; arg != NULL; arg = arg->next) {
      walk_expr(arg, depth + 1u);
    }
    break;
  }
  case EXPR_INDEX:
    walk_expr(e->as.index.target, depth + 1u);
    walk_expr(e->as.index.index, depth + 1u);
    break;
  case EXPR_FIELD:
    walk_expr(e->as.field.target, depth + 1u);
    break;
  case EXPR_PROPAGATE:
    walk_expr(e->as.propagate, depth + 1u);
    break;
  case EXPR_COALESCE:
    walk_expr(e->as.coalesce.value, depth + 1u);
    walk_expr(e->as.coalesce.fallback, depth + 1u);
    walk_stmts(&e->as.coalesce.block, depth + 1u);
    break;
  case EXPR_INT:
  case EXPR_FLOAT:
  case EXPR_RAW_STR:
  case EXPR_BOOL:
  case EXPR_NIL:
  case EXPR_IDENT:
  case EXPR_SELF:
  case EXPR_VARIANT:
  case EXPR_KIND_COUNT:
    break;
  }
}

static void walk_stmts(const stmt_list *l, uint32_t depth) {
  const stmt *s;

  if (depth > 64u) {
    return;
  }
  for (s = l->first; s != NULL; s = s->next) {
    check_span(s->at);
    switch (s->kind) {
    case STMT_LET:
      walk_expr(s->as.let.value, depth + 1u);
      break;
    case STMT_ASSIGN:
      walk_expr(s->as.assign.target, depth + 1u);
      walk_expr(s->as.assign.value, depth + 1u);
      break;
    case STMT_IF:
      walk_expr(s->as.if_.cond, depth + 1u);
      walk_stmts(&s->as.if_.then_body, depth + 1u);
      walk_stmts(&s->as.if_.else_body, depth + 1u);
      if (s->as.if_.else_if != NULL) {
        stmt_list one;

        one.first = s->as.if_.else_if;
        one.last = s->as.if_.else_if;
        one.count = 1u;
        /* The chain links through `else_if`, not `next`, so walk it as a
         * single-element list to avoid running off into sibling statements. */
        check_span(s->as.if_.else_if->at);
        walk_expr(s->as.if_.else_if->as.if_.cond, depth + 1u);
        walk_stmts(&s->as.if_.else_if->as.if_.then_body, depth + 1u);
        walk_stmts(&s->as.if_.else_if->as.if_.else_body, depth + 1u);
        (void)one;
      }
      break;
    case STMT_FOR:
      walk_expr(s->as.for_.iter, depth + 1u);
      walk_stmts(&s->as.for_.body, depth + 1u);
      break;
    case STMT_WHILE:
      walk_expr(s->as.while_.cond, depth + 1u);
      walk_stmts(&s->as.while_.body, depth + 1u);
      break;
    case STMT_MATCH: {
      const match_arm *arm;

      walk_expr(s->as.match.value, depth + 1u);
      for (arm = s->as.match.arms.first; arm != NULL; arm = arm->next) {
        check_span(arm->at);
        if (arm->pat != NULL) {
          check_span(arm->pat->at);
        }
        walk_expr(arm->value, depth + 1u);
        walk_stmts(&arm->body, depth + 1u);
      }
      break;
    }
    case STMT_RETURN:
      walk_expr(s->as.ret, depth + 1u);
      break;
    case STMT_SEND:
      walk_expr(s->as.send.name, depth + 1u);
      walk_expr(s->as.send.value, depth + 1u);
      break;
    case STMT_SPAWN:
      walk_expr(s->as.spawn, depth + 1u);
      break;
    case STMT_DEFER:
      walk_expr(s->as.defer, depth + 1u);
      break;
    case STMT_EXPR:
      walk_expr(s->as.expression, depth + 1u);
      break;
    case STMT_BREAK:
    case STMT_CONTINUE:
    case STMT_KIND_COUNT:
      break;
    }
  }
}

static void walk_decls(const decl_list *l, uint32_t depth) {
  const decl *d;

  if (depth > 32u) {
    return;
  }
  for (d = l->first; d != NULL; d = d->next) {
    const attr *a;

    check_span(d->at);
    for (a = d->attrs.first; a != NULL; a = a->next) {
      const expr *arg;

      check_span(a->at);
      for (arg = a->args.first; arg != NULL; arg = arg->next) {
        walk_expr(arg, depth + 1u);
      }
    }
    switch (d->kind) {
    case DECL_FN:
      walk_stmts(&d->as.fn.body, depth + 1u);
      break;
    case DECL_STRUCT: {
      const field *f;

      for (f = d->as.struct_.fields.first; f != NULL; f = f->next) {
        check_span(f->at);
        walk_expr(f->dflt, depth + 1u);
      }
      break;
    }
    case DECL_ENUM: {
      const variant *v;

      for (v = d->as.enum_.variants.first; v != NULL; v = v->next) {
        check_span(v->at);
      }
      break;
    }
    case DECL_LET:
      walk_expr(d->as.let.value, depth + 1u);
      break;
    case DECL_ROUTE:
    case DECL_STREAM:
      walk_stmts(&d->as.route.body, depth + 1u);
      break;
    case DECL_GROUP:
      walk_decls(&d->as.group.items, depth + 1u);
      break;
    case DECL_TEST:
      walk_stmts(&d->as.test.body, depth + 1u);
      break;
    case DECL_ALIAS:
    case DECL_KIND_COUNT:
      break;
    }
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  arena *a = arena_new(8192u);
  diag_sink sink;
  source *s;
  unit_ast *u;
  const lex_comment *c;
  uint32_t last = 0u;

  if (a == NULL) {
    return 0;
  }
  arena_set_limit(a, 64u * 1024u * 1024u);
  diag_sink_init(&sink, a, 0);

  s = source_from_memory(a, SLICE_LIT("fuzz.do"), slice_make((const char *)data, size), &sink);
  if (s == NULL) {
    if (!arena_exhausted(a) && diag_error_count(&sink) == 0) {
      abort(); /* a rejected source must always say why */
    }
    arena_destroy(a);
    return 0;
  }

  g_limit = source_size(s);
  g_bad = 0;

  u = parse_unit(a, s, &sink);
  if (u == NULL) {
    abort(); /* parse_unit always returns a unit; failure is in the sink */
  }

  walk_decls(&u->decls, 0u);
  if (g_bad) {
    abort(); /* a span outside the source would crash the renderer, not the parser */
  }

  /* Comments are recorded in source order, which the formatter depends on. */
  for (c = u->comments.first; c != NULL; c = c->next) {
    if (c->at.start > c->at.end || c->at.end > g_limit || c->at.start < last) {
      abort();
    }
    last = c->at.start;
  }

  /* Whatever was reported must render, both ways, without faulting. */
  {
    buf out;

    buf_init(&out, a, 512u);
    diag_render_human(&sink, &out, false);
    buf_clear(&out);
    diag_render_json(&sink, &out);
  }

  arena_destroy(a);
  return 0;
}
