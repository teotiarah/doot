#include "../../src/lex/lex.h"

#include <string.h>

#include "../../src/base/diag.h"
#include "../../src/lex/token.h"
#include "unit.h"

/* ---- fixture ----------------------------------------------------------- */

typedef struct {
  arena *a;
  diag_sink sink;
  source *src;
  lexer *lx;
  lex_comments comments;
} fixture;

static void fix_init(fixture *f, const char *text) {
  memset(f, 0, sizeof(*f));
  f->a = arena_new(1u << 16);
  diag_sink_init(&f->sink, f->a, 0u);
  f->src = source_from_memory(f->a, SLICE_LIT("t.do"), slice_from_cstr(text), &f->sink);
  f->lx = lex_new(f->a, f->src, &f->sink, &f->comments);
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

/* Drains the stream and compares kinds against a NULL-terminated expectation.
 * Reports the first divergence with both spellings, which is what makes a
 * failure readable without a debugger. */
static void expect_kinds(unit *t, const char *text, const token_kind *want, size_t want_n) {
  fixture f;
  size_t i;

  fix_init(&f, text);
  for (i = 0; i < want_n; i++) {
    token got = lex_next(f.lx);
    t->checks++;
    if (got.kind != want[i]) {
      unit_failf(t, __FILE__, __LINE__, "%s:\n    at token %lu expected %s, got %s", text,
                 (unsigned long)i, token_kind_name(want[i]), token_kind_name(got.kind));
      break;
    }
  }
  fix_free(&f);
}

#define EXPECT_KINDS(t, text, ...)                                                                 \
  do {                                                                                             \
    static const token_kind want_[] = {__VA_ARGS__};                                               \
    expect_kinds((t), (text), want_, sizeof(want_) / sizeof(want_[0]));                            \
  } while (0)

/* ---- words ------------------------------------------------------------- */

static void keywords_are_recognized(unit *t) {
  EXPECT_KINDS(t, "let x = true", TOK_KW_LET, TOK_IDENT, TOK_EQ, TOK_KW_TRUE, TOK_EOF);
  EXPECT_KINDS(t, "fn f() {}", TOK_KW_FN, TOK_IDENT, TOK_LPAREN, TOK_RPAREN, TOK_LBRACE, TOK_RBRACE,
               TOK_EOF);
  EXPECT_KINDS(t, "not a and b or c", TOK_KW_NOT, TOK_IDENT, TOK_KW_AND, TOK_IDENT, TOK_KW_OR,
               TOK_IDENT, TOK_EOF);
}

static void a_word_that_only_starts_with_a_keyword_is_an_identifier(unit *t) {
  EXPECT_KINDS(t, "letter", TOK_IDENT, TOK_EOF);
  EXPECT_KINDS(t, "iffy", TOK_IDENT, TOK_EOF);
  EXPECT_KINDS(t, "self_id", TOK_IDENT, TOK_EOF);
}

static void reserved_words_lex_as_reserved_not_identifiers(unit *t) {
  EXPECT_KINDS(t, "import", TOK_RESERVED, TOK_EOF);
  EXPECT_KINDS(t, "async await class", TOK_RESERVED, TOK_RESERVED, TOK_RESERVED, TOK_EOF);
  /* Reserved is a whole-word match, so a longer word containing one is fine. */
  EXPECT_KINDS(t, "important", TOK_IDENT, TOK_EOF);
}

static void every_reserved_word_has_help_naming_the_doot_construct(unit *t) {
  UNIT_NOT_NULL(t, token_reserved_help(SLICE_LIT("import")));
  UNIT_NOT_NULL(t, token_reserved_help(SLICE_LIT("await")));
  UNIT_NOT_NULL(t, token_reserved_help(SLICE_LIT("switch")));
  UNIT_NULL(t, token_reserved_help(SLICE_LIT("let")));
  UNIT_NULL(t, token_reserved_help(SLICE_LIT("nonsense")));
}

static void http_methods_and_end_stay_identifiers(unit *t) {
  /* D061: contextual words are matched by text in the parser, so the lexer must
   * hand them over as plain identifiers. */
  EXPECT_KINDS(t, "route GET \"/\"", TOK_KW_ROUTE, TOK_IDENT, TOK_STR_START, TOK_STR_TEXT,
               TOK_STR_END, TOK_EOF);
  EXPECT_KINDS(t, "end", TOK_IDENT, TOK_EOF);
}

/* ---- operators --------------------------------------------------------- */

static void operators_take_the_longest_match(unit *t) {
  EXPECT_KINDS(t, "-> => == != <= >= += -= *= /= %=", TOK_ARROW, TOK_FAT_ARROW, TOK_EQ_EQ,
               TOK_BANG_EQ, TOK_LE, TOK_GE, TOK_PLUS_EQ, TOK_MINUS_EQ, TOK_STAR_EQ, TOK_SLASH_EQ,
               TOK_PERCENT_EQ, TOK_EOF);
  EXPECT_KINDS(t, "- = ! < > + * / %", TOK_MINUS, TOK_EQ, TOK_BANG, TOK_LT, TOK_GT, TOK_PLUS,
               TOK_STAR, TOK_SLASH, TOK_PERCENT, TOK_EOF);
}

static void postfix_bang_is_distinct_from_not_equal(unit *t) {
  EXPECT_KINDS(t, "create(n)!", TOK_IDENT, TOK_LPAREN, TOK_IDENT, TOK_RPAREN, TOK_BANG, TOK_EOF);
  EXPECT_KINDS(t, "a != b", TOK_IDENT, TOK_BANG_EQ, TOK_IDENT, TOK_EOF);
}

/* ---- numbers ----------------------------------------------------------- */

static void integer_and_float_forms(unit *t) {
  EXPECT_KINDS(t, "42", TOK_INT, TOK_EOF);
  EXPECT_KINDS(t, "0xff", TOK_INT, TOK_EOF);
  EXPECT_KINDS(t, "0b1011", TOK_INT, TOK_EOF);
  EXPECT_KINDS(t, "1_000_000", TOK_INT, TOK_EOF);
  EXPECT_KINDS(t, "3.14", TOK_FLOAT, TOK_EOF);
  EXPECT_KINDS(t, "1e9", TOK_FLOAT, TOK_EOF);
  EXPECT_KINDS(t, "1.5e-3", TOK_FLOAT, TOK_EOF);
}

static void a_dot_after_a_number_is_a_method_call_unless_a_digit_follows(unit *t) {
  /* 16.mb is an int with a method call, not a malformed float. That distinction
   * is why the lexer requires a digit after the dot. */
  fixture f;
  token a;
  token b;

  EXPECT_KINDS(t, "16.mb", TOK_INT, TOK_DOT, TOK_IDENT, TOK_EOF);
  EXPECT_KINDS(t, "15.s", TOK_INT, TOK_DOT, TOK_IDENT, TOK_EOF);

  fix_init(&f, "16.mb");
  a = lex_next(f.lx);
  UNIT_EQ_SLICE(t, lex_text(f.lx, a), "16");
  b = lex_next(f.lx);
  UNIT_EQ_INT(t, b.kind, TOK_DOT);
  UNIT_FALSE(t, diag_has_errors(&f.sink));
  fix_free(&f);
}

static void malformed_numbers_are_reported(unit *t) {
  fixture f;

  fix_init(&f, "0x");
  (void)lex_next(f.lx);
  UNIT_TRUE(t, has_code(&f.sink, DIAG_MALFORMED_NUMBER));
  fix_free(&f);

  fix_init(&f, "0b");
  (void)lex_next(f.lx);
  UNIT_TRUE(t, has_code(&f.sink, DIAG_MALFORMED_NUMBER));
  fix_free(&f);

  fix_init(&f, "1e");
  (void)lex_next(f.lx);
  UNIT_TRUE(t, has_code(&f.sink, DIAG_MALFORMED_NUMBER));
  fix_free(&f);

  /* An uppercase radix prefix would otherwise lex as `0` and an identifier. */
  fix_init(&f, "0XFF");
  (void)lex_next(f.lx);
  UNIT_TRUE(t, has_code(&f.sink, DIAG_MALFORMED_NUMBER));
  fix_free(&f);
}

static void an_underscore_must_separate_two_digits(unit *t) {
  fixture f;

  fix_init(&f, "1_");
  (void)lex_next(f.lx);
  UNIT_TRUE(t, has_code(&f.sink, DIAG_MISPLACED_UNDERSCORE));
  fix_free(&f);

  fix_init(&f, "1__0");
  (void)lex_next(f.lx);
  UNIT_TRUE(t, has_code(&f.sink, DIAG_MISPLACED_UNDERSCORE));
  fix_free(&f);

  fix_init(&f, "0x_ff");
  (void)lex_next(f.lx);
  UNIT_TRUE(t, has_code(&f.sink, DIAG_MISPLACED_UNDERSCORE));
  fix_free(&f);

  fix_init(&f, "1_0");
  (void)lex_next(f.lx);
  UNIT_FALSE(t, diag_has_errors(&f.sink));
  fix_free(&f);
}

/* ---- strings ----------------------------------------------------------- */

static void a_plain_string_is_a_three_token_sequence(unit *t) {
  EXPECT_KINDS(t, "\"hi\"", TOK_STR_START, TOK_STR_TEXT, TOK_STR_END, TOK_EOF);
  /* No empty text token for an empty literal. */
  EXPECT_KINDS(t, "\"\"", TOK_STR_START, TOK_STR_END, TOK_EOF);
}

static void interpolation_becomes_nested_tokens(unit *t) {
  EXPECT_KINDS(t, "\"a${x}b\"", TOK_STR_START, TOK_STR_TEXT, TOK_INTERP_START, TOK_IDENT,
               TOK_INTERP_END, TOK_STR_TEXT, TOK_STR_END, TOK_EOF);
  EXPECT_KINDS(t, "\"${x}\"", TOK_STR_START, TOK_INTERP_START, TOK_IDENT, TOK_INTERP_END,
               TOK_STR_END, TOK_EOF);
}

static void braces_inside_an_interpolation_nest(unit *t) {
  /* The interpolation closes at the last brace, not at the map literal's. */
  EXPECT_KINDS(t, "\"${f({})}\"", TOK_STR_START, TOK_INTERP_START, TOK_IDENT, TOK_LPAREN,
               TOK_LBRACE, TOK_RBRACE, TOK_RPAREN, TOK_INTERP_END, TOK_STR_END, TOK_EOF);
}

static void escapes_are_left_for_the_parser(unit *t) {
  fixture f;
  token start;
  token text;

  fix_init(&f, "\"a\\\"b\"");
  start = lex_next(f.lx);
  UNIT_EQ_INT(t, start.kind, TOK_STR_START);
  text = lex_next(f.lx);
  UNIT_EQ_INT(t, text.kind, TOK_STR_TEXT);
  /* Raw bytes, escape unresolved: an escaped quote does not close the string. */
  UNIT_EQ_SLICE(t, lex_text(f.lx, text), "a\\\"b");
  UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_STR_END);
  UNIT_FALSE(t, diag_has_errors(&f.sink));
  fix_free(&f);
}

