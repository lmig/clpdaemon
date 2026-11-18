/* CLIPS trace integration: router line assembly and wf_id attribution,
 * fact and fire events, watch gating and the inactive trace, checked
 * through the trace files written by a real CLIPS environment. */
#define _GNU_SOURCE

#include "clips_trace.h"
#include "log.h"

#include "../common/expect.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char dir[] = "/tmp/clpd-clips-trace-XXXXXX";
static char text_path[256], jsonl_path[256];

#define TS "2026-11-18T10:37:25.641Z n1 "
#define JTS "{\"ts\":\"2026-11-18T10:37:25.641Z\",\"node_id\":\"n1\","

static void fixed_clock(long long *sec, long *nsec) {
  *sec = 1794998245;
  *nsec = 641999999;
}

/* Big enough for the line-cap test (three lines of up to 64 KiB). */
static char *slurp(const char *path) {
  static char buf[512 * 1024];
  size_t n;
  FILE *f = fopen(path, "r");
  if (f == NULL) return NULL;
  n = fread(buf, 1, sizeof buf - 1, f);
  buf[n] = '\0';
  fclose(f);
  return buf;
}

static void expect_file(const char *what, const char *path, const char *want) {
  char *got = slurp(path);
  EXPECT(got != NULL);
  if (got == NULL) return;
  if (strcmp(got, want) != 0) {
    failures++;
    fprintf(stderr, "FAIL %s (%s):\n--- want\n%s--- got\n%s---\n", what, path,
            want, got);
  }
}

/* One environment attached to a fresh pair of trace files. */
typedef struct {
  Environment *env;
  clpd_trace *trace;
  clpd_clips_trace *ct;
  FILE *err;
} fixture;

static int setup(fixture *f, int facts, int rules, const char *constructs) {
  unlink(text_path);
  unlink(jsonl_path);
  f->err = tmpfile();
  f->trace = clpd_trace_open(text_path, jsonl_path, "n1", f->err);
  EXPECT(f->trace != NULL);
  if (f->trace == NULL) return -1;
  clpd_trace_set_clock(f->trace, fixed_clock);
  f->env = CreateEnvironment();
  EXPECT(f->env != NULL);
  if (constructs != NULL)
    EXPECT(LoadFromString(f->env, constructs, strlen(constructs)));
  f->ct = NULL;
  EXPECT(clpd_clips_trace_attach(f->env, f->trace, facts, rules, &f->ct) == 0);
  EXPECT(f->ct != NULL);
  return f->ct == NULL ? -1 : 0;
}

static void teardown(fixture *f) {
  char buf[256];
  size_t n;
  clpd_clips_trace_detach(f->ct);
  DestroyEnvironment(f->env);
  clpd_trace_close(f->trace);
  rewind(f->err);
  n = fread(buf, 1, sizeof buf - 1, f->err);
  buf[n] = '\0';
  EXPECT(strcmp(buf, "") == 0);
  fclose(f->err);
}

static void eval(Environment *env, const char *expr) {
  EXPECT(Eval(env, expr, NULL) == EE_NO_ERROR);
}

/* Contents of a temporary stream, which is closed. */
static char *stream_text(FILE *f) {
  static char buf[1024];
  size_t n;
  rewind(f);
  n = fread(buf, 1, sizeof buf - 1, f);
  buf[n] = '\0';
  fclose(f);
  return buf;
}

/* Fragments become lines; a pending partial line is written by flush and
 * by detach; each stream has its own line. Watch stays off. */
