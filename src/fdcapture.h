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
#define CLPD_FDCAPTURE_LINE_MAX CLPD_TRACE_LINE_MAX

typedef struct clpd_fdcapture clpd_fdcapture;

/* Saves the original stderr as the capture log stream (close-on-exec, line
 * buffered), points fds 1 and 2 at two pipes, makes the log both the
 * trace's error stream and the daemon log stream (src/log.h), then starts
 * the reader thread (all signals blocked). While capturing, SIGPIPE is
 * caught by a no-op handler (never SIG_IGN, so exec'd children keep the
 * default; an inherited SIG_IGN is left alone) and restored at stop, so a
 * daemon write on a pipe whose reader failed gets EPIPE instead of killing
 * the process. stdout buffering is left as it
 * is: the daemon makes stdout line buffered at startup (src/main.c), before
 * any use, so printf lines reach the pipe as they end. On failure writes
 * "clpdaemon: cannot redirect stdout and stderr to the trace: <error>" to
 * err, leaves fds 1 and 2, the trace's error stream and the daemon log
 * stream as they were and returns NULL. err is also the trace's error
 * stream again after stop. t must outlive the capture. */
clpd_fdcapture *clpd_fdcapture_start(clpd_trace *t, FILE *err);
/* Daemon log stream (original stderr), valid until stop. */
FILE *clpd_fdcapture_log(clpd_fdcapture *c);
/* Flushes stdout and stderr, puts the original descriptors back on 1 and
 * 2, lets the reader read the bytes the pipes hold at that moment (no
 * more, so a busy child cannot delay the stop) and write partial lines as
 * events, stdout before stderr, joins it, restores the trace's error
 * stream and the daemon log stream, closes the log and the pipes and frees
 * c. Bytes a child process writes after this point are not traced; it gets
 * EPIPE (SIGPIPE). Also works after a reader failure, which reports
 * "clpdaemon: trace capture failed: <error>; stdout and stderr go back to
 * the console" on the log, restores fds 1 and 2 itself, traces what the
 * pipes held and closes their read ends. NULL is a no-op. */
void clpd_fdcapture_stop(clpd_fdcapture *c);

#endif