static void an_unterminated_string_is_reported_at_its_opening_quote(unit *t) {
  fixture f;
  const diag *d;

  fix_init(&f, "let s = \"abc\nlet n = 1");
  while (lex_next(f.lx).kind != TOK_EOF) {
    /* drain */
  }
  UNIT_TRUE(t, has_code(&f.sink, DIAG_UNTERMINATED_STRING));
  for (d = f.sink.first; d != NULL; d = d->next) {
    if (d->code == DIAG_UNTERMINATED_STRING) {
      /* The quote, not the end of the line. */
      UNIT_EQ_INT(t, d->at.start, 8);
      break;
    }
  }
  fix_free(&f);
}

static void raw_strings_span_lines_and_keep_escapes(unit *t) {
  fixture f;
  token tok;

  fix_init(&f, "`select *\nfrom t\\n`");
  tok = lex_next(f.lx);
  UNIT_EQ_INT(t, tok.kind, TOK_RAW_STR);
  UNIT_EQ_SLICE(t, lex_text(f.lx, tok), "`select *\nfrom t\\n`");
  UNIT_FALSE(t, diag_has_errors(&f.sink));
  fix_free(&f);

  fix_init(&f, "`unclosed");
  (void)lex_next(f.lx);
  UNIT_TRUE(t, has_code(&f.sink, DIAG_UNTERMINATED_RAW_STRING));
  fix_free(&f);
}

