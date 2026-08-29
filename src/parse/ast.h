/* ast.h -- the syntax tree (D063).
 *
 * Seven node families: decl, stmt, expr, type_ref, pattern, markup_node, attr.
 * Separate families rather than one universal node, so a function taking an
 * `expr *` cannot be handed a statement and each family's switch is exhaustive
 * over only what it can contain.
 *
 * Every node carries a `kind` tag, a `span at`, a union of per-kind payloads, and
 * a `next` pointer. Children are intrusive singly-linked lists held as
 * { first, last, count } -- the shape the base layer already uses for diag and
 * diag_label. That follows from the allocator: arena_extend grows only the
 * arena's most recent allocation, so a contiguous array of children cannot be
 * built while those children are themselves being allocated.
 *
 * Ownership: every node lives in the compilation arena and is released with it.
 * Nothing here is freed individually and nothing outlives the arena. Slices point
 * either into the source text or into arena-decoded bytes, so they share that
 * lifetime.
 *
 * Failure: constructors return a node and never an error, because the compilation
 * arena is built with arena_new_fatal and aborts on exhaustion (D047). This
 * differs from the base layer, which checks every allocation, and the difference
 * is deliberate: base is shared with the runtime, where a request arena returns
 * NULL so the VM can raise budget_exceeded.
 */
#ifndef DOOT_AST_H
#define DOOT_AST_H

#include <stdbool.h>

#include "../base/arena.h"
#include "../base/plat.h"
#include "../base/slice.h"
#include "../base/source.h"
#include "../lex/lex.h"

typedef struct expr expr;
typedef struct stmt stmt;
typedef struct decl decl;
typedef struct type_ref type_ref;
typedef struct pattern pattern;
typedef struct markup_node markup_node;
typedef struct attr attr;

typedef struct {
  expr *first;
  expr *last;
  uint32_t count;
} expr_list;

typedef struct {
  stmt *first;
  stmt *last;
  uint32_t count;
} stmt_list;

typedef struct {
  decl *first;
  decl *last;
  uint32_t count;
} decl_list;

typedef struct {
  type_ref *first;
  type_ref *last;
  uint32_t count;
} type_list;

typedef struct {
  pattern *first;
  pattern *last;
  uint32_t count;
} pattern_list;

typedef struct {
  markup_node *first;
  markup_node *last;
  uint32_t count;
} markup_list;

/* ---- paths ------------------------------------------------------------- */

/* A dotted name: `time.Time`, `models.user.find`. There are no imports (D030),
 * so every non-local name is a fully qualified path and the parser never has to
 * resolve an alias. */
typedef struct path_seg path_seg;

struct path_seg {
  slice name;
  span at;
  path_seg *next;
};

typedef struct {
  path_seg *first;
  path_seg *last;
  uint32_t count;
} path;

/* ---- attributes -------------------------------------------------------- */

/* A closed set of twelve (D043). The parser validates the name against that set
 * and reports DT0045 for anything else, because there are no user-defined
 * attributes to leave open. */
struct attr {
  slice name; /* without the leading @ */
  expr_list args;
  span at;
  attr *next;
};

typedef struct {
  attr *first;
  attr *last;
  uint32_t count;
} attr_list;

/* ---- types ------------------------------------------------------------- */

typedef enum {
  TYPE_PATH, /* int, User, time.Time, db.all[User]'s User */
  TYPE_LIST, /* [T] */
  TYPE_MAP,  /* {K: V} */
  TYPE_FN,   /* fn(A, B) -> C */
  TYPE_KIND_COUNT
} type_kind;

struct type_ref {
  type_kind kind;
  span at;
  bool optional; /* T? -- the only type that can hold nil (D017) */
  union {
    struct {
      path segs;
      type_list args; /* [T] applied to a stdlib slot; user code has none (D019) */
    } p;
    struct {
      type_ref *elem;
    } list;
    struct {
      type_ref *key;
      type_ref *val;
    } map;
    struct {
      type_list params;
      type_ref *ret; /* NULL when the signature returns nothing */
      bool fallible;
    } fn;
  } as;
  type_ref *next;
};

/* ---- patterns ---------------------------------------------------------- */

typedef enum {
  PAT_VARIANT, /* .active */
  PAT_INT,
  PAT_STR,
  PAT_BOOL,
  PAT_ALT, /* a | b | c */
  PAT_KIND_COUNT
} pattern_kind;