static void test_router_lines(void) {
  fixture f;
  if (setup(&f, 1, 1, NULL) != 0) return;
  EXPECT(!GetWatchState(f.env, FACTS));
  EXPECT(!GetWatchState(f.env, RULES));
  WriteString(f.env, STDOUT, "he");
  WriteString(f.env, STDERR, "e");
  WriteString(f.env, STDOUT, "llo\nwor");
  WriteString(f.env, STDWRN, "w1\n");
  WriteString(f.env, STDERR, "1\n");
  clpd_clips_trace_flush(f.ct);
  clpd_clips_trace_flush(f.ct); /* nothing pending: no line */
  eval(f.env, "(printout t \"via t\" crlf)");
  WriteString(f.env, STDOUT, "tail");
  teardown(&f);
  expect_file("router lines", text_path,
              TS "- STDOUT hello\n"
              TS "- STDWRN w1\n"
              TS "- STDERR e1\n"
              TS "- STDOUT wor\n"
              TS "- STDOUT via t\n"
              TS "- STDOUT tail\n");
  expect_file("router lines", jsonl_path,
              JTS "\"event\":\"output\",\"stream\":\"STDOUT\",\"text\":\"hello\"}\n"
              JTS "\"event\":\"output\",\"stream\":\"STDWRN\",\"text\":\"w1\"}\n"
              JTS "\"event\":\"output\",\"stream\":\"STDERR\",\"text\":\"e1\"}\n"
              JTS "\"event\":\"output\",\"stream\":\"STDOUT\",\"text\":\"wor\"}\n"
              JTS "\"event\":\"output\",\"stream\":\"STDOUT\",\"text\":\"via t\"}\n"
              JTS "\"event\":\"output\",\"stream\":\"STDOUT\",\"text\":\"tail\"}\n");
}

/* Text written while a rule fires carries the rule wf_id, also with rules
 * not watched; a partial line ends with the firing; text outside a firing
 * has no wf_id. Facts and rules off: no structured events. */
static void test_rule_wf_id(void) {
  fixture f;
  if (setup(&f, 0, 0,
            "(deftemplate order (slot wf-id))"
            "(defrule say (order (wf-id ?w)) =>"
            " (printout t \"in \" ?w crlf \"part\") (assert (order (wf-id x))))") != 0)
    return;
  Reset(f.env);
  WriteString(f.env, STDOUT, "be");
  EXPECT(AssertString(f.env, "(order (wf-id w7))") != NULL);
  WriteString(f.env, STDOUT, "fore\n");
  EXPECT(Run(f.env, 1) == 1);
  WriteString(f.env, STDOUT, "after\n");
  teardown(&f);
  expect_file("rule wf_id", text_path,
              TS "- STDOUT before\n"
              TS "w7 STDOUT in w7\n"
              TS "w7 STDOUT part\n"
              TS "- STDOUT after\n");
}

/* Assert and retract events with the wf-id slot (symbol, string, integer,
 * float; multifield and missing slot: none), ordered facts without wf_id,
 * one modify event per modify, a modify into a duplicate traced as the
 * retraction it is. Rules are not watched: no FIRE. */
static void test_fact_events(void) {
  fixture f;
  Fact *first;
  if (setup(&f, 1, 0,
            "(deftemplate order (slot wf-id) (slot qty))"
            "(deftemplate multi (multislot wf-id))"
            "(deftemplate item (slot code))"
            "(defrule grow (item (code grow)) => (assert (catalog grown)))") != 0)
    return;
  Reset(f.env);
  first = AssertString(f.env, "(order (wf-id w1) (qty 1))");
  EXPECT(first != NULL);
  EXPECT(AssertString(f.env, "(order (wf-id \"s2\") (qty 2))") != NULL);
  EXPECT(AssertString(f.env, "(order (wf-id 42) (qty 3))") != NULL);
  EXPECT(AssertString(f.env, "(order (wf-id 1.5) (qty 4))") != NULL);
  EXPECT(AssertString(f.env, "(multi (wf-id a b))") != NULL);
  EXPECT(AssertString(f.env, "(catalog a)") != NULL);
  EXPECT(AssertString(f.env, "(item (code grow))") != NULL);
  EXPECT(Retract(first) == RE_NO_ERROR);
  eval(f.env, "(modify 2 (qty 20))");
  eval(f.env, "(modify 3 (wf-id w9))");
  EXPECT(AssertString(f.env, "(order (wf-id d) (qty 0))") != NULL);
  eval(f.env, "(modify 8 (wf-id \"s2\") (qty 20))");
  EXPECT(Run(f.env, -1) == 1);
  /* The symbol nil (the slot default) means no wf-id; the string does not. */
  EXPECT(AssertString(f.env, "(order (qty 5))") != NULL);
  EXPECT(AssertString(f.env, "(order (wf-id \"nil\") (qty 6))") != NULL);
  teardown(&f);
  expect_file("fact events", text_path,
              TS "w1 FACT+ f-1 (order (wf-id w1) (qty 1))\n"
              TS "s2 FACT+ f-2 (order (wf-id \"s2\") (qty 2))\n"
              TS "42 FACT+ f-3 (order (wf-id 42) (qty 3))\n"
              TS "1.5 FACT+ f-4 (order (wf-id 1.5) (qty 4))\n"
              TS "- FACT+ f-5 (multi (wf-id a b))\n"
              TS "- FACT+ f-6 (catalog a)\n"
              TS "- FACT+ f-7 (item (code grow))\n"
              TS "w1 FACT- f-1 (order (wf-id w1) (qty 1))\n"
              TS "s2 FACT~ f-2 (order (wf-id \"s2\") (qty 20))\n"
              TS "w9 FACT~ f-3 (order (wf-id w9) (qty 3))\n"
              TS "d FACT+ f-8 (order (wf-id d) (qty 0))\n"
              TS "d FACT- f-8 (order (wf-id d) (qty 0))\n"
              TS "- FACT+ f-9 (catalog grown)\n"
              TS "- FACT+ f-10 (order (wf-id nil) (qty 5))\n"
              TS "nil FACT+ f-11 (order (wf-id \"nil\") (qty 6))\n");
}

