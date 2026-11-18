/* Daemon log stream: where every "clpdaemon: ..." message goes, fatal ones
 * included. It is stderr by default; while fds 1 and 2 are captured into
 * the trace (src/fdcapture.h) it is the capture's copy of the original
 * stderr, so daemon messages never land in the trace pipe, where they could
 * be lost on exit. */
#ifndef CLPD_LOG_H
#define CLPD_LOG_H

#include <stdio.h>

/* Current daemon log stream. Safe to call from any thread. */
FILE *clpd_log(void);
/* Makes f the daemon log stream; NULL means stderr again. Returns the
 * previous stream (never NULL), so a caller can put it back. */
FILE *clpd_log_set(FILE *f);
/* Fatal system error (for example out of memory): writes
 * "clpdaemon: <strerror(errno)>" to the daemon log stream and exits 1. */
#if defined(__GNUC__)
__attribute__((noreturn))
#endif
void clpd_log_die_errno(void);

#endif
