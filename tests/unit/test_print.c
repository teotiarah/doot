#include "../../src/parse/print.h"

#include <string.h>

#include "../../src/base/diag.h"
#include "../../src/parse/parse.h"
#include "unit.h"

/* ---- fixture ----------------------------------------------------------- */

/* Formats `text`, asserting it parsed cleanly. The returned slice lives in the
 * arena, which the caller destroys. */
static slice format_in(unit *t, arena *a, const char *text, bool *ok) {
  diag_sink sink;
  source *src;
  unit_ast *parsed;

  diag_sink_init(&sink, a, 0u);
  src = source_from_memory(a, SLICE_LIT("t.do"), slice_from_cstr(text), &sink);
  if (src == NULL) {
    unit_failf(t, __FILE__, __LINE__, "source did not load: %s", text);
    *ok = false;
    return SLICE_EMPTY;
  }
  parsed = parse_unit(a, src, &sink);
  if (diag_error_count(&sink) != 0) {
    const diag *d = sink.first;
    line_col lc = source_line_col(src, d->at.start);

    unit_failf(t, __FILE__, __LINE__, "expected a clean parse of:\n%s\n  got %s at %lu:%lu: %.*s",
               text, diag_code_str(d->code), (unsigned long)lc.line, (unsigned long)lc.col,
               (int)d->message.n, d->message.p);
    *ok = false;
    return SLICE_EMPTY;
  }
  *ok = true;
  return fmt_unit(a, parsed);
}

/* Asserts the exact formatted output, then asserts formatting it again is a
 * no-op. Idempotence is checked on every case rather than in one test of its own,
 * because it is the property most likely to break somewhere specific. */
static void expect_fmt(unit *t, const char *input, const char *want) {
  arena *a = arena_new_fatal(1u << 16);
  bool ok = false;
  slice got = format_in(t, a, input, &ok);

  if (ok) {
    t->checks++;
    if (!slice_eq_cstr(got, want)) {
      unit_failf(t, __FILE__, __LINE__, "formatting\n%s\n  expected:\n%s\n  got:\n%.*s", input,
                 want, (int)got.n, got.p);
    } else {
      arena *b = arena_new_fatal(1u << 16);
      bool ok2 = false;
      const char *once = slice_cstr(a, got);
      slice twice;

      if (once != NULL) {
        twice = format_in(t, b, once, &ok2);
        t->checks++;
        if (ok2 && !slice_eq(twice, got)) {
          unit_failf(t, __FILE__, __LINE__, "not idempotent:\n%.*s  became:\n%.*s", (int)got.n,
                     got.p, (int)twice.n, twice.p);
        }
      }
      arena_destroy(b);
    }
  }
  arena_destroy(a);
}

/* ---- fixed choices ----------------------------------------------------- */

static void indentation_is_two_spaces(unit *t) {
  expect_fmt(t, "fn f() {\nlet a = 1\nif a > 0 {\nlet b = 2\n}\n}\n",
             "fn f() {\n"
             "  let a = 1\n"
             "  if a > 0 {\n"
             "    let b = 2\n"
             "  }\n"
             "}\n");
}

static void a_one_line_body_expands_to_a_block(unit *t) {
  expect_fmt(t, "fn f() { return 1 }\n", "fn f() {\n  return 1\n}\n");
}

static void an_empty_block_stays_on_one_line(unit *t) {
  expect_fmt(t, "fn f() {\n}\n", "fn f() {}\n");
}

static void a_file_ends_with_exactly_one_newline(unit *t) {
  expect_fmt(t, "let a = 1", "let a = 1\n");
  expect_fmt(t, "let a = 1\n\n\n\n", "let a = 1\n");
}

static void blank_lines_collapse_to_at_most_one(unit *t) {
  expect_fmt(t, "let a = 1\n\n\n\n\nlet b = 2\n", "let a = 1\n\nlet b = 2\n");
  /* And none is preserved as none. */
  expect_fmt(t, "let a = 1\nlet b = 2\n", "let a = 1\nlet b = 2\n");
}

