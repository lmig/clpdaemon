/* clpdaemon entry point: command line, configuration (T2), plugins (T6),
 * the database (T8d) and the event loop running engine cycles (T3) inside
 * the cycle transaction (T10a), which ends with the automatic
 * synchronization of the mapped facts (T10b); a rolled-back cycle's
 * instances are rehydrated when the next one starts (T10c). The rules end
 * instances with the lifecycle functions (T11a, src/orm_life.h). The clock
 * fact is asserted after Reset and modified when each cycle starts (T11b,
 * src/clock.h), and then idle instances are evicted (T11c,
 * clpd_orm_sync_evict_idle). */
#include "admin.h"
#include "clips_trace.h"
#include "clock.h"
#include "config.h"
#include "db.h"
#include "engine.h"
#include "fdcapture.h"
#include "ingest.h"
#include "log.h"
#include "loop.h"
#include "orm_ddl.h"
#include "orm_sync.h"
#include "orm_tx.h"
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
  clpd_clock *clock;
  clpd_admin *admin;
  clpd_loop *loop;
  clpd_orm_tx tx; /* the cycle transaction; no database: does nothing */
  clpd_orm_syncer *sync; /* NULL without a database */
  int max_rules;
  unsigned long cycles; /* run so far (admin `status`) */
} cycle_ctx;

/* Logs a failed clock update; the cycle goes on with the previous value. */
static void update_clock(clpd_clock *clock) {
  if (clpd_clock_update(clock) != 0)
    fputs("clpdaemon: cannot update the clock fact\n", clpd_log());
}

/* One cycle: the clock fact gets the wall clock first (T11b, with or
 * without a database), then, with a database, the instances idle for
 * idle_eviction_after are evicted (T11c, src/orm_sync.h, outside the
 * cycle transaction, nothing written) and the rest runs inside the
 * cycle transaction (T10a,
 * docs/database.md#cycle-transaction): reload the instances a rolled-back
 * cycle evicted, then begin (src/orm_sync.h, T10c), assert the facts queued so
 * far (docs/ingest.md; a rejected one is logged under the plugin or admin
 * socket that posted it), the plugins' cycle_begin, focus every package
 * module, Run with the rule limit, write the changes of the mapped facts
 * and commit (src/orm_sync.h), the plugins' cycle_end, then write the
 * partial output lines the cycle left. Run stops on an
 * evaluation error and leaves HaltExecution set (it clears the error flags
 * when the next Run starts), so the cycle is not ok when HaltExecution is
 * set after Run or when the cycle transaction was rolled back; a halt on a
 * stop signal (HaltRules), or a rule's (halt), is not an error and
 * commits. That halt is left to the loop, which reads the signal next.
 * When the reload loses the connection or the begin fails nothing else
 * runs (the queued facts wait) but the
 * admin listener is resumed, as after every cycle (a failed reconnect may
 * have freed the descriptor it waited for); when the begin or the commit
 * fails, the loop runs the next cycle after the backoff (clpd_loop_defer),
 * serving signals and the admin socket meanwhile. */
static void run_cycle(void *ctx) {
  cycle_ctx *c = ctx;
  Environment *env = clpd_engine_env(c->engine);
  int committed;
  update_clock(c->clock);
  clpd_orm_sync_evict_idle(c->sync, clpd_log());
  if (clpd_orm_sync_begin_cycle(c->sync, &c->tx, clpd_log()) != 0) {
    clpd_admin_resume(c->admin);
    clpd_loop_defer(c->loop, c->tx.retry_ms);
    return;
  }
  clpd_ingest_drain(env, c->packages, clpd_admin_post_rejected, c->plugins);
  clpd_plugins_cycle_begin(c->plugins);
  clpd_packages_focus(env, c->packages);
  clpd_engine_run_cycle(c->engine, c->max_rules);
  committed = clpd_orm_sync_end_cycle(c->sync, &c->tx, clpd_log()) == 0;
  clpd_plugins_cycle_end(c->plugins, committed && !GetHaltExecution(env));
  clpd_clips_trace_flush(c->ct);
  clpd_admin_resume(c->admin);
  c->cycles++;
  if (!committed) clpd_loop_defer(c->loop, c->tx.retry_ms);
}

static void reopen_trace(void *ctx) { clpd_trace_reopen(ctx, clpd_log()); }

static void wake_loop(void *loop) { clpd_loop_notify(loop); }

/* Runs engine cycles until a stop signal. Daemon messages go to the daemon
 * log stream (src/log.h; the original stderr during a capture). Returns the
 * exit code. */
