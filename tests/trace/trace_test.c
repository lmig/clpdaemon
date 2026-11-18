/* Trace sink: open errors, both line formats, wf_id handling, escaping and
 * reopen after rotation, checked through the files the sink writes. */
#define _GNU_SOURCE

#include "trace.h"

#include "../common/expect.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char dir[] = "/tmp/clpd-trace-XXXXXX";
static char text_path[256], jsonl_path[256];

/* 2026-11-18T10:37:25.641Z */
static void fixed_clock(long long *sec, long *nsec) {
  *sec = 1794998245;
  *nsec = 641999999;
}

static char *slurp(const char *path) {
  static char buf[8192];
  size_t n;
  FILE *f = fopen(path, "r");
  if (f == NULL) return NULL;
  n = fread(buf, 1, sizeof buf - 1, f);
  buf[n] = '\0';
  fclose(f);
  return buf;
}

/* Text written to a temporary error stream. */
static char *err_text(FILE *err) {
  static char buf[1024];
  size_t n;
  rewind(err);
  n = fread(buf, 1, sizeof buf - 1, err);
  buf[n] = '\0';
  fclose(err);
  return buf;
}

static void expect_file(const char *path, const char *want) {
  char *got = slurp(path);
  EXPECT(got != NULL);
  if (got == NULL) return;
  if (strcmp(got, want) != 0) {
    failures++;
    fprintf(stderr, "FAIL %s:\n--- want\n%s--- got\n%s---\n", path, want, got);
  }
}

static void test_formats(void) {
  static const long long basis[] = {12, 13};
  FILE *err = tmpfile();
  clpd_trace *t = clpd_trace_open(text_path, jsonl_path, "n1", err);

  EXPECT(t != NULL);
  if (t == NULL) return;
  EXPECT(clpd_trace_active(t));
  clpd_trace_set_clock(t, fixed_clock);
  clpd_trace_fact(t, CLPD_TRACE_ASSERT, "wf-42", 12, "order",
                  "(order (wf-id wf-42) (qty 3))");
  clpd_trace_fact(t, CLPD_TRACE_MODIFY, "wf-42", 12, "order",
                  "(order (wf-id wf-42) (qty 4))");
  clpd_trace_fact(t, CLPD_TRACE_RETRACT, NULL, 3, "catalog", "(catalog a)");
  clpd_trace_fire(t, "wf-42", "ship@1", "send", basis, 2);
  clpd_trace_fire(t, NULL, "ship@1", "start", NULL, 0);
  clpd_trace_output(t, NULL, "STDOUT", "hello", 5);
  clpd_trace_output(t, "wf-42", "STDERR", "a \"q\" \\ \t\x01z", 11);
  clpd_trace_close(t);
  EXPECT(strcmp(err_text(err), "") == 0);

  expect_file(text_path,
              "2026-11-18T10:37:25.641Z n1 wf-42 FACT+ f-12 (order (wf-id wf-42) (qty 3))\n"
              "2026-11-18T10:37:25.641Z n1 wf-42 FACT~ f-12 (order (wf-id wf-42) (qty 4))\n"
              "2026-11-18T10:37:25.641Z n1 - FACT- f-3 (catalog a)\n"
              "2026-11-18T10:37:25.641Z n1 wf-42 FIRE ship@1::send f-12,f-13\n"
              "2026-11-18T10:37:25.641Z n1 - FIRE ship@1::start\n"
              "2026-11-18T10:37:25.641Z n1 - STDOUT hello\n"
              "2026-11-18T10:37:25.641Z n1 wf-42 STDERR a \"q\" \\ \\x09\\x01z\n");
  expect_file(jsonl_path,
              "{\"ts\":\"2026-11-18T10:37:25.641Z\",\"node_id\":\"n1\",\"wf_id\":\"wf-42\",\"event\":\"assert\",\"fact\":12,\"template\":\"order\",\"text\":\"(order (wf-id wf-42) (qty 3))\"}\n"
              "{\"ts\":\"2026-11-18T10:37:25.641Z\",\"node_id\":\"n1\",\"wf_id\":\"wf-42\",\"event\":\"modify\",\"fact\":12,\"template\":\"order\",\"text\":\"(order (wf-id wf-42) (qty 4))\"}\n"
              "{\"ts\":\"2026-11-18T10:37:25.641Z\",\"node_id\":\"n1\",\"event\":\"retract\",\"fact\":3,\"template\":\"catalog\",\"text\":\"(catalog a)\"}\n"
              "{\"ts\":\"2026-11-18T10:37:25.641Z\",\"node_id\":\"n1\",\"wf_id\":\"wf-42\",\"event\":\"fire\",\"module\":\"ship@1\",\"rule\":\"send\",\"facts\":[12,13]}\n"
              "{\"ts\":\"2026-11-18T10:37:25.641Z\",\"node_id\":\"n1\",\"event\":\"fire\",\"module\":\"ship@1\",\"rule\":\"start\",\"facts\":[]}\n"
              "{\"ts\":\"2026-11-18T10:37:25.641Z\",\"node_id\":\"n1\",\"event\":\"output\",\"stream\":\"STDOUT\",\"text\":\"hello\"}\n"
              "{\"ts\":\"2026-11-18T10:37:25.641Z\",\"node_id\":\"n1\",\"wf_id\":\"wf-42\",\"event\":\"output\",\"stream\":\"STDERR\",\"text\":\"a \\\"q\\\" \\\\ \\t\\u0001z\"}\n");
}

