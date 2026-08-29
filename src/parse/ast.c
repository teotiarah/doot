#include "ast.h"

#include "../base/assert.h"
#include "../base/buf.h"

/* ---- constructors ------------------------------------------------------ */

/* ARENA_NEW zeroes, so every node starts with an empty union, empty child lists,
 * and a NULL `next`; only the tag and the span need setting. A NULL return is
 * impossible because the compilation arena is fatal on exhaustion (D047), which
 * is what keeps the parser free of allocation-failure plumbing (D063). The assert
 * states that expectation rather than handling it. */

expr *ast_expr(arena *a, expr_kind kind, span at) {
  expr *n = ARENA_NEW(a, expr);

  DOOT_ASSERT(n != NULL);
  n->kind = kind;
  n->at = at;
  return n;
}

stmt *ast_stmt(arena *a, stmt_kind kind, span at) {
  stmt *n = ARENA_NEW(a, stmt);

  DOOT_ASSERT(n != NULL);
  n->kind = kind;
  n->at = at;
  return n;
}

decl *ast_decl(arena *a, decl_kind kind, span at) {
  decl *n = ARENA_NEW(a, decl);

  DOOT_ASSERT(n != NULL);
  n->kind = kind;
  n->at = at;
  return n;
}

type_ref *ast_type(arena *a, type_kind kind, span at) {
  type_ref *n = ARENA_NEW(a, type_ref);

  DOOT_ASSERT(n != NULL);
  n->kind = kind;
  n->at = at;
  return n;
}

pattern *ast_pattern(arena *a, pattern_kind kind, span at) {
  pattern *n = ARENA_NEW(a, pattern);

  DOOT_ASSERT(n != NULL);
  n->kind = kind;
  n->at = at;
  return n;
}

markup_node *ast_markup(arena *a, markup_kind kind, span at) {
  markup_node *n = ARENA_NEW(a, markup_node);

  DOOT_ASSERT(n != NULL);
  n->kind = kind;
  n->at = at;
  return n;
}

attr *ast_attr(arena *a, slice name, span at) {
  attr *n = ARENA_NEW(a, attr);

  DOOT_ASSERT(n != NULL);
  n->name = name;
  n->at = at;
  return n;
}

path_seg *ast_path_seg(arena *a, slice name, span at) {
  path_seg *n = ARENA_NEW(a, path_seg);

  DOOT_ASSERT(n != NULL);
  n->name = name;
  n->at = at;
  return n;
}

str_part *ast_str_part(arena *a, span at) {
  str_part *n = ARENA_NEW(a, str_part);

  DOOT_ASSERT(n != NULL);
  n->at = at;
  return n;
}

field_init *ast_field_init(arena *a, slice name, span at) {
  field_init *n = ARENA_NEW(a, field_init);

  DOOT_ASSERT(n != NULL);
  n->name = name;
  n->at = at;
  return n;
}

map_entry *ast_map_entry(arena *a, span at) {
  map_entry *n = ARENA_NEW(a, map_entry);

  DOOT_ASSERT(n != NULL);
  n->at = at;
  return n;
}

param *ast_param(arena *a, span at) {
  param *n = ARENA_NEW(a, param);

  DOOT_ASSERT(n != NULL);
  n->at = at;
  return n;
}

field *ast_field(arena *a, slice name, span at) {
  field *n = ARENA_NEW(a, field);

  DOOT_ASSERT(n != NULL);
  n->name = name;
  n->at = at;
  return n;
}

variant *ast_variant(arena *a, slice name, span at) {
  variant *n = ARENA_NEW(a, variant);

  DOOT_ASSERT(n != NULL);
  n->name = name;
  n->at = at;
  return n;
}

match_arm *ast_match_arm(arena *a, span at) {
  match_arm *n = ARENA_NEW(a, match_arm);

  DOOT_ASSERT(n != NULL);
  n->at = at;
  return n;
}

markup_attr *ast_markup_attr(arena *a, span at) {
  markup_attr *n = ARENA_NEW(a, markup_attr);

  DOOT_ASSERT(n != NULL);
  n->at = at;
  return n;
}

markup_branch *ast_markup_branch(arena *a, span at) {
  markup_branch *n = ARENA_NEW(a, markup_branch);

  DOOT_ASSERT(n != NULL);
  n->at = at;
  return n;
}

/* ---- list append ------------------------------------------------------- */

/* Seventeen lists with identical mechanics. Writing them out by hand would be
 * seventeen chances to get the tail update wrong; the macro has one. */
#define AST_LIST_PUSH(fn, LT, T)                                                                   \
  void fn(LT *l, T *n) {                                                                           \
    DOOT_ASSERT(l != NULL && n != NULL);                                                           \
    if (l->last == NULL) {                                                                         \
      l->first = n;                                                                                \
    } else {                                                                                       \
      l->last->next = n;                                                                           \
    }                                                                                              \
    l->last = n;                                                                                   \
    l->count++;                                                                                    \
  }