static int serve(const clpd_config *cfg, clpd_engine *engine,
                 const clpd_packages *packages, clpd_plugins *plugins,
                 clpd_trace *trace, clpd_clips_trace *ct, clpd_clock *clock,
                 clpd_admin *admin, clpd_db *db, clpd_orm_syncer *sync) {
  FILE *log = clpd_log();
  cycle_ctx ctx;
  clpd_admin_state as;
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
  ctx.cycles = 0;
  as.env = clpd_engine_env(engine);
  as.packages = packages;
  as.node_id = cfg->node_id;
  as.cycles = &ctx.cycles;
  as.trace = trace;
  if (admin != NULL && clpd_admin_attach(admin, loop, &as, log) != 0) {
    clpd_loop_close(loop);
    return 1;
  }
  /* Facts posted before the loop existed (for example from a plugin's
   * init) are already queued: setting the notify function wakes the loop
   * for them, and every later post wakes it too. */
  clpd_ingest_set_notify(wake_loop, loop);
  ctx.engine = engine;
  ctx.packages = packages;
  ctx.plugins = plugins;
  ctx.ct = ct;
  ctx.clock = clock;
  ctx.admin = admin;
  ctx.loop = loop;
  clpd_orm_tx_init(&ctx.tx, db, cfg->database, cfg->cycle_retry_initial_ms,
                   cfg->cycle_retry_max_ms);
  ctx.sync = sync;
  ctx.max_rules = cfg->max_rules_per_cycle;
  fprintf(log, "clpdaemon: %s: running\n", cfg->node_id);
  sig = clpd_loop_run(loop, run_cycle, &ctx);
  if (sig < 0)
    fprintf(log, "clpdaemon: event loop failed: %s\n", strerror(errno));
  else
    fprintf(log, "clpdaemon: received %s, stopping\n",
            sig == SIGINT ? "SIGINT" : "SIGTERM");
  clpd_ingest_set_notify(NULL, NULL);
  clpd_admin_detach(admin);
  clpd_loop_close(loop);
  return sig < 0 ? 1 : 0;
}

/* The workflow_instance table (T10a), when a database is configured and a
 * package has an ORM mapping; never under --check, which writes nothing.
 * An existing table must have the daemon's columns (T11a). Returns 0, or
 * -1 after logging the failure. */
static int create_instance_table(clpd_db *db, const clpd_packages *p) {
  char msg[512];
  size_t i;
  int rc;
  for (i = 0; i < p->count && p->items[i].orm == NULL; i++) {
  }
  if (db == NULL || i == p->count) return 0;
  if (clpd_orm_tx_create_instance_table(db, msg, sizeof msg) != 0) {
    fprintf(clpd_log(),
            "clpdaemon: cannot create the workflow_instance table: %s\n",
            msg);
    return -1;
  }
  rc = clpd_orm_tx_check_instance_table(db, msg, sizeof msg);
  if (rc == 0) return 0;
  fprintf(clpd_log(),
          rc < 0 ? "clpdaemon: cannot check the workflow_instance table: %s\n"
                 : "clpdaemon: the workflow_instance table does not match the "
                   "daemon's: %s\n",
          msg);
  return -1;
}

/* Startup after the configuration (FR-2): trace files, environment, the
 * database when one is configured (docs/database.md) with the ORM DDL
 * functions (docs/orm.md), plugins (load and init), rule packages, then either the --check report or
 * the workflow_instance table (T10a) and the synchronization hooks (T10b),
 * the admin socket (FR-20, unless admin_socket = none), the trace hooks,
 * the plugins' after_load, Reset once, the clock fact (T11b; its assert is
 * traced, its per-cycle modifies are not) and the event loop; the socket file
 * is removed on every path after it was created. The synchronization hooks
 * come before after_load and Reset, so the facts asserted there (deffacts
 * included) are changes the first cycle writes.
 * --check opens the trace files too, so it reports paths the daemon could
 * not write. The trace hooks come after loading, so package load
 * diagnostics keep going to stderr, and before Reset, so the facts Reset
 * asserts are traced. With a trace, fds 1 and 2 are captured into it from
 * Reset until the plugins are shut down (FR-16); the daemon log stream
 * (src/log.h) is then the original stderr. Teardown (docs/plugins.md): the
 * plugins' shutdown while the environment exists, the environment, the
 * database, then the plugin libraries. */
