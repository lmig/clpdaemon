#define _GNU_SOURCE

#include "fdcapture.h"
#include "log.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* One captured descriptor: the pipe read end and the line being built. */
typedef struct {
  int fd;
  const char *name; /* trace stream name */
  int done;         /* EOF, or drained at stop or after a failure */
  size_t len;
  char buf[CLPD_FDCAPTURE_LINE_MAX];
} stream;

struct clpd_fdcapture {
  clpd_trace *trace;
  FILE *err;      /* the caller's err: the trace's error stream again at stop */
  FILE *log;      /* daemon messages: a copy of the original stderr */
  FILE *prev_log; /* daemon log stream (clpd_log) before start */
  int saved[2];   /* original fds 1 and 2, closed after the join */
  int ctl[2];     /* closing ctl[1] tells the reader to drain and exit */
  /* The originals go back on 1 and 2 once, by stop or by the reader after
   * a failure, whichever comes first. */
  pthread_mutex_t fd_lock;
  int restored;
  pthread_t thread;
  /* SIGPIPE disposition before start, put back at stop (see catch_sigpipe). */
  struct sigaction old_pipe;
  int pipe_caught;
  stream s[2];    /* [0] fd 1, [1] fd 2 */
};

static void on_sigpipe(int sig) { (void)sig; }

/* While capturing, SIGPIPE is caught by a handler that does nothing, so a
 * daemon write still blocked on a pipe when the reader fails and closes the
 * read ends gets EPIPE (or a short count) instead of killing the daemon. A
 * caught signal goes back to the default action at exec, so children
 * started by CLIPS `system` keep the default SIGPIPE; SIG_IGN would be
 * inherited. A SIGPIPE the daemon was started with ignored stays ignored. */
static void catch_sigpipe(clpd_fdcapture *c) {
  struct sigaction sa;
  if (sigaction(SIGPIPE, NULL, &c->old_pipe) != 0 ||
      c->old_pipe.sa_handler == SIG_IGN)
    return;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_sigpipe;
  sa.sa_flags = SA_RESTART;
  sigemptyset(&sa.sa_mask);
  c->pipe_caught = sigaction(SIGPIPE, &sa, NULL) == 0;
}

static void restore_sigpipe(clpd_fdcapture *c) {
  if (c->pipe_caught) sigaction(SIGPIPE, &c->old_pipe, NULL);
  c->pipe_caught = 0;
}

/* Writes every complete line. A full buffer without any newline is written
 * as one event (the cap); otherwise the unfinished tail moves to the front
 * and waits for more. With final, the unfinished tail is written too. */
static void emit_lines(clpd_fdcapture *c, stream *s, int final) {
  size_t start = 0, i;
  for (i = 0; i < s->len; i++) {
    if (s->buf[i] != '\n') continue;
    clpd_trace_output(c->trace, NULL, s->name, s->buf + start, i - start);
    start = i + 1;
  }
  if (start < s->len &&
      (final || (start == 0 && s->len == sizeof s->buf))) {
    clpd_trace_output(c->trace, NULL, s->name, s->buf + start,
                      s->len - start);
    start = s->len;
  }
  memmove(s->buf, s->buf + start, s->len - start);
  s->len -= start;
}

/* Reads at most *left bytes (no limit when left is NULL). Returns 1 after
 * reading data or EINTR, 0 when the pipe is empty or at EOF (done set),
 * -1 on a read error (errno set). A read of 0 bytes means EOF only because
 * room is never 0: emit_lines never leaves the buffer full (a full buffer
 * either holds a newline, and everything up to the last one is written, or
 * it is written whole as the cap), and drain calls with *left > 0. */
static int read_some(clpd_fdcapture *c, stream *s, size_t *left) {
  size_t room = sizeof s->buf - s->len;
  ssize_t n;
  if (left != NULL && *left < room) room = *left;
  assert(room > 0);
  n = read(s->fd, s->buf + s->len, room);
  if (n > 0) {
    s->len += (size_t)n;
    if (left != NULL) *left -= (size_t)n;
    emit_lines(c, s, 0);
    return 1;
  }
  if (n < 0 && errno == EINTR) return 1;
  if (n < 0 && errno == EAGAIN) return 0;
  if (n < 0) return -1;
  emit_lines(c, s, 1);
  s->done = 1;
  return 0;
}

static void restore_fds(clpd_fdcapture *c) {
  pthread_mutex_lock(&c->fd_lock);
  if (!c->restored) {
    dup2(c->saved[0], 1);
    dup2(c->saved[1], 2);
    c->restored = 1;
  }
  pthread_mutex_unlock(&c->fd_lock);
}

static void close_fd(int *fd) {
  if (*fd >= 0) close(*fd);
  *fd = -1;
}

/* Final pass, after fds 1 and 2 are the originals again: reads only the
 * bytes each pipe held when it starts (a child still writing cannot keep
 * it going), then writes the partial lines, stdout first. */
static void drain(clpd_fdcapture *c) {
  int i;
  for (i = 0; i < 2; i++) {
    stream *s = &c->s[i];
    int avail = 0;
    size_t left;
    if (s->done) continue;
    if (ioctl(s->fd, FIONREAD, &avail) != 0 || avail < 0) avail = 0;
    left = (size_t)avail;
    while (left > 0 && !s->done && read_some(c, s, &left) > 0) {
    }
    if (!s->done) emit_lines(c, s, 1);
    s->done = 1;
  }
}

/* Reader thread: turns pipe bytes into lines until stop. If polling or
 * reading fails, it reports once on the daemon log, puts the original
 * descriptors back on 1 and 2, drains what the pipes hold, then closes the
 * read ends, so a process still writing to a pipe (a child that inherited
 * it) gets EPIPE (SIGPIPE) instead of blocking on a pipe nobody reads, and
 * exits; stop still works afterwards (it only joins). */
