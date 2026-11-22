/* Unit tests for the engine wrapper: cycle limit and halt on a pending
 * stop signal (FR-4, FR-5, L25). */
#define _POSIX_C_SOURCE 200809L

#include "engine.h"

#include <signal.h>
#include <time.h>

#include "../common/expect.h"

/* A rule that fires forever: each firing replaces (tick n) by (tick n+1). */
static clpd_engine *looping_engine(void) {
  clpd_engine *e = clpd_engine_create();
  Environment *env;
  EXPECT(e != NULL);
  if (e == NULL) return NULL;
  env = clpd_engine_env(e);
  EXPECT(Build(env, "(defrule step ?f <- (tick ?n) => (retract ?f) "
                    "(assert (tick (+ ?n 1))))") == BE_NO_ERROR);
  EXPECT(AssertString(env, "(tick 0)") != NULL);
  return e;
}

static void test_cycle_limit(void) {
  clpd_engine *e = looping_engine();
  if (e == NULL) return;
  EXPECT(clpd_engine_run_cycle(e, 5) == 5);
  EXPECT(clpd_engine_halted(e) == 0);
  EXPECT(clpd_engine_run_cycle(e, 3) == 3); /* working memory persists */
  clpd_engine_destroy(e);
}

static void test_no_rules_no_firings(void) {
  clpd_engine *e = clpd_engine_create();
  EXPECT(e != NULL);
  if (e == NULL) return;
  EXPECT(clpd_engine_run_cycle(e, -1) == 0);
  EXPECT(clpd_engine_halted(e) == 0);
  clpd_engine_destroy(e);
}

/* With SIGTERM (or SIGINT) pending, an unlimited cycle on a rule that never
 * stops returns after the first firing and reports the halt. */
static void test_pending_stop_signal_halts(int sig) {
  sigset_t set;
  struct timespec zero = {0, 0};
  clpd_engine *e;

  sigemptyset(&set);
  sigaddset(&set, sig);
  sigprocmask(SIG_BLOCK, &set, NULL);
  e = looping_engine();
  if (e == NULL) return;
  raise(sig);
  EXPECT(clpd_engine_run_cycle(e, -1) == 1);
  EXPECT(clpd_engine_halted(e) == 1);
  /* The signal stays pending for the event loop to read. */
  EXPECT(sigtimedwait(&set, NULL, &zero) == sig);
  EXPECT(clpd_engine_run_cycle(e, 2) == 2); /* no signal: runs again */
  EXPECT(clpd_engine_halted(e) == 0);
  clpd_engine_destroy(e);
}

static void test_pending_sighup_does_not_halt(void) {
  sigset_t set;
  struct timespec zero = {0, 0};
  clpd_engine *e;

  sigemptyset(&set);
  sigaddset(&set, SIGHUP);
  sigprocmask(SIG_BLOCK, &set, NULL);
  e = looping_engine();
  if (e == NULL) return;
  raise(SIGHUP);
  EXPECT(clpd_engine_run_cycle(e, 50) == 50);
  EXPECT(clpd_engine_halted(e) == 0);
  EXPECT(sigtimedwait(&set, NULL, &zero) == SIGHUP);
  clpd_engine_destroy(e);
}

/* The core defines MAIN::clpd-clock with the environment (T11b), exported
 * so a module can import it; no clock fact until the daemon asserts it. */
static void test_clock_template_defined(void) {
  clpd_engine *e = clpd_engine_create();
  Environment *env;
  EXPECT(e != NULL);
  if (e == NULL) return;
  env = clpd_engine_env(e);
  EXPECT(FindDeftemplate(env, "MAIN::clpd-clock") != NULL);
  EXPECT(Build(env, "(defmodule p@1 (import MAIN deftemplate clpd-clock))") ==
         BE_NO_ERROR);
  EXPECT(GetNextFact(env, NULL) == NULL);
  clpd_engine_destroy(e);
}

int main(void) {
  test_clock_template_defined();
  test_cycle_limit();
  test_no_rules_no_firings();
  test_pending_stop_signal_halts(SIGTERM);
  test_pending_stop_signal_halts(SIGINT);
  test_pending_sighup_does_not_halt();
  if (failures != 0) {
    fprintf(stderr, "%d engine checks failed\n", failures);
    return 1;
  }
  puts("engine checks passed");
  return 0;
}