static void there_is_no_trailing_whitespace(unit *t) {
  arena *a = arena_new_fatal(1u << 16);
  bool ok = false;
  slice got = format_in(t, a, "fn f() {\n  if a {\n    b()\n  }\n}\n\nfn g() {\n  c()\n}\n", &ok);
  size_t i;

  if (ok) {
    for (i = 0; i + 1u < got.n; i++) {
      if (got.p[i + 1u] == '\n' && (got.p[i] == ' ' || got.p[i] == '\t')) {
        unit_failf(t, __FILE__, __LINE__, "trailing whitespace at byte %lu", (unsigned long)i);
        break;
      }
    }
    UNIT_TRUE(t, got.n > 0u);
  }
  arena_destroy(a);
}

/* ---- declarations ------------------------------------------------------ */

static void struct_fields_align_on_the_colon(unit *t) {
  expect_fmt(t, "type Msg {\nid: int\nroom: str\nat: time.Time\n}\n",
             "type Msg {\n"
             "  id:   int\n"
             "  room: str\n"
             "  at:   time.Time\n"
             "}\n");
}

static void a_field_keeps_its_default_and_attributes(unit *t) {
  expect_fmt(t, "type Search {\nq: str @len(1, 100) @trim\npage: int = 1\n}\n",
             "type Search {\n"
             "  q:    str @len(1, 100) @trim\n"
             "  page: int = 1\n"
             "}\n");
}

static void an_enum_stays_on_one_line(unit *t) {
  expect_fmt(t, "type Status enum {\nactive,\nbanned\n}\n",
             "type Status enum { active, banned }\n");
}

static void a_method_and_a_route_keep_their_signatures(unit *t) {
  expect_fmt(t, "fn User.display(self) -> str {\nreturn self.name\n}\n",
             "fn User.display(self) -> str {\n  return self.name\n}\n");
  expect_fmt(t, "route GET \"/users/:id\" (id: int) -> html! {\nreturn page(id)\n}\n",
             "route GET \"/users/:id\" (id: int) -> html! {\n  return page(id)\n}\n");
}

static void a_group_indents_its_routes(unit *t) {
  expect_fmt(t,
             "@before(auth.require)\ngroup \"/admin\" {\n"
             "route GET \"/\" () -> html! { return a() }\n"
             "route GET \"/x\" () -> html! { return b() }\n}\n",
             "@before(auth.require)\n"
             "group \"/admin\" {\n"
             "  route GET \"/\" () -> html! {\n"
             "    return a()\n"
             "  }\n"
             "  route GET \"/x\" () -> html! {\n"
             "    return b()\n"
             "  }\n"
             "}\n");
}

static void pub_is_preserved(unit *t) {
  expect_fmt(t, "pub fn f() {\nreturn\n}\n", "pub fn f() {\n  return\n}\n");
}

/* ---- expressions ------------------------------------------------------- */

static void parentheses_are_re_derived_from_precedence(unit *t) {
  /* The AST does not record the author's parentheses, so the printer puts back
   * exactly the ones the tree requires -- which makes it a check on the parser. */
  expect_fmt(t, "let a = (1 + 2) * 3\n", "let a = (1 + 2) * 3\n");
  expect_fmt(t, "let a = 1 + 2 * 3\n", "let a = 1 + 2 * 3\n");
  expect_fmt(t, "let a = 1 + (2 * 3)\n", "let a = 1 + 2 * 3\n");
  expect_fmt(t, "let a = (1 - 2) - 3\n", "let a = 1 - 2 - 3\n");
  expect_fmt(t, "let a = 1 - (2 - 3)\n", "let a = 1 - (2 - 3)\n");
  expect_fmt(t, "let a = not (b and c)\n", "let a = not (b and c)\n");
  expect_fmt(t, "let a = (b or c) and d\n", "let a = (b or c) and d\n");
}

