/* Unit tests for the hybrid cycle scheduler (FR-3, D27, L25). */
#include "scheduler.h"

#include "../common/expect.h"

static void test_first_cycle_runs_at_start(void) {
  clpd_sched s;
  clpd_sched_init(&s, 1000, 100, 5000);
  EXPECT(clpd_sched_next_due(&s) == 5000);
}

static void test_tick_from_previous_start(void) {
  clpd_sched s;
  clpd_sched_init(&s, 1000, 100, 0);
  clpd_sched_cycle_started(&s, 0);
  EXPECT(clpd_sched_next_due(&s) == 1000);
  clpd_sched_cycle_started(&s, 1003); /* late wake-up: fixed rate from start */
  EXPECT(clpd_sched_next_due(&s) == 2003);
}

static void test_notification_waits_for_min_interval(void) {
  clpd_sched s;
  clpd_sched_init(&s, 1000, 100, 0);
  clpd_sched_cycle_started(&s, 0);
  clpd_sched_notify(&s);
  EXPECT(clpd_sched_next_due(&s) == 100);
  clpd_sched_notify(&s); /* repeated notifications coalesce */
  EXPECT(clpd_sched_next_due(&s) == 100);
  clpd_sched_cycle_started(&s, 100); /* the cycle consumes the notification */
  EXPECT(clpd_sched_next_due(&s) == 1100);
}

static void test_notification_after_min_interval_is_due_now(void) {
  clpd_sched s;
  clpd_sched_init(&s, 1000, 100, 0);
  clpd_sched_cycle_started(&s, 0);
  clpd_sched_notify(&s); /* arrives at t=500, already past t=100 */
  EXPECT(clpd_sched_next_due(&s) == 100);
}

static void test_equal_intervals_are_pure_periodic(void) {
  clpd_sched s;
  clpd_sched_init(&s, 250, 250, 0);
  clpd_sched_cycle_started(&s, 0);
  EXPECT(clpd_sched_next_due(&s) == 250);
  clpd_sched_notify(&s);
  EXPECT(clpd_sched_next_due(&s) == 250);
}

static void test_zero_min_interval(void) {
  clpd_sched s;
  clpd_sched_init(&s, 1000, 0, 0);
  clpd_sched_cycle_started(&s, 40);
  clpd_sched_notify(&s);
  EXPECT(clpd_sched_next_due(&s) == 40);
}

static void test_notification_before_first_cycle(void) {
  clpd_sched s;
  clpd_sched_init(&s, 1000, 100, 7);
  clpd_sched_notify(&s);
  EXPECT(clpd_sched_next_due(&s) == 7);
}

int main(void) {
  test_first_cycle_runs_at_start();
  test_tick_from_previous_start();
  test_notification_waits_for_min_interval();
  test_notification_after_min_interval_is_due_now();
  test_equal_intervals_are_pure_periodic();
  test_zero_min_interval();
  test_notification_before_first_cycle();
  if (failures != 0) {
    fprintf(stderr, "%d scheduler checks failed\n", failures);
    return 1;
  }
  puts("scheduler checks passed");
  return 0;
}
