/* fd 1 / fd 2 capture (FR-16, L48): bytes written to the process
 * descriptors become trace output lines, the daemon log keeps the original
 * stderr, and stop restores the descriptors. Checks run after stop, because
 * failure messages written during a capture would land in the trace. */
#define _GNU_SOURCE

#include "fdcapture.h"
#include "log.h"
#include "trace.h"

#include "../common/expect.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char dir[] = "/tmp/clpd-fdcapture-XXXXXX";
static char text_path[256], jsonl_path[256];

#define TS "2026-11-18T10:37:25.641Z"

/* 2026-11-18T10:37:25.641Z */
static void fixed_clock(long long *sec, long *nsec) {
  *sec = 1794998245;
  *nsec = 641999999;
}

/* Whole file in a malloc'd buffer (NUL-terminated), NULL when missing. */
static char *slurp(const char *path, size_t *len) {
  FILE *f = fopen(path, "r");
  char *buf = NULL;
  size_t cap = 0, n = 0, r;
  if (f == NULL) return NULL;
  do {
    if (cap - n < 4096) {
      char *p = realloc(buf, cap = cap ? cap * 2 : 8192);
      if (p == NULL) break;
      buf = p;
    }
    r = fread(buf + n, 1, cap - n - 1, f);
    n += r;
  } while (r > 0);
  fclose(f);
  if (buf != NULL) buf[n] = '\0';
  if (len != NULL) *len = n;
  return buf;
}

static size_t count_lines(const char *path) {
  size_t n = 0;
  char *s = slurp(path, NULL), *p;
  if (s == NULL) return 0;
  for (p = s; *p; p++) n += *p == '\n';
  free(s);
  return n;
}

/* Waits up to 5 s for the text trace to hold n lines, so lines written to
 * the two pipes reach the trace in a known order. */
static void wait_lines(size_t n) {
  struct timespec ms = {0, 1000000};
  int i;
  for (i = 0; i < 5000 && count_lines(text_path) < n; i++) nanosleep(&ms, NULL);
}

static size_t occurrences(const char *path, const char *needle) {
  size_t n = 0, k = strlen(needle);
  char *s = slurp(path, NULL), *p;
  if (s == NULL) return 0;
  for (p = s; (p = strstr(p, needle)) != NULL; p += k) n++;
  free(s);
  return n;
}

static void expect_file(const char *path, const char *want) {
  char *got = slurp(path, NULL);
  EXPECT(got != NULL);
  if (got == NULL) return;
  if (strcmp(got, want) != 0) {
    failures++;
    fprintf(stderr, "FAIL %s:\n--- want\n%s--- got\n%s---\n", path, want, got);
  }
  free(got);
}

static void write_all(int fd, const char *s, size_t n) {
  while (n > 0) {
    ssize_t w = write(fd, s, n);
    if (w <= 0) return;
    s += w;
    n -= (size_t)w;
  }
}

static int same_file(int fd, const struct stat *want) {
  struct stat st;
  return fstat(fd, &st) == 0 && st.st_dev == want->st_dev &&
         st.st_ino == want->st_ino;
}

/* One capture session over fresh trace files. */
static clpd_trace *trace;
static clpd_fdcapture *cap;
static struct stat orig_out, orig_err;

static int begin(void) {
  unlink(text_path);
  unlink(jsonl_path);
  trace = clpd_trace_open(text_path, jsonl_path, "n1", stderr);
  EXPECT(trace != NULL);
  if (trace == NULL) return 0;
  clpd_trace_set_clock(trace, fixed_clock);
  EXPECT(fstat(1, &orig_out) == 0 && fstat(2, &orig_err) == 0);
  fflush(stdout);
  cap = clpd_fdcapture_start(trace, stderr);
  EXPECT(cap != NULL);
  if (cap == NULL) {
    clpd_trace_close(trace);
    return 0;
  }
  return 1;
}

/* Stops the capture and checks fds 1 and 2 are the originals again. */
static void finish(void) {
  clpd_fdcapture_stop(cap);
  EXPECT(same_file(1, &orig_out));
  EXPECT(same_file(2, &orig_err));
  clpd_trace_close(trace);
}

