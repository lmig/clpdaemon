/* Unit tests for the admin socket commands (docs/admin-socket.md, T7c):
 * clpd_admin_command against a real CLIPS environment with a package that
 * lists an `ingest` template, and the drain rejection attribution. The
 * socket itself is tested through the daemon (tests/cli/cli_test.sh). */
#define _POSIX_C_SOURCE 200809L

#include "admin.h"
#include "ingest.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../common/expect.h"

static char root[] = "/tmp/clpd_admin_test.XXXXXX";
static Environment *env;
static clpd_packages pkgs;
static unsigned long cycles = 42;
static clpd_admin_state state;

static void write_file(const char *path, const char *text) {
  FILE *f = fopen(path, "w");
  if (f == NULL || fputs(text, f) < 0 || fclose(f) != 0) {
    perror("admin_test");
    exit(2);
  }
}

static void setup(void) {
  char dir[sizeof root + 8], path[sizeof root + 32];
  clpd_list dirs;
  char *item;
  if (mkdtemp(root) == NULL) {
    perror("admin_test");
    exit(2);
  }
  snprintf(dir, sizeof dir, "%s/pkg", root);
  mkdir(dir, 0700);
  snprintf(path, sizeof path, "%s/package.conf", dir);
  write_file(path, "definition = shop\nversion = 1\nfiles = t.clp\n"
                   "ingest = order\n");
  snprintf(path, sizeof path, "%s/t.clp", dir);
  write_file(path, "(deftemplate order (slot wf-id) (slot qty (type INTEGER)))\n");
  env = CreateEnvironment();
  item = dir;
  dirs.items = &item;
  dirs.count = 1;
  if (env == NULL || clpd_packages_load(env, &dirs, &pkgs, stderr) != 0) {
    fputs("admin_test: cannot load the test package\n", stderr);
    exit(2);
  }
  snprintf(path, sizeof path, "%s/package.conf", dir);
  remove(path);
  snprintf(path, sizeof path, "%s/t.clp", dir);
  remove(path);
  rmdir(dir);
  state.env = env;
  state.packages = &pkgs;
  state.node_id = "n1";
  state.cycles = &cycles;
  state.trace = clpd_trace_open(NULL, NULL, "n1", stderr);
}

/* Runs a command given as a string literal (NUL bytes included) and
 * compares the reply. */
static int run(const char *line, size_t len, const char *want) {
  char reply[CLPD_ADMIN_REPLY_MAX];
  memset(reply, 'z', sizeof reply);
  clpd_admin_command(&state, line, len, reply, sizeof reply);
  if (strcmp(reply, want) != 0) {
    fprintf(stderr, "  reply [%s]\n  want  [%s]\n", reply, want);
    return 0;
  }
  return 1;
}
#define RUN(lit, want) run(lit, sizeof lit - 1, want)

static const char order[] = "{\"template\":\"order\",\"slots\":{\"wf-id\":\"w1\"}}";

static void test_status(void) {
  char msg[CLPD_INGEST_MESSAGE_MAX];
  EXPECT(RUN("status", "OK node_id=n1 cycles=42 queue=0 facts=0"));
  EXPECT(clpd_ingest_assert(env, &pkgs, order, strlen(order), msg, sizeof msg) == 0);
  cycles = 43;
  EXPECT(RUN("status", "OK node_id=n1 cycles=43 queue=0 facts=1"));
  EXPECT(RUN("status\r", "OK node_id=n1 cycles=43 queue=0 facts=1"));
  EXPECT(RUN("status x", "ERR 'status' takes no arguments"));
  EXPECT(RUN("status ", "ERR 'status' takes no arguments"));
}

static void test_inject(void) {
  char line[256], want[256];
  int i;
  snprintf(line, sizeof line, "inject %s", order);
  EXPECT(run(line, strlen(line), "OK queued"));
  EXPECT(clpd_ingest_queue_length() == 1);
  /* Queued, not asserted: the cycle's drain asserts it. */
  EXPECT(RUN("status", "OK node_id=n1 cycles=43 queue=1 facts=1"));
  snprintf(line, sizeof line, "inject %s\r", order);
  EXPECT(run(line, strlen(line), "OK queued"));
  EXPECT(clpd_ingest_queue_length() == 2);
  /* Every rejection leaves the queue unchanged. */
  EXPECT(RUN("inject", "ERR invalid JSON: offset 0: unexpected end of input"));
  EXPECT(RUN("inject {", "ERR invalid JSON: offset 1: unexpected end of input"));
  EXPECT(RUN("inject {\"template\":\"nope\",\"slots\":{}}",
             "ERR template 'nope' is not ingestible"));
  EXPECT(RUN("inject {\"template\":\"order\",\"slots\":{\"qty\":\"x\"}}",
             "ERR slot 'qty': value type not allowed by the slot"));
  EXPECT(RUN("inject {\"template\":\"order\",\"slots\":{\"b\\u0001d\":1}}",
             "ERR template 'order' has no slot 'b?d'"));
  EXPECT(RUN("inject {\"template\":\"or\0der\"}", "ERR NUL byte not allowed"));
  EXPECT(clpd_ingest_queue_length() == 2);
  EXPECT(RUN("status", "OK node_id=n1 cycles=43 queue=2 facts=1"));
  /* A full queue. */
  for (i = 2; i < CLPD_INGEST_QUEUE_MAX; i++)
    EXPECT(clpd_ingest_post(NULL, order, strlen(order)) == 0);
  snprintf(line, sizeof line, "inject %s", order);
  EXPECT(run(line, strlen(line), "ERR ingest queue full"));
  snprintf(want, sizeof want, "OK node_id=n1 cycles=43 queue=%d facts=1",
           CLPD_INGEST_QUEUE_MAX);
  EXPECT(run("status", 6, want));
  EXPECT(clpd_ingest_clear() == CLPD_INGEST_QUEUE_MAX);
}