static void else_is_right_associative(unit *t) {
  expect_fmt(t, "let a = x else y else z\n", "let a = x else y else z\n");
  expect_fmt(t, "let a = (x else y) else z\n", "let a = (x else y) else z\n");
}

static void the_postfix_chain_prints_without_parentheses(unit *t) {
  expect_fmt(t, "let a = db.all[Msg](q, r)!\n", "let a = db.all[Msg](q, r)!\n");
  expect_fmt(t, "let a = f().name\n", "let a = f().name\n");
  expect_fmt(t, "let a = xs[i].name!\n", "let a = xs[i].name!\n");
  expect_fmt(t, "let a = u with { name: \"Ada\" }\n", "let a = u with { name: \"Ada\" }\n");
}

static void a_lambda_operand_is_parenthesized(unit *t) {
  expect_fmt(t, "let a = xs.map(fn(u: User) => u.name)\n",
             "let a = xs.map(fn(u: User) => u.name)\n");
  expect_fmt(t, "let a = xs.each(fn(x: int) {\nlog(x)\n})\n",
             "let a = xs.each(fn(x: int) {\n  log(x)\n})\n");
}

static void a_match_prints_one_arm_per_line(unit *t) {
  expect_fmt(t, "fn f() {\nmatch s {\n.active -> a()\n1 | 2 -> b()\nelse -> {\nc()\n}\n}\n}\n",
             "fn f() {\n"
             "  match s {\n"
             "    .active -> a()\n"
             "    1 | 2 -> b()\n"
             "    else -> {\n"
             "      c()\n"
             "    }\n"
             "  }\n"
             "}\n");
}

static void else_on_its_own_line_is_normalized(unit *t) {
  /* `else` is not a follow token, so the parser accepts a newline before it and
   * the printer settles on one spelling. */
  expect_fmt(t, "fn f() {\nif a {\nb()\n}\nelse {\nc()\n}\n}\n",
             "fn f() {\n"
             "  if a {\n"
             "    b()\n"
             "  } else {\n"
             "    c()\n"
             "  }\n"
             "}\n");
}

/* ---- literals ---------------------------------------------------------- */

static void escapes_round_trip(unit *t) {
  expect_fmt(t, "let a = \"x\\ny\\tz\\\\w\\\"v\"\n", "let a = \"x\\ny\\tz\\\\w\\\"v\"\n");
  /* A `$` needs escaping only when a brace follows it. */
  expect_fmt(t, "let a = \"costs $5\"\n", "let a = \"costs $5\"\n");
  expect_fmt(t, "let a = \"\\${literal}\"\n", "let a = \"\\${literal}\"\n");
}

static void interpolation_round_trips(unit *t) {
  expect_fmt(t, "let a = \"hi ${name}, ${n} left\"\n", "let a = \"hi ${name}, ${n} left\"\n");
}

static void a_raw_string_is_untouched(unit *t) {
  expect_fmt(t, "let a = `select * from t where a = ?`\n",
             "let a = `select * from t where a = ?`\n");
  /* Backticks interpret nothing, so a backslash stays a backslash. */
  expect_fmt(t, "let a = `raw \\n text`\n", "let a = `raw \\n text`\n");
}

static void numbers_print_in_a_canonical_form(unit *t) {
  expect_fmt(t, "let a = 42\n", "let a = 42\n");
  expect_fmt(t, "let a = -42\n", "let a = -42\n");
  /* Radix and separators are not preserved: there is one spelling of a value. */
  expect_fmt(t, "let a = 0xff\n", "let a = 255\n");
  expect_fmt(t, "let a = 1_000_000\n", "let a = 1000000\n");
  expect_fmt(t, "let a = 3.14\n", "let a = 3.14\n");
  /* A float keeps a `.` so it does not lex back as an int. */
  expect_fmt(t, "let a = 3.0\n", "let a = 3.0\n");
  expect_fmt(t, "let a = 1e9\n", "let a = 1000000000.0\n");
}