static void test_lines(void) {
  if (!begin()) return;
  write_all(1, "w1\n", 3);
  wait_lines(1);
  write_all(2, "w2\n", 3);
  wait_lines(2);
  printf("p%d\n", 1);
  wait_lines(3);
  fprintf(stderr, "e%d\n", 2);
  wait_lines(4);
  write_all(1, "a\nb\n\n", 5);
  wait_lines(7);
  finish();
  expect_file(text_path, TS " n1 - stdout w1\n"
                         TS " n1 - stderr w2\n"
                         TS " n1 - stdout p1\n"
                         TS " n1 - stderr e2\n"
                         TS " n1 - stdout a\n"
                         TS " n1 - stdout b\n"
                         TS " n1 - stdout \n");
  expect_file(jsonl_path,
              "{\"ts\":\"" TS "\",\"node_id\":\"n1\",\"event\":\"output\",\"stream\":\"stdout\",\"text\":\"w1\"}\n"
              "{\"ts\":\"" TS "\",\"node_id\":\"n1\",\"event\":\"output\",\"stream\":\"stderr\",\"text\":\"w2\"}\n"
              "{\"ts\":\"" TS "\",\"node_id\":\"n1\",\"event\":\"output\",\"stream\":\"stdout\",\"text\":\"p1\"}\n"
              "{\"ts\":\"" TS "\",\"node_id\":\"n1\",\"event\":\"output\",\"stream\":\"stderr\",\"text\":\"e2\"}\n"
              "{\"ts\":\"" TS "\",\"node_id\":\"n1\",\"event\":\"output\",\"stream\":\"stdout\",\"text\":\"a\"}\n"
              "{\"ts\":\"" TS "\",\"node_id\":\"n1\",\"event\":\"output\",\"stream\":\"stdout\",\"text\":\"b\"}\n"
              "{\"ts\":\"" TS "\",\"node_id\":\"n1\",\"event\":\"output\",\"stream\":\"stdout\",\"text\":\"\"}\n");
}

/* S5/T5c D1: short lines are never split when one read fills the buffer. */
static void test_short_lines_bulk(void) {
  enum { N = 20000 }; /* 20000 x "abcdefghi\n" = 200 KB */
  char *buf = malloc((size_t)N * 10);
  int i;
  if (buf == NULL) return;
  for (i = 0; i < N; i++) memcpy(buf + (size_t)i * 10, "abcdefghi\n", 10);
  if (!begin()) {
    free(buf);
    return;
  }
  write_all(1, buf, (size_t)N * 10);
  finish();
  EXPECT(count_lines(text_path) == N);
  EXPECT(occurrences(text_path, TS " n1 - stdout abcdefghi\n") == N);
  free(buf);
}

/* S5/T5c D1: lines of varying length (like `seq 1 200000`), whose ends
 * cannot align with the buffer size, arrive whole and in order. */
static void test_varying_lines(void) {
  enum { N = 200000 };
  size_t cap = (size_t)N * 8, n = 0, head = strlen(TS " n1 - stdout ");
  char *buf = malloc(cap), *want = malloc(cap + (size_t)N * head + 1), *p;
  int i;
  if (buf == NULL || want == NULL) {
    free(buf);
    free(want);
    return;
  }
  p = want;
  for (i = 1; i <= N; i++) {
    int k = sprintf(buf + n, "%d\n", i);
    memcpy(p, TS " n1 - stdout ", head);
    p += head;
    memcpy(p, buf + n, (size_t)k);
    p += k;
    n += (size_t)k;
  }
  *p = '\0';
  if (begin()) {
    write_all(1, buf, n);
    finish();
    EXPECT(count_lines(text_path) == N);
    expect_file(text_path, want);
  }
  free(buf);
  free(want);
}

/* Text without a final newline is written at stop, stdout first. */
static void test_partial(void) {
  if (!begin()) return;
  printf("tail");
  write_all(2, "err-tail", 8);
  finish();
  expect_file(text_path, TS " n1 - stdout tail\n" TS " n1 - stderr err-tail\n");
}

