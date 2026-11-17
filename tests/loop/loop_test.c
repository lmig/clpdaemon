/* Tests for the event loop: hybrid scheduling with real timers, notification,
 * SIGHUP and stop signals (FR-3, FR-5, L25). Timing bounds are generous. */
#define _POSIX_C_SOURCE 200809L

#include "loop.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../common/expect.h"

#define MAX_CYCLES 16

typedef struct {
  clpd_loop *loop;
  int cycles;
  long long at[MAX_CYCLES];
  int script; /* which scenario drives the callback */
} probe;

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void on_cycle(void *ctx) {
  probe *p = ctx;
  if (p->cycles < MAX_CYCLES) p->at[p->cycles] = now_ms();
  p->cycles++;
  switch (p->script) {
  case 0: /* notify after cycle 1, stop on cycle 4 */
    if (p->cycles == 1) clpd_loop_notify(p->loop);
    if (p->cycles == 4) raise(SIGTERM);
    break;
  case 1: /* SIGHUP on cycle 1, SIGINT on cycle 3 */
    if (p->cycles == 1) raise(SIGHUP);
    if (p->cycles == 3) raise(SIGINT);
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

int main(void) {
  test_schedule_and_stop();
  test_sighup_continues_and_sigint_stops();
  test_pending_stop_before_run();
  if (failures != 0) {
    fprintf(stderr, "%d loop checks failed\n", failures);
    return 1;
  }
  puts("loop checks passed");
  return 0;
}
