/* Event loop (FR-3, FR-5): poll over signalfd (SIGTERM, SIGINT, SIGHUP),
 * a monotonic timerfd armed at the next cycle and an eventfd for wake-up
 * notifications. The loop runs on the main thread; with a trace, the fd
 * capture reader thread (fdcapture.h) blocks every signal, so the signals
 * blocked here reach only the signalfd. */
#ifndef CLPD_LOOP_H
#define CLPD_LOOP_H

#include <stdio.h>

typedef struct clpd_loop clpd_loop;
typedef void clpd_cycle_fn(void *ctx);
typedef void clpd_hup_fn(void *ctx);
/* Called on the loop thread when a registered fd is ready; revents is the
 * poll result (POLLIN, POLLOUT, POLLHUP, POLLERR...). */
typedef void clpd_fd_fn(void *ctx, int fd, short revents);

/* Registered fds per loop (the admin socket uses 1 + 16). */
#define CLPD_LOOP_FDS_MAX 32

/* Blocks SIGTERM, SIGINT and SIGHUP in the calling thread and restores their
 * default disposition. A shell may start background jobs with SIGINT
 * ignored; POSIX leaves unspecified whether a blocked, ignored signal stays
 * pending (Linux keeps it), so the reset makes delivery to the signalfd
 * independent of the parent. Threads created afterwards inherit the mask, so
 * call it before any thread that must not take these signals is started
 * (the daemon calls it before loading plugins). A signal that arrives
 * before clpd_loop_open stays pending and is read by the loop. Returns -1
 * with errno set on failure. */
int clpd_loop_block_signals(void);
/* clpd_loop_block_signals (idempotent) and creates the descriptors.
 * Messages go to log. Returns NULL with errno set on failure. */
clpd_loop *clpd_loop_open(int tick_ms, int min_ms, FILE *log);
/* Requests a cycle (subject to the minimum interval). Safe to call from a
 * cycle callback, and from any thread while the loop is open: it only
 * writes to the loop's eventfd, which is thread-safe (the ingest queue
 * calls it from the posting thread, src/ingest.h). */
void clpd_loop_notify(clpd_loop *l);
/* From a cycle callback (T10a, the retry of a failed cycle transaction):
 * the next cycle starts delay_ms after this call, neither sooner (a tick
 * or a notification in between waits; the notification is kept for that
 * cycle) nor later. Meanwhile the loop keeps reading the signals (a stop
 * still ends it at once) and serving the registered fds. The next cycle
 * runs on the normal schedule again unless it defers too. A delay_ms
 * below 1 means the next loop iteration. */
void clpd_loop_defer(clpd_loop *l, int delay_ms);
/* Called on SIGHUP after its message is logged (trace reopen). */
void clpd_loop_on_hup(clpd_loop *l, clpd_hup_fn *fn, void *ctx);
/* Registers fd, polled for events (POLLIN and/or POLLOUT) while the loop
 * runs: when it is ready, fn(ctx, fd, revents) runs on the loop thread,
 * after the signals are read (a pending stop still wins) and before a due
 * cycle. Returns 0, or -1 with errno EINVAL (fd < 0 or fn NULL), EEXIST
 * (fd already registered) or ENOSPC (CLPD_LOOP_FDS_MAX registered). The
 * loop never closes fd. These three calls may be made from a callback: an
 * fd added there is first polled in the next iteration, one removed there
 * gets no further callback, also in the current iteration. */
int clpd_loop_add_fd(clpd_loop *l, int fd, short events, clpd_fd_fn *fn,
                     void *ctx);
/* Replaces the polled events of a registered fd. -1 with ENOENT if it is
 * not registered. */
int clpd_loop_set_events(clpd_loop *l, int fd, short events);
/* Unregisters fd. -1 with ENOENT if it is not registered. */
int clpd_loop_remove_fd(clpd_loop *l, int fd);
/* Runs cycles until SIGTERM or SIGINT arrives and returns that signal; a
 * pending stop signal wins over a due cycle and over the registered fds.
 * SIGHUP is logged and the loop continues. Returns -1 with errno set on a
 * system error, including a failed read of the signal descriptor. */
int clpd_loop_run(clpd_loop *l, clpd_cycle_fn *cycle, void *ctx);
void clpd_loop_close(clpd_loop *l);

#endif