static void test_other_commands(void) {
  char line[160], want[160];
  EXPECT(RUN("bogus", "ERR unknown command 'bogus'"));
  EXPECT(RUN("bogus arg", "ERR unknown command 'bogus'"));
  EXPECT(RUN("Status", "ERR unknown command 'Status'"));
  EXPECT(RUN("INJECT {}", "ERR unknown command 'INJECT'"));
  EXPECT(RUN("bo\033[2Jgus\r", "ERR unknown command 'bo?[2Jgus'"));
  EXPECT(RUN("a\tb", "ERR unknown command 'a?b'"));
  EXPECT(RUN("st\0atus", "ERR NUL byte not allowed"));
  EXPECT(RUN("", "ERR empty command"));
  EXPECT(RUN("\r", "ERR empty command"));
  EXPECT(RUN(" status", "ERR unknown command ''"));
  memset(line, 'x', 100);
  memset(want, 0, sizeof want);
  strcpy(want, "ERR unknown command '");
  memset(want + strlen(want), 'x', 64);
  strcat(want, "...'");
  EXPECT(run(line, 100, want));
  /* 64 bytes are shown whole. */
  memset(want, 0, sizeof want);
  strcpy(want, "ERR unknown command '");
  memset(want + strlen(want), 'x', 64);
  strcat(want, "'");
  EXPECT(run(line, 64, want));
  EXPECT(RUN("reopen", "OK reopened")); /* no trace file: nothing to reopen */
  EXPECT(RUN("reopen now", "ERR 'reopen' takes no arguments"));
}

static void test_reopen_failure(void) {
  char dir[sizeof root + 8], moved[sizeof root + 8], path[sizeof root + 16];
  char buf[512], want[512];
  clpd_trace *t, *saved = state.trace;
  FILE *log = tmpfile(), *old;
  size_t n;
  snprintf(dir, sizeof dir, "%s/d", root);
  snprintf(moved, sizeof moved, "%s/m", root);
  snprintf(path, sizeof path, "%s/t.log", dir);
  EXPECT(log != NULL && mkdir(dir, 0700) == 0);
  t = clpd_trace_open(path, NULL, "n1", stderr);
  EXPECT(t != NULL);
  if (t == NULL || log == NULL) return;
  state.trace = t;
  EXPECT(RUN("reopen", "OK reopened"));
  EXPECT(rename(dir, moved) == 0);
  old = clpd_log_set(log);
  EXPECT(RUN("reopen", "ERR 1 trace files could not be reopened"));
  clpd_log_set(old);
  rewind(log);
  n = fread(buf, 1, sizeof buf - 1, log);
  buf[n] = '\0';
  snprintf(want, sizeof want,
           "clpdaemon: %s: cannot reopen: No such file or directory\n", path);
  EXPECT(strcmp(buf, want) == 0);
  fclose(log);
  clpd_trace_close(t);
  state.trace = saved;
  snprintf(path, sizeof path, "%s/t.log", moved);
  remove(path);
  rmdir(moved);
}

static void test_post_rejected(void) {
  char buf[256];
  FILE *log = tmpfile(), *old;
  size_t n;
  EXPECT(log != NULL);
  if (log == NULL) return;
  old = clpd_log_set(log);
  clpd_admin_post_rejected(NULL, clpd_admin_origin(), "cannot assert the fact");
  clpd_admin_post_rejected(NULL, NULL, "m");
  clpd_log_set(old);
  rewind(log);
  n = fread(buf, 1, sizeof buf - 1, log);
  buf[n] = '\0';
  EXPECT(strcmp(buf,
                "clpdaemon: admin socket: inject rejected: cannot assert the fact\n"
                "clpdaemon: ?: post_fact rejected: m\n") == 0);
  fclose(log);
}

int main(void) {
  setup();
  test_status();
  test_inject();
  test_other_commands();
  test_reopen_failure();
  test_post_rejected();
  clpd_trace_close(state.trace);
  clpd_packages_free(&pkgs);
  DestroyEnvironment(env);
  rmdir(root);
  if (failures != 0) {
    fprintf(stderr, "%d admin checks failed\n", failures);
    return 1;
  }
  puts("admin checks passed");
  return 0;
}