/* A line longer than the cap is cut into pieces of at most the cap. */
static void test_cap(void) {
  size_t n = CLPD_FDCAPTURE_LINE_MAX + 10;
  char *buf = malloc(n + 1), *want;
  size_t head = strlen(TS " n1 - stdout ");
  if (buf == NULL) return;
  memset(buf, 'a', n);
  buf[n] = '\n';
  if (!begin()) {
    free(buf);
    return;
  }
  write_all(1, buf, n + 1);
  finish();
  want = malloc(2 * head + n + 3);
  if (want != NULL) {
    char *p = want;
    memcpy(p, TS " n1 - stdout ", head);
    p += head;
    memset(p, 'a', CLPD_FDCAPTURE_LINE_MAX);
    p += CLPD_FDCAPTURE_LINE_MAX;
    *p++ = '\n';
    memcpy(p, TS " n1 - stdout ", head);
    p += head;
    memcpy(p, "aaaaaaaaaa\n", 12);
    expect_file(text_path, want);
  }
  free(want);
  free(buf);
}

/* 1 MiB in one write from this thread completes while the reader drains
 * the pipe (a deadlock would hit the ctest TIMEOUT). */
static void test_large(void) {
  enum { LINES = 1024, LEN = 1023 };
  char *buf = malloc((size_t)LINES * (LEN + 1));
  size_t got = 0, text_head = strlen(TS " n1 - stdout ");
  size_t json_head =
      strlen("{\"ts\":\"" TS "\",\"node_id\":\"n1\",\"event\":\"output\",\"stream\":\"stdout\",\"text\":\"\"}");
  char *s;
  int i;
  if (buf == NULL) return;
  for (i = 0; i < LINES; i++) {
    memset(buf + (size_t)i * (LEN + 1), 'x', LEN);
    buf[(size_t)i * (LEN + 1) + LEN] = '\n';
  }
  if (!begin()) {
    free(buf);
    return;
  }
  write_all(1, buf, (size_t)LINES * (LEN + 1));
  finish();
  s = slurp(text_path, &got);
  EXPECT(got == (size_t)LINES * (text_head + LEN + 1));
  free(s);
  s = slurp(jsonl_path, &got);
  EXPECT(got == (size_t)LINES * (json_head + LEN + 1));
  free(s);
  EXPECT(count_lines(text_path) == LINES);
  free(buf);
}

/* The daemon log is the original stderr: its messages are not traced. */
static void test_log(void) {
  char log_path[300], *got;
  int saved = dup(2), fd;
  snprintf(log_path, sizeof log_path, "%s/stderr.txt", dir);
  fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  EXPECT(saved >= 0 && fd >= 0);
  if (saved < 0 || fd < 0) return;
  dup2(fd, 2);
  close(fd);
  if (begin()) {
    FILE *log = clpd_fdcapture_log(cap);
    fprintf(log, "clpdaemon: log line\n");
    fprintf(stderr, "captured\n");
    finish();
  }
  dup2(saved, 2);
  close(saved);
  got = slurp(log_path, NULL);
  EXPECT(got != NULL && strcmp(got, "clpdaemon: log line\n") == 0);
  free(got);
  expect_file(text_path, TS " n1 - stderr captured\n");
  unlink(log_path);
}

/* This thread emits engine events and reopens the trace while the reader
 * thread emits pipe lines; every line arrives whole. */
static void test_concurrent(void) {
  int i;
  if (!begin()) return;
  for (i = 0; i < 500; i++) {
    clpd_trace_output(trace, NULL, "STDOUT", "m", 1);
    write_all(1, "c\n", 2);
    if (i % 100 == 0) clpd_trace_reopen(trace, clpd_fdcapture_log(cap));
  }
  finish();
  EXPECT(count_lines(text_path) == 1000);
  EXPECT(occurrences(text_path, TS " n1 - STDOUT m\n") == 500);
  EXPECT(occurrences(text_path, TS " n1 - stdout c\n") == 500);
  EXPECT(count_lines(jsonl_path) == 1000);
}

/* Link seam (-Wl,--wrap=poll -Wl,--wrap=read, this test binary only): the
 * next poll or read fails once with the stored errno. Only the reader
 * thread calls read directly (stdio reads do not go through the wrapper). */
