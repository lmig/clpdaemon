/* clpdaemon entry point: command line, configuration (T2), plugins (T6)
 * and the event loop running engine cycles (T3). */
#include "clips_trace.h"
#include "config.h"
#include "engine.h"
#include "fdcapture.h"
#include "log.h"
#include "loop.h"
#include "packages.h"
#include "plugins.h"
#include "trace.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static const char usage[] =
    "usage: clpdaemon --config <file> [--foreground] [--check]\n";

static int usage_error(const char *fmt, const char *arg) {
  FILE *log = clpd_log();
  fputs("clpdaemon: ", log);
  fprintf(log, fmt, arg);
  fputc('\n', log);
  fputs(usage, log);
  return 2;
}

typedef struct {
  clpd_engine *engine;
  const clpd_packages *packages;
  clpd_plugins *plugins;
  clpd_clips_trace *ct;
  int max_rules;
} cycle_ctx;

/* One cycle: the plugins' cycle_begin, focus every package module, Run with
 * the rule limit, the plugins' cycle_end, then write the partial output
 * lines the cycle left. Run stops on an evaluation error and leaves
 * HaltExecution set (it clears the error flags when the next Run starts),
 * so the cycle is not ok exactly when HaltExecution is set after Run; a halt
 * on a stop signal (HaltRules) is not an error. That halt is left to the
 * loop, which reads the signal next. */
static void run_cycle(void *ctx) {
  cycle_ctx *c = ctx;
  Environment *env = clpd_engine_env(c->engine);
  clpd_plugins_cycle_begin(c->plugins);
  clpd_packages_focus(env, c->packages);
  clpd_engine_run_cycle(c->engine, c->max_rules);
  clpd_plugins_cycle_end(c->plugins, !GetHaltExecution(env));
  clpd_clips_trace_flush(c->ct);
}

static void reopen_trace(void *ctx) { clpd_trace_reopen(ctx, clpd_log()); }

/* Runs engine cycles until a stop signal. Daemon messages go to the daemon
 * log stream (src/log.h; the original stderr during a capture). Returns the
 * exit code. */
static int serve(const clpd_config *cfg, clpd_engine *engine,
                 const clpd_packages *packages, clpd_plugins *plugins,
                 clpd_trace *trace, clpd_clips_trace *ct) {
  FILE *log = clpd_log();
  cycle_ctx ctx;
  clpd_loop *loop;
  int sig;

  loop = clpd_loop_open(cfg->tick_interval_ms, cfg->cycle_min_interval_ms,
                        log);
  if (loop == NULL) {
    fprintf(log, "clpdaemon: cannot start the event loop: %s\n",
            strerror(errno));
    return 1;
  }
  clpd_loop_on_hup(loop, reopen_trace, trace);
  ctx.engine = engine;
  ctx.packages = packages;
  ctx.plugins = plugins;
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

/* Startup after the configuration (FR-2): trace files, environment,
 * plugins (load and init), rule packages, then either the --check report or
 * the trace hooks, the plugins' after_load, Reset once and the event loop.
 * --check opens the trace files too, so it reports paths the daemon could
 * not write. The trace hooks come after loading, so package load
 * diagnostics keep going to stderr, and before Reset, so the facts Reset
 * asserts are traced. With a trace, fds 1 and 2 are captured into it from
 * Reset until the plugins are shut down (FR-16); the daemon log stream
 * (src/log.h) is then the original stderr. Teardown (docs/plugins.md): the
 * plugins' shutdown while the environment exists, the environment, then the
 * plugin libraries. */
static int start(const char *config_path, const clpd_config *cfg, int check) {
  clpd_packages packages = {NULL, 0};
  clpd_plugins *plugins = NULL;
  clpd_clips_trace *ct = NULL;
  clpd_engine *engine;
  clpd_trace *trace;
  int rc;

  /* Before the plugins load (T6c): threads a plugin starts in init inherit
   * the blocked mask, so a stop signal reaches only the loop's signalfd,
   * and one that arrives during startup waits for the loop instead of
   * killing the process before the plugins' shutdown. --check never runs
   * the loop and keeps the default dispositions. */
  if (!check && clpd_loop_block_signals() != 0) {
    fprintf(clpd_log(), "clpdaemon: cannot block the stop signals: %s\n",
            strerror(errno));
    return 1;
  }
  trace = clpd_trace_open(cfg->trace_text, cfg->trace_jsonl, cfg->node_id,
                          clpd_log());
  if (trace == NULL) return 1;
  engine = clpd_engine_create();
  if (engine == NULL) {
    fputs("clpdaemon: cannot create the CLIPS environment\n", clpd_log());
    clpd_trace_close(trace);
    return 1;
  }
  rc = clpd_plugins_load(clpd_engine_env(engine), &cfg->plugins, cfg->node_id,
                         &plugins, clpd_log());
  if (rc == 0)
    rc = clpd_packages_load(clpd_engine_env(engine), &cfg->packages,
                            &packages, clpd_log());
  if (rc != 0) {
    rc = 1;
  } else if (check) {
    printf("clpdaemon: %s: configuration OK\n", config_path);
  } else if (clpd_clips_trace_attach(clpd_engine_env(engine), trace,
                                     cfg->watch_facts, cfg->watch_rules,
                                     &ct) != 0) {
    fputs("clpdaemon: cannot attach the trace to the CLIPS environment\n",
          clpd_log());
    rc = 1;
  } else {
    clpd_fdcapture *cap = NULL;
    if (clpd_trace_active(trace) &&
        (cap = clpd_fdcapture_start(trace, clpd_log())) == NULL) {
      rc = 1;
    } else {
      clpd_plugins_after_load(plugins);
      Reset(clpd_engine_env(engine));
      rc = serve(cfg, engine, &packages, plugins, trace, ct);
      /* Shut down before the capture stops, so what the plugins print in
       * shutdown still reaches the trace; the call after this block is
       * then a no-op (it covers the paths that never got here). */
      clpd_plugins_shutdown(plugins);
      clpd_fdcapture_stop(cap);
    }
  }
  clpd_plugins_shutdown(plugins);
  clpd_clips_trace_detach(ct);
  clpd_packages_free(&packages);
  clpd_engine_destroy(engine);
  clpd_plugins_close(plugins);
  clpd_trace_close(trace);
  return rc;
}

int main(int argc, char **argv) {
  const char *config_path = NULL;
  int check = 0, i, rc;
  clpd_config cfg;

  /* Before any other use of stdout (setvbuf must come first): lines written
   * with printf, by the engine or a plugin, reach the trace capture pipe as
   * they end (docs/trace.md). */
  setvbuf(stdout, NULL, _IOLBF, 0);
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

  rc = clpd_config_load(config_path, &cfg, clpd_log());
  rc = rc != 0 ? 1 : start(config_path, &cfg, check);
  clpd_config_free(&cfg);
  return rc;
}