/* Appends to existing files instead of truncating them. */
static void test_append(void) {
  int i;
  for (i = 0; i < 2; i++) {
    clpd_trace *t = clpd_trace_open(text_path, NULL, "n1", stderr);
    EXPECT(t != NULL);
    if (t == NULL) return;
    clpd_trace_set_clock(t, fixed_clock);
    clpd_trace_output(t, NULL, "STDOUT", i == 0 ? "x" : "y", 1);
    clpd_trace_close(t);
  }
  expect_file(text_path, "2026-11-18T10:37:25.641Z n1 - STDOUT x\n"
                         "2026-11-18T10:37:25.641Z n1 - STDOUT y\n");
}

static void test_inactive(void) {
  clpd_trace *t = clpd_trace_open(NULL, NULL, "n1", stderr);
  EXPECT(t != NULL);
  if (t == NULL) return;
  EXPECT(!clpd_trace_active(t));
  clpd_trace_output(t, NULL, "STDOUT", "dropped", 7);
  EXPECT(clpd_trace_reopen(t, stderr) == 0);
  clpd_trace_close(t);
}

static void test_open_errors(void) {
  char missing[300], buf[1024];
  FILE *err = tmpfile();
  clpd_trace *t;

  snprintf(missing, sizeof missing, "%s/no/such.log", dir);
  t = clpd_trace_open(missing, dir, "n1", err);
  EXPECT(t == NULL);
  snprintf(buf, sizeof buf,
           "clpdaemon: %s: cannot open: No such file or directory\n"
           "clpdaemon: %s: cannot open: Is a directory\n",
           missing, dir);
  EXPECT(strcmp(err_text(err), buf) == 0);
}

static void test_reopen(void) {
  char rotated[300], sub[300], sub_file[320], buf[1024];
  FILE *err;
  clpd_trace *t;

  snprintf(rotated, sizeof rotated, "%s/trace.log.1", dir);
  snprintf(sub, sizeof sub, "%s/sub", dir);
  snprintf(sub_file, sizeof sub_file, "%s/t.log", sub);
  unlink(text_path);
  EXPECT(mkdir(sub, 0700) == 0);

  t = clpd_trace_open(text_path, sub_file, "n1", stderr);
  EXPECT(t != NULL);
  if (t == NULL) return;
  clpd_trace_set_clock(t, fixed_clock);
  clpd_trace_output(t, NULL, "STDOUT", "before", 6);

  /* Rotation: the old file moves away, the reopen creates a new one. The
   * directory of the second file is gone, so that one keeps its stream. */
  EXPECT(rename(text_path, rotated) == 0);
  EXPECT(unlink(sub_file) == 0);
  EXPECT(rmdir(sub) == 0);
  err = tmpfile();
  EXPECT(clpd_trace_reopen(t, err) == 1);
  snprintf(buf, sizeof buf,
           "clpdaemon: %s: cannot reopen: No such file or directory\n",
           sub_file);
  EXPECT(strcmp(err_text(err), buf) == 0);
  clpd_trace_output(t, NULL, "STDOUT", "after", 5);
  clpd_trace_close(t);

  expect_file(rotated, "2026-11-18T10:37:25.641Z n1 - STDOUT before\n");
  expect_file(text_path, "2026-11-18T10:37:25.641Z n1 - STDOUT after\n");
  unlink(rotated);
}