static void an_unterminated_interpolation_is_reported(unit *t) {
  fixture f;

  fix_init(&f, "\"${x");
  while (lex_next(f.lx).kind != TOK_EOF) {
    /* drain */
  }
  UNIT_TRUE(t, has_code(&f.sink, DIAG_UNTERMINATED_INTERP));
  fix_free(&f);
}

/* ---- comments ---------------------------------------------------------- */

static void comments_are_recorded_not_emitted(unit *t) {
  fixture f;

  fix_init(&f, "// a\nlet x = 1 // b\n/* c */\n");
  while (lex_next(f.lx).kind != TOK_EOF) {
    /* drain */
  }
  /* Never in the token stream, always in the list, in source order. */
  UNIT_EQ_INT(t, f.comments.count, 3);
  UNIT_NOT_NULL(t, f.comments.first);
  UNIT_FALSE(t, f.comments.first->block);
  UNIT_TRUE(t, f.comments.last->block);
  UNIT_FALSE(t, diag_has_errors(&f.sink));
  fix_free(&f);
}

static void a_null_comment_list_discards_them(unit *t) {
  arena *a = arena_new(1u << 16);
  diag_sink sink;
  source *src;
  lexer *lx;

  diag_sink_init(&sink, a, 0u);
  src = source_from_memory(a, SLICE_LIT("t.do"), SLICE_LIT("// x\nlet y = 1"), &sink);
  lx = lex_new(a, src, &sink, NULL);
  UNIT_EQ_INT(t, lex_next(lx).kind, TOK_KW_LET);
  UNIT_FALSE(t, diag_has_errors(&sink));
  arena_destroy(a);
}

static void block_comments_nest(unit *t) {
  fixture f;

  fix_init(&f, "/* a /* b */ c */ let x = 1");
  UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_KW_LET);
  UNIT_FALSE(t, diag_has_errors(&f.sink));
  fix_free(&f);

  /* An inner opener consumes one closer, so this one is still open. */
  fix_init(&f, "/* a /* b */");
  UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_EOF);
  UNIT_TRUE(t, has_code(&f.sink, DIAG_UNTERMINATED_BLOCK_COMMENT));
  fix_free(&f);
}

/* ---- line structure ---------------------------------------------------- */

static void a_newline_terminates_a_statement(unit *t) {
  EXPECT_KINDS(t, "a\nb", TOK_IDENT, TOK_NEWLINE, TOK_IDENT, TOK_EOF);
}

static void a_run_of_newlines_collapses_to_one(unit *t) {
  EXPECT_KINDS(t, "a\n\n\n\nb", TOK_IDENT, TOK_NEWLINE, TOK_IDENT, TOK_EOF);
}

