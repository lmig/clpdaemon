/* Unit tests for the plugin loader and lifecycle (docs/plugins.md, T6a).
 * The plugins are built in the directory named by CLPD_PLUGIN_DIR. */
#define _POSIX_C_SOURCE 200809L

#include "plugins.h"

#include "clpd_plugin.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/expect.h"

static const char *dir;
static FILE *log_stream;

/* Returns <dir>/<name>.so (static; 8 slots). */
static const char *plugin(const char *name) {
  static char paths[8][512];
  static int next;
  char *p = paths[next++ % 8];
  snprintf(p, 512, "%s/%s.so", dir, name);
  return p;
}

/* Text written to the daemon log stream since the last call. */
static const char *take_log(void) {
  static char buf[4096];
  size_t n;
  fflush(log_stream);
  rewind(log_stream);
  n = fread(buf, 1, sizeof buf - 1, log_stream);
  buf[n] = '\0';
  fclose(log_stream);
  log_stream = tmpfile();
  if (log_stream == NULL) exit(2);
  clpd_log_set(log_stream);
  return buf;
}

typedef struct {
  Environment *env;
  clpd_plugins *p;
  char err[4096];
  int rc;
} run;

static void load(run *r, const char *const *paths, size_t n) {
  clpd_list list;
  FILE *err = tmpfile();
  size_t len;
  if (err == NULL) exit(2);
  list.items = (char **)paths;
  list.count = n;
  r->env = CreateEnvironment();
  r->p = NULL;
  r->rc = clpd_plugins_load(r->env, &list, "n7", &r->p, err);
  rewind(err);
  len = fread(r->err, 1, sizeof r->err - 1, err);
  r->err[len] = '\0';
  fclose(err);
}

static void done(run *r) {
  clpd_plugins_shutdown(r->p);
  DestroyEnvironment(r->env);
  clpd_plugins_close(r->p);
}

static long long eval_int(Environment *env, const char *expr) {
  CLIPSValue v;
  if (Eval(env, expr, &v) != EE_NO_ERROR || v.header->type != INTEGER_TYPE)
    return -999;
  return v.integerValue->contents;
}

static const char *eval_str(Environment *env, const char *expr) {
  CLIPSValue v;
  if (Eval(env, expr, &v) != EE_NO_ERROR || v.header->type != STRING_TYPE)
    return "";
  return v.lexemeValue->contents;
}

static void test_no_plugins(void) {
  run r;
  load(&r, NULL, 0);
  EXPECT(r.rc == 0);
  EXPECT(r.p != NULL);
  EXPECT(strcmp(r.err, "") == 0);
  clpd_plugins_after_load(r.p);
  clpd_plugins_cycle_begin(r.p);
  clpd_plugins_cycle_end(r.p, 1);
  done(&r);
  EXPECT(strcmp(take_log(), "") == 0);
}

static void test_example(void) {
  const char *paths[1];
  run r;
  paths[0] = plugin("clpd_example");
  load(&r, paths, 1);
  EXPECT(r.rc == 0);
  EXPECT(strcmp(r.err, "") == 0);
  EXPECT(strcmp(take_log(), "clpdaemon: example: init\n") == 0);
  EXPECT(eval_int(r.env, "(clpd-example-add 2 3)") == 5);
  EXPECT(eval_int(r.env, "(clpd-example-add -7 3)") == -4);
  EXPECT(strcmp(eval_str(r.env, "(clpd-example-greet ana)"), "hello, ana") == 0);
  EXPECT(strcmp(eval_str(r.env, "(clpd-example-greet \"x y\")"), "hello, x y") == 0);
  EXPECT(eval_int(r.env, "(clpd-example-cycles)") == 0);
  clpd_plugins_after_load(r.p);
  clpd_plugins_cycle_begin(r.p);
  clpd_plugins_cycle_end(r.p, 1);
  clpd_plugins_cycle_begin(r.p);
  clpd_plugins_cycle_end(r.p, 1);
  clpd_plugins_cycle_begin(r.p);
  clpd_plugins_cycle_end(r.p, 0);
  EXPECT(eval_int(r.env, "(clpd-example-cycles)") == 3);
  EXPECT(strcmp(take_log(), "clpdaemon: example: after_load\n"
                            "clpdaemon: example: cycle_begin 1\n"
                            "clpdaemon: example: cycle_end 1 ok=1\n"
                            "clpdaemon: example: cycle_end 3 ok=0\n") == 0);
  clpd_plugins_shutdown(r.p);
  EXPECT(strcmp(take_log(), "clpdaemon: example: shutdown\n") == 0);
  /* Shutdown runs once; later hooks are no-ops. */
  clpd_plugins_shutdown(r.p);
  clpd_plugins_cycle_begin(r.p);
  clpd_plugins_cycle_end(r.p, 1);
  done(&r);
  EXPECT(strcmp(take_log(), "") == 0);
}

