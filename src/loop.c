#define _GNU_SOURCE

#include "loop.h"

#include "scheduler.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

/* A registered fd. id is unique per registration, so a callback that
 * removes an entry (and maybe registers the same fd number again) is never
 * followed by a callback for the old registration's poll result. */
typedef struct {
  int fd;
  short events;
  clpd_fd_fn *fn;
  void *ctx;
  unsigned long id;
} watch;

struct clpd_loop {
  int sig_fd, timer_fd, event_fd;
  watch w[CLPD_LOOP_FDS_MAX];
  size_t nw;
  unsigned long next_id;
  int tick_ms, min_ms;
  FILE *log;
  clpd_hup_fn *on_hup;
  void *hup_ctx;
};

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void close_fds(clpd_loop *l) {
  if (l->sig_fd >= 0) close(l->sig_fd);
  if (l->timer_fd >= 0) close(l->timer_fd);
  if (l->event_fd >= 0) close(l->event_fd);
}

static const int stop_sigs[] = {SIGTERM, SIGINT, SIGHUP};

static void stop_set(sigset_t *set) {
  size_t i;
  sigemptyset(set);
  for (i = 0; i < sizeof stop_sigs / sizeof stop_sigs[0]; i++)
    sigaddset(set, stop_sigs[i]);
}

int clpd_loop_block_signals(void) {
  sigset_t set;
  size_t i;
  stop_set(&set);
  if (sigprocmask(SIG_BLOCK, &set, NULL) != 0) return -1;
  for (i = 0; i < sizeof stop_sigs / sizeof stop_sigs[0]; i++)
    if (signal(stop_sigs[i], SIG_DFL) == SIG_ERR) return -1;
  return 0;
}

clpd_loop *clpd_loop_open(int tick_ms, int min_ms, FILE *log) {
  sigset_t set;
  int saved;
  clpd_loop *l = calloc(1, sizeof *l);

  if (l == NULL) return NULL;
  l->sig_fd = l->timer_fd = l->event_fd = -1;
  l->tick_ms = tick_ms;
  l->min_ms = min_ms;
  l->log = log;
  if (clpd_loop_block_signals() != 0) goto fail;
  stop_set(&set);
  l->sig_fd = signalfd(-1, &set, SFD_CLOEXEC | SFD_NONBLOCK);
  l->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  l->event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (l->sig_fd < 0 || l->timer_fd < 0 || l->event_fd < 0) goto fail;
  return l;
fail:
  saved = errno;
  close_fds(l);
  free(l);
  errno = saved;
  return NULL;
}

void clpd_loop_notify(clpd_loop *l) {
  uint64_t one = 1;
  /* EAGAIN means the counter is saturated: a wake-up is already pending. */
  if (write(l->event_fd, &one, sizeof one) < 0) {
  }
}

void clpd_loop_on_hup(clpd_loop *l, clpd_hup_fn *fn, void *ctx) {
  l->on_hup = fn;
  l->hup_ctx = ctx;
}

static watch *find(clpd_loop *l, int fd) {
  size_t i;
  for (i = 0; i < l->nw; i++)
    if (l->w[i].fd == fd) return &l->w[i];
  return NULL;
}