static void *reader(void *arg) {
  clpd_fdcapture *c = arg;
  int stopping = 0, failed = 0, i;

  while (!stopping && !failed && !(c->s[0].done && c->s[1].done)) {
    struct pollfd p[3];
    for (i = 0; i < 2; i++) {
      p[i].fd = c->s[i].done ? -1 : c->s[i].fd;
      p[i].events = POLLIN;
      p[i].revents = 0;
    }
    p[2].fd = c->ctl[0];
    p[2].events = POLLIN;
    p[2].revents = 0;
    if (poll(p, 3, -1) < 0) {
      if (errno == EINTR) continue;
      failed = errno;
      break;
    }
    if (p[2].revents != 0) stopping = 1;
    for (i = 0; i < 2 && !failed; i++)
      if (p[i].revents != 0 && read_some(c, &c->s[i], NULL) < 0)
        failed = errno;
  }
  if (failed) {
    fprintf(c->log,
            "clpdaemon: trace capture failed: %s; stdout and stderr go back "
            "to the console\n",
            strerror(failed));
    restore_fds(c);
  }
  drain(c);
  if (failed)
    for (i = 0; i < 2; i++) close_fd(&c->s[i].fd);
  return NULL;
}

clpd_fdcapture *clpd_fdcapture_start(clpd_trace *t, FILE *err) {
  clpd_fdcapture *c = calloc(1, sizeof *c);
  int out[2] = {-1, -1}, errp[2] = {-1, -1}, log_fd = -1, moved = 0, e, i;
  sigset_t all, old;

  if (c == NULL) goto fail;
  pthread_mutex_init(&c->fd_lock, NULL);
  c->trace = t;
  c->err = err;
  c->saved[0] = c->saved[1] = c->ctl[0] = c->ctl[1] = -1;
  c->s[0].fd = c->s[1].fd = -1;
  c->s[0].name = "stdout";
  c->s[1].name = "stderr";
  if ((log_fd = fcntl(2, F_DUPFD_CLOEXEC, 3)) < 0 ||
      (c->saved[0] = fcntl(1, F_DUPFD_CLOEXEC, 3)) < 0 ||
      (c->saved[1] = fcntl(2, F_DUPFD_CLOEXEC, 3)) < 0 ||
      pipe2(out, O_CLOEXEC) != 0 || pipe2(errp, O_CLOEXEC) != 0 ||
      pipe2(c->ctl, O_CLOEXEC) != 0 ||
      fcntl(out[0], F_SETFL, O_NONBLOCK) != 0 ||
      fcntl(errp[0], F_SETFL, O_NONBLOCK) != 0)
    goto fail;
  c->log = fdopen(log_fd, "w");
  if (c->log == NULL) goto fail;
  log_fd = -1;
  setvbuf(c->log, NULL, _IOLBF, 0);
  c->s[0].fd = out[0];
  c->s[1].fd = errp[0];
  out[0] = errp[0] = -1;

  fflush(stdout);
  fflush(stderr);
  if (dup2(out[1], 1) < 0) goto fail;
  moved = 1;
  if (dup2(errp[1], 2) < 0) goto fail;
  moved = 2;
  close_fd(&out[1]);
  close_fd(&errp[1]);

  /* Before the reader can emit: from now on a sink write failure and every
   * daemon message go to the original stderr, never into a pipe. */
  clpd_trace_set_err(t, c->log);
  c->prev_log = clpd_log_set(c->log);
  catch_sigpipe(c);

  /* The thread inherits this mask: signals stay with the main thread's
   * signalfd. */
  sigfillset(&all);
  pthread_sigmask(SIG_SETMASK, &all, &old);
  e = pthread_create(&c->thread, NULL, reader, c);
  pthread_sigmask(SIG_SETMASK, &old, NULL);
  if (e != 0) {
    clpd_trace_set_err(t, err);
    clpd_log_set(c->prev_log);
    restore_sigpipe(c);
    errno = e;
    goto fail;
  }
  return c;

fail:
  e = errno;
  if (c != NULL) {
    for (i = 0; i < moved; i++) dup2(c->saved[i], i + 1);
    if (c->log != NULL) fclose(c->log);
    for (i = 0; i < 2; i++) {
      close_fd(&c->saved[i]);
      close_fd(&c->ctl[i]);
      close_fd(&c->s[i].fd);
    }
    pthread_mutex_destroy(&c->fd_lock);
  }
  close_fd(&log_fd);
  for (i = 0; i < 2; i++) {
    close_fd(&out[i]);
    close_fd(&errp[i]);
  }
  free(c);
  fprintf(err, "clpdaemon: cannot redirect stdout and stderr to the trace: %s\n",
          strerror(e));
  return NULL;
}

FILE *clpd_fdcapture_log(clpd_fdcapture *c) { return c->log; }

void clpd_fdcapture_stop(clpd_fdcapture *c) {
  int i;
  if (c == NULL) return;
  fflush(stdout);
  fflush(stderr);
  restore_fds(c);
  close_fd(&c->ctl[1]);
  pthread_join(c->thread, NULL);
  clpd_trace_set_err(c->trace, c->err);
  clpd_log_set(c->prev_log);
  fclose(c->log);
  close_fd(&c->ctl[0]);
  for (i = 0; i < 2; i++) {
    close_fd(&c->saved[i]);
    close_fd(&c->s[i].fd);
  }
  restore_sigpipe(c);
  pthread_mutex_destroy(&c->fd_lock);
  free(c);
}
