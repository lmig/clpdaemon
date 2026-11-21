/* Unit tests for the clock fact (FR-10, T11b, docs/packages.md): the
 * template MAIN::clpd-clock and its export, the slot values for a given
 * wall-clock reading, one fact asserted by the first update and modified
 * by the next ones, and a new fact after a reset removed it. */
#define _POSIX_C_SOURCE 200809L

#include "clock.h"

#include <string.h>
#include <time.h>

#include "../common/expect.h"

static long long src_sec;
static long src_nsec;

static void test_source(long long *sec, long *nsec) {
  *sec = src_sec;
  *nsec = src_nsec;
}

static size_t clock_facts(Environment *env) {
  size_t n = 0;
  Fact *f;
  for (f = GetNextFact(env, NULL); f != NULL; f = GetNextFact(env, f))
    if (clpd_clock_is_template(FactDeftemplate(f))) n++;
  return n;
}

static size_t all_facts(Environment *env) {
  size_t n = 0;
  Fact *f;
  for (f = GetNextFact(env, NULL); f != NULL; f = GetNextFact(env, f)) n++;
  return n;
}

/* The slots of f: now-ms and now (copied into iso). */
static void slots(Fact *f, long long *ms, char iso[64]) {
  CLIPSValue v;
  *ms = -1;
  iso[0] = '\0';
  if (f == NULL) return;
  if (GetFactSlot(f, "now-ms", &v) == GSE_NO_ERROR &&
      v.header->type == INTEGER_TYPE)
    *ms = v.integerValue->contents;
  if (GetFactSlot(f, "now", &v) == GSE_NO_ERROR &&
      v.header->type == STRING_TYPE) {
    strncpy(iso, v.lexemeValue->contents, 63);
    iso[63] = '\0';
  }
}

/* The formatted values: milliseconds truncated, like the trace. */
static void test_format(void) {
  long long ms;
  char iso[32];
  clpd_clock_format(1794998245, 641999999, &ms, iso);
  EXPECT(ms == 1794998245641LL);
  EXPECT(strcmp(iso, "2026-11-18T10:37:25.641Z") == 0);
  clpd_clock_format(0, 0, &ms, iso);
  EXPECT(ms == 0);
  EXPECT(strcmp(iso, "1970-01-01T00:00:00.000Z") == 0);
  clpd_clock_format(951782400, 5000000, &ms, iso); /* leap day */
  EXPECT(ms == 951782400005LL);
  EXPECT(strcmp(iso, "2000-02-29T00:00:00.005Z") == 0);
}

/* The template: MAIN::clpd-clock with an INTEGER now-ms and a STRING now,
 * exported (a module importing it builds and matches it); defining it
 * twice is a no-op. */
static void test_define(void) {
  Environment *env = CreateEnvironment();
  Deftemplate *d;
  CLIPSValue v;
  EXPECT(clpd_clock_define(env) == 0);
  EXPECT(clpd_clock_define(env) == 0);
  d = FindDeftemplate(env, "MAIN::clpd-clock");
  EXPECT(d != NULL);
  if (d == NULL) {
    DestroyEnvironment(env);
    return;
  }
  EXPECT(clpd_clock_is_template(d));
  EXPECT(DeftemplateSlotTypes(d, "now-ms", &v) &&
         v.multifieldValue->length == 1 &&
         strcmp(v.multifieldValue->contents[0].lexemeValue->contents,
                "INTEGER") == 0);
  EXPECT(DeftemplateSlotTypes(d, "now", &v) &&
         v.multifieldValue->length == 1 &&
         strcmp(v.multifieldValue->contents[0].lexemeValue->contents,
                "STRING") == 0);
  EXPECT(Build(env, "(defmodule p@1 (import MAIN deftemplate clpd-clock))") ==
         BE_NO_ERROR);
  EXPECT(Build(env, "(defrule p@1::r (clpd-clock (now-ms ?n)) =>)") ==
         BE_NO_ERROR);
  /* Another template of that name in another module is not the clock. */
  EXPECT(Build(env, "(defmodule q@1)") == BE_NO_ERROR);
  EXPECT(Build(env, "(deftemplate q@1::clpd-clock (slot x))") == BE_NO_ERROR);
  EXPECT(!clpd_clock_is_template(FindDeftemplate(env, "q@1::clpd-clock")));
  DestroyEnvironment(env);
}

/* MAIN already redefined (CLIPS redefines it once): the export cannot be
 * added, -1 and no template. */
static void test_define_main_in_use(void) {
  Environment *env = CreateEnvironment();
  EXPECT(Build(env, "(defmodule MAIN (export ?ALL))") == BE_NO_ERROR);
  EXPECT(clpd_clock_define(env) == -1);
  EXPECT(FindDeftemplate(env, "MAIN::clpd-clock") == NULL);
  DestroyEnvironment(env);
}

/* The first update asserts the one clock fact; later updates modify it
 * (same fact, same index, still one); after a reset removed it, the next
 * update asserts a new one. */