static int fail_poll, fail_read, slow_read;
int __real_poll(struct pollfd *fds, nfds_t n, int timeout);
int __wrap_poll(struct pollfd *fds, nfds_t n, int timeout);
int __wrap_poll(struct pollfd *fds, nfds_t n, int timeout) {
  int e = __atomic_exchange_n(&fail_poll, 0, __ATOMIC_SEQ_CST);
  if (e != 0) {
    errno = e;
    return -1;
  }
  return __real_poll(fds, n, timeout);
}
ssize_t __real_read(int fd, void *buf, size_t n);
ssize_t __wrap_read(int fd, void *buf, size_t n);
ssize_t __wrap_read(int fd, void *buf, size_t n) {
  int e = __atomic_exchange_n(&fail_read, 0, __ATOMIC_SEQ_CST);
  if (e != 0) {
    errno = e;
    return -1;
  }
  if (__atomic_load_n(&slow_read, __ATOMIC_SEQ_CST)) {
    struct timespec t = {0, 2000000};
    nanosleep(&t, NULL);
  }
  return __real_read(fd, buf, n);
}

/* 1 once no process holds the read end of the pipe w writes to. */
static int readers_gone(int w) {
  struct pollfd p = {w, POLLOUT, 0};
  return poll(&p, 1, 0) == 1 && (p.revents & POLLERR) != 0;
}

/* A reader failure (poll or read, through *seam) is reported once on the
 * daemon log, the original descriptors come back so writers never block,
 * the pipe read ends are closed after the failure drain so a writer still
 * holding a pipe (a child) gets EPIPE instead of blocking, and stop still
 * works. */
