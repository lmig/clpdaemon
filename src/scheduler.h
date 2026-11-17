/* Hybrid cycle scheduler (FR-3, D27): pure, no I/O. Times are monotonic
 * milliseconds. A cycle is due on notification no sooner than min_ms after
 * the previous cycle start, and on tick tick_ms after it (fixed rate). */
#ifndef CLPD_SCHEDULER_H
#define CLPD_SCHEDULER_H

typedef struct {
  long long tick_ms;
  long long min_ms;
  long long last_start_ms; /* start of the previous cycle */
  int started;             /* a cycle has run */
  int notified;            /* a notification arrived since the last start */
} clpd_sched;

/* min_ms must not exceed tick_ms (the configuration enforces it). The first
 * cycle is due at now_ms. */
void clpd_sched_init(clpd_sched *s, int tick_ms, int min_ms, long long now_ms);
void clpd_sched_notify(clpd_sched *s);
void clpd_sched_cycle_started(clpd_sched *s, long long now_ms);
/* Time at which the next cycle is due; it may lie in the past (overrun). */
long long clpd_sched_next_due(const clpd_sched *s);

#endif