static void test_order(void) {
  const char *paths[2];
  run r;
  paths[0] = plugin("test_hooks_a");
  paths[1] = plugin("test_hooks_b");
  load(&r, paths, 2);
  EXPECT(r.rc == 0);
  EXPECT(strcmp(r.err, "") == 0);
  EXPECT(eval_int(r.env, "(hooks-a-answer)") == 42);
  EXPECT(eval_int(r.env, "(hooks-b-answer)") == 42);
  clpd_plugins_after_load(r.p);
  clpd_plugins_cycle_begin(r.p);
  clpd_plugins_cycle_end(r.p, 0);
  done(&r);
  EXPECT(strcmp(take_log(),
                "clpdaemon: hooks-a: init node_id=n7 post_fact=-1 ENOSYS\n"
                "clpdaemon: hooks-b: init node_id=n7 post_fact=-1 ENOSYS\n"
                "clpdaemon: hooks-a: after_load\n"
                "clpdaemon: hooks-b: after_load\n"
                "clpdaemon: hooks-a: cycle_begin\n"
                "clpdaemon: hooks-b: cycle_begin\n"
                "clpdaemon: hooks-a: cycle_end ok=0\n"
                "clpdaemon: hooks-b: cycle_end ok=0\n"
                "clpdaemon: hooks-b: shutdown\n"
                "clpdaemon: hooks-a: shutdown\n") == 0);
}

/* hooks-a loads, then <bad> fails with message <want>; hooks-a is shut
 * down and nothing else is logged after the fail-init line (if any). */
static void expect_failure(const char *bad, const char *want,
                           const char *want_log) {
  const char *paths[2];
  char full[1024], log_want[1024];
  run r;
  paths[0] = plugin("test_hooks_a");
  paths[1] = bad;
  load(&r, paths, 2);
  snprintf(full, sizeof full, "clpdaemon: %s: %s\n", bad, want);
  snprintf(log_want, sizeof log_want,
           "clpdaemon: hooks-a: init node_id=n7 post_fact=-1 ENOSYS\n%s"
           "clpdaemon: hooks-a: shutdown\n",
           want_log);
  EXPECT(r.rc != 0);
  if (strcmp(r.err, full) != 0) {
    failures++;
    fprintf(stderr, "FAIL %s: err [%s], want [%s]\n", bad, r.err, full);
  }
  if (strcmp(take_log(), log_want) != 0) {
    failures++;
    fprintf(stderr, "FAIL %s: unexpected log\n", bad);
  }
  /* Hooks after a failed load do nothing. */
  clpd_plugins_after_load(r.p);
  clpd_plugins_cycle_begin(r.p);
  done(&r);
  EXPECT(strcmp(take_log(), "") == 0);
}

static void test_errors(void) {
  char size_msg[128], abs_dup[600];
  const char *dup;
  expect_failure("test_hooks_b.so", "plugin path must contain a '/'", "");
  expect_failure(plugin("test_bad_nosym"), "missing clpd_plugin_get", "");
  expect_failure(plugin("test_bad_null"), "clpd_plugin_get returned NULL", "");
  expect_failure(plugin("test_bad_abi"), "ABI version 2, expected 1", "");
  snprintf(size_msg, sizeof size_msg, "descriptor size 8, expected at least %zu",
           sizeof(clpd_plugin));
  expect_failure(plugin("test_bad_size"), size_msg, "");
  expect_failure(plugin("test_bad_name"),
                 "plugin name: expected 1-64 characters from [A-Za-z0-9._-], "
                 "got 'bad name'",
                 "");
  expect_failure(plugin("test_bad_noinit"), "descriptor has no init", "");
  expect_failure(plugin("test_fail_init"), "init failed",
                 "clpdaemon: fail-init: init node_id=n7 post_fact=-1 ENOSYS\n");
  /* The same path twice, and the same library under another path. */
  expect_failure(plugin("test_hooks_a"), "plugin path listed more than once", "");
  snprintf(abs_dup, sizeof abs_dup, "%s/./test_hooks_a.so", dir);
  dup = abs_dup;
  {
    char want[1200];
    snprintf(want, sizeof want, "plugin hooks-a is already loaded from %s",
             plugin("test_hooks_a"));
    expect_failure(dup, want, "");
  }
}

static void test_cannot_load(void) {
  const char *paths[1];
  const char *prefix;
  run r;
  paths[0] = plugin("no_such_plugin");
  load(&r, paths, 1);
  EXPECT(r.rc != 0);
  prefix = "clpdaemon: ";
  EXPECT(strncmp(r.err, prefix, strlen(prefix)) == 0);
  EXPECT(strstr(r.err, ": cannot load: ") != NULL);
  EXPECT(strstr(r.err, "cannot open shared object file") != NULL);
  EXPECT(strchr(r.err, '\n') == r.err + strlen(r.err) - 1);
  done(&r);
  EXPECT(strcmp(take_log(), "") == 0);
}

int main(void) {
  dir = getenv("CLPD_PLUGIN_DIR");
  if (dir == NULL) {
    fputs("plugins_test: CLPD_PLUGIN_DIR is not set\n", stderr);
    return 2;
  }
  log_stream = tmpfile();
  if (log_stream == NULL) return 2;
  clpd_log_set(log_stream);
  test_no_plugins();
  test_example();
  test_order();
  test_errors();
  test_cannot_load();
  clpd_log_set(NULL);
  fclose(log_stream);
  if (failures != 0) fprintf(stderr, "%d failed checks\n", failures);
  return failures != 0;
}
