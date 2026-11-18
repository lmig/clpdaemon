#define _GNU_SOURCE /* accept4 */

#include "admin.h"

#include "ingest.h"
#include "log.h"
#include "plugins.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* --- commands ------------------------------------------------------------ */

static const char origin_tag = 0;

const void *clpd_admin_origin(void) { return &origin_tag; }

void clpd_admin_post_rejected(void *plugins, const void *origin,
                              const char *message) {
  FILE *log = clpd_log();
  if (origin != &origin_tag) {
    clpd_plugins_post_rejected(plugins, origin, message);
    return;
  }
  flockfile(log);
  fprintf(log, "clpdaemon: admin socket: inject rejected: %s\n", message);
  fflush(log);
  funlockfile(log);
}

static int is(const char *word, size_t n, const char *name) {
  return n == strlen(name) && memcmp(word, name, n) == 0;
}

static void inject(const clpd_admin_state *s, const char *json, size_t len,
                   char *reply, size_t n) {
  char msg[CLPD_INGEST_MESSAGE_MAX];
  if (clpd_ingest_check(s->env, s->packages, json, len, msg, sizeof msg) != 0)
    snprintf(reply, n, "ERR %s", msg);
  else if (clpd_ingest_post(clpd_admin_origin(), json, len) == 0)
    snprintf(reply, n, "OK queued");
  else if (errno == EAGAIN)
    snprintf(reply, n, "ERR ingest queue full");
  else
    snprintf(reply, n, "ERR %s", strerror(errno));
}

void clpd_admin_command(const clpd_admin_state *s, const char *line,
                        size_t len, char *reply, size_t n) {
  const char *sp;
  size_t wlen;

  if (len > 0 && line[len - 1] == '\r') len--;
  if (memchr(line, '\0', len) != NULL) {
    snprintf(reply, n, "ERR NUL byte not allowed");
    return;
  }
  if (len == 0) {
    snprintf(reply, n, "ERR empty command");
    return;
  }
  sp = memchr(line, ' ', len);
  wlen = sp != NULL ? (size_t)(sp - line) : len;
  if (is(line, wlen, "inject")) {
    /* Never evaluated: the JSON goes through the ingest whitelist. */
    inject(s, line + wlen + (sp != NULL), len - wlen - (sp != NULL), reply, n);
  } else if ((is(line, wlen, "status") || is(line, wlen, "reopen")) &&
             sp != NULL) {
    snprintf(reply, n, "ERR '%.*s' takes no arguments", (int)wlen, line);
  } else if (is(line, wlen, "status")) {
    unsigned long facts = 0;
    Fact *f;
    for (f = GetNextFact(s->env, NULL); f != NULL; f = GetNextFact(s->env, f))
      facts++;
    snprintf(reply, n, "OK node_id=%s cycles=%lu queue=%zu facts=%lu",
             s->node_id, *s->cycles, clpd_ingest_queue_length(), facts);
  } else if (is(line, wlen, "reopen")) {
    int failed = clpd_trace_reopen(s->trace, clpd_log());
    if (failed == 0)
      snprintf(reply, n, "OK reopened");
    else
      snprintf(reply, n, "ERR %d trace files could not be reopened", failed);
  } else {
    /* One byte past the shown length, so a longer word ends in "...". */
    char word[CLPD_INGEST_SHOWN_MAX + 2], buf[CLPD_INGEST_SHOWN_MAX + 4];
    if (wlen > CLPD_INGEST_SHOWN_MAX + 1) wlen = CLPD_INGEST_SHOWN_MAX + 1;
    memcpy(word, line, wlen);
    word[wlen] = '\0';
    snprintf(reply, n, "ERR unknown command '%s'", clpd_ingest_shown(word, buf));
  }
}

/* --- socket ---------------------------------------------------------------- */

typedef struct client {
  clpd_admin *admin;
  int fd;
  int eof; /* the peer sent EOF: close once the replies are sent */
  size_t in_len, out_len;
  char in[CLPD_ADMIN_LINE_MAX + 1]; /* a full line and its LF */
  char out[CLPD_ADMIN_OUTPUT_MAX];
} client;