static void reader_failure(int *seam, int err, const char *message) {
  enum { BIG = 256 * 1024 };
  char out_path[300], err_path[300], *got, *big = malloc(BIG);
  struct timespec ms = {0, 1000000};
  int saved_out = dup(1), saved_err = dup(2), fo, fe, i, w = -1, gone = 0,
      epipe = 0;
  size_t n;
  snprintf(out_path, sizeof out_path, "%s/stdout.txt", dir);
  snprintf(err_path, sizeof err_path, "%s/stderr.txt", dir);
  fo = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  fe = open(err_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  EXPECT(big != NULL && saved_out >= 0 && saved_err >= 0 && fo >= 0 && fe >= 0);
  if (big == NULL || saved_out < 0 || saved_err < 0 || fo < 0 || fe < 0) return;
  memset(big, 'z', BIG - 1);
  big[BIG - 1] = '\n';
  fflush(stdout);
  dup2(fo, 1);
  dup2(fe, 2);
  close(fo);
  close(fe);
  if (begin()) {
    void (*old_pipe)(int);
    w = dup(1); /* a write end kept by "a child" */
    write_all(1, "warm\n", 5);
    wait_lines(1);
    __atomic_store_n(seam, err, __ATOMIC_SEQ_CST);
    write_all(1, "x\n", 2); /* wakes the reader, which then fails */
    for (i = 0; i < 5000 && !same_file(1, &orig_out); i++) nanosleep(&ms, NULL);
    write_all(1, big, BIG); /* blocks forever if fd 1 were still a pipe */
    fprintf(stderr, "after\n");
    for (i = 0; i < 5000 && !(gone = readers_gone(w)); i++) nanosleep(&ms, NULL);
    old_pipe = signal(SIGPIPE, SIG_IGN);
    epipe = write(w, "y\n", 2) < 0 && errno == EPIPE;
    signal(SIGPIPE, old_pipe);
    close(w);
    finish();
  }
  fflush(stdout);
  dup2(saved_out, 1);
  dup2(saved_err, 2);
  close(saved_out);
  close(saved_err);
  EXPECT(gone);
  EXPECT(epipe);
  got = slurp(err_path, NULL);
  EXPECT(got != NULL && strcmp(got, message) == 0);
  if (got != NULL && strcmp(got, message) != 0)
    fprintf(stderr, "--- got\n%s---\n", got);
  free(got);
  got = slurp(out_path, &n);
  /* "x" reached either the trace (still in the pipe) or the console. */
  EXPECT(got != NULL && (n == BIG || (n == BIG + 2 && strncmp(got, "x\n", 2) == 0)));
  EXPECT(occurrences(text_path, TS " n1 - stdout warm\n") == 1);
  EXPECT(occurrences(text_path, TS " n1 - stdout x\n") + (n == BIG + 2) == 1);
  EXPECT(count_lines(text_path) == 1 + (n == BIG));
  free(got);
  free(big);
  unlink(out_path);
  unlink(err_path);
}

static void test_reader_failure(void) {
  reader_failure(&fail_poll, ENOMEM,
                 "clpdaemon: trace capture failed: Cannot allocate memory; "
                 "stdout and stderr go back to the console\n"
                 "after\n");
}

static void test_read_failure(void) {
  reader_failure(&fail_read, EIO,
                 "clpdaemon: trace capture failed: Input/output error; "
                 "stdout and stderr go back to the console\n"
                 "after\n");
}

/* Redirects fd 2 to a fresh file under dir; returns the saved fd 2. */
static int stderr_to(const char *path) {
  int saved = dup(2), fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  EXPECT(saved >= 0 && fd >= 0);
  if (saved < 0 || fd < 0) return -1;
  dup2(fd, 2);
  close(fd);
  return saved;
}

/* During a capture the daemon log stream (clpd_log) is the original
 * stderr, so a daemon message is not traced; after stop it is stderr. */
static void test_log_accessor(void) {
  char log_path[300], *got;
  int saved, during = 0;
  snprintf(log_path, sizeof log_path, "%s/stderr.txt", dir);
  if ((saved = stderr_to(log_path)) < 0) return;
  if (begin()) {
    during = clpd_log() == clpd_fdcapture_log(cap);
    fprintf(clpd_log(), "clpdaemon: via the accessor\n");
    fprintf(stderr, "captured\n");
    finish();
  }
  dup2(saved, 2);
  close(saved);
  EXPECT(during);
  EXPECT(clpd_log() == stderr);
  got = slurp(log_path, NULL);
  EXPECT(got != NULL && strcmp(got, "clpdaemon: via the accessor\n") == 0);
  free(got);
  expect_file(text_path, TS " n1 - stderr captured\n");
  unlink(log_path);
}

/* Arms a read failure while the main thread is blocked writing. */
static void *arm_read_failure(void *arg) {
  struct timespec t = {0, 30000000};
  (void)arg;
  nanosleep(&t, NULL);
  __atomic_store_n(&fail_read, EIO, __ATOMIC_SEQ_CST);
  return NULL;
}

/* L52/T5d A1: a write already blocked on the pipe when the reader fails
 * gets EPIPE (or a short count) once the read ends close, and the daemon
 * survives: the capture catches SIGPIPE while it runs. In a child, since a
 * regression kills the process with SIGPIPE. */
static void test_blocked_writer_survives(void) {
  enum { BIG = 16 * 1024 * 1024 };
  static const char want[] =
      "clpdaemon: trace capture failed: Input/output error; stdout and "
      "stderr go back to the console\n";
  char log_path[300], *got;
  int saved, status = 0;
  pid_t pid;
  snprintf(log_path, sizeof log_path, "%s/stderr.txt", dir);
  if ((saved = stderr_to(log_path)) < 0) return;
  fflush(stdout);
  pid = fork();
  if (pid == 0) {
    char *big = malloc(BIG);
    pthread_t th;
    ssize_t w;
    if (big == NULL || !begin()) _exit(3);
    memset(big, 'q', BIG);
    __atomic_store_n(&slow_read, 1, __ATOMIC_SEQ_CST);
    if (pthread_create(&th, NULL, arm_read_failure, NULL) != 0) _exit(4);
    w = write(1, big, BIG); /* blocked when the reader fails */
    pthread_join(th, NULL);
    __atomic_store_n(&slow_read, 0, __ATOMIC_SEQ_CST);
    finish();
    _exit(w < BIG && failures == 0 ? 0 : 5);
  }
  dup2(saved, 2);
  close(saved);
  EXPECT(pid > 0 && waitpid(pid, &status, 0) == pid);
  EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  if (WIFSIGNALED(status))
    fprintf(stderr, "child killed by signal %d\n", WTERMSIG(status));
  else if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
    fprintf(stderr, "child exit %d\n", WEXITSTATUS(status));
  got = slurp(log_path, NULL);
  EXPECT(got != NULL && strcmp(got, want) == 0);
  free(got);
  unlink(log_path);
}

/* SigIgn mask of a process started by sh during a capture: SIGPIPE must not
 * be in it (the capture catches SIGPIPE, it never ignores it, so children
 * get the default disposition back at exec). */
static void test_child_default_sigpipe(void) {
  char path[300], cmd[400], *got, *p;
  unsigned long long ign = ~0ULL;
  struct sigaction during, after;
  int rc = -1;
  snprintf(path, sizeof path, "%s/sig.txt", dir);
  snprintf(cmd, sizeof cmd, "grep '^SigIgn:' /proc/self/status > %s", path);
  if (!begin()) return;
  sigaction(SIGPIPE, NULL, &during);
  rc = system(cmd);
  finish();
  sigaction(SIGPIPE, NULL, &after);
  EXPECT(rc == 0);
  EXPECT(during.sa_handler != SIG_IGN && during.sa_handler != SIG_DFL);
  EXPECT(after.sa_handler == SIG_DFL);
  got = slurp(path, NULL);
  p = got != NULL ? strchr(got, ':') : NULL;
  if (p != NULL) ign = strtoull(p + 1, NULL, 16);
  EXPECT((ign & (1ULL << (SIGPIPE - 1))) == 0);
  free(got);
  unlink(path);
}

/* A fatal error during a capture (the OOM paths) reaches the original
 * stderr before exit 1 and is not traced. */
static void test_fatal_during_capture(void) {
  char log_path[300], *got;
  int saved, status = 0;
  pid_t pid;
  snprintf(log_path, sizeof log_path, "%s/stderr.txt", dir);
  if ((saved = stderr_to(log_path)) < 0) return;
  fflush(stdout);
  pid = fork();
  if (pid == 0) {
    if (begin()) {
      write_all(1, "before\n", 7);
      wait_lines(1);
      errno = ENOMEM;
      clpd_log_die_errno();
    }
    _exit(3);
  }
  dup2(saved, 2);
  close(saved);
  EXPECT(pid > 0 && waitpid(pid, &status, 0) == pid);
  EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 1);
  got = slurp(log_path, NULL);
  EXPECT(got != NULL && strcmp(got, "clpdaemon: Cannot allocate memory\n") == 0);
  free(got);
  expect_file(text_path, TS " n1 - stdout before\n");
  unlink(log_path);
}