static void the_most_negative_int_survives(unit *t) {
  expect_fmt(t, "let a = -9223372036854775808\n", "let a = -9223372036854775808\n");
}

/* ---- collections ------------------------------------------------------- */

static void a_collection_keeps_the_authors_line_structure(unit *t) {
  /* The printer cannot wrap a long line, so it must not join one either. */
  expect_fmt(t, "let a = [1, 2, 3]\n", "let a = [1, 2, 3]\n");
  expect_fmt(t, "let a = [\n1,\n2\n]\n", "let a = [\n  1,\n  2,\n]\n");
  expect_fmt(t, "let a = f(1, 2)\n", "let a = f(1, 2)\n");
  expect_fmt(t, "let a = f(\n1,\n2\n)\n", "let a = f(\n  1,\n  2,\n)\n");
  expect_fmt(t, "let a = {\"k\": 1}\n", "let a = {\"k\": 1}\n");
}

static void a_multiline_struct_literal_aligns_on_the_colon(unit *t) {
  /* The shape the configuration example in 02-syntax.md is written in:
   * newline-separated rather than comma-separated, and aligned. */
  expect_fmt(t, "let config = Config {\nlisten: \":8080\"\nrequest_timeout: 15.s\n}\n",
             "let config = Config {\n"
             "  listen:          \":8080\"\n"
             "  request_timeout: 15.s\n"
             "}\n");
  expect_fmt(t, "let u = User { id: 1, name: \"a\" }\n", "let u = User { id: 1, name: \"a\" }\n");
}

/* ---- types ------------------------------------------------------------- */

static void every_type_form_round_trips(unit *t) {
  expect_fmt(t, "fn f(a: int, b: [str], c: {str: int}, d: fn(int) -> str!, e: time.Time?) {\n}\n",
             "fn f(a: int, b: [str], c: {str: int}, d: fn(int) -> str!, e: time.Time?) {}\n");
}

/* ---- markup ------------------------------------------------------------ */

static void an_inline_element_stays_inline(unit *t) {
  expect_fmt(t, "let a = <h2>${u.name}</h2>\n", "let a = <h2>${u.name}</h2>\n");
  expect_fmt(t, "let a = <p>hello</p>\n", "let a = <p>hello</p>\n");
}

static void a_multiline_element_is_re_indented(unit *t) {
  expect_fmt(t, "let a = <div>\n<h2>hi</h2>\n<p>there</p>\n</div>\n",
             "let a = <div>\n"
             "  <h2>hi</h2>\n"
             "  <p>there</p>\n"
             "</div>\n");
  /* Badly indented input reaches the same output, which is the point. */
  expect_fmt(t, "let a = <div>\n        <h2>hi</h2>\n<p>there</p>\n   </div>\n",
             "let a = <div>\n"
             "  <h2>hi</h2>\n"
             "  <p>there</p>\n"
             "</div>\n");
}

static void inline_whitespace_between_elements_is_preserved(unit *t) {
  /* A single space between two inline elements is rendered content, so the
   * printer leaves it exactly as written. */
  expect_fmt(t, "let a = <p><b>x</b> <b>y</b></p>\n", "let a = <p><b>x</b> <b>y</b></p>\n");
  expect_fmt(t, "let a = <p><b>x</b><b>y</b></p>\n", "let a = <p><b>x</b><b>y</b></p>\n");
}

static void void_elements_normalize_to_a_slash(unit *t) {
  expect_fmt(t, "let a = <div>\n<br>\n<input name=\"q\">\n</div>\n",
             "let a = <div>\n  <br/>\n  <input name=\"q\"/>\n</div>\n");
  expect_fmt(t, "let a = <br/>\n", "let a = <br/>\n");
}

static void markup_control_flow_is_indented(unit *t) {
  expect_fmt(t,
             "let a = <ul>\n{for m in msgs}\n<li>${m.body}</li>\n{else}\n<li>none</li>\n"
             "{end}\n</ul>\n",
             "let a = <ul>\n"
             "  {for m in msgs}\n"
             "    <li>${m.body}</li>\n"
             "  {else}\n"
             "    <li>none</li>\n"
             "  {end}\n"
             "</ul>\n");
}