/* Fire events: basis facts in pattern order, not and exists CEs and a COOL
 * instance skipped, no patterns -> no facts, wf_id from the first basis fact
 * with one. Facts are not watched: no FACT lines. */
static void test_fire_events(void) {
  fixture f;
  if (setup(&f, 0, 1,
            "(deftemplate order (slot wf-id))"
            "(deftemplate item (slot wf-id))"
            "(defclass C (is-a USER))"
            "(defrule none (declare (salience 40)) =>)"
            "(defrule join (declare (salience 30)) (order (wf-id ?w)) (item (wf-id ?w)) =>)"
            "(defrule lonely (declare (salience 20)) (order (wf-id ?w)) (not (item (wf-id ?w))) =>)"
            "(defrule ordered (declare (salience 10)) (catalog ?x) (order (wf-id ?w&w2)) =>)"
            "(defrule obj (declare (salience 5)) (object (is-a C)) (order (wf-id ?w&w1)) =>)"
            "(defrule ex (declare (salience 1)) (order (wf-id ?w&w1)) (exists (item (wf-id ?w))) =>)") != 0)
    return;
  Reset(f.env);
  EXPECT(AssertString(f.env, "(order (wf-id w1))") != NULL);
  EXPECT(AssertString(f.env, "(item (wf-id w1))") != NULL);
  EXPECT(AssertString(f.env, "(order (wf-id w2))") != NULL);
  EXPECT(AssertString(f.env, "(catalog a)") != NULL);
  eval(f.env, "(make-instance c1 of C)");
  EXPECT(Run(f.env, -1) == 6);
  teardown(&f);
  expect_file("fire events", text_path,
              TS "- FIRE MAIN::none\n"
              TS "w1 FIRE MAIN::join f-1,f-2\n"
              TS "w2 FIRE MAIN::lonely f-3\n"
              TS "w2 FIRE MAIN::ordered f-4,f-3\n"
              TS "w1 FIRE MAIN::obj f-1\n"
              TS "w1 FIRE MAIN::ex f-1\n");
  expect_file("fire events", jsonl_path,
              JTS "\"event\":\"fire\",\"module\":\"MAIN\",\"rule\":\"none\",\"facts\":[]}\n"
              JTS "\"wf_id\":\"w1\",\"event\":\"fire\",\"module\":\"MAIN\",\"rule\":\"join\",\"facts\":[1,2]}\n"
              JTS "\"wf_id\":\"w2\",\"event\":\"fire\",\"module\":\"MAIN\",\"rule\":\"lonely\",\"facts\":[3]}\n"
              JTS "\"wf_id\":\"w2\",\"event\":\"fire\",\"module\":\"MAIN\",\"rule\":\"ordered\",\"facts\":[4,3]}\n"
              JTS "\"wf_id\":\"w1\",\"event\":\"fire\",\"module\":\"MAIN\",\"rule\":\"obj\",\"facts\":[1]}\n"
              JTS "\"wf_id\":\"w1\",\"event\":\"fire\",\"module\":\"MAIN\",\"rule\":\"ex\",\"facts\":[1]}\n");
}