/* A setup failure is reported and leaves the descriptors as they were. */
static void test_setup_failure(void) {
  struct stat before;
  FILE *err = tmpfile();
  clpd_trace *t = clpd_trace_open(text_path, NULL, "n1", stderr);
  int saved = dup(1);
  char buf[256];
  size_t n;
  EXPECT(t != NULL && err != NULL && saved >= 0 && fstat(2, &before) == 0);
  if (t == NULL || err == NULL || saved < 0) return;
  fflush(stdout);
  close(1);
  EXPECT(clpd_fdcapture_start(t, err) == NULL);
  dup2(saved, 1);
  close(saved);
  EXPECT(same_file(2, &before));
  EXPECT(clpd_log() == stderr); /* the daemon log stream is unchanged */
  rewind(err);
  n = fread(buf, 1, sizeof buf - 1, err);
  buf[n] = '\0';
  fclose(err);
  EXPECT(strcmp(buf, "clpdaemon: cannot redirect stdout and stderr to the "
                     "trace: Bad file descriptor\n") == 0);
  clpd_trace_close(t);
}

int main(void) {
  /* As src/main.c does at startup: printf lines reach the pipe as they end
   * (the capture no longer changes stdout buffering). */
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (mkdtemp(dir) == NULL) {
    perror("mkdtemp");
    return 1;
  }
  snprintf(text_path, sizeof text_path, "%s/trace.log", dir);
  snprintf(jsonl_path, sizeof jsonl_path, "%s/trace.jsonl", dir);
  test_lines();
  test_partial();
  test_cap();
  test_large();
  test_log();
  test_concurrent();
  test_setup_failure();
  test_short_lines_bulk();
  test_varying_lines();
  test_reader_failure();
  test_read_failure();
  test_log_accessor();
  test_fatal_during_capture();
  test_blocked_writer_survives();
  test_child_default_sigpipe();
  unlink(text_path);
  unlink(jsonl_path);
  rmdir(dir);
  if (failures) fprintf(stderr, "%d failed checks\n", failures);
  return failures != 0;
}