int clpd_loop_add_fd(clpd_loop *l, int fd, short events, clpd_fd_fn *fn,
                     void *ctx) {
  watch *w;
  if (fd < 0 || fn == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (find(l, fd) != NULL) {
    errno = EEXIST;
    return -1;
  }
  if (l->nw == CLPD_LOOP_FDS_MAX) {
    errno = ENOSPC;
    return -1;
  }
  w = &l->w[l->nw++];
  w->fd = fd;
  w->events = events;
  w->fn = fn;
  w->ctx = ctx;
  w->id = ++l->next_id;
  return 0;
}

int clpd_loop_set_events(clpd_loop *l, int fd, short events) {
  watch *w = find(l, fd);
  if (w == NULL) {
    errno = ENOENT;
    return -1;
  }
  w->events = events;
  return 0;
}

int clpd_loop_remove_fd(clpd_loop *l, int fd) {
  watch *w = find(l, fd);
  if (w == NULL) {
    errno = ENOENT;
    return -1;
  }
  *w = l->w[--l->nw];
  return 0;
}

/* Calls the callbacks of the registered fds that poll found ready: polled
 * holds the ids of the entries in poll order (fds[3 + i]). An entry
 * removed by an earlier callback is skipped; one added is not in polled. */
static void dispatch(clpd_loop *l, const struct pollfd *fds,
                     const unsigned long *polled, size_t n) {
  size_t i, j;
  for (i = 0; i < n; i++) {
    if (fds[i].revents == 0) continue;
    for (j = 0; j < l->nw && l->w[j].id != polled[i]; j++) {
    }
    if (j < l->nw) l->w[j].fn(l->w[j].ctx, l->w[j].fd, fds[i].revents);
  }
}

static int arm(clpd_loop *l, long long due_ms) {
  struct itimerspec its = {{0, 0}, {0, 0}};
  if (due_ms < 1) due_ms = 1; /* zero would disarm; a past time fires at once */
  its.it_value.tv_sec = due_ms / 1000;
  its.it_value.tv_nsec = (due_ms % 1000) * 1000000;
  return timerfd_settime(l->timer_fd, TFD_TIMER_ABSTIME, &its, NULL);
}

/* Drains a counter descriptor (eventfd or timerfd). */
static void drain(int fd) {
  uint64_t count;
  while (read(fd, &count, sizeof count) == (ssize_t)sizeof count) {
  }
}

int clpd_loop_run(clpd_loop *l, clpd_cycle_fn *cycle, void *ctx) {
  clpd_sched s;
  struct pollfd fds[3 + CLPD_LOOP_FDS_MAX];
  unsigned long polled[CLPD_LOOP_FDS_MAX];

  clpd_sched_init(&s, l->tick_ms, l->min_ms, now_ms());
  fds[0].fd = l->sig_fd;
  fds[1].fd = l->event_fd;
  fds[2].fd = l->timer_fd;
  fds[0].events = fds[1].events = fds[2].events = POLLIN;

  for (;;) {
    struct signalfd_siginfo si;
    long long now;
    size_t i, nw = l->nw;

    for (i = 0; i < nw; i++) {
      fds[3 + i].fd = l->w[i].fd;
      fds[3 + i].events = l->w[i].events;
      fds[3 + i].revents = 0;
      polled[i] = l->w[i].id;
    }
    if (arm(l, clpd_sched_next_due(&s)) != 0) return -1;
    if (poll(fds, 3 + nw, -1) < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    /* Signals first: a pending stop wins over a due cycle. */
    for (;;) {
      ssize_t n = read(l->sig_fd, &si, sizeof si);
      if (n < 0 && errno == EINTR) continue;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      if (n != (ssize_t)sizeof si) {
        if (n >= 0) errno = EIO; /* short read: the kernel never does this */
        return -1;
      }
      if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT)
        return (int)si.ssi_signo;
      if (si.ssi_signo == SIGHUP) {
        fputs("clpdaemon: received SIGHUP, reopening trace files\n", l->log);
        fflush(l->log);
        if (l->on_hup != NULL) l->on_hup(l->hup_ctx);
      }
    }
    if (fds[1].revents & POLLIN) {
      drain(l->event_fd);
      clpd_sched_notify(&s);
    }
    if (fds[2].revents & POLLIN) drain(l->timer_fd);
    dispatch(l, fds + 3, polled, nw);
    now = now_ms();
    if (now >= clpd_sched_next_due(&s)) {
      clpd_sched_cycle_started(&s, now);
      cycle(ctx);
    }
  }
}

void clpd_loop_close(clpd_loop *l) {
  if (l == NULL) return;
  close_fds(l);
  free(l);
}

