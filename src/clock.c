/* Clock fact (FR-10, T11b): src/clock.h, docs/packages.md#the-clock-fact. */
#define _POSIX_C_SOURCE 200809L

#include "clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define QUALIFIED "MAIN::" CLPD_CLOCK_TEMPLATE

struct clpd_clock {
  Environment *env;
  clpd_clock_source *source;
  Fact *fact; /* retained while held; NULL before the first update */
};

static void real_clock(long long *sec, long *nsec) {
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  *sec = (long long)now.tv_sec;
  *nsec = now.tv_nsec;
}

int clpd_clock_define(Environment *env) {
  if (FindDeftemplate(env, QUALIFIED) != NULL) return 0;
  /* MAIN is redefined (CLIPS allows it once) to export the template,
   * which every package module imports (src/packages.c). */
  if (Build(env, "(defmodule MAIN (export deftemplate " CLPD_CLOCK_TEMPLATE
                 "))") != BE_NO_ERROR ||
      Build(env, "(deftemplate " QUALIFIED " (slot now-ms (type INTEGER)) "
                 "(slot now (type STRING)))") != BE_NO_ERROR)
    return -1;
  return 0;
}

int clpd_clock_is_template(Deftemplate *d) {
  return d != NULL && strcmp(DeftemplateName(d), CLPD_CLOCK_TEMPLATE) == 0 &&
         strcmp(DeftemplateModule(d), "MAIN") == 0;
}

/* Like the trace timestamp (src/trace.c): milliseconds truncated. */
void clpd_clock_format(long long sec, long nsec, long long *now_ms,
                       char iso[32]) {
  time_t secs = (time_t)sec;
  struct tm tm;
  size_t n;
  *now_ms = sec * 1000 + nsec / 1000000;
  gmtime_r(&secs, &tm);
  n = strftime(iso, 32, "%Y-%m-%dT%H:%M:%S", &tm);
  snprintf(iso + n, 32 - n, ".%03ldZ", nsec / 1000000);
}

clpd_clock *clpd_clock_create(Environment *env) {
  clpd_clock *c = calloc(1, sizeof *c);
  if (c == NULL) return NULL;
  c->env = env;
  c->source = real_clock;
  return c;
}

void clpd_clock_set_source(clpd_clock *c, clpd_clock_source *source) {
  c->source = source != NULL ? source : real_clock;
}

static void drop(clpd_clock *c) {
  if (c->fact == NULL) return;
  ReleaseFact(c->fact);
  c->fact = NULL;
}

/* Holds f (retained), or nothing when f is NULL. */
static void hold(clpd_clock *c, Fact *f) {
  if (f == c->fact) return;
  drop(c);
  if (f == NULL) return;
  RetainFact(f);
  c->fact = f;
}

static Fact *assert_new(clpd_clock *c, long long ms, const char *iso) {
  FactBuilder *fb = CreateFactBuilder(c->env, QUALIFIED);
  Fact *f;
  if (fb == NULL) return NULL;
  if (FBPutSlotInteger(fb, "now-ms", ms) != PSE_NO_ERROR ||
      FBPutSlotString(fb, "now", iso) != PSE_NO_ERROR) {
    FBDispose(fb);
    return NULL;
  }
  f = FBAssert(fb);
  FBDispose(fb);
  return f;
}

static Fact *modify(clpd_clock *c, long long ms, const char *iso) {
  FactModifier *fm = CreateFactModifier(c->env, c->fact);
  Fact *f;
  if (fm == NULL) return NULL;
  if (FMPutSlotInteger(fm, "now-ms", ms) != PSE_NO_ERROR ||
      FMPutSlotString(fm, "now", iso) != PSE_NO_ERROR) {
    FMDispose(fm);
    return NULL;
  }
  f = FMModify(fm);
  FMDispose(fm);
  return f;
}

int clpd_clock_update(clpd_clock *c) {
  long long sec, ms;
  long nsec;
  char iso[32];
  Fact *f;
  c->source(&sec, &nsec);
  clpd_clock_format(sec, nsec, &ms, iso);
  if (c->fact != NULL && FactExistp(c->fact)) {
    f = modify(c, ms, iso);
    /* Modified in place, or a failed modify that left it: keep it. */
    if (FactExistp(c->fact)) return f != NULL ? 0 : -1;
    /* A modify into a duplicate (another clock fact with those values)
     * only retracts the old version and returns the other fact. */
    if (f == NULL) f = assert_new(c, ms, iso);
  } else {
    f = assert_new(c, ms, iso);
  }
  hold(c, f);
  return f != NULL ? 0 : -1;
}

Fact *clpd_clock_fact(const clpd_clock *c) { return c->fact; }

void clpd_clock_destroy(clpd_clock *c) {
  if (c == NULL) return;
  drop(c);
  free(c);
}