/* One modify event in JSONL too, with the template and the new text; the
 * rule that modifies is traced first and its output carries its wf_id. */
static void test_modify_jsonl(void) {
  fixture f;
  if (setup(&f, 1, 1,
            "(deftemplate order (slot wf-id) (slot status))"
            "(defrule start ?o <- (order (wf-id ?w) (status new)) =>"
            " (modify ?o (status started)) (printout t \"started\" crlf))") != 0)
    return;
  Reset(f.env);
  EXPECT(AssertString(f.env, "(order (wf-id w1) (status new))") != NULL);
  EXPECT(Run(f.env, -1) == 1);
  teardown(&f);
  expect_file("modify", text_path,
              TS "w1 FACT+ f-1 (order (wf-id w1) (status new))\n"
              TS "w1 FIRE MAIN::start f-1\n"
              TS "w1 FACT~ f-1 (order (wf-id w1) (status started))\n"
              TS "w1 STDOUT started\n");
  expect_file("modify", jsonl_path,
              JTS "\"wf_id\":\"w1\",\"event\":\"assert\",\"fact\":1,\"template\":\"order\",\"text\":\"(order (wf-id w1) (status new))\"}\n"
              JTS "\"wf_id\":\"w1\",\"event\":\"fire\",\"module\":\"MAIN\",\"rule\":\"start\",\"facts\":[1]}\n"
              JTS "\"wf_id\":\"w1\",\"event\":\"modify\",\"fact\":1,\"template\":\"order\",\"text\":\"(order (wf-id w1) (status started))\"}\n"
              JTS "\"wf_id\":\"w1\",\"event\":\"output\",\"stream\":\"STDOUT\",\"text\":\"started\"}\n");
}

/* After detach the environment writes to the console again and its events
 * are no longer traced. */
static void test_detach(void) {
  fixture f;
  if (setup(&f, 1, 1, NULL) != 0) return;
  clpd_clips_trace_detach(f.ct);
  f.ct = NULL;
  EXPECT(AssertString(f.env, "(catalog a)") != NULL);
  EXPECT(!DeleteRouter(f.env, "clpd-trace"));
  teardown(&f);
  expect_file("detach", text_path, "");
}

/* An inactive trace installs nothing: printout reaches the console (fd 1)
 * and no router or callback is added. */
static void test_inactive(void) {
  char out_path[300];
  /* Non-NULL sentinel, never dereferenced: attach must overwrite it. */
  static char sentinel;
  clpd_clips_trace *ct = (clpd_clips_trace *)&sentinel;
  clpd_trace *t = clpd_trace_open(NULL, NULL, "n1", stderr);
  Environment *env = CreateEnvironment();
  int saved, fd;
  char *got;

  EXPECT(t != NULL && env != NULL);
  if (t == NULL || env == NULL) return;
  EXPECT(clpd_clips_trace_attach(env, t, 1, 1, &ct) == 0);
  EXPECT(ct == NULL);
  EXPECT(!DeleteRouter(env, "clpd-trace"));
  snprintf(out_path, sizeof out_path, "%s/console.txt", dir);
  fflush(stdout);
  saved = dup(1);
  fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  EXPECT(saved >= 0 && fd >= 0);
  if (saved < 0 || fd < 0) return;
  dup2(fd, 1);
  close(fd);
  eval(env, "(printout t \"console\" crlf)");
  EXPECT(AssertString(env, "(catalog a)") != NULL);
  fflush(stdout);
  dup2(saved, 1);
  close(saved);
  got = slurp(out_path);
  EXPECT(got != NULL && strcmp(got, "console\n") == 0);
  unlink(out_path);
  clpd_clips_trace_flush(ct);
  clpd_clips_trace_detach(ct);
  DestroyEnvironment(env);
  clpd_trace_close(t);
}

/* L52 R4: while the sink fails to write, STDERR and STDWRN lines also go
 * to the daemon log stream (STDOUT does not); with a healthy sink they
 * only go to the trace. */
