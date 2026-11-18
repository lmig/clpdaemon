/* clpdaemon entry point: command line and configuration (T2). */
#include "config.h"

#include <stdio.h>
#include <string.h>

static const char usage[] =
    "usage: clpdaemon --config <file> [--foreground] [--check]\n";

static int usage_error(const char *fmt, const char *arg) {
  fputs("clpdaemon: ", stderr);
  fprintf(stderr, fmt, arg);
  fputc('\n', stderr);
  fputs(usage, stderr);
  return 2;
}

int main(int argc, char **argv) {
  const char *config_path = NULL;
  int check = 0, i, rc;
  clpd_config cfg;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0) {
      fputs(usage, stdout);
      return 0;
    } else if (strcmp(argv[i], "--config") == 0) {
      if (i + 1 >= argc) return usage_error("option --config requires a file%s", "");
      if (config_path != NULL)
        return usage_error("option --config given more than once%s", "");
      config_path = argv[++i];
    } else if (strcmp(argv[i], "--foreground") == 0) {
      /* Default mode: the daemon never double-forks (FR-1). */
    } else if (strcmp(argv[i], "--check") == 0) {
      check = 1;
    } else {
      return usage_error("unknown option '%s'", argv[i]);
    }
  }
  if (config_path == NULL)
    return usage_error("missing required option --config%s", "");

  rc = clpd_config_load(config_path, &cfg, stderr);
  clpd_config_free(&cfg);
  if (rc != 0) return 1;
  if (check) printf("clpdaemon: %s: configuration OK\n", config_path);
  /* The event loop arrives with T3; until then a run validates and exits. */
  return 0;
}
