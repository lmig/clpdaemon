/* Event loop (FR-3, FR-5): poll over signalfd (SIGTERM, SIGINT, SIGHUP),
 * a monotonic timerfd armed at the next cycle and an eventfd for wake-up
 * notifications. Single-threaded. */
#ifndef CLPD_LOOP_H
#define CLPD_LOOP_H

#include <stdio.h>

typedef struct clpd_loop clpd_loop;
typedef void clpd_cycle_fn(void *ctx);

/* Blocks SIGTERM, SIGINT and SIGHUP for the process, restores their default
 * disposition and creates the descriptors. A shell may start background jobs
 * with SIGINT ignored; POSIX leaves unspecified whether a blocked, ignored
 * signal stays pending (Linux keeps it), so the reset makes delivery to the
 * signalfd independent of the parent. Messages go to log. Returns NULL with
 * errno set on failure. */
clpd_loop *clpd_loop_open(int tick_ms, int min_ms, FILE *log);
/* Requests a cycle (subject to the minimum interval). Safe to call from a
 * cycle callback. */
void clpd_loop_notify(clpd_loop *l);
/* Runs cycles until SIGTERM or SIGINT arrives and returns that signal; a
 * pending stop signal wins over a due cycle. SIGHUP is logged and the loop
 * continues. Returns -1 with errno set on a system error, including a
 * failed read of the signal descriptor. */
int clpd_loop_run(clpd_loop *l, clpd_cycle_fn *cycle, void *ctx);
void clpd_loop_close(clpd_loop *l);

#endif