struct pattern {
  pattern_kind kind;
  span at;
  union {
    slice variant;
    int64_t int_value;
    slice str_value; /* decoded */
    bool bool_value;
    pattern_list alts;
  } as;
  pattern *next;
};

/* ---- expressions ------------------------------------------------------- */

typedef enum {
  UNOP_NEG, /* -x */
  UNOP_NOT, /* not x */
  UNOP_KIND_COUNT
} unop;

typedef enum {
  BINOP_ADD,
  BINOP_SUB,
  BINOP_MUL,
  BINOP_DIV,
  BINOP_MOD,
  BINOP_EQ,
  BINOP_NE,
  BINOP_LT,
  BINOP_LE,
  BINOP_GT,
  BINOP_GE,
  BINOP_IN,
  BINOP_AND,
  BINOP_OR,
  BINOP_KIND_COUNT
} binop;

typedef enum {
  EXPR_INT,
  EXPR_FLOAT,
  EXPR_STR,
  EXPR_RAW_STR,
  EXPR_BOOL,
  EXPR_NIL,
  EXPR_IDENT,
  EXPR_SELF,
  EXPR_VARIANT, /* .name, with the type inferred from context */
  EXPR_LIST,
  EXPR_MAP,
  EXPR_STRUCT,
  EXPR_LAMBDA,
  EXPR_MARKUP,
  EXPR_UNARY,
  EXPR_BINARY,
  EXPR_CAST, /* x as int */
  EXPR_CALL,
  EXPR_INDEX,     /* xs[i] */
  EXPR_FIELD,     /* x.name */
  EXPR_PROPAGATE, /* x! */
  EXPR_COALESCE,  /* x else y */
  EXPR_WITH,      /* x with { f: v } */
  EXPR_KIND_COUNT
} expr_kind;

/* One piece of a string literal. A literal is a sequence of parts because
 * interpolation makes it structurally a tree (D058); `value == NULL` marks a run
 * of literal text, whose escapes are already resolved. */
typedef struct str_part str_part;

struct str_part {
  slice text;
  expr *value;
  span at;
  str_part *next;
};

typedef struct {
  str_part *first;
  str_part *last;
  uint32_t count;
} str_part_list;

typedef struct field_init field_init;

struct field_init {
  slice name;
  expr *value;
  span at;
  field_init *next;
};

typedef struct {
  field_init *first;
  field_init *last;
  uint32_t count;
} field_init_list;

typedef struct map_entry map_entry;

struct map_entry {
  expr *key;
  expr *value;
  span at;
  map_entry *next;
};

typedef struct {
  map_entry *first;
  map_entry *last;
  uint32_t count;
} map_entry_list;

typedef struct param param;

struct param {
  slice name;
  type_ref *type;  /* NULL for `self` */
  expr *dflt;      /* default value, or NULL */
  attr_list attrs; /* validation attributes on a bound struct field */
  bool is_self;
  span at;
  param *next;
};

typedef struct {
  param *first;
  param *last;
  uint32_t count;
} param_list;

/* `else` has two shapes, and they are not interchangeable: the expression form
 * supplies a replacement value, the block form must diverge on every path
 * (grammar rule 16). Keeping them apart here is what lets the resolver check
 * that without re-deriving it from the tree. */
typedef enum {
  COALESCE_VALUE, /* else <expr> */
  COALESCE_BLOCK  /* else { ... } or else err { ... } */
} coalesce_form;

struct expr {
  expr_kind kind;
  span at;
  union {
    int64_t int_value;
    double float_value;
    bool bool_value;
    str_part_list str;
    slice raw_str; /* the bytes between the backticks, verbatim */
    path ident;
    slice variant;
    expr_list list;
    map_entry_list map;
    struct {
      path type_name;
      field_init_list fields;
    } struct_lit;
    struct {
      param_list params;
      type_ref *ret;
      bool fallible;
      expr *body_expr; /* => expr form */
      stmt_list body;  /* block form */
      bool has_block;
    } lambda;
    markup_node *markup;
    struct {
      unop op;
      expr *operand;
    } unary;
    struct {
      binop op;
      expr *lhs;
      expr *rhs;
    } binary;
    struct {
      expr *value;
      type_ref *type;
    } cast;
    struct {
      expr *callee;
      type_list type_args;
      expr_list args;
    } call;
    struct {
      expr *target;
      expr *index;
    } index;
    struct {
      expr *target;
      slice name;
    } field;
    expr *propagate;
    struct {
      expr *value;
      coalesce_form form;
      slice err_name; /* `else err { ... }`; empty when unbound */
      bool binds_err;
      expr *fallback; /* COALESCE_VALUE */
      stmt_list block;
    } coalesce;
    struct {
      expr *value;
      field_init_list fields;
    } with;
  } as;
  expr *next;
};