static void leading_and_trailing_blank_lines_produce_no_newline(unit *t) {
  EXPECT_KINDS(t, "\n\n\na", TOK_IDENT, TOK_EOF);
  EXPECT_KINDS(t, "a\n\n\n", TOK_IDENT, TOK_EOF);
  EXPECT_KINDS(t, "", TOK_EOF);
  EXPECT_KINDS(t, "\n\n", TOK_EOF);
}

static void a_continuation_token_suppresses_the_newline(unit *t) {
  EXPECT_KINDS(t, "a +\nb", TOK_IDENT, TOK_PLUS, TOK_IDENT, TOK_EOF);
  EXPECT_KINDS(t, "f(\na)", TOK_IDENT, TOK_LPAREN, TOK_IDENT, TOK_RPAREN, TOK_EOF);
  EXPECT_KINDS(t, "a =\nb", TOK_IDENT, TOK_EQ, TOK_IDENT, TOK_EOF);
  EXPECT_KINDS(t, "a and\nb", TOK_IDENT, TOK_KW_AND, TOK_IDENT, TOK_EOF);
}

static void the_compound_assignments_continue_like_plain_assignment(unit *t) {
  /* D060: `=` was in the set and `+=` was not, which made these disagree. */
  EXPECT_KINDS(t, "a +=\n1", TOK_IDENT, TOK_PLUS_EQ, TOK_INT, TOK_EOF);
  EXPECT_KINDS(t, "a -=\n1", TOK_IDENT, TOK_MINUS_EQ, TOK_INT, TOK_EOF);
  EXPECT_KINDS(t, "a *=\n1", TOK_IDENT, TOK_STAR_EQ, TOK_INT, TOK_EOF);
  EXPECT_KINDS(t, "a /=\n1", TOK_IDENT, TOK_SLASH_EQ, TOK_INT, TOK_EOF);
  EXPECT_KINDS(t, "a %=\n1", TOK_IDENT, TOK_PERCENT_EQ, TOK_INT, TOK_EOF);
}

static void a_pattern_alternation_bar_continues(unit *t) {
  EXPECT_KINDS(t, "a |\nb", TOK_IDENT, TOK_PIPE, TOK_IDENT, TOK_EOF);
}

static void postfix_bang_does_not_suppress_the_newline(unit *t) {
  /* The defect D060 corrects. With `!` in the continuation set this swallowed
   * its own terminator and joined the next line to the statement. */
  EXPECT_KINDS(t, "create(n)!\nlet x = 1", TOK_IDENT, TOK_LPAREN, TOK_IDENT, TOK_RPAREN, TOK_BANG,
               TOK_NEWLINE, TOK_KW_LET, TOK_IDENT, TOK_EQ, TOK_INT, TOK_EOF);
}

static void a_follow_token_suppresses_the_newline(unit *t) {
  EXPECT_KINDS(t, "f(a\n)", TOK_IDENT, TOK_LPAREN, TOK_IDENT, TOK_RPAREN, TOK_EOF);
  EXPECT_KINDS(t, "[a\n]", TOK_LBRACKET, TOK_IDENT, TOK_RBRACKET, TOK_EOF);
  EXPECT_KINDS(t, "{a\n}", TOK_LBRACE, TOK_IDENT, TOK_RBRACE, TOK_EOF);
}

static void a_follow_token_cannot_begin_a_construct(unit *t) {
  /* The rule that settles membership of the follow set. Suppressing the newline
   * before a token costs the parser the ability to tell "continues the previous
   * line" from "starts a new one", so only a token that can never begin anything
   * may be in it. `.` and `else` can, and both were in it originally: `.` begins a
   * match pattern, `else` begins a match arm. */
  EXPECT_KINDS(t, "f()\nelse", TOK_IDENT, TOK_LPAREN, TOK_RPAREN, TOK_NEWLINE, TOK_KW_ELSE,
               TOK_EOF);
  /* On one line, which is doot style, there is no newline to suppress anyway. */
  EXPECT_KINDS(t, "} else {", TOK_RBRACE, TOK_KW_ELSE, TOK_LBRACE, TOK_EOF);
}

static void a_match_arms_else_keeps_its_own_line(unit *t) {
  EXPECT_KINDS(t, "match a {\n  1 -> two()\n  else -> other()\n}", TOK_KW_MATCH, TOK_IDENT,
               TOK_LBRACE, TOK_INT, TOK_ARROW, TOK_IDENT, TOK_LPAREN, TOK_RPAREN, TOK_NEWLINE,
               TOK_KW_ELSE, TOK_ARROW, TOK_IDENT, TOK_LPAREN, TOK_RPAREN, TOK_RBRACE, TOK_EOF);
}

static void a_dot_continues_a_line_but_does_not_follow_one(unit *t) {
  /* A method chain breaks *after* the dot. Leading-dot continuation had to go: a
   * `match` arm's pattern begins with a dot, and once the newline is suppressed
   * there is no way to tell the two apart, so
   *
   *     .active -> render()
   *     .banned -> deny()
   *
   * became `render().banned`. Required syntax wins over optional style. */
  EXPECT_KINDS(t, "a.\nb", TOK_IDENT, TOK_DOT, TOK_IDENT, TOK_EOF);
  EXPECT_KINDS(t, "a\n.b", TOK_IDENT, TOK_NEWLINE, TOK_DOT, TOK_IDENT, TOK_EOF);
}

