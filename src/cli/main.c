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
#include "../base/slice.h"
#include "version.h"

#define EXIT_OK 0
#define EXIT_DIAGNOSTIC 1
#define EXIT_USAGE 2

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
                "  explain <code>   explain a diagnostic code, e.g. DT0001\n"
                "  codes            list every diagnostic code doot can emit\n"
                "\n"
                "options:\n"
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
