/* Tests for the event loop: hybrid scheduling with real timers, notification,
 * SIGHUP and stop signals (FR-3, FR-5, L25). Timing bounds are generous. */
#define _POSIX_C_SOURCE 200809L

#include "engine.h"
#include "loop.h"

#include <errno.h>
#include <poll.h>
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
    if (p->cycles == 2) sleep_ms(400);
    if (p->cycles == 4) raise(SIGTERM);
    break;
  }
}

/* tick 400 ms, minimum 15 ms: cycle 1 at start, cycle 2 by notification
 * (about 15 ms later), cycles 3 and 4 by tick (about 400 ms apart). The
 * upper bounds leave hundreds of milliseconds for loaded or sanitized hosts
 * while still telling a notification from a tick. */
static void test_schedule_and_stop(void) {
  probe p;
  clpd_loop *loop = clpd_loop_open(400, 15, stderr);
  long long t0;
  EXPECT(loop != NULL);
  if (loop == NULL) return;
  memset(&p, 0, sizeof p);
  p.loop = loop;
  t0 = now_ms();
  EXPECT(clpd_loop_run(loop, on_cycle, &p) == SIGTERM);
  EXPECT(p.cycles == 4);
  EXPECT(p.at[0] - t0 < 300);
  /* Lower bounds allow 1 ms: the loop truncates its cycle start to whole
   * milliseconds before the callback takes its own (possibly later)
   * timestamp, so a correct interval can measure 1 ms short here. */
  EXPECT(p.at[1] - p.at[0] >= 15 - 1 && p.at[1] - p.at[0] < 300);
  EXPECT(p.at[2] - p.at[1] >= 400 - 1 && p.at[2] - p.at[1] < 1000);
  EXPECT(p.at[3] - p.at[2] >= 400 - 1 && p.at[3] - p.at[2] < 1000);
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

/* tick 300 ms: cycle 2 takes 400 ms, so cycle 3 starts at once when it ends
 * (about 400 ms after cycle 2 starts; waiting for the next tick of the grid
 * would give about 600 ms, counting the tick from the end about 700 ms) and
 * cycle 4 comes one tick later; the missed tick is not replayed. */
static void test_overrun_no_burst(void) {
  probe p;
  clpd_loop *loop = clpd_loop_open(300, 10, stderr);
  EXPECT(loop != NULL);
  if (loop == NULL) return;
  memset(&p, 0, sizeof p);
  p.loop = loop;
  p.script = 3;
  EXPECT(clpd_loop_run(loop, on_cycle, &p) == SIGTERM);
  EXPECT(p.cycles == 4);
  EXPECT(p.at[2] - p.at[1] >= 400 && p.at[2] - p.at[1] < 550);
  EXPECT(p.at[3] - p.at[2] >= 300 - 1); /* 1 ms skew, see above */
  clpd_loop_close(loop);
}

typedef struct {
  clpd_engine *engine;
  int cycles;
} unlimited;

static void unlimited_cycle(void *ctx) {
  unlimited *u = ctx;
  u->cycles++;
  /* No cycle cap helps here: if the halt regresses, the first unlimited Run
   * never returns. The ctest TIMEOUT is the guard for this test. */
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
  if (u.engine == NULL) {
    clpd_loop_close(loop);
    return;
  }
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
  if (child < 0) { /* nobody would send the SIGTERM: do not run */
    clpd_engine_destroy(u.engine);
    clpd_loop_close(loop);
    return;
  }
  t0 = now_ms();
  EXPECT(clpd_loop_run(loop, unlimited_cycle, &u) == SIGTERM);
  EXPECT(u.cycles == 1);
  EXPECT(clpd_engine_halted(u.engine) == 1);
  EXPECT(now_ms() - t0 >= 150 && now_ms() - t0 < 2000);
  waitpid(child, NULL, 0);
  clpd_engine_destroy(u.engine);
  clpd_loop_close(loop);
}

/* --- registered fds (T7c) ------------------------------------------------ */

typedef struct {
  clpd_loop *loop;
  char order[16]; /* one letter per callback, in call order */
  size_t n;
  short revents;
  int cycles;
  int a, b, w; /* fds */
} fdprobe;

static void note(fdprobe *p, char c, short revents) {
  if (p->n < sizeof p->order - 1) p->order[p->n++] = c;
  p->revents = revents;
  if (p->n >= sizeof p->order - 1) raise(SIGTERM); /* cap: no hang */
}

static void fd_cycle(void *ctx) {
  fdprobe *p = ctx;
  p->cycles++;
  if (p->cycles == MAX_CYCLES) raise(SIGTERM);
}

static void eat(int fd) {
  char c;
  if (read(fd, &c, 1) < 0) {
  }
}

static void cb_stop(void *ctx, int fd, short revents) {
  fdprobe *p = ctx;
  eat(fd);
  note(p, 'S', revents);
  raise(SIGTERM);
}

static void cb_never(void *ctx, int fd, short revents) {
  fdprobe *p = ctx;
  (void)fd;
  note(p, 'X', revents);
}

static void cb_b2(void *ctx, int fd, short revents) {
  fdprobe *p = ctx;
  eat(fd);
  note(p, 'C', revents);
  raise(SIGTERM);
}

/* Removes b and registers its fd again with cb_b2: neither the old nor the
 * new registration runs in this iteration; cb_b2 runs in the next one. */
static void cb_a(void *ctx, int fd, short revents) {
  fdprobe *p = ctx;
  eat(fd);
  note(p, 'A', revents);
  EXPECT(clpd_loop_remove_fd(p->loop, p->b) == 0);
  EXPECT(clpd_loop_add_fd(p->loop, p->b, POLLIN, cb_b2, p) == 0);
}

static void cb_write(void *ctx, int fd, short revents) {
  fdprobe *p = ctx;
  (void)fd;
  note(p, 'W', revents);
  raise(SIGTERM);
}

/* Arms the write end for POLLOUT from the first cycle. */
static void arm_write_cycle(void *ctx) {
  fdprobe *p = ctx;
  fd_cycle(ctx);
  if (p->cycles == 1) EXPECT(clpd_loop_set_events(p->loop, p->w, POLLOUT) == 0);
}

/* Runs the loop under a 5 s alarm: a lost wake-up kills the test with
 * SIGALRM instead of waiting for a one-minute tick. */
static int run_guarded(clpd_loop *loop, clpd_cycle_fn *cycle, void *ctx) {
  int sig;
  alarm(5);
  sig = clpd_loop_run(loop, cycle, ctx);
  alarm(0);
  return sig;
}

static void dummy(void *ctx, int fd, short revents) {
  (void)ctx; (void)fd; (void)revents;
}

static void test_fd_registration_errors(void) {
  clpd_loop *loop = clpd_loop_open(1000, 10, stderr);
  int i, fds[2];
  EXPECT(loop != NULL && pipe(fds) == 0);
  if (loop == NULL) return;
  errno = 0;
  EXPECT(clpd_loop_add_fd(loop, -1, POLLIN, dummy, NULL) == -1 && errno == EINVAL);
  errno = 0;
  EXPECT(clpd_loop_add_fd(loop, fds[0], POLLIN, NULL, NULL) == -1 && errno == EINVAL);
  EXPECT(clpd_loop_add_fd(loop, fds[0], POLLIN, dummy, NULL) == 0);
  errno = 0;
  EXPECT(clpd_loop_add_fd(loop, fds[0], POLLIN, dummy, NULL) == -1 && errno == EEXIST);
  errno = 0;
  EXPECT(clpd_loop_set_events(loop, fds[1], POLLOUT) == -1 && errno == ENOENT);
  errno = 0;
  EXPECT(clpd_loop_remove_fd(loop, fds[1]) == -1 && errno == ENOENT);
  /* The bound: CLPD_LOOP_FDS_MAX registrations (numbers need not be open). */
  for (i = 1; i < CLPD_LOOP_FDS_MAX; i++)
    EXPECT(clpd_loop_add_fd(loop, 1000 + i, POLLIN, dummy, NULL) == 0);
  errno = 0;
  EXPECT(clpd_loop_add_fd(loop, 2000, POLLIN, dummy, NULL) == -1 && errno == ENOSPC);
  EXPECT(clpd_loop_remove_fd(loop, 1001) == 0);
  EXPECT(clpd_loop_add_fd(loop, 2000, POLLIN, dummy, NULL) == 0);
  EXPECT(clpd_loop_remove_fd(loop, fds[0]) == 0);
  errno = 0;
  EXPECT(clpd_loop_remove_fd(loop, fds[0]) == -1 && errno == ENOENT);
  clpd_loop_close(loop);
  close(fds[0]);
  close(fds[1]);
}

/* A readable fd wakes a loop whose next tick is a minute away. */
static void test_fd_readable_callback(void) {
  fdprobe p;
  int fds[2];
  long long t0;
  clpd_loop *loop = clpd_loop_open(60000, 10, stderr);
  EXPECT(loop != NULL && pipe(fds) == 0);
  if (loop == NULL) return;
  memset(&p, 0, sizeof p);
  p.loop = loop;
  EXPECT(clpd_loop_add_fd(loop, fds[0], POLLIN, cb_stop, &p) == 0);
  EXPECT(write(fds[1], "x", 1) == 1);
  t0 = now_ms();
  EXPECT(run_guarded(loop, fd_cycle, &p) == SIGTERM);
  EXPECT(now_ms() - t0 < 2000);
  EXPECT(strcmp(p.order, "S") == 0);
  EXPECT(p.revents & POLLIN);
  EXPECT(p.cycles == 1); /* the start cycle only */
  clpd_loop_close(loop);
  close(fds[0]);
  close(fds[1]);
}

/* Events 0 polls nothing but errors; set_events turns POLLOUT on. */
static void test_fd_set_events(void) {
  fdprobe p;
  int fds[2];
  clpd_loop *loop = clpd_loop_open(60000, 10, stderr);
  EXPECT(loop != NULL && pipe(fds) == 0);
  if (loop == NULL) return;
  memset(&p, 0, sizeof p);
  p.loop = loop;
  p.w = fds[1];
  EXPECT(clpd_loop_add_fd(loop, fds[1], 0, cb_write, &p) == 0);
  EXPECT(run_guarded(loop, arm_write_cycle, &p) == SIGTERM);
  EXPECT(strcmp(p.order, "W") == 0);
  EXPECT(p.revents & POLLOUT);
  EXPECT(p.cycles == 1);
  clpd_loop_close(loop);
  close(fds[0]);
  close(fds[1]);
}

static void test_fd_remove_and_add_in_callback(void) {
  fdprobe p;
  int a[2], b[2];
  clpd_loop *loop = clpd_loop_open(60000, 10, stderr);
  EXPECT(loop != NULL && pipe(a) == 0 && pipe(b) == 0);
  if (loop == NULL) return;
  memset(&p, 0, sizeof p);
  p.loop = loop;
  p.a = a[0];
  p.b = b[0];
  EXPECT(clpd_loop_add_fd(loop, a[0], POLLIN, cb_a, &p) == 0);
  EXPECT(clpd_loop_add_fd(loop, b[0], POLLIN, cb_never, &p) == 0);
  EXPECT(write(a[1], "x", 1) == 1 && write(b[1], "x", 1) == 1);
  EXPECT(run_guarded(loop, fd_cycle, &p) == SIGTERM);
  EXPECT(strcmp(p.order, "AC") == 0);
  clpd_loop_close(loop);
  close(a[0]);
  close(a[1]);
  close(b[0]);
  close(b[1]);
}

/* A pending stop signal wins over a readable fd. */
static void test_fd_stop_wins(void) {
  fdprobe p;
  int fds[2];
  clpd_loop *loop = clpd_loop_open(60000, 10, stderr);
  EXPECT(loop != NULL && pipe(fds) == 0);
  if (loop == NULL) return;
  memset(&p, 0, sizeof p);
  p.loop = loop;
  EXPECT(clpd_loop_add_fd(loop, fds[0], POLLIN, cb_never, &p) == 0);
  EXPECT(write(fds[1], "x", 1) == 1);
  raise(SIGTERM);
  EXPECT(run_guarded(loop, fd_cycle, &p) == SIGTERM);
  EXPECT(p.n == 0 && p.cycles == 0);
  clpd_loop_close(loop);
  close(fds[0]);
  close(fds[1]);
}

/* clpd_loop_defer (T10a). */
typedef struct {
  clpd_loop *loop;
  int cycles, fd_calls, rd, wr;
  long long at[MAX_CYCLES], fd_at;
} deferprobe;

/* Cycle 1 defers 150 ms and notifies (the notification must wait);
 * cycle 2 does nothing, so cycle 3 comes on the 20 ms tick; cycle 3
 * stops. */
static void defer_cycle(void *ctx) {
  deferprobe *p = ctx;
  if (p->cycles < MAX_CYCLES) p->at[p->cycles] = now_ms();
  p->cycles++;
  if (p->cycles == MAX_CYCLES) raise(SIGTERM);
  if (p->cycles == 1) {
    clpd_loop_defer(p->loop, 150);
    clpd_loop_notify(p->loop);
  }
  if (p->cycles == 3) raise(SIGTERM);
}

static void test_defer(void) {
  deferprobe p;
  clpd_loop *loop = clpd_loop_open(20, 10, stderr);
  EXPECT(loop != NULL);
  if (loop == NULL) return;
  memset(&p, 0, sizeof p);
  p.loop = loop;
  EXPECT(clpd_loop_run(loop, defer_cycle, &p) == SIGTERM);
  EXPECT(p.cycles == 3);
  EXPECT(p.at[1] - p.at[0] >= 150 - 1 && p.at[1] - p.at[0] < 400);
  EXPECT(p.at[2] - p.at[1] >= 20 - 1 && p.at[2] - p.at[1] < 150);
  clpd_loop_close(loop);
}

static void defer_fd_cb(void *ctx, int fd, short revents) {
  deferprobe *p = ctx;
  char c;
  (void)revents;
  if (read(fd, &c, 1) != 1) return;
  p->fd_calls++;
  p->fd_at = now_ms();
  /* Served again and again for 100 ms (ten ticks), then a stop during
   * the wait. */
  if (p->fd_at - p->at[0] >= 100) raise(SIGTERM);
  else if (write(p->wr, "y", 1) != 1) p->fd_calls = -1000000;
}

/* Cycle 1 defers 5 s and makes a registered fd readable: the fd is served
 * during the wait (for 100 ms, ten 10 ms ticks that must not start a
 * cycle), and a SIGTERM raised there ends the loop at once. */
static void defer_fd_cycle(void *ctx) {
  deferprobe *p = ctx;
  if (p->cycles < MAX_CYCLES) p->at[p->cycles] = now_ms();
  p->cycles++;
  if (p->cycles == MAX_CYCLES) raise(SIGTERM);
  if (p->cycles == 1) {
    clpd_loop_defer(p->loop, 5000);
    if (write(p->wr, "x", 1) != 1) p->fd_calls = 100;
  }
}

static void test_defer_serves_fds_and_stops(void) {
  deferprobe p;
  int fds[2];
  long long t0;
  clpd_loop *loop = clpd_loop_open(10, 0, stderr);
  EXPECT(loop != NULL && pipe(fds) == 0);
  if (loop == NULL) return;
  memset(&p, 0, sizeof p);
  p.loop = loop;
  p.rd = fds[0];
  p.wr = fds[1];
  EXPECT(clpd_loop_add_fd(loop, fds[0], POLLIN, defer_fd_cb, &p) == 0);
  t0 = now_ms();
  EXPECT(clpd_loop_run(loop, defer_fd_cycle, &p) == SIGTERM);
  EXPECT(p.cycles == 1);
  EXPECT(p.fd_calls >= 2);
  EXPECT(p.fd_at - p.at[0] >= 100);
  EXPECT(now_ms() - t0 < 2000);
  clpd_loop_close(loop);
  close(fds[0]);
  close(fds[1]);
}

int main(void) {
  test_schedule_and_stop();
  test_defer();
  test_defer_serves_fds_and_stops();
  test_sighup_continues_and_sigint_stops();
  test_pending_stop_before_run();
  test_overrun_no_burst();
  test_external_stop_halts_unlimited_run();
  test_fd_registration_errors();
  test_fd_readable_callback();
  test_fd_set_events();
  test_fd_remove_and_add_in_callback();
  test_fd_stop_wins();
  if (failures != 0) {
    fprintf(stderr, "%d loop checks failed\n", failures);
    return 1;
  }
  puts("loop checks passed");
  return 0;
}
