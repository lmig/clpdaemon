/* Trace sink (FR-13, FR-15, FR-5): text and JSON Lines files, one line per
 * event, each carrying node_id and, when known, wf_id. Formats are
 * documented in docs/trace.md. */
#ifndef CLPD_TRACE_H
#define CLPD_TRACE_H

#include <stddef.h>
#include <stdio.h>

typedef struct clpd_trace clpd_trace;

/* Wall clock used for timestamps (seconds and nanoseconds since the
 * epoch); replaceable for tests. */
typedef void clpd_trace_clock(long long *sec, long *nsec);

typedef enum {
  CLPD_TRACE_ASSERT,
  CLPD_TRACE_RETRACT,
  CLPD_TRACE_MODIFY
} clpd_trace_fact_kind;

/* Opens the configured files in append mode; a NULL path disables that
 * format. With both NULL the trace is inactive and every event is dropped.
 * On failure writes "clpdaemon: <path>: cannot open: <error>" to err for
 * each file that failed and returns NULL. */
clpd_trace *clpd_trace_open(const char *text_path, const char *jsonl_path,
                            const char *node_id, FILE *err);
void clpd_trace_set_clock(clpd_trace *t, clpd_trace_clock *clock);
/* 1 when at least one file is configured. */
int clpd_trace_active(const clpd_trace *t);
/* Reopens every file by path (log rotation). A file that cannot be reopened
 * keeps its previous stream and "clpdaemon: <path>: cannot reopen: <error>"
 * goes to err. Returns the number of files that failed. */
int clpd_trace_reopen(clpd_trace *t, FILE *err);

/* wf_id may be NULL (unknown). Text arguments are not NUL-sensitive beyond
 * their terminator, except output, which takes an explicit length. */
void clpd_trace_fact(clpd_trace *t, clpd_trace_fact_kind kind,
                     const char *wf_id, long long index, const char *tmpl,
                     const char *text);
void clpd_trace_fire(clpd_trace *t, const char *wf_id, const char *module,
                     const char *rule, const long long *facts, size_t n);
void clpd_trace_output(clpd_trace *t, const char *wf_id, const char *stream,
                       const char *text, size_t len);

void clpd_trace_close(clpd_trace *t);

#endif
