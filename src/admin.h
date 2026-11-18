/* Admin socket (FR-20, NFR-2; docs/admin-socket.md): a Unix stream socket
 * served on the engine thread through the event loop, one command per
 * line. Commands never evaluate CLIPS text: `inject` goes through the
 * ingest pipeline (src/ingest.h). Engine thread only. */
#ifndef CLPD_ADMIN_H
#define CLPD_ADMIN_H

#include <stddef.h>
#include <stdio.h>

#include "clips.h"
#include "loop.h"
#include "packages.h"
#include "trace.h"

#define CLPD_ADMIN_CLIENTS_MAX 16    /* connections served at once */
#define CLPD_ADMIN_LINE_MAX 65536    /* bytes of a command line before the LF */
#define CLPD_ADMIN_OUTPUT_MAX 65536  /* unsent reply bytes per client */
#define CLPD_ADMIN_REPLY_MAX 512     /* one reply, newline and NUL included */

/* What the commands read. The pointers must stay valid while the socket
 * is attached; cycles is the number of cycles run so far. */
typedef struct {
  Environment *env;
  const clpd_packages *packages;
  const char *node_id;
  const unsigned long *cycles;
  clpd_trace *trace;
} clpd_admin_state;

/* Runs one command: line[0..len) is the text before the LF (a final CR is
 * ignored). Writes the reply, without a newline, to reply (size n, at
 * least CLPD_ADMIN_REPLY_MAX). Outside Run. */
void clpd_admin_command(const clpd_admin_state *s, const char *line,
                        size_t len, char *reply, size_t n);

/* The origin given to clpd_ingest_post for facts the socket queues. */
const void *clpd_admin_origin(void);
/* clpd_ingest_reject_fn for the cycle's drain: a fact the socket queued is
 * logged as "clpdaemon: admin socket: inject rejected: <message>"; any
 * other goes to clpd_plugins_post_rejected (plugins is a clpd_plugins). */
void clpd_admin_post_rejected(void *plugins, const void *origin,
                              const char *message);

typedef struct clpd_admin clpd_admin;

/* Creates the listening socket at path with the permission bits mode
 * (never wider, also while it is being created); a socket already at path
 * is replaced. On failure writes one "clpdaemon: <path>: ..." line to log
 * and returns NULL. */
clpd_admin *clpd_admin_bind(const char *path, int mode, FILE *log);
/* Starts serving through loop (copies *s). On failure writes one line to
 * log and returns -1. */
int clpd_admin_attach(clpd_admin *a, clpd_loop *loop,
                      const clpd_admin_state *s, FILE *log);
/* Polls the listener again if accept ran out of descriptors or memory
 * (it is then not polled until a client closes); called once per cycle.
 * NULL is allowed. */
void clpd_admin_resume(clpd_admin *a);
/* Closes every client and unregisters the socket; call it before the loop
 * closes. No-op when not attached; NULL is allowed. */
void clpd_admin_detach(clpd_admin *a);
/* Detaches, closes the socket and removes its file, unless another file
 * replaced it meanwhile. NULL is allowed. */
void clpd_admin_close(clpd_admin *a);

#endif
