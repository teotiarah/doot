/* main.c -- command dispatch.
 *
 * Commands appear here only when they fully work (D054). The set grows one
 * working command at a time as subsystems land; there are no stubs and no
 * flag-gated partial features.
 */
#include <stdio.h>
#include <string.h>

#include "../base/arena.h"
#include "../base/assert.h"
#include "../base/buf.h"
#include "../base/diag.h"
#include "../base/fs.h"
#include "../base/slice.h"
#include "../base/source.h"
#include "../parse/parse.h"
#include "../parse/print.h"
#include "version.h"

#define EXIT_OK 0
#define EXIT_DIAGNOSTIC 1
#define EXIT_USAGE 2

/* The compiler's arena is fatal on exhaustion (D047), which is what lets the
 * parser and the printer allocate without checking. */
#define CLI_ARENA_CHUNK ((size_t)256u * 1024u)

static void print_version(void) {
  (void)printf("doot %s (%s)\n", DOOT_VERSION, DOOT_BUILD_REV);
}

static void print_usage(FILE *to) {
  (void)fprintf(to,
                "doot %s -- a language for the web\n"
                "\n"
                "usage: doot <command> [options]\n"
                "\n"
                "commands:\n"
                "  fmt [path...]    format doot source, canonically and in place\n"
                "  explain <code>   explain a diagnostic code, e.g. DT0001\n"
                "  codes            list every diagnostic code doot can emit\n"
                "\n"
                "options:\n"
                "  --json           machine-readable diagnostics\n"
                "  -h, --help       show this message\n"
                "  -V, --version    show the version\n"
                "\n"
                "Commands are added as subsystems land; see docs/07-roadmap.md.\n",
                DOOT_VERSION);
}

static int cmd_explain(int argc, char **argv) {
  diag_code code;

  if (argc != 1) {
    (void)fprintf(stderr, "doot explain: expected exactly one diagnostic code\n"
                          "usage: doot explain <code>    e.g. doot explain DT0001\n");
    return EXIT_USAGE;
  }

  if (!diag_code_parse(slice_from_cstr(argv[0]), &code)) {
    (void)fprintf(stderr,
                  "doot explain: `%s` is not a diagnostic code doot emits\n"
                  "run `doot codes` to list them\n",
                  argv[0]);
    return EXIT_USAGE;
  }

  (void)printf("%s[%s]: %s\n\n%s\n", diag_severity_str(diag_code_severity(code)),
               diag_code_str(code), diag_code_brief(code), diag_code_explain(code));
  return EXIT_OK;
}

/* ---- fmt ---------------------------------------------------------------- */

/* Every file this walks ends in exactly one of three states, and they are
 * counted separately because collapsing them is a lie: a file that could not be
 * formatted is not a file that needed no formatting. */
typedef struct {
  arena *a;
  diag_sink sink;
  bool json;
  unsigned long changed; /* rewritten in place */
  unsigned long clean;   /* parsed, and already byte-for-byte canonical */
  unsigned long skipped; /* left alone: it did not parse, or the write failed */
  buf changed_list;
} fmt_run;

static bool has_do_extension(slice name) {
  return name.n > 3u && slice_ends_with(name, SLICE_LIT(".do"));
}

static void fmt_file(fmt_run *r, slice path) {
  size_t errors_before = diag_error_count(&r->sink);
  source *src;
  unit_ast *unit;
  slice formatted;

  src = source_from_file(r->a, path, &r->sink);
  if (src == NULL) {
    /* Counted in no bucket: DT1001 is already reported, and a path that could
     * not be read is not a source file whose formatting we have an opinion on. */
    return;
  }

  unit = parse_unit(r->a, src, &r->sink);
  if (diag_error_count(&r->sink) != errors_before) {
    /* A file that did not parse is left alone. The tree has holes where the
     * parser recovered, so printing it would produce plausible source that
     * silently differs from what the author wrote -- much worse than not
     * formatting it.
     *
     * It counts as skipped rather than clean. Reporting it as already formatted
     * would assert the one thing this branch does not know, and the assertion
     * would be wrong precisely when the file is worst. */
    r->skipped++;
    return;
  }

  formatted = fmt_unit(r->a, unit);
  if (slice_eq(formatted, source_text(src))) {
    r->clean++;
    return;
  }
  if (!fs_write_file(r->a, path, formatted, &r->sink)) {
    /* DT1002. The write is a temporary plus a rename, so the file still holds
     * exactly what the author wrote -- unformatted, hence skipped. */
    r->skipped++;
    return;
  }
  r->changed++;
  (void)buf_append_cstr(&r->changed_list, "  ");
  (void)buf_append_slice(&r->changed_list, path);
  (void)buf_append_byte(&r->changed_list, '\n');
}

