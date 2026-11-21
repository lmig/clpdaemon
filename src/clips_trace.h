/* Connects a CLIPS environment to the trace sink (FR-13 to FR-16, L48):
 * a router for STDOUT, STDERR and STDWRN turned into output lines, and the
 * structured fact and rule-fire events selected by `watch`; modifies of
 * the core's clock fact (src/clock.h) are not traced. Attribution rules
 * are documented in docs/trace.md. */
#ifndef CLPD_CLIPS_TRACE_H
#define CLPD_CLIPS_TRACE_H

#include "clips.h"
#include "trace.h"

typedef struct clpd_clips_trace clpd_clips_trace;

/* Installs the router and callbacks on env when the trace is active and
 * sets *out; with an inactive trace installs nothing and sets *out to NULL.
 * CLIPS Watch is left as it is (off): the events come from the callbacks.
 * Returns 0, or -1 when the hooks cannot be installed: then *out is NULL
 * and none of the hooks it added is left installed (a router or callback of
 * the same name owned by someone else is kept). t must outlive the
 * attachment. Out of memory inside a callback is fatal: the message goes
 * to the daemon log stream (src/log.h) and the process exits 1. */
int clpd_clips_trace_attach(Environment *env, clpd_trace *t, int watch_facts,
                            int watch_rules, clpd_clips_trace **out);
/* End of an engine cycle: writes every pending partial output line and
 * drops modify suppression state a cut-short modify may have left. NULL is
 * a no-op. */
void clpd_clips_trace_flush(clpd_clips_trace *ct);
/* Flushes, removes the router and callbacks and frees ct. NULL is a
 * no-op. Must run before the environment is destroyed. */
void clpd_clips_trace_detach(clpd_clips_trace *ct);

#endif