static void a_markup_if_chain_is_indented(unit *t) {
  expect_fmt(t,
             "let a = <p>\n{if b}\n<i>1</i>\n{else if c}\n<i>2</i>\n{else}\n<i>3</i>\n"
             "{end}\n</p>\n",
             "let a = <p>\n"
             "  {if b}\n"
             "    <i>1</i>\n"
             "  {else if c}\n"
             "    <i>2</i>\n"
             "  {else}\n"
             "    <i>3</i>\n"
             "  {end}\n"
             "</p>\n");
}

static void whitespace_significant_elements_are_left_alone(unit *t) {
  /* Re-indenting inside `pre` would change what the page renders. */
  expect_fmt(t, "let a = <pre>\n  keep   this\n    exactly\n</pre>\n",
             "let a = <pre>\n  keep   this\n    exactly\n</pre>\n");
}

static void an_attribute_spread_round_trips(unit *t) {
  expect_fmt(t, "let a = <div ...attrs/>\n", "let a = <div ...attrs/>\n");
  expect_fmt(t, "let a = <div ...${html.attrs(m)}/>\n", "let a = <div ...${html.attrs(m)}/>\n");
}

static void long_attribute_lists_wrap(unit *t) {
  expect_fmt(t,
             "let a = <input name=\"a_fairly_long_name\" value=\"${some.long.expression}\" "
             "placeholder=\"and another long one here\" required/>\n",
             "let a = <input\n"
             "  name=\"a_fairly_long_name\"\n"
             "  value=\"${some.long.expression}\"\n"
             "  placeholder=\"and another long one here\"\n"
             "  required\n"
             "/>\n");
}

/* ---- comments ---------------------------------------------------------- */

static void a_comment_above_a_declaration_keeps_its_place(unit *t) {
  expect_fmt(t, "// a note\nlet a = 1\n", "// a note\nlet a = 1\n");
  expect_fmt(t, "let a = 1\n\n// between\nlet b = 2\n", "let a = 1\n\n// between\nlet b = 2\n");
}

static void a_trailing_comment_stays_on_its_line(unit *t) {
  expect_fmt(t, "let a = 1 // why\n", "let a = 1  // why\n");
  expect_fmt(t, "fn f() {\nlet a = 1 // why\n}\n", "fn f() {\n  let a = 1  // why\n}\n");
}

static void comments_inside_a_block_are_indented(unit *t) {
  expect_fmt(t, "fn f() {\n// first\nlet a = 1\n// second\nlet b = 2\n}\n",
             "fn f() {\n"
             "  // first\n"
             "  let a = 1\n"
             "  // second\n"
             "  let b = 2\n"
             "}\n");
}

static void a_comment_after_the_last_declaration_survives(unit *t) {
  expect_fmt(t, "let a = 1\n// the end\n", "let a = 1\n// the end\n");
}

static void a_block_comment_survives(unit *t) {
  expect_fmt(t, "/* a block */\nlet a = 1\n", "/* a block */\nlet a = 1\n");
}

/* ---- the documented application ---------------------------------------- */

static void the_chat_application_is_already_canonical(unit *t) {
  /* The program from docs/02-syntax.md in full, minus only its stream handler,
   * which is DT0046-gated until v0.2 and so cannot reach the printer. Formatting
   * the documentation's own example must be a no-op, or the documentation is
   * wrong -- and it was: both db call sites were written with their arguments
   * packed onto one line, which D068 normalizes to one argument per line with a
   * trailing comma. This case only says which of the two is wrong if it carries
   * the whole program, so every declaration in the doc is reproduced here. */
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
      "    \"select * from msgs where room = ? order by id desc limit 50\",\n"
      "    room,\n"
      "  )!\n"
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
      "    room,\n"
      "    form.body,\n"
      "    time.now(),\n"
      "  )!\n"
      "\n"
      "  topic.publish(\"room:\" + room, m)\n"
      "  return http.see_other(\"/rooms/\" + room)\n"
      "}\n";

  expect_fmt(t, src, src);
}

