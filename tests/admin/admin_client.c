/* Test client for the admin socket (docs/admin-socket.md), used by
 * tests/cli/cli_test.sh because socat and nc -U may be missing.
 *
 * usage: admin_client [-c] [-f] [-h <n>] <socket> [<chunk>...]
 *
 * Connects to <socket> and writes each chunk with a separate write, 100 ms
 * apart; a chunk may contain \n, \r, \t, \0 and \\ escapes. Then it
 * half-closes the connection and copies everything the daemon sends, until
 * EOF, to stdout. A write the daemon refuses (it closed the connection)
 * ends the writing, not the run.
 *   -c      close right after writing, without reading
 *   -f      flood: write "status\n" without ever reading, until the daemon
 *           disconnects; prints "disconnected"
 *   -h <n>  first open <n> (at most 64) other connections, each served
 *           once (status), and keep them open until the end
 * Exit 0, or 2 on a usage or connect error, 3 when a held connection was
 * not served. Every run is bounded by a 10 s alarm. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int dial(const char *path) {
  struct sockaddr_un sa;
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  memset(&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX;
  strncpy(sa.sun_path, path, sizeof sa.sun_path - 1);
  if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
    fprintf(stderr, "admin_client: connect: %s\n", strerror(errno));
    exit(2);
  }
  return fd;
}

/* Writes all of s; returns -1 once the peer is gone. */
static int put(int fd, const char *s, size_t n) {
  while (n > 0) {
    ssize_t w = write(fd, s, n);
    if (w < 0 && errno == EINTR) continue;
    if (w < 0) return -1;
    s += w;
    n -= (size_t)w;
  }
  return 0;
}

static size_t unescape(const char *in, char *out) {
  size_t n = 0;
  for (; *in != '\0'; in++) {
    if (*in == '\\' && in[1] != '\0') {
      in++;
      out[n++] = *in == 'n'   ? '\n'
                 : *in == 'r' ? '\r'
                 : *in == 't' ? '\t'
                 : *in == '0' ? '\0'
                              : *in;
    } else {
      out[n++] = *in;
    }
  }
  return n;
}

static void pause_ms(long ms) {
  struct timespec t;
  t.tv_sec = ms / 1000;
  t.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep(&t, NULL);
}

int main(int argc, char **argv) {
  int close_only = 0, flood = 0, held = 0, i, fd, hold[64];
  char buf[4096];
  ssize_t r;

  signal(SIGPIPE, SIG_IGN);
  alarm(10);
  for (i = 1; i < argc && argv[i][0] == '-'; i++) {
    if (strcmp(argv[i], "-c") == 0) close_only = 1;
    else if (strcmp(argv[i], "-f") == 0) flood = 1;
    else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) held = atoi(argv[++i]);
    else break;
  }
  if (i >= argc) {
    fputs("usage: admin_client [-c] [-f] [-h <n>] <socket> [<chunk>...]\n", stderr);
    return 2;
  }
  if (held < 0 || held > 64) return 2;
  for (fd = 0; fd < held; fd++) {
    size_t got = 0;
    hold[fd] = dial(argv[i]);
    if (put(hold[fd], "status\n", 7) != 0) return 3;
    while (got < sizeof buf - 1 && (r = read(hold[fd], buf + got, 1)) == 1 &&
           buf[got] != '\n')
      got++;
    buf[got] = '\0';
    if (strncmp(buf, "OK ", 3) != 0) {
      fprintf(stderr, "admin_client: held connection %d: [%s]\n", fd, buf);
      return 3;
    }
  }
  fd = dial(argv[i]);
  if (flood) {
    while (put(fd, "status\n", 7) == 0) {
    }
    puts("disconnected");
    return 0;
  }
  for (i++; i < argc; i++) {
    char *chunk = malloc(strlen(argv[i]) + 1);
    size_t n;
    if (chunk == NULL) return 2;
    n = unescape(argv[i], chunk);
    r = put(fd, chunk, n);
    free(chunk);
    if (r != 0) break;
    if (i + 1 < argc) pause_ms(100);
  }
  if (close_only) return 0;
  shutdown(fd, SHUT_WR);
  while ((r = read(fd, buf, sizeof buf)) > 0 || (r < 0 && errno == EINTR))
    if (r > 0) fwrite(buf, 1, (size_t)r, stdout);
  return 0;
}