/* ---- markup ------------------------------------------------------------ */

typedef enum {
  MARKUP_ELEMENT,
  MARKUP_TEXT,
  MARKUP_INTERP,
  MARKUP_COMMENT,
  MARKUP_IF,
  MARKUP_FOR,
  MARKUP_KIND_COUNT
} markup_kind;

typedef struct markup_attr markup_attr;

struct markup_attr {
  slice name;
  bool is_spread; /* ...expr */
  expr *value;    /* NULL for a bare boolean attribute, or the spread source */
  span at;
  markup_attr *next;
};

typedef struct {
  markup_attr *first;
  markup_attr *last;
  uint32_t count;
} markup_attr_list;

/* One arm of a `{if}` chain. A NULL `cond` is the `{else}`. */
typedef struct markup_branch markup_branch;

struct markup_branch {
  expr *cond;
  markup_list body;
  span at;
  markup_branch *next;
};

typedef struct {
  markup_branch *first;
  markup_branch *last;
  uint32_t count;
} markup_branch_list;

struct markup_node {
  markup_kind kind;
  span at;
  union {
    struct {
      slice tag;
      markup_attr_list attrs;
      markup_list children;
      bool self_closing;
    } element;
    slice text; /* verbatim source bytes; escaping happens at compile time */
    expr *interp;
    struct {
      markup_branch_list branches;
    } if_;
    struct {
      slice first_name;
      slice second_name;
      bool has_second;
      expr *iter;
      markup_list body;
      markup_list empty; /* the {else} arm of a {for} */
      bool has_empty;
    } for_;
  } as;
  markup_node *next;
};

/* ---- statements -------------------------------------------------------- */

typedef enum {
  STMT_LET,
  STMT_ASSIGN,
  STMT_IF,
  STMT_FOR,
  STMT_WHILE,
  STMT_MATCH,
  STMT_RETURN,
  STMT_SEND,
  STMT_SPAWN,
  STMT_DEFER,
  STMT_BREAK,
  STMT_CONTINUE,
  STMT_EXPR,
  STMT_KIND_COUNT
} stmt_kind;

typedef enum {
  ASSIGN_SET,
  ASSIGN_ADD,
  ASSIGN_SUB,
  ASSIGN_MUL,
  ASSIGN_DIV,
  ASSIGN_MOD,
  ASSIGN_KIND_COUNT
} assign_op;

typedef struct match_arm match_arm;

struct match_arm {
  pattern *pat; /* NULL when this is the `else` arm */
  expr *value;  /* -> expr form */
  stmt_list body;
  bool has_block;
  span at;
  match_arm *next;
};

typedef struct {
  match_arm *first;
  match_arm *last;
  uint32_t count;
} match_arm_list;

struct stmt {
  stmt_kind kind;
  span at;
  union {
    struct {
      bool is_var;
      slice name;
      type_ref *type; /* NULL when inferred */
      expr *value;
    } let;
    struct {
      assign_op op;
      expr *target;
      expr *value;
    } assign;
    struct {
      expr *cond;
      stmt_list then_body;
      stmt_list else_body;
      stmt *else_if; /* `else if` chains as a nested STMT_IF */
      bool has_else;
    } if_;
    struct {
      slice first_name;
      slice second_name;
      bool has_second;
      expr *iter;
      stmt_list body;
    } for_;
    struct {
      expr *cond;
      stmt_list body;
    } while_;
    struct {
      expr *value;
      match_arm_list arms;
    } match;
    expr *ret; /* may be NULL: `return` with no value */
    struct {
      expr *name; /* `send "name", value`; NULL for the one-argument form */
      expr *value;
    } send;
    expr *spawn;
    expr *defer;
    expr *expression;
  } as;
  stmt *next;
};

/* ---- declarations ------------------------------------------------------ */

typedef enum {
  DECL_FN,
  DECL_STRUCT,
  DECL_ENUM,
  DECL_ALIAS,
  DECL_LET,
  DECL_ROUTE,
  DECL_STREAM,
  DECL_GROUP,
  DECL_TEST,
  DECL_KIND_COUNT
} decl_kind;