AST_LIST_PUSH(expr_list_push, expr_list, expr)
AST_LIST_PUSH(stmt_list_push, stmt_list, stmt)
AST_LIST_PUSH(decl_list_push, decl_list, decl)
AST_LIST_PUSH(type_list_push, type_list, type_ref)
AST_LIST_PUSH(pattern_list_push, pattern_list, pattern)
AST_LIST_PUSH(markup_list_push, markup_list, markup_node)
AST_LIST_PUSH(attr_list_push, attr_list, attr)
AST_LIST_PUSH(path_push, path, path_seg)
AST_LIST_PUSH(str_part_list_push, str_part_list, str_part)
AST_LIST_PUSH(field_init_list_push, field_init_list, field_init)
AST_LIST_PUSH(map_entry_list_push, map_entry_list, map_entry)
AST_LIST_PUSH(param_list_push, param_list, param)
AST_LIST_PUSH(field_list_push, field_list, field)
AST_LIST_PUSH(variant_list_push, variant_list, variant)
AST_LIST_PUSH(match_arm_list_push, match_arm_list, match_arm)
AST_LIST_PUSH(markup_attr_list_push, markup_attr_list, markup_attr)
AST_LIST_PUSH(markup_branch_list_push, markup_branch_list, markup_branch)

/* ---- names ------------------------------------------------------------- */

const char *expr_kind_name(expr_kind kind) {
  switch (kind) {
  case EXPR_INT:
    return "integer literal";
  case EXPR_FLOAT:
    return "float literal";
  case EXPR_STR:
    return "string literal";
  case EXPR_RAW_STR:
    return "raw string literal";
  case EXPR_BOOL:
    return "boolean literal";
  case EXPR_NIL:
    return "nil";
  case EXPR_IDENT:
    return "name";
  case EXPR_SELF:
    return "self";
  case EXPR_VARIANT:
    return "enum variant";
  case EXPR_LIST:
    return "list literal";
  case EXPR_MAP:
    return "map literal";
  case EXPR_STRUCT:
    return "struct literal";
  case EXPR_LAMBDA:
    return "lambda";
  case EXPR_MARKUP:
    return "markup literal";
  case EXPR_UNARY:
    return "unary operation";
  case EXPR_BINARY:
    return "binary operation";
  case EXPR_CAST:
    return "cast";
  case EXPR_CALL:
    return "call";
  case EXPR_INDEX:
    return "index";
  case EXPR_FIELD:
    return "field access";
  case EXPR_PROPAGATE:
    return "error propagation";
  case EXPR_COALESCE:
    return "else";
  case EXPR_WITH:
    return "with";
  case EXPR_KIND_COUNT:
    break;
  }
  DOOT_UNREACHABLE();
}

const char *stmt_kind_name(stmt_kind kind) {
  switch (kind) {
  case STMT_LET:
    return "binding";
  case STMT_ASSIGN:
    return "assignment";
  case STMT_IF:
    return "if";
  case STMT_FOR:
    return "for";
  case STMT_WHILE:
    return "while";
  case STMT_MATCH:
    return "match";
  case STMT_RETURN:
    return "return";
  case STMT_SEND:
    return "send";
  case STMT_SPAWN:
    return "spawn";
  case STMT_DEFER:
    return "defer";
  case STMT_BREAK:
    return "break";
  case STMT_CONTINUE:
    return "continue";
  case STMT_EXPR:
    return "expression statement";
  case STMT_KIND_COUNT:
    break;
  }
  DOOT_UNREACHABLE();
}

const char *decl_kind_name(decl_kind kind) {
  switch (kind) {
  case DECL_FN:
    return "function";
  case DECL_STRUCT:
    return "struct type";
  case DECL_ENUM:
    return "enum type";
  case DECL_ALIAS:
    return "type alias";
  case DECL_LET:
    return "binding";
  case DECL_ROUTE:
    return "route";
  case DECL_STREAM:
    return "stream";
  case DECL_GROUP:
    return "group";
  case DECL_TEST:
    return "test";
  case DECL_KIND_COUNT:
    break;
  }
  DOOT_UNREACHABLE();
}

slice path_text(arena *a, const path *p) {
  const path_seg *s;
  buf out;

  DOOT_ASSERT(a != NULL && p != NULL);
  buf_init(&out, a, 32u);
  for (s = p->first; s != NULL; s = s->next) {
    if (s != p->first) {
      (void)buf_append_byte(&out, '.');
    }
    (void)buf_append_slice(&out, s->name);
  }
  return buf_slice(&out);
}