struct clpd_admin {
  int fd;
  char *path;
  dev_t dev; /* of the socket file, to remove only our own at close */
  ino_t ino;
  clpd_loop *loop; /* NULL when not attached */
  int paused;      /* listener not polled: accept ran out of resources */
  clpd_admin_state state;
  client *clients[CLPD_ADMIN_CLIENTS_MAX];
};

/* Polls the listener again after accept ran out of descriptors or
 * memory. */
static void resume(clpd_admin *a) {
  if (a->loop == NULL || !a->paused) return;
  if (clpd_loop_set_events(a->loop, a->fd, POLLIN) == 0) a->paused = 0;
}

static void drop(client *c) {
  clpd_admin *a = c->admin;
  size_t i;
  clpd_loop_remove_fd(a->loop, c->fd);
  close(c->fd);
  for (i = 0; i < CLPD_ADMIN_CLIENTS_MAX; i++)
    if (a->clients[i] == c) a->clients[i] = NULL;
  free(c);
  resume(a); /* a descriptor is free again */
}

/* Sends what the socket takes now. MSG_NOSIGNAL: a peer that is gone gives
 * EPIPE, never SIGPIPE. Returns -1 when the connection failed. */
static int flush(client *c) {
  while (c->out_len > 0) {
    ssize_t w = send(c->fd, c->out, c->out_len, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (w < 0 && errno == EINTR) continue;
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (w < 0) return -1;
    memmove(c->out, c->out + w, c->out_len - (size_t)w);
    c->out_len -= (size_t)w;
  }
  return 0;
}

/* Queues one reply line. A client that leaves more than
 * CLPD_ADMIN_OUTPUT_MAX bytes unread is disconnected (-1). */
static int queue_reply(client *c, const char *text) {
  size_t n = strlen(text);
  if (c->out_len + n + 1 > sizeof c->out &&
      (flush(c) != 0 || c->out_len + n + 1 > sizeof c->out))
    return -1;
  memcpy(c->out + c->out_len, text, n);
  c->out[c->out_len + n] = '\n';
  c->out_len += n + 1;
  return 0;
}

/* Runs every complete line in the input buffer. Returns -1 to close. */
static int run_lines(client *c) {
  char text[CLPD_ADMIN_REPLY_MAX];
  size_t start = 0;
  char *lf;
  while ((lf = memchr(c->in + start, '\n', c->in_len - start)) != NULL) {
    size_t len = (size_t)(lf - (c->in + start));
    clpd_admin_command(&c->admin->state, c->in + start, len, text, sizeof text);
    if (queue_reply(c, text) != 0) return -1;
    start += len + 1;
  }
  memmove(c->in, c->in + start, c->in_len - start);
  c->in_len -= start;
  if (c->in_len == sizeof c->in) { /* no LF within the limit */
    if (queue_reply(c, "ERR line too long") == 0) flush(c);
    return -1;
  }
  return 0;
}

static void on_client(void *ctx, int fd, short revents) {
  client *c = ctx;
  if (revents & POLLIN) {
    ssize_t r = read(fd, c->in + c->in_len, sizeof c->in - c->in_len);
    if (r == 0) {
      c->eof = 1;
      c->in_len = 0; /* a partial last line is never run */
    } else if (r > 0) {
      c->in_len += (size_t)r;
      if (run_lines(c) != 0) goto close;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      goto close;
    }
  } else if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
    goto close;
  }
  if (flush(c) != 0 || (c->eof && c->out_len == 0)) goto close;
  clpd_loop_set_events(c->admin->loop, fd,
                       (short)((c->eof ? 0 : POLLIN) |
                               (c->out_len > 0 ? POLLOUT : 0)));
  return;
close:
  drop(c);
}