static void a_match_arm_pattern_keeps_its_own_line(unit *t) {
  EXPECT_KINDS(t, "match s {\n  .a -> f()\n  .b -> g()\n}", TOK_KW_MATCH, TOK_IDENT, TOK_LBRACE,
               TOK_DOT, TOK_IDENT, TOK_ARROW, TOK_IDENT, TOK_LPAREN, TOK_RPAREN, TOK_NEWLINE,
               TOK_DOT, TOK_IDENT, TOK_ARROW, TOK_IDENT, TOK_LPAREN, TOK_RPAREN, TOK_RBRACE,
               TOK_EOF);
}

static void a_closing_bracket_does_not_suppress_the_newline_after_it(unit *t) {
  /* A closing bracket is a follow token, never a continuation one. Treating it as
   * both let `f()` swallow its own statement terminator and join the next line --
   * the same shape of defect D060 corrected for postfix `!`. */
  EXPECT_KINDS(t, "f()\ng()", TOK_IDENT, TOK_LPAREN, TOK_RPAREN, TOK_NEWLINE, TOK_IDENT, TOK_LPAREN,
               TOK_RPAREN, TOK_EOF);
  EXPECT_KINDS(t, "let a = [1]\nlet b = 2", TOK_KW_LET, TOK_IDENT, TOK_EQ, TOK_LBRACKET, TOK_INT,
               TOK_RBRACKET, TOK_NEWLINE, TOK_KW_LET, TOK_IDENT, TOK_EQ, TOK_INT, TOK_EOF);
  /* A `}` that closes a block still terminates the line after it. */
  EXPECT_KINDS(t, "fn f() {}\nfn g() {}", TOK_KW_FN, TOK_IDENT, TOK_LPAREN, TOK_RPAREN, TOK_LBRACE,
               TOK_RBRACE, TOK_NEWLINE, TOK_KW_FN, TOK_IDENT, TOK_LPAREN, TOK_RPAREN, TOK_LBRACE,
               TOK_RBRACE, TOK_EOF);
}

static void comments_are_not_significant_for_line_structure(unit *t) {
  /* The rule looks past trivia in both directions. */
  EXPECT_KINDS(t, "a +\n// c\nb", TOK_IDENT, TOK_PLUS, TOK_IDENT, TOK_EOF);
  EXPECT_KINDS(t, "a\n// c\nb", TOK_IDENT, TOK_NEWLINE, TOK_IDENT, TOK_EOF);
}

static void the_newline_span_covers_the_whole_run(unit *t) {
  /* How `doot fmt` recovers the author's blank lines without an extra field. */
  fixture f;
  token nl;
  slice text;
  size_t i;
  size_t newlines = 0;

  fix_init(&f, "a\n\n\nb");
  (void)lex_next(f.lx);
  nl = lex_next(f.lx);
  UNIT_EQ_INT(t, nl.kind, TOK_NEWLINE);
  text = lex_text(f.lx, nl);
  for (i = 0; i < text.n; i++) {
    if (text.p[i] == '\n') {
      newlines++;
    }
  }
  UNIT_EQ_INT(t, newlines, 3);
  fix_free(&f);
}

static void peeking_does_not_lose_the_token_after_a_newline(unit *t) {
  /* The newline lookahead and the peek slot are distinct; sharing one would drop
   * the token scanned past the newline. */
  fixture f;

  fix_init(&f, "a\nb");
  UNIT_EQ_INT(t, lex_peek(f.lx).kind, TOK_IDENT);
  UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_IDENT);
  UNIT_EQ_INT(t, lex_peek(f.lx).kind, TOK_NEWLINE);
  UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_NEWLINE);
  UNIT_EQ_INT(t, lex_peek(f.lx).kind, TOK_IDENT);
  UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_IDENT);
  UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_EOF);
  fix_free(&f);
}

/* ---- markup ------------------------------------------------------------ */

/* Consumes a TOK_MARKUP_START and enters markup, as the parser does in
 * expression position (D059). */
static bool enter_markup(unit *t, fixture *f) {
  token tok = lex_next(f->lx);

  t->checks++;
  if (tok.kind != TOK_MARKUP_START) {
    unit_failf(t, __FILE__, __LINE__, "expected markup start, got %s", token_kind_name(tok.kind));
    return false;
  }
  lex_open_markup(f->lx, tok.at);
  return true;
}

static void a_less_than_is_markup_only_when_the_bytes_allow_it(unit *t) {
  /* Conditions 2 and 3 are lexical; the parser decides expression position. */
  EXPECT_KINDS(t, "<div>", TOK_MARKUP_START);
  EXPECT_KINDS(t, "a < 3", TOK_IDENT, TOK_LT, TOK_INT, TOK_EOF);
  EXPECT_KINDS(t, "a <3", TOK_IDENT, TOK_LT, TOK_INT, TOK_EOF);
  EXPECT_KINDS(t, "a <= b", TOK_IDENT, TOK_LE, TOK_IDENT, TOK_EOF);
  /* A name not terminated by whitespace, `>`, `/`, or `=` fails condition 3. */
  EXPECT_KINDS(t, "a <b.c", TOK_IDENT, TOK_LT, TOK_IDENT, TOK_DOT, TOK_IDENT, TOK_EOF);
}

