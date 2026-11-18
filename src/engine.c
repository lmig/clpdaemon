#define _POSIX_C_SOURCE 200809L

#include "engine.h"

#include <signal.h>
#include <stdlib.h>

struct clpd_engine {
  Environment *env;
  int halted;
};

/* CLIPS calls periodic functions after every rule firing; a pending stop
 * signal asks the engine to halt. Run clears HaltRules on entry and exit,
 * so the outcome is kept in the wrapper. */
static void halt_check(Environment *env, void *ctx) {
  clpd_engine *e = ctx;
  sigset_t pending;
  if (e->halted || sigpending(&pending) != 0) return;
  if (sigismember(&pending, SIGTERM) == 1 || sigismember(&pending, SIGINT) == 1) {
    e->halted = 1;
    SetHaltRules(env, true);
  }
}

clpd_engine *clpd_engine_create(void) {
  clpd_engine *e = calloc(1, sizeof *e);
  if (e == NULL) return NULL;
  e->env = CreateEnvironment();
  if (e->env == NULL ||
      !AddPeriodicFunction(e->env, "clpd-halt-check", halt_check, 0, e)) {
    if (e->env != NULL) DestroyEnvironment(e->env);
    free(e);
    return NULL;
  }
  return e;
}

Environment *clpd_engine_env(clpd_engine *e) { return e->env; }

long long clpd_engine_run_cycle(clpd_engine *e, int max_rules) {
  e->halted = 0;
  return Run(e->env, max_rules);
}

int clpd_engine_halted(const clpd_engine *e) { return e->halted; }

void clpd_engine_destroy(clpd_engine *e) {
  if (e == NULL) return;
  DestroyEnvironment(e->env);
  free(e);
}