static void fmt_path(fmt_run *r, slice path) {
  fs_entries entries;
  const fs_entry *e;

  if (!fs_is_dir(r->a, path)) {
    /* Named explicitly, so format it whatever it is called. */
    fmt_file(r, path);
    return;
  }
  if (!fs_read_dir(r->a, path, &entries, &r->sink)) {
    return;
  }
  for (e = entries.first; e != NULL; e = e->next) {
    slice child = fs_join(r->a, path, e->name);

    if (e->is_dir) {
      fmt_path(r, child);
    } else if (has_do_extension(e->name)) {
      fmt_file(r, child);
    }
  }
}

static int cmd_fmt(int argc, char **argv) {
  fmt_run r;
  buf out;
  int i;
  int paths = 0;

  memset(&r, 0, sizeof(r));
  r.a = arena_new_fatal(CLI_ARENA_CHUNK);
  diag_sink_init(&r.sink, r.a, 0u);
  buf_init(&r.changed_list, r.a, 256u);

  for (i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--json") == 0) {
      r.json = true;
      continue;
    }
    if (argv[i][0] == '-') {
      (void)fprintf(stderr,
                    "doot fmt: unknown option `%s`\n"
                    "usage: doot fmt [--json] [path...]\n",
                    argv[i]);
      arena_destroy(r.a);
      return EXIT_USAGE;
    }
    paths++;
  }

  if (paths == 0) {
    fmt_path(&r, SLICE_LIT("."));
  } else {
    for (i = 0; i < argc; i++) {
      if (argv[i][0] != '-') {
        fmt_path(&r, slice_from_cstr(argv[i]));
      }
    }
  }

  buf_init(&out, r.a, 1024u);
  if (r.json) {
    /* Exactly the schema pinned in docs/06-tooling.md#diagnostics, with nothing
     * added: it is what every other command emits and what the spec runner
     * consumes, so it stays one shape. */
    diag_render_json(&r.sink, &out);
    (void)fputs(buf_cstr(&out), stdout);
  } else {
    /* The denominator is the files that were actually formatted, not every file
     * seen: the skipped ones are accounted for on their own line, and folding
     * them in here would imply they had been found canonical. */
    unsigned long formatted = r.changed + r.clean;

    diag_render_human(&r.sink, &out, false);
    (void)fputs(buf_cstr(&out), stderr);
    if (r.changed > 0u) {
      (void)printf("reformatted %lu of %lu file%s\n", r.changed, formatted,
                   formatted == 1u ? "" : "s");
      (void)fputs(buf_cstr(&r.changed_list), stdout);
    } else if (r.clean > 0u || r.skipped == 0u) {
      /* The `r.skipped == 0u` arm is what keeps a walk that formatted nothing at
       * all from being silent -- including an empty directory, which reports
       * zero. When every file was skipped, the skipped line below says so on its
       * own and a `0 files already formatted` above it would only add noise. */
      (void)printf("%lu file%s already formatted\n", r.clean, r.clean == 1u ? "" : "s");
    }
    if (r.skipped > 0u) {
      (void)printf("skipped %lu file%s with errors\n", r.skipped, r.skipped == 1u ? "" : "s");
    }
  }

  {
    int status = diag_has_errors(&r.sink) ? EXIT_DIAGNOSTIC : EXIT_OK;
    arena_destroy(r.a);
    return status;
  }
}

static int cmd_codes(void) {
  int i;

  for (i = 0; i < (int)DIAG_CODE_COUNT; i++) {
    diag_code code = (diag_code)i;
    (void)printf("%-8s %-8s %s\n", diag_code_str(code), diag_severity_str(diag_code_severity(code)),
                 diag_code_brief(code));
  }
  return EXIT_OK;
}

int main(int argc, char **argv) {
  const char *cmd;

  if (argc < 2) {
    print_usage(stdout);
    return EXIT_USAGE;
  }

  cmd = argv[1];

  if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0) {
    print_usage(stdout);
    return EXIT_OK;
  }
  if (strcmp(cmd, "-V") == 0 || strcmp(cmd, "--version") == 0 || strcmp(cmd, "version") == 0) {
    print_version();
    return EXIT_OK;
  }
  if (strcmp(cmd, "fmt") == 0) {
    return cmd_fmt(argc - 2, argv + 2);
  }
  if (strcmp(cmd, "explain") == 0) {
    return cmd_explain(argc - 2, argv + 2);
  }
  if (strcmp(cmd, "codes") == 0) {
    return cmd_codes();
  }

  (void)fprintf(stderr, "doot: unknown command `%s`\n\n", cmd);
  print_usage(stderr);
  return EXIT_USAGE;
}