static void a_simple_element_lexes_to_tag_and_content(unit *t) {
  fixture f;

  fix_init(&f, "<p>hi</p>");
  if (enter_markup(t, &f)) {
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_NAME);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_END);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_MARKUP_TEXT);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_CLOSE_START);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_NAME);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_END);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_EOF);
    UNIT_FALSE(t, diag_has_errors(&f.sink));
  }
  fix_free(&f);
}

static void attributes_and_self_closing(unit *t) {
  fixture f;

  fix_init(&f, "<input name=\"q\" required/>");
  if (enter_markup(t, &f)) {
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_NAME);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_ATTR_NAME);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_ATTR_EQ);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_STR_START);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_STR_TEXT);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_STR_END);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_ATTR_NAME);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_SELF_CLOSE);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_EOF);
    UNIT_FALSE(t, diag_has_errors(&f.sink));
  }
  fix_free(&f);
}

static void hyphenated_names_are_one_token(unit *t) {
  fixture f;
  token tok;

  fix_init(&f, "<my-widget data-live=\"/x\"/>");
  if (enter_markup(t, &f)) {
    tok = lex_next(f.lx);
    UNIT_EQ_INT(t, tok.kind, TOK_TAG_NAME);
    UNIT_EQ_SLICE(t, lex_text(f.lx, tok), "my-widget");
    tok = lex_next(f.lx);
    UNIT_EQ_INT(t, tok.kind, TOK_ATTR_NAME);
    UNIT_EQ_SLICE(t, lex_text(f.lx, tok), "data-live");
  }
  fix_free(&f);
}

static void nested_elements_track_their_own_levels(unit *t) {
  fixture f;

  fix_init(&f, "<ul><li>x</li></ul>");
  if (enter_markup(t, &f)) {
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_NAME);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_END);
    /* A nested `<tag` inside content needs no parser involvement. */
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_MARKUP_START);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_NAME);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_END);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_MARKUP_TEXT);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_CLOSE_START);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_NAME);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_END);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_CLOSE_START);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_NAME);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_END);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_EOF);
    UNIT_FALSE(t, diag_has_errors(&f.sink));
  }
  fix_free(&f);
}

static void interpolation_and_control_flow_inside_content(unit *t) {
  fixture f;

  fix_init(&f, "<ul>{for m in ms}<li>${m.body}</li>{end}</ul>");
  if (enter_markup(t, &f)) {
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_NAME);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_END);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_CTRL_START);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_KW_FOR);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_IDENT);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_KW_IN);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_IDENT);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_CTRL_END);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_MARKUP_START);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_NAME);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_END);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_INTERP_START);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_IDENT);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_DOT);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_IDENT);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_INTERP_END);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_CLOSE_START);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_NAME);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_END);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_CTRL_START);
    /* `end` is not a keyword: it arrives as an identifier (D061). */
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_IDENT);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_CTRL_END);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_CLOSE_START);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_NAME);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_END);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_EOF);
    UNIT_FALSE(t, diag_has_errors(&f.sink));
  }
  fix_free(&f);
}

static void newlines_inside_markup_are_content_not_terminators(unit *t) {
  fixture f;
  token tok;

  fix_init(&f, "<p>\n  hi\n</p>");
  if (enter_markup(t, &f)) {
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_NAME);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_END);
    tok = lex_next(f.lx);
    UNIT_EQ_INT(t, tok.kind, TOK_MARKUP_TEXT);
    UNIT_EQ_SLICE(t, lex_text(f.lx, tok), "\n  hi\n");
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_CLOSE_START);
  }
  fix_free(&f);
}

static void a_markup_comment_is_one_token(unit *t) {
  fixture f;

  fix_init(&f, "<p><!-- note --></p>");
  if (enter_markup(t, &f)) {
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_NAME);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_END);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_MARKUP_COMMENT);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_CLOSE_START);
    UNIT_FALSE(t, diag_has_errors(&f.sink));
  }
  fix_free(&f);

  fix_init(&f, "<p><!-- open");
  if (enter_markup(t, &f)) {
    while (lex_next(f.lx).kind != TOK_EOF) {
      /* drain */
    }
    UNIT_TRUE(t, has_code(&f.sink, DIAG_UNTERMINATED_MARKUP_COMMENT));
  }
  fix_free(&f);
}

static void an_unclosed_element_is_reported_at_its_opening_angle(unit *t) {
  fixture f;

  fix_init(&f, "<div>text");
  if (enter_markup(t, &f)) {
    while (lex_next(f.lx).kind != TOK_EOF) {
      /* drain */
    }
    UNIT_TRUE(t, has_code(&f.sink, DIAG_UNCLOSED_ELEMENT));
  }
  fix_free(&f);
}