typedef struct field field;

struct field {
  slice name;
  type_ref *type;
  expr *dflt;
  attr_list attrs;
  span at;
  field *next;
};

typedef struct {
  field *first;
  field *last;
  uint32_t count;
} field_list;

typedef struct variant variant;

struct variant {
  slice name;
  span at;
  variant *next;
};

typedef struct {
  variant *first;
  variant *last;
  uint32_t count;
} variant_list;

struct decl {
  decl_kind kind;
  span at;
  bool is_pub;
  attr_list attrs;
  union {
    struct {
      slice recv; /* `fn User.display` -> "User" */
      bool has_recv;
      slice name;
      param_list params;
      type_ref *ret;
      bool fallible;
      stmt_list body;
    } fn;
    struct {
      slice name;
      field_list fields;
    } struct_;
    struct {
      slice name;
      variant_list variants;
    } enum_;
    struct {
      slice name;
      type_ref *target;
    } alias;
    struct {
      bool is_var; /* always false at top level: DT0033 (grammar rule 1) */
      slice name;
      type_ref *type;
      expr *value;
    } let;
    /* DECL_ROUTE and DECL_STREAM share this shape. */
    struct {
      slice method; /* GET, POST, ... matched by text (D061) */
      span method_at;
      slice pattern; /* the decoded path pattern */
      span pattern_at;
      param_list params;
      type_ref *ret;
      bool fallible;
      stmt_list body;
    } route;
    struct {
      slice prefix;
      decl_list items;
    } group;
    struct {
      slice name;
      stmt_list body;
    } test;
  } as;
  decl *next;
};

/* ---- the compilation unit ---------------------------------------------- */

typedef struct {
  const source *src;
  decl_list decls;
  lex_comments comments; /* in source order, for `doot fmt` (D067) */
} unit_ast;

/* ---- constructors ------------------------------------------------------ */

/* None of these can fail; see the failure note at the top of this header. */

expr *ast_expr(arena *a, expr_kind kind, span at);
stmt *ast_stmt(arena *a, stmt_kind kind, span at);
decl *ast_decl(arena *a, decl_kind kind, span at);
type_ref *ast_type(arena *a, type_kind kind, span at);
pattern *ast_pattern(arena *a, pattern_kind kind, span at);
markup_node *ast_markup(arena *a, markup_kind kind, span at);
attr *ast_attr(arena *a, slice name, span at);

path_seg *ast_path_seg(arena *a, slice name, span at);
str_part *ast_str_part(arena *a, span at);
field_init *ast_field_init(arena *a, slice name, span at);
map_entry *ast_map_entry(arena *a, span at);
param *ast_param(arena *a, span at);
field *ast_field(arena *a, slice name, span at);
variant *ast_variant(arena *a, slice name, span at);
match_arm *ast_match_arm(arena *a, span at);
markup_attr *ast_markup_attr(arena *a, span at);
markup_branch *ast_markup_branch(arena *a, span at);

/* ---- list append ------------------------------------------------------- */

void expr_list_push(expr_list *l, expr *n);
void stmt_list_push(stmt_list *l, stmt *n);
void decl_list_push(decl_list *l, decl *n);
void type_list_push(type_list *l, type_ref *n);
void pattern_list_push(pattern_list *l, pattern *n);
void markup_list_push(markup_list *l, markup_node *n);
void attr_list_push(attr_list *l, attr *n);
void path_push(path *l, path_seg *n);
void str_part_list_push(str_part_list *l, str_part *n);
void field_init_list_push(field_init_list *l, field_init *n);
void map_entry_list_push(map_entry_list *l, map_entry *n);
void param_list_push(param_list *l, param *n);
void field_list_push(field_list *l, field *n);
void variant_list_push(variant_list *l, variant *n);
void match_arm_list_push(match_arm_list *l, match_arm *n);
void markup_attr_list_push(markup_attr_list *l, markup_attr *n);
void markup_branch_list_push(markup_branch_list *l, markup_branch *n);

/* ---- names ------------------------------------------------------------- */

/* Stable spellings for diagnostics and test output. */
const char *expr_kind_name(expr_kind kind);
const char *stmt_kind_name(stmt_kind kind);
const char *decl_kind_name(decl_kind kind);

/* The dotted spelling of a path, allocated in the arena: `models.user.find`. */
slice path_text(arena *a, const path *p);

#endif /* DOOT_AST_H */