static void test_failing_sink_to_log(void) {
  FILE *err = tmpfile(), *log = tmpfile(), *prev;
  clpd_trace *t;
  clpd_clips_trace *ct = NULL;
  Environment *env = CreateEnvironment();
  EXPECT(err != NULL && log != NULL && env != NULL);
  if (err == NULL || log == NULL || env == NULL) return;
  prev = clpd_log_set(log);

  unlink(text_path);
  t = clpd_trace_open(text_path, NULL, "n1", err);
  EXPECT(t != NULL);
  if (t == NULL) return;
  clpd_trace_set_clock(t, fixed_clock);
  EXPECT(clpd_clips_trace_attach(env, t, 1, 1, &ct) == 0);
  EXPECT(!clpd_trace_failing(t));
  WriteString(env, STDERR, "healthy\n");
  clpd_clips_trace_detach(ct);
  clpd_trace_close(t);

  t = clpd_trace_open("/dev/full", NULL, "n1", err);
  EXPECT(t != NULL);
  if (t == NULL) return;
  EXPECT(clpd_clips_trace_attach(env, t, 1, 1, &ct) == 0);
  WriteString(env, STDOUT, "o1\n");
  EXPECT(clpd_trace_failing(t));
  WriteString(env, STDERR, "e1\n");
  WriteString(env, STDWRN, "w1\n");
  WriteString(env, STDERR, "e2");
  EXPECT(AssertString(env, "(catalog a)") != NULL);
  clpd_clips_trace_flush(ct);
  clpd_clips_trace_detach(ct);
  clpd_trace_close(t);
  DestroyEnvironment(env);

  clpd_log_set(prev);
  EXPECT(strcmp(stream_text(err), "clpdaemon: /dev/full: cannot write: No "
                                  "space left on device\n") == 0);
  EXPECT(strcmp(stream_text(log), "clpdaemon: STDERR: e1\n"
                                  "clpdaemon: STDWRN: w1\n"
                                  "clpdaemon: STDERR: e2\n") == 0);
  expect_file("healthy sink", text_path, TS "- STDERR healthy\n");
}

/* L52 R4: a pending output line that reaches CLPD_TRACE_LINE_MAX bytes is
 * written as one event and the rest continues, whether it arrives in
 * pieces or in one string (same rule as the fd capture, so a line of
 * exactly the cap is followed by an empty line); shorter lines are never
 * split. */
static void test_line_cap(void) {
  enum { CAP = CLPD_TRACE_LINE_MAX };
  size_t head = strlen(TS "- STDOUT ");
  char *in = malloc(CAP + 16), *want = malloc(3 * CAP + 8 * head + 64), *p;
  fixture f;
  EXPECT(in != NULL && want != NULL);
  if (in == NULL || want == NULL || setup(&f, 0, 0, NULL) != 0) {
    free(in);
    free(want);
    return;
  }
  /* CAP + 10 'a' in two pieces, then the newline. */
  memset(in, 'a', CAP / 2);
  in[CAP / 2] = '\0';
  WriteString(f.env, STDOUT, in);
  memset(in, 'a', CAP / 2 + 10);
  in[CAP / 2 + 10] = '\0';
  WriteString(f.env, STDOUT, in);
  WriteString(f.env, STDOUT, "\n");
  /* CAP + 5 'b' and the newline in one string. */
  memset(in, 'b', CAP + 5);
  memcpy(in + CAP + 5, "\n", 2);
  WriteString(f.env, STDOUT, in);
  /* Exactly CAP 'c', then the newline. */
  memset(in, 'c', CAP);
  memcpy(in + CAP, "\n", 2);
  WriteString(f.env, STDOUT, in);
  /* A short line in pieces stays one line. */
  WriteString(f.env, STDOUT, "sh");
  WriteString(f.env, STDOUT, "ort\n");
  teardown(&f);

  p = want;
#define LINE(ch, n)                         \
  do {                                      \
    memcpy(p, TS "- STDOUT ", head);        \
    p += head;                              \
    memset(p, ch, n);                       \
    p += n;                                 \
    *p++ = '\n';                            \
  } while (0)
  LINE('a', CAP);
  LINE('a', 10);
  LINE('b', CAP);
  LINE('b', 5);
  LINE('c', CAP);
#undef LINE
  strcpy(p, TS "- STDOUT \n" TS "- STDOUT short\n");
  expect_file("line cap", text_path, want);
  free(in);
  free(want);
}

/* Calls the trace's modify callback with (fact, NULL) only: the first half
 * of a modify that never reaches its second callback. */