static void a_malformed_tag_or_attribute_name_is_reported(unit *t) {
  fixture f;

  fix_init(&f, "<div #x>");
  if (enter_markup(t, &f)) {
    while (lex_next(f.lx).kind != TOK_EOF) {
      /* drain */
    }
    UNIT_TRUE(t, has_code(&f.sink, DIAG_MALFORMED_ATTR_NAME));
  }
  fix_free(&f);

  fix_init(&f, "</ 9>");
  if (enter_markup(t, &f)) {
    while (lex_next(f.lx).kind != TOK_EOF) {
      /* drain */
    }
    UNIT_TRUE(t, has_code(&f.sink, DIAG_MALFORMED_TAG_NAME));
  }
  fix_free(&f);
}

static void a_spread_attribute_lexes_as_an_ellipsis(unit *t) {
  fixture f;

  fix_init(&f, "<div ...attrs/>");
  if (enter_markup(t, &f)) {
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_NAME);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_ELLIPSIS);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_ATTR_NAME);
    UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_TAG_SELF_CLOSE);
  }
  fix_free(&f);
}

/* ---- limits and bad bytes ---------------------------------------------- */

static void nesting_beyond_the_bound_is_a_diagnostic_not_a_crash(unit *t) {
  /* Each `"${` pushes two levels, so 40 of them passes 64. */
  fixture f;
  char text[256];
  size_t i;

  for (i = 0; i < 40u; i++) {
    text[i * 3u] = '"';
    text[i * 3u + 1u] = '$';
    text[i * 3u + 2u] = '{';
  }
  text[120] = '\0';

  fix_init(&f, text);
  while (lex_next(f.lx).kind != TOK_EOF) {
    /* drain */
  }
  UNIT_TRUE(t, has_code(&f.sink, DIAG_NESTING_TOO_DEEP));
  fix_free(&f);
}

static void a_byte_that_begins_no_token_is_reported_and_skipped(unit *t) {
  fixture f;

  fix_init(&f, "let x = 1;");
  while (lex_next(f.lx).kind != TOK_EOF) {
    /* drain */
  }
  UNIT_TRUE(t, has_code(&f.sink, DIAG_UNEXPECTED_CHAR));
  fix_free(&f);

  /* A run of them costs no stack and still terminates. */
  fix_init(&f, "#####################");
  UNIT_EQ_INT(t, lex_next(f.lx).kind, TOK_EOF);
  UNIT_TRUE(t, has_code(&f.sink, DIAG_UNEXPECTED_CHAR));
  fix_free(&f);
}

static void a_bad_byte_span_covers_the_whole_utf8_character(unit *t) {
  fixture f;
  const diag *d;

  /* A smart quote: three bytes, one character, one caret. */
  fix_init(&f, "let x = \xe2\x80\x9c");
  while (lex_next(f.lx).kind != TOK_EOF) {
    /* drain */
  }
  UNIT_TRUE(t, has_code(&f.sink, DIAG_UNEXPECTED_CHAR));
  for (d = f.sink.first; d != NULL; d = d->next) {
    if (d->code == DIAG_UNEXPECTED_CHAR) {
      UNIT_EQ_INT(t, d->at.end - d->at.start, 3);
      break;
    }
  }
  fix_free(&f);
}

/* ---- the chat application ---------------------------------------------- */

static void the_documented_example_lexes_without_diagnostics(unit *t) {
  /* The route handler from docs/02-syntax.md, minus its markup, which needs the
   * parser to enter. Nothing here should produce a diagnostic. */
  static const char *src = "type Msg {\n"
                           "  id:   int\n"
                           "  room: str\n"
                           "  body: str\n"
                           "  at:   time.Time\n"
                           "}\n"
                           "\n"
                           "route POST \"/rooms/:room\" (room: str, form: NewMsg) -> redirect! {\n"
                           "  let m = db.one[Msg](\n"
                           "    `insert into msgs (room, body, at) values (?, ?, ?) returning *`,\n"
                           "    room, form.body, time.now())!\n"
                           "\n"
                           "  topic.publish(\"room:\" + room, m)\n"
                           "  return http.see_other(\"/rooms/\" + room)\n"
                           "}\n";
  fixture f;
  size_t count = 0;

  fix_init(&f, src);
  while (lex_next(f.lx).kind != TOK_EOF) {
    count++;
    if (count > 4096u) {
      break;
    }
  }
  UNIT_TRUE(t, count > 60u);
  UNIT_EQ_INT(t, diag_error_count(&f.sink), 0);
  fix_free(&f);
}