static void test_update(void) {
  Environment *env = CreateEnvironment();
  clpd_clock *c;
  Fact *f;
  long long ms, index;
  char iso[64];
  EXPECT(clpd_clock_define(env) == 0);
  c = clpd_clock_create(env);
  EXPECT(c != NULL);
  if (c == NULL) {
    DestroyEnvironment(env);
    return;
  }
  EXPECT(clpd_clock_fact(c) == NULL);
  clpd_clock_set_source(c, test_source);
  src_sec = 1794998245;
  src_nsec = 641999999;
  EXPECT(clpd_clock_update(c) == 0);
  f = clpd_clock_fact(c);
  EXPECT(f != NULL);
  EXPECT(clock_facts(env) == 1 && all_facts(env) == 1);
  slots(f, &ms, iso);
  EXPECT(ms == 1794998245641LL);
  EXPECT(strcmp(iso, "2026-11-18T10:37:25.641Z") == 0);
  index = f != NULL ? FactIndex(f) : -1;

  src_sec = 1794998246;
  src_nsec = 2000000;
  EXPECT(clpd_clock_update(c) == 0);
  EXPECT(clpd_clock_fact(c) == f);
  EXPECT(f != NULL && FactIndex(f) == index && FactExistp(f));
  EXPECT(clock_facts(env) == 1 && all_facts(env) == 1);
  slots(clpd_clock_fact(c), &ms, iso);
  EXPECT(ms == 1794998246002LL);
  EXPECT(strcmp(iso, "2026-11-18T10:37:26.002Z") == 0);

  /* The same reading again: still the one fact. */
  EXPECT(clpd_clock_update(c) == 0);
  EXPECT(clock_facts(env) == 1);

  Reset(env);
  EXPECT(clock_facts(env) == 0);
  src_sec = 1794998300;
  src_nsec = 0;
  EXPECT(clpd_clock_update(c) == 0);
  EXPECT(clock_facts(env) == 1 && all_facts(env) == 1);
  slots(clpd_clock_fact(c), &ms, iso);
  EXPECT(ms == 1794998300000LL);
  EXPECT(strcmp(iso, "2026-11-18T10:38:20.000Z") == 0);

  /* A rule's retract too. */
  EXPECT(Retract(clpd_clock_fact(c)) == RE_NO_ERROR);
  EXPECT(clock_facts(env) == 0);
  EXPECT(clpd_clock_update(c) == 0);
  EXPECT(clock_facts(env) == 1);
  clpd_clock_destroy(c);
  clpd_clock_destroy(NULL);
  DestroyEnvironment(env);
}

/* Without a source, the real UTC wall clock: within a second of
 * CLOCK_REALTIME, and now is now-ms formatted. */
static void test_real_clock(void) {
  Environment *env = CreateEnvironment();
  struct timespec before, after;
  clpd_clock *c;
  long long ms, want_ms;
  char iso[64], want[32];
  EXPECT(clpd_clock_define(env) == 0);
  c = clpd_clock_create(env);
  EXPECT(c != NULL);
  if (c == NULL) {
    DestroyEnvironment(env);
    return;
  }
  clock_gettime(CLOCK_REALTIME, &before);
  EXPECT(clpd_clock_update(c) == 0);
  clock_gettime(CLOCK_REALTIME, &after);
  slots(clpd_clock_fact(c), &ms, iso);
  EXPECT(ms >= (long long)before.tv_sec * 1000 + before.tv_nsec / 1000000);
  EXPECT(ms <= (long long)after.tv_sec * 1000 + after.tv_nsec / 1000000);
  clpd_clock_format(ms / 1000, (long)(ms % 1000) * 1000000, &want_ms, want);
  EXPECT(want_ms == ms && strcmp(iso, want) == 0);
  clpd_clock_destroy(c);
  DestroyEnvironment(env);
}

/* A rule fires on a deadline: (due (at 1000)) only once now-ms >= 1000. */
static void test_deadline_rule(void) {
  Environment *env = CreateEnvironment();
  clpd_clock *c;
  Fact *f;
  size_t due;
  EXPECT(clpd_clock_define(env) == 0);
  EXPECT(Build(env, "(defmodule d@1 (import MAIN deftemplate clpd-clock))") ==
         BE_NO_ERROR);
  EXPECT(Build(env, "(deftemplate d@1::due (slot wf-id) (slot at))") ==
         BE_NO_ERROR);
  EXPECT(Build(env, "(defrule d@1::ring ?d <- (due (wf-id ?w) (at ?at)) "
                    "(clpd-clock (now-ms ?n&:(>= ?n ?at))) => (retract ?d))") ==
         BE_NO_ERROR);
  c = clpd_clock_create(env);
  EXPECT(c != NULL);
  if (c == NULL) {
    DestroyEnvironment(env);
    return;
  }
  clpd_clock_set_source(c, test_source);
  src_sec = 0;
  src_nsec = 999000000; /* 999 ms */
  EXPECT(clpd_clock_update(c) == 0);
  SetCurrentModule(env, FindDefmodule(env, "d@1"));
  EXPECT(AssertString(env, "(due (wf-id w1) (at 1000))") != NULL);
  SetCurrentModule(env, FindDefmodule(env, "MAIN"));
  Focus(FindDefmodule(env, "d@1"));
  EXPECT(Run(env, -1) == 0);
  src_sec = 1;
  src_nsec = 0;
  EXPECT(clpd_clock_update(c) == 0);
  Focus(FindDefmodule(env, "d@1"));
  EXPECT(Run(env, -1) == 1);
  for (due = 0, f = GetNextFact(env, NULL); f != NULL;
       f = GetNextFact(env, f))
    if (!clpd_clock_is_template(FactDeftemplate(f))) due++;
  EXPECT(due == 0);
  clpd_clock_destroy(c);
  DestroyEnvironment(env);
}

int main(void) {
  test_format();
  test_define();
  test_define_main_in_use();
  test_update();
  test_real_clock();
  test_deadline_rule();
  if (failures != 0) {
    fprintf(stderr, "%d clock checks failed\n", failures);
    return 1;
  }
  puts("clock checks passed");
  return 0;
}
