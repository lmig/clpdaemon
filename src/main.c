/* clpdaemon entry point: command line, configuration (T2) and the event
 * loop running engine cycles (T3). */
#include "clips_trace.h"
#include "config.h"
#include "engine.h"
#include "fdcapture.h"
#include "loop.h"
#include "packages.h"
#include "trace.h"

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
  const clpd_packages *packages;
  clpd_clips_trace *ct;
  int max_rules;
} cycle_ctx;

/* One cycle: focus every package module, then Run with the rule limit and
 * write the partial output lines the cycle left. A halt on a stop signal is
 * left to the loop, which reads the signal next. */
static void run_cycle(void *ctx) {
  cycle_ctx *c = ctx;
  clpd_packages_focus(clpd_engine_env(c->engine), c->packages);
  clpd_engine_run_cycle(c->engine, c->max_rules);
  clpd_clips_trace_flush(c->ct);
}

typedef struct {
  clpd_trace *trace;
  FILE *log;
} reopen_ctx;

static void reopen_trace(void *ctx) {
  reopen_ctx *r = ctx;
  clpd_trace_reopen(r->trace, r->log);
}

/* Runs engine cycles until a stop signal. Daemon messages go to log (the
 * original stderr). Returns the exit code. */
static int serve(const clpd_config *cfg, clpd_engine *engine,
                 const clpd_packages *packages, clpd_trace *trace,
                 clpd_clips_trace *ct, FILE *log) {
  cycle_ctx ctx;
  reopen_ctx rctx;
  clpd_loop *loop;
  int sig;

  loop = clpd_loop_open(cfg->tick_interval_ms, cfg->cycle_min_interval_ms,
                        log);
  if (loop == NULL) {
    fprintf(log, "clpdaemon: cannot start the event loop: %s\n",
            strerror(errno));
    return 1;
  }
  rctx.trace = trace;
  rctx.log = log;
  clpd_loop_on_hup(loop, reopen_trace, &rctx);
  ctx.engine = engine;
  ctx.packages = packages;
  ctx.ct = ct;
  ctx.max_rules = cfg->max_rules_per_cycle;
  fprintf(log, "clpdaemon: %s: running\n", cfg->node_id);
  sig = clpd_loop_run(loop, run_cycle, &ctx);
  if (sig < 0)
    fprintf(log, "clpdaemon: event loop failed: %s\n", strerror(errno));
  else
    fprintf(log, "clpdaemon: received %s, stopping\n",
            sig == SIGINT ? "SIGINT" : "SIGTERM");
  clpd_loop_close(loop);
  return sig < 0 ? 1 : 0;
}

/* Startup after the configuration (FR-2): trace files, environment, rule
 * packages, then either the --check report or the trace hooks, Reset once
 * and the event loop. --check opens the trace files too, so it reports paths
 * the daemon could not write. The hooks come after loading, so package load
 * diagnostics keep going to stderr, and before Reset, so the facts Reset
 * asserts are traced. With a trace, fds 1 and 2 are captured into it from
 * Reset until the loop ends (FR-16); daemon messages then go to the
 * capture's log stream, the original stderr. */
static int start(const char *config_path, const clpd_config *cfg, int check) {
  clpd_packages packages;
  clpd_clips_trace *ct = NULL;
  clpd_engine *engine;
  clpd_trace *trace;
  int rc;

  trace = clpd_trace_open(cfg->trace_text, cfg->trace_jsonl, cfg->node_id,
                          stderr);
  if (trace == NULL) return 1;
  engine = clpd_engine_create();
  if (engine == NULL) {
    fputs("clpdaemon: cannot create the CLIPS environment\n", stderr);
    clpd_trace_close(trace);
    return 1;
  }
  rc = clpd_packages_load(clpd_engine_env(engine), &cfg->packages, &packages,
                          stderr);
  if (rc != 0) {
    rc = 1;
  } else if (check) {
    printf("clpdaemon: %s: configuration OK\n", config_path);
  } else if (clpd_clips_trace_attach(clpd_engine_env(engine), trace,
                                     cfg->watch_facts, cfg->watch_rules,
                                     &ct) != 0) {
    fputs("clpdaemon: cannot attach the trace to the CLIPS environment\n",
          stderr);
    rc = 1;
  } else {
    clpd_fdcapture *cap = NULL;
    FILE *log = stderr;
    if (clpd_trace_active(trace)) {
      cap = clpd_fdcapture_start(trace, stderr);
      if (cap != NULL) log = clpd_fdcapture_log(cap);
    }
    if (clpd_trace_active(trace) && cap == NULL) {
      rc = 1;
    } else {
      Reset(clpd_engine_env(engine));
      rc = serve(cfg, engine, &packages, trace, ct, log);
    }
    clpd_fdcapture_stop(cap);
  }
  clpd_clips_trace_detach(ct);
  clpd_packages_free(&packages);
  clpd_engine_destroy(engine);
  clpd_trace_close(trace);
  return rc;
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
  rc = rc != 0 ? 1 : start(config_path, &cfg, check);
  clpd_config_free(&cfg);
  return rc;
}