static const unit_case cases[] = {
    {"keywords_are_recognized", keywords_are_recognized},
    {"a_word_that_only_starts_with_a_keyword_is_an_identifier",
     a_word_that_only_starts_with_a_keyword_is_an_identifier},
    {"reserved_words_lex_as_reserved_not_identifiers",
     reserved_words_lex_as_reserved_not_identifiers},
    {"every_reserved_word_has_help_naming_the_doot_construct",
     every_reserved_word_has_help_naming_the_doot_construct},
    {"http_methods_and_end_stay_identifiers", http_methods_and_end_stay_identifiers},
    {"operators_take_the_longest_match", operators_take_the_longest_match},
    {"postfix_bang_is_distinct_from_not_equal", postfix_bang_is_distinct_from_not_equal},
    {"integer_and_float_forms", integer_and_float_forms},
    {"a_dot_after_a_number_is_a_method_call_unless_a_digit_follows",
     a_dot_after_a_number_is_a_method_call_unless_a_digit_follows},
    {"malformed_numbers_are_reported", malformed_numbers_are_reported},
    {"an_underscore_must_separate_two_digits", an_underscore_must_separate_two_digits},
    {"a_plain_string_is_a_three_token_sequence", a_plain_string_is_a_three_token_sequence},
    {"interpolation_becomes_nested_tokens", interpolation_becomes_nested_tokens},
    {"braces_inside_an_interpolation_nest", braces_inside_an_interpolation_nest},
    {"escapes_are_left_for_the_parser", escapes_are_left_for_the_parser},
    {"an_unterminated_string_is_reported_at_its_opening_quote",
     an_unterminated_string_is_reported_at_its_opening_quote},
    {"raw_strings_span_lines_and_keep_escapes", raw_strings_span_lines_and_keep_escapes},
    {"an_unterminated_interpolation_is_reported", an_unterminated_interpolation_is_reported},
    {"comments_are_recorded_not_emitted", comments_are_recorded_not_emitted},
    {"a_null_comment_list_discards_them", a_null_comment_list_discards_them},
    {"block_comments_nest", block_comments_nest},
    {"a_newline_terminates_a_statement", a_newline_terminates_a_statement},
    {"a_run_of_newlines_collapses_to_one", a_run_of_newlines_collapses_to_one},
    {"leading_and_trailing_blank_lines_produce_no_newline",
     leading_and_trailing_blank_lines_produce_no_newline},
    {"a_continuation_token_suppresses_the_newline", a_continuation_token_suppresses_the_newline},
    {"the_compound_assignments_continue_like_plain_assignment",
     the_compound_assignments_continue_like_plain_assignment},
    {"a_pattern_alternation_bar_continues", a_pattern_alternation_bar_continues},
    {"postfix_bang_does_not_suppress_the_newline", postfix_bang_does_not_suppress_the_newline},
    {"a_follow_token_suppresses_the_newline", a_follow_token_suppresses_the_newline},
    {"a_closing_bracket_does_not_suppress_the_newline_after_it",
     a_closing_bracket_does_not_suppress_the_newline_after_it},
    {"a_dot_continues_a_line_but_does_not_follow_one",
     a_dot_continues_a_line_but_does_not_follow_one},
    {"a_match_arm_pattern_keeps_its_own_line", a_match_arm_pattern_keeps_its_own_line},
    {"a_follow_token_cannot_begin_a_construct", a_follow_token_cannot_begin_a_construct},
    {"a_match_arms_else_keeps_its_own_line", a_match_arms_else_keeps_its_own_line},
    {"comments_are_not_significant_for_line_structure",
     comments_are_not_significant_for_line_structure},
    {"the_newline_span_covers_the_whole_run", the_newline_span_covers_the_whole_run},
    {"peeking_does_not_lose_the_token_after_a_newline",
     peeking_does_not_lose_the_token_after_a_newline},
    {"a_less_than_is_markup_only_when_the_bytes_allow_it",
     a_less_than_is_markup_only_when_the_bytes_allow_it},
    {"a_simple_element_lexes_to_tag_and_content", a_simple_element_lexes_to_tag_and_content},
    {"attributes_and_self_closing", attributes_and_self_closing},
    {"hyphenated_names_are_one_token", hyphenated_names_are_one_token},
    {"nested_elements_track_their_own_levels", nested_elements_track_their_own_levels},
    {"interpolation_and_control_flow_inside_content",
     interpolation_and_control_flow_inside_content},
    {"newlines_inside_markup_are_content_not_terminators",
     newlines_inside_markup_are_content_not_terminators},
    {"a_markup_comment_is_one_token", a_markup_comment_is_one_token},
    {"an_unclosed_element_is_reported_at_its_opening_angle",
     an_unclosed_element_is_reported_at_its_opening_angle},
    {"a_malformed_tag_or_attribute_name_is_reported",
     a_malformed_tag_or_attribute_name_is_reported},
    {"a_spread_attribute_lexes_as_an_ellipsis", a_spread_attribute_lexes_as_an_ellipsis},
    {"nesting_beyond_the_bound_is_a_diagnostic_not_a_crash",
     nesting_beyond_the_bound_is_a_diagnostic_not_a_crash},
    {"a_byte_that_begins_no_token_is_reported_and_skipped",
     a_byte_that_begins_no_token_is_reported_and_skipped},
    {"a_bad_byte_span_covers_the_whole_utf8_character",
     a_bad_byte_span_covers_the_whole_utf8_character},
    {"the_documented_example_lexes_without_diagnostics",
     the_documented_example_lexes_without_diagnostics},
};

UNIT_SUITE(suite_lex, "lex", cases);