static void half_modify(Environment *env, Fact *fact) {
  ModifyCallFunctionItem *m;
  for (m = FactData(env)->ListOfModifyFunctions; m != NULL; m = m->next)
    if (strcmp(m->name, "clpd-trace") == 0) {
      m->func(env, fact, NULL, m->context);
      return;
    }
  EXPECT(!"modify callback not installed");
}

/* L52 R3: suppression state left by a modify that ends between its two
 * callbacks does not survive the end of the cycle (flush) or the start of
 * the next rule firing: the fact's later retract is traced. */
static void test_aborted_modify(void) {
  fixture f;
  Fact *a, *b;
  if (setup(&f, 1, 0,
            "(deftemplate order (slot wf-id) (slot qty))"
            "(defrule drop ?o <- (order (qty 2)) => (retract ?o))") != 0)
    return;
  Reset(f.env);
  a = AssertString(f.env, "(order (wf-id w1) (qty 1))");
  b = AssertString(f.env, "(order (wf-id w2) (qty 2))");
  EXPECT(a != NULL && b != NULL);
  if (a == NULL || b == NULL) {
    teardown(&f);
    return;
  }
  half_modify(f.env, a);
  clpd_clips_trace_flush(f.ct);
  EXPECT(Retract(a) == RE_NO_ERROR);
  half_modify(f.env, b);
  EXPECT(Run(f.env, -1) == 1);
  teardown(&f);
  expect_file("aborted modify", text_path,
              TS "w1 FACT+ f-1 (order (wf-id w1) (qty 1))\n"
              TS "w2 FACT+ f-2 (order (wf-id w2) (qty 2))\n"
              TS "w1 FACT- f-1 (order (wf-id w1) (qty 1))\n"
              TS "w2 FACT- f-2 (order (wf-id w2) (qty 2))\n");
}

static bool query_none(Environment *env, const char *name, void *ctx) {
  (void)env;
  (void)name;
  (void)ctx;
  return false;
}

/* L52 R3: when the hooks cannot be installed (here a router already holds
 * the name), attach returns -1, sets *out to NULL and leaves nothing of its
 * own installed, without removing what it did not install. */
static void test_attach_failure(void) {
  static char sentinel;
  clpd_clips_trace *ct = (clpd_clips_trace *)&sentinel;
  clpd_trace *t = clpd_trace_open(text_path, NULL, "n1", stderr);
  Environment *env = CreateEnvironment();
  EXPECT(t != NULL && env != NULL);
  if (t == NULL || env == NULL) return;
  EXPECT(AddRouter(env, "clpd-trace", 0, query_none, NULL, NULL, NULL, NULL,
                   NULL));
  EXPECT(clpd_clips_trace_attach(env, t, 1, 1, &ct) == -1);
  EXPECT(ct == NULL);
  EXPECT(!RemoveBeforeRuleFiresFunction(env, "clpd-trace"));
  EXPECT(!RemoveAfterRuleFiresFunction(env, "clpd-trace"));
  EXPECT(!RemoveAssertFunction(env, "clpd-trace"));
  EXPECT(!RemoveRetractFunction(env, "clpd-trace"));
  EXPECT(!RemoveModifyFunction(env, "clpd-trace"));
  EXPECT(DeleteRouter(env, "clpd-trace")); /* the other router survived */
  EXPECT(!DeleteRouter(env, "clpd-trace"));
  DestroyEnvironment(env);
  clpd_trace_close(t);
}

int main(void) {
  if (mkdtemp(dir) == NULL) {
    perror("mkdtemp");
    return 2;
  }
  snprintf(text_path, sizeof text_path, "%s/t.log", dir);
  snprintf(jsonl_path, sizeof jsonl_path, "%s/t.jsonl", dir);
  test_router_lines();
  test_rule_wf_id();
  test_fact_events();
  test_fire_events();
  test_modify_jsonl();
  test_detach();
  test_inactive();
  test_failing_sink_to_log();
  test_line_cap();
  test_aborted_modify();
  test_attach_failure();
  unlink(text_path);
  unlink(jsonl_path);
  rmdir(dir);
  if (failures != 0) fprintf(stderr, "%d check(s) failed\n", failures);
  return failures != 0;
}