/* A failing write is reported once per file until a reopen succeeds. */
static void test_write_error(void) {
  FILE *err = tmpfile();
  clpd_trace *t = clpd_trace_open("/dev/full", NULL, "n1", err);
  EXPECT(t != NULL);
  if (t == NULL) return;
  clpd_trace_output(t, NULL, "STDOUT", "a", 1);
  clpd_trace_output(t, NULL, "STDOUT", "b", 1);
  EXPECT(clpd_trace_reopen(t, err) == 0);
  clpd_trace_output(t, NULL, "STDOUT", "c", 1);
  clpd_trace_close(t);
  EXPECT(strcmp(err_text(err),
                "clpdaemon: /dev/full: cannot write: No space left on device\n"
                "clpdaemon: /dev/full: cannot write: No space left on device\n") == 0);
}

/* The write-failure state is visible through clpd_trace_failing until a
 * reopen succeeds; an inactive or healthy trace is not failing. */
static void test_failing(void) {
  FILE *err = tmpfile();
  clpd_trace *t = clpd_trace_open(NULL, NULL, "n1", err);
  EXPECT(t != NULL);
  if (t == NULL) return;
  EXPECT(!clpd_trace_failing(t));
  clpd_trace_close(t);
  t = clpd_trace_open(text_path, "/dev/full", "n1", err);
  EXPECT(t != NULL);
  if (t == NULL) return;
  EXPECT(!clpd_trace_failing(t));
  clpd_trace_output(t, NULL, "STDOUT", "a", 1);
  EXPECT(clpd_trace_failing(t));
  EXPECT(clpd_trace_reopen(t, err) == 0);
  EXPECT(!clpd_trace_failing(t));
  clpd_trace_close(t);
  EXPECT(strcmp(err_text(err),
                "clpdaemon: /dev/full: cannot write: No space left on device\n") == 0);
  unlink(text_path);
}

/* After clpd_trace_set_err, write failures go to the new stream only. */
static void test_set_err(void) {
  FILE *first = tmpfile(), *second = tmpfile();
  clpd_trace *t = clpd_trace_open("/dev/full", NULL, "n1", first);
  EXPECT(t != NULL);
  if (t == NULL) return;
  clpd_trace_set_err(t, second);
  clpd_trace_output(t, NULL, "STDOUT", "a", 1);
  clpd_trace_close(t);
  EXPECT(strcmp(err_text(first), "") == 0);
  EXPECT(strcmp(err_text(second),
                "clpdaemon: /dev/full: cannot write: No space left on device\n") == 0);
}

/* The largest fact index is written in full, not cut or over-read. */
static void test_large_index(void) {
  clpd_trace *t = clpd_trace_open(NULL, jsonl_path, "n1", stderr);
  EXPECT(t != NULL);
  if (t == NULL) return;
  clpd_trace_set_clock(t, fixed_clock);
  clpd_trace_fact(t, CLPD_TRACE_ASSERT, NULL, 9223372036854775807LL, "x",
                  "(x)");
  clpd_trace_close(t);
  expect_file(jsonl_path,
              "{\"ts\":\"2026-11-18T10:37:25.641Z\",\"node_id\":\"n1\",\"event\":\"assert\",\"fact\":9223372036854775807,\"template\":\"x\",\"text\":\"(x)\"}\n");
  unlink(jsonl_path);
}

int main(void) {
  if (mkdtemp(dir) == NULL) {
    perror("mkdtemp");
    return 1;
  }
  snprintf(text_path, sizeof text_path, "%s/trace.log", dir);
  snprintf(jsonl_path, sizeof jsonl_path, "%s/trace.jsonl", dir);
  test_formats();
  unlink(text_path);
  test_append();
  test_inactive();
  test_open_errors();
  test_reopen();
  test_write_error();
  test_set_err();
  test_failing();
  unlink(jsonl_path);
  test_large_index();
  unlink(text_path);
  unlink(jsonl_path);
  rmdir(dir);
  if (failures) fprintf(stderr, "%d failed checks\n", failures);
  return failures != 0;
}
