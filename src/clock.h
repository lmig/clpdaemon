/* Clock fact (FR-10, T11b; docs/packages.md#the-clock-fact): the core
 * defines the deftemplate MAIN::clpd-clock, exported to every package
 * module, and keeps exactly one fact of it whose slots hold the UTC wall
 * clock: now-ms (INTEGER, Unix time in milliseconds) and now (STRING,
 * ISO-8601 UTC with milliseconds and Z, the trace format). The fact is
 * asserted at startup after Reset and modified at the start of every
 * cycle, so rules can express deadlines and timeouts. */
#ifndef CLPD_CLOCK_H
#define CLPD_CLOCK_H

#include "clips.h"

/* The deftemplate name, reserved: a package cannot define its own. */
#define CLPD_CLOCK_TEMPLATE "clpd-clock"

/* Wall clock read by the updates (seconds and nanoseconds since the
 * epoch, like clpd_trace_clock); replaceable for tests. */
typedef void clpd_clock_source(long long *sec, long *nsec);

/* Makes MAIN export the deftemplate clpd-clock (by redefining MAIN, which
 * CLIPS allows once) and defines it; does nothing when MAIN::clpd-clock
 * already exists. Returns 0, or -1 when it cannot be defined (MAIN was
 * already redefined, for example). */
int clpd_clock_define(Environment *env);

/* 1 when d is the core's MAIN::clpd-clock. */
int clpd_clock_is_template(Deftemplate *d);

/* Formats a wall-clock reading as the slots hold it: *now_ms, and iso
 * (UTC ISO-8601 with milliseconds, truncated, and Z). */
void clpd_clock_format(long long sec, long nsec, long long *now_ms,
                       char iso[32]);

typedef struct clpd_clock clpd_clock;

/* The clock fact of env (MAIN::clpd-clock must be defined); no fact is
 * asserted until the first update. NULL when out of memory. */
clpd_clock *clpd_clock_create(Environment *env);
/* Replaces the wall clock (NULL: the real one, CLOCK_REALTIME). */
void clpd_clock_set_source(clpd_clock *c, clpd_clock_source *source);
/* Reads the wall clock and asserts the clock fact, or modifies it when it
 * still exists (a rule's reset or retract removed it: a new one is
 * asserted). Returns 0, or -1 when the fact cannot be asserted or
 * modified. */
int clpd_clock_update(clpd_clock *c);
/* The clock fact, NULL before the first update or after a failed one. */
Fact *clpd_clock_fact(const clpd_clock *c);
/* Releases the fact; before the environment is destroyed. NULL is a
 * no-op. */
void clpd_clock_destroy(clpd_clock *c);

#endif
