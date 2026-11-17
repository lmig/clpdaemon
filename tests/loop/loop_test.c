/* Tests for the event loop: hybrid scheduling with real timers, notification,
 * SIGHUP and stop signals (FR-3, FR-5, L25). Timing bounds are generous. */
#define _POSIX_C_SOURCE 200809L

#include "engine.h"
#include "loop.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../common/expect.h"

#define MAX_CYCLES 16

typedef struct {
  clpd_loop *loop;
  int cycles;
  long long at[MAX_CYCLES];
  /* Scenario driving the callback: 0 notify + SIGTERM, 1 SIGHUP + SIGINT,
   * 2 nothing (no cycle expected), 3 overrun on cycle 2 + SIGTERM. */
  int script;
} probe;

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(long ms) {
  struct timespec t;
  t.tv_sec = ms / 1000;
  t.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep(&t, NULL);
}

static void on_cycle(void *ctx) {
  probe *p = ctx;
  if (p->cycles < MAX_CYCLES) p->at[p->cycles] = now_ms();
  p->cycles++;
  /* Cap: a lost stop signal ends the test with a failed cycle count
   * instead of a hang. */
  if (p->cycles == MAX_CYCLES) raise(SIGTERM);
  switch (p->script) {
  case 0: /* notify after cycle 1, stop on cycle 4 */
    if (p->cycles == 1) clpd_loop_notify(p->loop);
    if (p->cycles == 4) raise(SIGTERM);
    break;
  case 1: /* SIGHUP on cycle 1, SIGINT on cycle 3 */
    if (p->cycles == 1) raise(SIGHUP);
    if (p->cycles == 3) raise(SIGINT);
    break;
  case 3:
    if (p->cycles == 2) sleep_ms(130);
    if (p->cycles == 4) raise(SIGTERM);
    break;
  }
}

/* tick 60 ms, minimum 15 ms: cycle 1 at start, cycle 2 by notification
 * (about 15 ms later), cycles 3 and 4 by tick (about 60 ms apart). */
static void test_schedule_and_stop(void) {
  probe p;
  clpd_loop *loop = clpd_loop_open(60, 15, stderr);
  long long t0;
  EXPECT(loop != NULL);
  if (loop == NULL) return;
  memset(&p, 0, sizeof p);
  p.loop = loop;
  t0 = now_ms();
  EXPECT(clpd_loop_run(loop, on_cycle, &p) == SIGTERM);
  EXPECT(p.cycles == 4);
  EXPECT(p.at[0] - t0 < 30);
  EXPECT(p.at[1] - p.at[0] >= 15 && p.at[1] - p.at[0] < 50);
  EXPECT(p.at[2] - p.at[1] >= 60 && p.at[2] - p.at[1] < 120);
  EXPECT(p.at[3] - p.at[2] >= 60 && p.at[3] - p.at[2] < 120);
  clpd_loop_close(loop);
}

static void test_sighup_continues_and_sigint_stops(void) {
  probe p;
  char buf[256];
  size_t n;
  FILE *log = tmpfile();
  clpd_loop *loop = clpd_loop_open(20, 20, log);
  EXPECT(loop != NULL && log != NULL);
  if (loop == NULL || log == NULL) return;
  memset(&p, 0, sizeof p);
  p.loop = loop;
  p.script = 1;
  EXPECT(clpd_loop_run(loop, on_cycle, &p) == SIGINT);
  EXPECT(p.cycles == 3);
  clpd_loop_close(loop);
  rewind(log);
  n = fread(buf, 1, sizeof buf - 1, log);
  buf[n] = '\0';
  EXPECT(strcmp(buf, "clpdaemon: received SIGHUP, reopening trace files\n") == 0);
  fclose(log);
}

/* A stop signal that is already pending stops the loop before any cycle. */
static void test_pending_stop_before_run(void) {
  probe p;
  clpd_loop *loop = clpd_loop_open(20, 0, stderr);
  EXPECT(loop != NULL);
  if (loop == NULL) return;
  memset(&p, 0, sizeof p);
  p.loop = loop;
  p.script = 2;
  raise(SIGTERM);
  EXPECT(clpd_loop_run(loop, on_cycle, &p) == SIGTERM);
  EXPECT(p.cycles == 0);
  clpd_loop_close(loop);
}

/* tick 50 ms: cycle 2 takes 130 ms, so cycle 3 starts at once when it ends
 * and cycle 4 comes one tick later; the missed ticks are not replayed. */
static void test_overrun_no_burst(void) {
  probe p;
  clpd_loop *loop = clpd_loop_open(50, 10, stderr);
  EXPECT(loop != NULL);
  if (loop == NULL) return;
  memset(&p, 0, sizeof p);
  p.loop = loop;
  p.script = 3;
  EXPECT(clpd_loop_run(loop, on_cycle, &p) == SIGTERM);
  EXPECT(p.cycles == 4);
  EXPECT(p.at[2] - p.at[1] >= 130 && p.at[2] - p.at[1] < 250);
  EXPECT(p.at[3] - p.at[2] >= 50);
  clpd_loop_close(loop);
}

typedef struct {
  clpd_engine *engine;
  int cycles;
} unlimited;

static void unlimited_cycle(void *ctx) {
  unlimited *u = ctx;
  u->cycles++;
  if (u->cycles == MAX_CYCLES) raise(SIGTERM); /* cap, see on_cycle */
  clpd_engine_run_cycle(u->engine, -1);
}

/* A SIGTERM sent by another process while an unlimited Run fires a rule that
 * never stops halts the Run and then stops the loop (FR-5). */
static void test_external_stop_halts_unlimited_run(void) {
  unlimited u;
  pid_t me = getpid(), child;
  long long t0;
  clpd_loop *loop = clpd_loop_open(1000, 10, stderr);
  EXPECT(loop != NULL);
  if (loop == NULL) return;
  u.cycles = 0;
  u.engine = clpd_engine_create();
  EXPECT(u.engine != NULL);
  if (u.engine == NULL) return;
  EXPECT(Build(clpd_engine_env(u.engine),
               "(defrule step ?f <- (tick ?n) => (retract ?f) "
               "(assert (tick (+ ?n 1))))") == BE_NO_ERROR);
  EXPECT(AssertString(clpd_engine_env(u.engine), "(tick 0)") != NULL);
  child = fork();
  if (child == 0) {
    sleep_ms(200);
    kill(me, SIGTERM);
    _exit(0);
  }
  EXPECT(child > 0);
  t0 = now_ms();
  EXPECT(clpd_loop_run(loop, unlimited_cycle, &u) == SIGTERM);
  EXPECT(u.cycles == 1);
  EXPECT(clpd_engine_halted(u.engine) == 1);
  EXPECT(now_ms() - t0 >= 150 && now_ms() - t0 < 2000);
  if (child > 0) waitpid(child, NULL, 0);
  clpd_engine_destroy(u.engine);
  clpd_loop_close(loop);
}

int main(void) {
  test_schedule_and_stop();
  test_sighup_continues_and_sigint_stops();
  test_pending_stop_before_run();
  test_overrun_no_burst();
  test_external_stop_halts_unlimited_run();
  if (failures != 0) {
    fprintf(stderr, "%d loop checks failed\n", failures);
    return 1;
  }
  puts("loop checks passed");
  return 0;
}