static const unit_case cases[] = {
    {"indentation_is_two_spaces", indentation_is_two_spaces},
    {"a_one_line_body_expands_to_a_block", a_one_line_body_expands_to_a_block},
    {"an_empty_block_stays_on_one_line", an_empty_block_stays_on_one_line},
    {"a_file_ends_with_exactly_one_newline", a_file_ends_with_exactly_one_newline},
    {"blank_lines_collapse_to_at_most_one", blank_lines_collapse_to_at_most_one},
    {"there_is_no_trailing_whitespace", there_is_no_trailing_whitespace},
    {"struct_fields_align_on_the_colon", struct_fields_align_on_the_colon},
    {"a_field_keeps_its_default_and_attributes", a_field_keeps_its_default_and_attributes},
    {"an_enum_stays_on_one_line", an_enum_stays_on_one_line},
    {"a_method_and_a_route_keep_their_signatures", a_method_and_a_route_keep_their_signatures},
    {"a_group_indents_its_routes", a_group_indents_its_routes},
    {"pub_is_preserved", pub_is_preserved},
    {"parentheses_are_re_derived_from_precedence", parentheses_are_re_derived_from_precedence},
    {"else_is_right_associative", else_is_right_associative},
    {"the_postfix_chain_prints_without_parentheses", the_postfix_chain_prints_without_parentheses},
    {"a_lambda_operand_is_parenthesized", a_lambda_operand_is_parenthesized},
    {"a_match_prints_one_arm_per_line", a_match_prints_one_arm_per_line},
    {"else_on_its_own_line_is_normalized", else_on_its_own_line_is_normalized},
    {"escapes_round_trip", escapes_round_trip},
    {"interpolation_round_trips", interpolation_round_trips},
    {"a_raw_string_is_untouched", a_raw_string_is_untouched},
    {"numbers_print_in_a_canonical_form", numbers_print_in_a_canonical_form},
    {"the_most_negative_int_survives", the_most_negative_int_survives},
    {"a_collection_keeps_the_authors_line_structure",
     a_collection_keeps_the_authors_line_structure},
    {"a_multiline_struct_literal_aligns_on_the_colon",
     a_multiline_struct_literal_aligns_on_the_colon},
    {"every_type_form_round_trips", every_type_form_round_trips},
    {"an_inline_element_stays_inline", an_inline_element_stays_inline},
    {"a_multiline_element_is_re_indented", a_multiline_element_is_re_indented},
    {"inline_whitespace_between_elements_is_preserved",
     inline_whitespace_between_elements_is_preserved},
    {"void_elements_normalize_to_a_slash", void_elements_normalize_to_a_slash},
    {"markup_control_flow_is_indented", markup_control_flow_is_indented},
    {"a_markup_if_chain_is_indented", a_markup_if_chain_is_indented},
    {"whitespace_significant_elements_are_left_alone",
     whitespace_significant_elements_are_left_alone},
    {"an_attribute_spread_round_trips", an_attribute_spread_round_trips},
    {"long_attribute_lists_wrap", long_attribute_lists_wrap},
    {"a_comment_above_a_declaration_keeps_its_place",
     a_comment_above_a_declaration_keeps_its_place},
    {"a_trailing_comment_stays_on_its_line", a_trailing_comment_stays_on_its_line},
    {"comments_inside_a_block_are_indented", comments_inside_a_block_are_indented},
    {"a_comment_after_the_last_declaration_survives",
     a_comment_after_the_last_declaration_survives},
    {"a_block_comment_survives", a_block_comment_survives},
    {"the_chat_application_is_already_canonical", the_chat_application_is_already_canonical},
};

UNIT_SUITE(suite_print, "print", cases);
