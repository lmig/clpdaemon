#include "scheduler.h"

void clpd_sched_init(clpd_sched *s, int tick_ms, int min_ms, long long now_ms) {
  s->tick_ms = tick_ms;
  s->min_ms = min_ms;
  s->last_start_ms = now_ms;
  s->started = 0;
  s->notified = 0;
}

void clpd_sched_notify(clpd_sched *s) { s->notified = 1; }

void clpd_sched_cycle_started(clpd_sched *s, long long now_ms) {
  s->last_start_ms = now_ms;
  s->started = 1;
  s->notified = 0;
}

long long clpd_sched_next_due(const clpd_sched *s) {
  if (!s->started) return s->last_start_ms;
  return s->last_start_ms + (s->notified ? s->min_ms : s->tick_ms);
}
