/* clpdaemon entry point: command line, configuration (T2) and the event
 * loop running engine cycles (T3). */
#include "config.h"
#include "engine.h"
#include "loop.h"

#include <errno.h>
#include <signal.h>
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

typedef struct {
  clpd_engine *engine;
  int max_rules;
} cycle_ctx;

static void run_cycle(void *ctx) {
  cycle_ctx *c = ctx;
  clpd_engine_run_cycle(c->engine, c->max_rules);
}

/* Runs engine cycles until a stop signal. Returns the exit code. */
static int serve(const clpd_config *cfg) {
  cycle_ctx ctx;
  clpd_loop *loop;
  int sig;

  loop = clpd_loop_open(cfg->tick_interval_ms, cfg->cycle_min_interval_ms,
                        stderr);
  if (loop == NULL) {
    fprintf(stderr, "clpdaemon: cannot start the event loop: %s\n",
            strerror(errno));
    return 1;
  }
  ctx.engine = clpd_engine_create();
  ctx.max_rules = cfg->max_rules_per_cycle;
  if (ctx.engine == NULL) {
    fputs("clpdaemon: cannot create the CLIPS environment\n", stderr);
    clpd_loop_close(loop);
    return 1;
  }
  fprintf(stderr, "clpdaemon: %s: running\n", cfg->node_id);
  sig = clpd_loop_run(loop, run_cycle, &ctx);
  if (sig < 0)
    fprintf(stderr, "clpdaemon: event loop failed: %s\n", strerror(errno));
  else
    fprintf(stderr, "clpdaemon: received %s, stopping\n",
            sig == SIGINT ? "SIGINT" : "SIGTERM");
  clpd_engine_destroy(ctx.engine);
  clpd_loop_close(loop);
  return sig < 0 ? 1 : 0;
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
  if (rc != 0) {
    rc = 1;
  } else if (check) {
    printf("clpdaemon: %s: configuration OK\n", config_path);
  } else {
    rc = serve(&cfg);
  }
  clpd_config_free(&cfg);
  return rc;
}
