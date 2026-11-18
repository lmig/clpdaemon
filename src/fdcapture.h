/* Capture of the process descriptors 1 and 2 into the trace (FR-16, L48):
 * anything written to them (plugins, children started by CLIPS `system`,
 * stray printf) becomes trace `output` events with stream `stdout` or
 * `stderr` and no wf_id, while daemon messages go to a log stream on the
 * original stderr. Behaviour is documented in docs/trace.md. */
#ifndef CLPD_FDCAPTURE_H
#define CLPD_FDCAPTURE_H

#include "trace.h"

#include <stdio.h>

/* Longest pending line: a line that reaches it without a newline is
 * written as one event and the rest continues in the next. */
#define CLPD_FDCAPTURE_LINE_MAX 65536

typedef struct clpd_fdcapture clpd_fdcapture;

/* Saves the original stderr as the daemon log stream (close-on-exec, line
 * buffered), points fds 1 and 2 at two pipes, makes stdout line buffered,
 * starts the reader thread (all signals blocked) and makes the log the
 * trace's error stream. On failure writes "clpdaemon: cannot redirect
 * stdout and stderr to the trace: <error>" to err, leaves fds 1 and 2 as
 * they were and returns NULL. err is also the trace's error stream again
 * after stop. t must outlive the capture. */
clpd_fdcapture *clpd_fdcapture_start(clpd_trace *t, FILE *err);
/* Daemon log stream (original stderr), valid until stop. */
FILE *clpd_fdcapture_log(clpd_fdcapture *c);
/* Flushes stdout and stderr, puts the original descriptors back on 1 and
 * 2, lets the reader read the bytes the pipes hold at that moment (no
 * more, so a busy child cannot delay the stop) and write partial lines as
 * events, stdout before stderr, joins it, restores the trace's error
 * stream, closes the log stream and frees c. Bytes a child process writes
 * after this point are not traced. Also works after a reader failure,
 * which reports "clpdaemon: trace capture failed: <error>; stdout and
 * stderr go back to the console" on the log and restores fds 1 and 2
 * itself. NULL is a no-op. */
void clpd_fdcapture_stop(clpd_fdcapture *c);

#endif