static int start(const char *config_path, const clpd_config *cfg, int check) {
  clpd_packages packages = {NULL, 0};
  clpd_plugins *plugins = NULL;
  clpd_clips_trace *ct = NULL;
  clpd_clock *clock = NULL;
  clpd_admin *admin = NULL;
  clpd_db *db = NULL;
  clpd_orm_syncer *sync = NULL;
  /* Context of the ORM DDL functions: lives as long as the environment. */
  clpd_orm_ddl ddl = {NULL, &packages, cfg->orm_allow_drop, NULL};
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
  /* Before the plugins' init, also under --check. The message never holds
   * the spec, which may name a user or host but never a password. */
  if (cfg->database != NULL) {
    char msg[512];
    if (clpd_db_open(cfg->database, &db, msg, sizeof msg) != CLPD_DB_OK) {
      fprintf(clpd_log(), "clpdaemon: cannot open the database: %s\n", msg);
      clpd_engine_destroy(engine);
      clpd_trace_close(trace);
      return 1;
    }
    /* The ORM DDL functions exist only with a database (T9b), before the
     * packages that call them load; they see the packages loaded so far. */
    ddl.db = db;
    if (clpd_orm_ddl_register(clpd_engine_env(engine), &ddl) != 0) {
      fputs("clpdaemon: cannot define the ORM functions\n", clpd_log());
      clpd_engine_destroy(engine);
      clpd_db_close(db);
      clpd_trace_close(trace);
      return 1;
    }
  }
  rc = clpd_plugins_load(clpd_engine_env(engine), &cfg->plugins, cfg->node_id,
                         &plugins, clpd_log());
  if (rc == 0)
    rc = clpd_packages_load_ex(clpd_engine_env(engine), &cfg->packages,
                               cfg->database != NULL, &packages, clpd_log());
  if (rc != 0) {
    rc = 1;
  } else if (check) {
    printf("clpdaemon: %s: configuration OK\n", config_path);
  } else if (create_instance_table(db, &packages) != 0) {
    rc = 1;
  } else if (db != NULL && clpd_orm_sync_attach(clpd_engine_env(engine), db,
                                                &packages, &sync) != 0) {
    fputs("clpdaemon: cannot attach the ORM synchronization to the CLIPS "
          "environment\n",
          clpd_log());
    rc = 1;
  } else if (cfg->admin_socket != NULL &&
             (admin = clpd_admin_bind(cfg->admin_socket,
                                      cfg->admin_socket_mode,
                                      clpd_log())) == NULL) {
    rc = 1;
  } else if (clpd_clips_trace_attach(clpd_engine_env(engine), trace,
                                     cfg->watch_facts, cfg->watch_rules,
                                     &ct) != 0) {
    fputs("clpdaemon: cannot attach the trace to the CLIPS environment\n",
          clpd_log());
    rc = 1;
  } else {
    clpd_fdcapture *cap = NULL;
    /* The DML functions suspend it while loading and note the instances
     * they write (T10d); no rule has run yet. */
    ddl.sync = sync;
    /* The poison retry (T10e, docs/database.md#cycle-transaction). */
    clpd_orm_sync_set_isolate_after(sync, cfg->instance_isolate_after);
    /* The idle eviction (T11c, docs/orm.md#idle-eviction), before the
     * first fact is asserted. */
    clpd_orm_sync_set_idle_after(sync, cfg->idle_eviction_after_s * 1000LL);
    if ((clock = clpd_clock_create(clpd_engine_env(engine))) == NULL) {
      fputs("clpdaemon: cannot create the clock fact\n", clpd_log());
      rc = 1;
    } else if (clpd_trace_active(trace) &&
               (cap = clpd_fdcapture_start(trace, clpd_log())) == NULL) {
      rc = 1;
    } else {
      clpd_plugins_after_load(plugins);
      Reset(clpd_engine_env(engine));
      if (clpd_clock_update(clock) != 0) {
        fputs("clpdaemon: cannot assert the clock fact\n", clpd_log());
        rc = 1;
      } else {
        rc = serve(cfg, engine, &packages, plugins, trace, ct, clock, admin,
                   db, sync);
      }
      /* Shut down before the capture stops, so what the plugins print in
       * shutdown still reaches the trace; the call after this block is
       * then a no-op (it covers the paths that never got here). */
      clpd_plugins_shutdown(plugins);
      clpd_fdcapture_stop(cap);
    }
  }
  clpd_plugins_shutdown(plugins);
  /* After shutdown, which stops the plugins' threads: facts still queued
   * (posted after the last cycle, under --check, or before a startup
   * failure) are dropped, and the daemon log stream says how many. */
  {
    size_t dropped = clpd_ingest_clear();
    if (dropped > 0)
      fprintf(clpd_log(), "clpdaemon: %zu queued fact%s dropped at shutdown\n",
              dropped, dropped == 1 ? "" : "s");
  }
  clpd_admin_close(admin);
  clpd_clips_trace_detach(ct);
  clpd_orm_sync_detach(sync); /* before the packages it points into */
  ddl.sync = NULL;
  clpd_packages_free(&packages);
  clpd_clock_destroy(clock); /* before the environment */
  clpd_engine_destroy(engine);
  clpd_db_close(db); /* NULL when no database is configured */
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