static void on_listen(void *ctx, int fd, short revents) {
  clpd_admin *a = ctx;
  int i, cfd, n;
  (void)revents;
  /* Bounded, so a burst of connections cannot hold the engine thread. */
  for (n = 0; n < 2 * CLPD_ADMIN_CLIENTS_MAX; n++) {
    client *c = NULL;
    cfd = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (cfd < 0 && (errno == EINTR || errno == ECONNABORTED)) continue;
    if (cfd < 0) {
      /* The pending connection stays readable: stop polling the listener
       * (or the engine thread spins) until a client closes or the next
       * cycle (clpd_admin_resume). */
      if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS ||
          errno == ENOMEM) {
        if (clpd_loop_set_events(a->loop, fd, 0) == 0) a->paused = 1;
      }
      return;
    }
    for (i = 0; i < CLPD_ADMIN_CLIENTS_MAX && a->clients[i] != NULL; i++) {
    }
    if (i == CLPD_ADMIN_CLIENTS_MAX) {
      static const char full[] = "ERR too many clients\n";
      if (send(cfd, full, sizeof full - 1, MSG_NOSIGNAL | MSG_DONTWAIT) < 0) {
      }
    } else if ((c = calloc(1, sizeof *c)) != NULL) {
      c->admin = a;
      c->fd = cfd;
      if (clpd_loop_add_fd(a->loop, cfd, POLLIN, on_client, c) == 0) {
        a->clients[i] = c;
        continue;
      }
      free(c);
    }
    close(cfd);
  }
}

clpd_admin *clpd_admin_bind(const char *path, int mode, FILE *log) {
  struct sockaddr_un sa;
  struct stat st;
  clpd_admin *a;
  mode_t old;
  int fd, rc, saved;

  if (strlen(path) >= sizeof sa.sun_path) {
    fprintf(log, "clpdaemon: %s: admin socket path too long (max %zu bytes)\n",
            path, sizeof sa.sun_path - 1);
    return NULL;
  }
  if (lstat(path, &st) == 0) {
    if (!S_ISSOCK(st.st_mode)) {
      fprintf(log, "clpdaemon: %s: not a socket\n", path);
      return NULL;
    }
    if (unlink(path) != 0 && errno != ENOENT) {
      fprintf(log, "clpdaemon: %s: cannot remove the old socket: %s\n", path,
              strerror(errno));
      return NULL;
    }
  }
  memset(&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX;
  memcpy(sa.sun_path, path, strlen(path));
  a = calloc(1, sizeof *a);
  fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (a == NULL || fd < 0) goto fail;
  /* Created with no permission at all, then opened up to the mode: never
   * wider than configured, also before the chmod. */
  old = umask(0777);
  rc = bind(fd, (struct sockaddr *)&sa, sizeof sa);
  umask(old);
  if (rc != 0) goto fail;
  if (chmod(path, (mode_t)mode) != 0 || lstat(path, &st) != 0 ||
      listen(fd, SOMAXCONN) != 0 || (a->path = strdup(path)) == NULL) {
    saved = errno;
    unlink(path);
    errno = saved;
    goto fail;
  }
  a->fd = fd;
  a->dev = st.st_dev;
  a->ino = st.st_ino;
  return a;
fail:
  saved = errno;
  fprintf(log, "clpdaemon: %s: cannot bind the admin socket: %s\n", path,
          strerror(saved));
  if (fd >= 0) close(fd);
  free(a);
  return NULL;
}

int clpd_admin_attach(clpd_admin *a, clpd_loop *loop,
                      const clpd_admin_state *s, FILE *log) {
  a->state = *s;
  if (clpd_loop_add_fd(loop, a->fd, POLLIN, on_listen, a) != 0) {
    fprintf(log, "clpdaemon: %s: cannot serve the admin socket: %s\n",
            a->path, strerror(errno));
    return -1;
  }
  a->loop = loop;
  return 0;
}

void clpd_admin_resume(clpd_admin *a) {
  if (a != NULL) resume(a);
}

void clpd_admin_detach(clpd_admin *a) {
  size_t i;
  if (a == NULL || a->loop == NULL) return;
  for (i = 0; i < CLPD_ADMIN_CLIENTS_MAX; i++)
    if (a->clients[i] != NULL) drop(a->clients[i]);
  clpd_loop_remove_fd(a->loop, a->fd);
  a->loop = NULL;
}

void clpd_admin_close(clpd_admin *a) {
  struct stat st;
  if (a == NULL) return;
  clpd_admin_detach(a);
  close(a->fd);
  /* Another daemon may have replaced the file since (it unlinks a socket
   * it finds): leave that one alone. */
  if (lstat(a->path, &st) == 0 && st.st_dev == a->dev && st.st_ino == a->ino)
    unlink(a->path);
  free(a->path);
  free(a);
}
