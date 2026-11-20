/* Unit tests for clpd_config_load: parsed values and defaults
 * (docs/configuration.md). usage: config_test */
#define _POSIX_C_SOURCE 200809L

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
static char dir[] = "/tmp/clpd_config_test.XXXXXX";
static char path[sizeof dir + 32];

#define EXPECT(cond)                                                       \
  do {                                                                     \
    if (!(cond)) {                                                         \
      failures++;                                                          \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
    }                                                                      \
  } while (0)

/* Writes `len` bytes of `text` to a fresh file and loads it. The error
 * stream is captured into `errbuf`. Returns the loader result. */
static int load(const char *name, const char *text, size_t len,
                clpd_config *cfg, char *errbuf, size_t errcap) {
  FILE *f, *err;
  size_t n;
  int rc;

  snprintf(path, sizeof path, "%s/%s", dir, name);
  f = fopen(path, "wb");
  if (f == NULL || fwrite(text, 1, len, f) != len || fclose(f) != 0) {
    perror("config_test");
    exit(2);
  }
  err = tmpfile();
  if (err == NULL) {
    perror("config_test");
    exit(2);
  }
  rc = clpd_config_load(path, cfg, err);
  rewind(err);
  n = fread(errbuf, 1, errcap - 1, err);
  errbuf[n] = '\0';
  fclose(err);
  remove(path);
  return rc;
}

#define LOAD(name, lit, cfg, eb) load(name, lit, sizeof lit - 1, cfg, eb, sizeof eb)

static void test_defaults(void) {
  clpd_config cfg;
  char eb[512];
  EXPECT(LOAD("defaults.conf", "node_id = n1\n", &cfg, eb) == 0);
  EXPECT(strcmp(eb, "") == 0);
  EXPECT(strcmp(cfg.node_id, "n1") == 0);
  EXPECT(cfg.tick_interval_ms == 5000);
  EXPECT(cfg.cycle_min_interval_ms == 200);
  EXPECT(cfg.max_rules_per_cycle == 10000);
  EXPECT(cfg.safety_poll_interval_ms == 5000);
  EXPECT(cfg.idle_eviction_after_s == 30 * 60);
  EXPECT(cfg.admin_socket != NULL &&
         strcmp(cfg.admin_socket, "/run/clpdaemon/admin.sock") == 0);
  EXPECT(cfg.admin_socket_mode == 0600);
  EXPECT(cfg.trace_text == NULL);
  EXPECT(cfg.trace_jsonl == NULL);
  EXPECT(cfg.watch_facts == 1 && cfg.watch_rules == 1);
  EXPECT(cfg.packages.count == 0 && cfg.plugins.count == 0);
  clpd_config_free(&cfg);
}

static void test_all_values(void) {
  clpd_config cfg;
  char eb[512];
  EXPECT(LOAD("all.conf",
              "node_id = node-1.a_b\n"
              "tick_interval_ms = 2147483647\n"
              "cycle_min_interval_ms = 0\n"
              "max_rules_per_cycle = -1\n"
              "safety_poll_interval_ms = 750\n"
              "idle_eviction_after = 45s\n"
              "admin_socket = /tmp/a.sock\n"
              "trace_text = /tmp/t.log\n"
              "trace_jsonl = /tmp/t.jsonl\n"
              "watch = rules\n"
              "packages = /p/a@v1 , /p/b@v2\n"
              "plugins = /l/x.so\n",
              &cfg, eb) == 0);
  EXPECT(strcmp(eb, "") == 0);
  EXPECT(strcmp(cfg.node_id, "node-1.a_b") == 0);
  EXPECT(cfg.tick_interval_ms == 2147483647);
  EXPECT(cfg.cycle_min_interval_ms == 0);
  EXPECT(cfg.max_rules_per_cycle == -1);
  EXPECT(cfg.safety_poll_interval_ms == 750);
  EXPECT(cfg.idle_eviction_after_s == 45);
  EXPECT(cfg.admin_socket != NULL && strcmp(cfg.admin_socket, "/tmp/a.sock") == 0);
  EXPECT(cfg.trace_text != NULL && strcmp(cfg.trace_text, "/tmp/t.log") == 0);
  EXPECT(cfg.trace_jsonl != NULL && strcmp(cfg.trace_jsonl, "/tmp/t.jsonl") == 0);
  EXPECT(cfg.watch_facts == 0 && cfg.watch_rules == 1);
  EXPECT(cfg.packages.count == 2 && strcmp(cfg.packages.items[0], "/p/a@v1") == 0 &&
         strcmp(cfg.packages.items[1], "/p/b@v2") == 0);
  EXPECT(cfg.plugins.count == 1 && strcmp(cfg.plugins.items[0], "/l/x.so") == 0);
  clpd_config_free(&cfg);
}

static void test_durations_and_empty_lists(void) {
  clpd_config cfg;
  char eb[512];
  EXPECT(LOAD("m.conf", "node_id = n1\nidle_eviction_after = 2m\nwatch =\n",
              &cfg, eb) == 0);
  EXPECT(cfg.idle_eviction_after_s == 120);
  EXPECT(cfg.watch_facts == 0 && cfg.watch_rules == 0);
  clpd_config_free(&cfg);
  EXPECT(LOAD("h.conf", "node_id = n1\nidle_eviction_after = 3h\n", &cfg, eb) == 0);
  EXPECT(cfg.idle_eviction_after_s == 3 * 3600);
  clpd_config_free(&cfg);
}

static void test_crlf_and_tabs(void) {
  clpd_config cfg;
  char eb[512];
  EXPECT(LOAD("crlf.conf",
              "node_id = n1\r\ntick_interval_ms = 900\r\npackages = /a,/b\r\n",
              &cfg, eb) == 0);
  EXPECT(strcmp(eb, "") == 0);
  EXPECT(strcmp(cfg.node_id, "n1") == 0);
  EXPECT(cfg.tick_interval_ms == 900);
  EXPECT(cfg.packages.count == 2 && strcmp(cfg.packages.items[1], "/b") == 0);
  clpd_config_free(&cfg);
  EXPECT(LOAD("tabs.conf", "\tnode_id\t=\tn2\t\nplugins =\t/x.so\t,\t/y.so\n",
              &cfg, eb) == 0);
  EXPECT(strcmp(eb, "") == 0);
  EXPECT(strcmp(cfg.node_id, "n2") == 0);
  EXPECT(cfg.plugins.count == 2 && strcmp(cfg.plugins.items[0], "/x.so") == 0 &&
         strcmp(cfg.plugins.items[1], "/y.so") == 0);
  clpd_config_free(&cfg);
}

/* admin_socket_mode (T7c): 3 or 4 octal digits (a 4-digit value starts
 * with 0), owner and group bits only. */
static void test_admin_socket_mode(void) {
  static const char *const good[] = {"0660", "660", "0000", "0770", "0600"};
  static const int want[] = {0660, 0660, 0, 0770, 0600};
  static const char *const bad[] = {"0666", "0601", "0777", "1600", "06000",
                                    "60", "0800", "600a", "0x60", "-600"};
  clpd_config cfg;
  char text[128], eb[512], expect[sizeof path + 256];
  size_t i;
  for (i = 0; i < sizeof good / sizeof good[0]; i++) {
    snprintf(text, sizeof text, "node_id = n1\nadmin_socket_mode = %s\n", good[i]);
    EXPECT(load("mode.conf", text, strlen(text), &cfg, eb, sizeof eb) == 0);
    EXPECT(strcmp(eb, "") == 0);
    EXPECT(cfg.admin_socket_mode == want[i]);
    clpd_config_free(&cfg);
  }
  for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    snprintf(text, sizeof text, "node_id = n1\nadmin_socket_mode = %s\n", bad[i]);
    EXPECT(load("mode.conf", text, strlen(text), &cfg, eb, sizeof eb) == 1);
    snprintf(expect, sizeof expect,
             "clpdaemon: %s:2: admin_socket_mode: expected an octal mode "
             "such as 0600 or 0660, with no permission bits for others, "
             "got '%s'\n", path, bad[i]);
    EXPECT(strcmp(eb, expect) == 0);
    EXPECT(cfg.admin_socket_mode == 0600); /* the rejected line set nothing */
    clpd_config_free(&cfg);
  }
}

/* orm_allow_drop (T9a): yes or no, default no. */
static void test_orm_allow_drop(void) {
  static const char *const bad[] = {"", "true", "1", "Yes", "NO", "y"};
  clpd_config cfg;
  char text[128], eb[512], expect[sizeof path + 128];
  size_t i;
  EXPECT(LOAD("drop0.conf", "node_id = n1\n", &cfg, eb) == 0);
  EXPECT(cfg.orm_allow_drop == 0);
  clpd_config_free(&cfg);
  EXPECT(LOAD("drop1.conf", "node_id = n1\norm_allow_drop = yes\n", &cfg, eb) == 0);
  EXPECT(strcmp(eb, "") == 0 && cfg.orm_allow_drop == 1);
  clpd_config_free(&cfg);
  EXPECT(LOAD("drop2.conf", "node_id = n1\norm_allow_drop = no\n", &cfg, eb) == 0);
  EXPECT(strcmp(eb, "") == 0 && cfg.orm_allow_drop == 0);
  clpd_config_free(&cfg);
  for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    snprintf(text, sizeof text, "node_id = n1\norm_allow_drop = %s\n", bad[i]);
    EXPECT(load("drop.conf", text, strlen(text), &cfg, eb, sizeof eb) == 1);
    snprintf(expect, sizeof expect,
             "clpdaemon: %s:2: orm_allow_drop: expected yes or no, got '%s'\n",
             path, bad[i]);
    EXPECT(strcmp(eb, expect) == 0);
    EXPECT(cfg.orm_allow_drop == 0);
    clpd_config_free(&cfg);
  }
  EXPECT(LOAD("drop3.conf", "node_id = n1\norm_allow_drop = yes\n"
                            "orm_allow_drop = no\n", &cfg, eb) == 1);
  snprintf(expect, sizeof expect,
           "clpdaemon: %s:3: duplicate key 'orm_allow_drop' (first set on "
           "line 2)\n", path);
  EXPECT(strcmp(eb, expect) == 0 && cfg.orm_allow_drop == 1);
  clpd_config_free(&cfg);
}

/* admin_socket = none disables the admin socket (L71); any other value,
 * ./none included, is a path. */
static void test_admin_socket_none(void) {
  clpd_config cfg;
  char eb[512];
  EXPECT(LOAD("none.conf", "node_id = n1\nadmin_socket = none\n", &cfg, eb) == 0);
  EXPECT(strcmp(eb, "") == 0);
  EXPECT(cfg.admin_socket == NULL);
  clpd_config_free(&cfg);
  EXPECT(LOAD("none.conf", "node_id = n1\nadmin_socket = ./none\n", &cfg, eb) == 0);
  EXPECT(cfg.admin_socket != NULL && strcmp(cfg.admin_socket, "./none") == 0);
  clpd_config_free(&cfg);
}

/* database (T8a, L74): sqlite:<path> or a libpq conninfo without a
 * password; absent = NULL = no database; the value is never echoed. */
static void test_database(void) {
  static const char *const good[] = {
      "sqlite:/var/lib/clpd/x.db", "sqlite:rel.db",
      "host=/tmp dbname=clpd user=clpd", "postgresql://clpd@localhost/clpd",
      "postgres://localhost:5432/clpd?sslmode=disable"};
  static const struct {
    const char *value, *message;
  } bad[] = {
      {"", "database: expected a connection string"},
      {"sqlite:", "database: expected sqlite:<path>"},
      {"sqlite", "database: expected sqlite:<path>"},
      {"not a conninfo", "database: invalid PostgreSQL connection string"},
      {"host='unterminated", "database: invalid PostgreSQL connection string"},
      {"postgresql://[::1", "database: invalid PostgreSQL connection string"},
      {"host=/tmp password=s3cr3t",
       "database: the connection string must not contain a password (use a "
       "password file, see PGPASSFILE)"},
      {"host=/tmp sslpassword=s3cr3t",
       "database: the connection string must not contain a password (use a "
       "password file, see PGPASSFILE)"},
      {"postgresql://clpd:s3cr3t@localhost/clpd",
       "database: the connection string must not contain a password (use a "
       "password file, see PGPASSFILE)"},
      {"postgresql://localhost/clpd?password=s3cr3t",
       "database: the connection string must not contain a password (use a "
       "password file, see PGPASSFILE)"},
  };
  clpd_config cfg;
  char text[256], eb[512], expect[sizeof path + 256];
  size_t i;

  EXPECT(LOAD("db.conf", "node_id = n1\n", &cfg, eb) == 0);
  EXPECT(cfg.database == NULL);
  clpd_config_free(&cfg);
  for (i = 0; i < sizeof good / sizeof good[0]; i++) {
    snprintf(text, sizeof text, "node_id = n1\ndatabase = %s\n", good[i]);
    EXPECT(load("db.conf", text, strlen(text), &cfg, eb, sizeof eb) == 0);
    EXPECT(strcmp(eb, "") == 0);
    EXPECT(cfg.database != NULL && strcmp(cfg.database, good[i]) == 0);
    clpd_config_free(&cfg);
  }
  for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    snprintf(text, sizeof text, "node_id = n1\ndatabase = %s\n", bad[i].value);
    EXPECT(load("db.conf", text, strlen(text), &cfg, eb, sizeof eb) == 1);
    snprintf(expect, sizeof expect, "clpdaemon: %s:2: %s\n", path,
             bad[i].message);
    EXPECT(strcmp(eb, expect) == 0);
    EXPECT(strstr(eb, "s3cr3t") == NULL); /* never echoed */
    EXPECT(cfg.database == NULL);          /* the rejected line set nothing */
    clpd_config_free(&cfg);
  }
  /* A duplicate key is still reported once, as for every key. */
  EXPECT(LOAD("db.conf", "node_id = n1\ndatabase = sqlite:a\ndatabase = sqlite:b\n",
              &cfg, eb) == 1);
  clpd_config_free(&cfg);
}

/* cycle_retry_initial_ms and cycle_retry_max_ms (T10a): positive
 * integers, default 100 and 30000, initial not above max. */
static void test_cycle_retry(void) {
  static const char *const bad[] = {"0", "-1", "", "1.5", "100ms",
                                    "2147483648"};
  static const char *const keys[] = {"cycle_retry_initial_ms",
                                     "cycle_retry_max_ms"};
  clpd_config cfg;
  char text[160], eb[512], expect[2 * sizeof path + 320];
  size_t i, k;
  EXPECT(LOAD("retry0.conf", "node_id = n1\n", &cfg, eb) == 0);
  EXPECT(cfg.cycle_retry_initial_ms == 100);
  EXPECT(cfg.cycle_retry_max_ms == 30000);
  clpd_config_free(&cfg);
  EXPECT(LOAD("retry1.conf", "node_id = n1\ncycle_retry_initial_ms = 7\n"
                             "cycle_retry_max_ms = 2147483647\n", &cfg, eb) == 0);
  EXPECT(strcmp(eb, "") == 0);
  EXPECT(cfg.cycle_retry_initial_ms == 7);
  EXPECT(cfg.cycle_retry_max_ms == 2147483647);
  clpd_config_free(&cfg);
  /* Equal values are valid; one key alone keeps the other's default. */
  EXPECT(LOAD("retry2.conf", "node_id = n1\ncycle_retry_initial_ms = 250\n"
                             "cycle_retry_max_ms = 250\n", &cfg, eb) == 0);
  EXPECT(strcmp(eb, "") == 0 && cfg.cycle_retry_initial_ms == 250 &&
         cfg.cycle_retry_max_ms == 250);
  clpd_config_free(&cfg);
  EXPECT(LOAD("retry3.conf", "node_id = n1\ncycle_retry_max_ms = 100\n", &cfg,
              eb) == 0);
  EXPECT(cfg.cycle_retry_initial_ms == 100 && cfg.cycle_retry_max_ms == 100);
  clpd_config_free(&cfg);
  for (k = 0; k < 2; k++) {
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
      snprintf(text, sizeof text, "node_id = n1\n%s = %s\n", keys[k], bad[i]);
      EXPECT(load("retry.conf", text, strlen(text), &cfg, eb, sizeof eb) == 1);
      snprintf(expect, sizeof expect,
               "clpdaemon: %s:2: %s: expected a positive integer, got '%s'\n",
               path, keys[k], bad[i]);
      EXPECT(strcmp(eb, expect) == 0);
      EXPECT(cfg.cycle_retry_initial_ms == 100 &&
             cfg.cycle_retry_max_ms == 30000);
      clpd_config_free(&cfg);
    }
  }
  /* initial above max: a file-level error after the line errors. */
  EXPECT(LOAD("retry4.conf", "node_id = n1\ncycle_retry_initial_ms = 500\n"
                             "cycle_retry_max_ms = 499\n", &cfg, eb) == 1);
  snprintf(expect, sizeof expect,
           "clpdaemon: %s: cycle_retry_initial_ms (500) must not exceed "
           "cycle_retry_max_ms (499)\n", path);
  EXPECT(strcmp(eb, expect) == 0);
  clpd_config_free(&cfg);
  EXPECT(LOAD("retry5.conf", "node_id = n1\ncycle_retry_initial_ms = 40000\n",
              &cfg, eb) == 1);
  snprintf(expect, sizeof expect,
           "clpdaemon: %s: cycle_retry_initial_ms (40000) must not exceed "
           "cycle_retry_max_ms (30000)\n", path);
  EXPECT(strcmp(eb, expect) == 0);
  clpd_config_free(&cfg);
  /* Not checked when a value is invalid. */
  EXPECT(LOAD("retry6.conf", "node_id = n1\ncycle_retry_initial_ms = 40000\n"
                             "cycle_retry_max_ms = 0\n", &cfg, eb) == 1);
  snprintf(expect, sizeof expect,
           "clpdaemon: %s:3: cycle_retry_max_ms: expected a positive integer, "
           "got '0'\n", path);
  EXPECT(strcmp(eb, expect) == 0);
  clpd_config_free(&cfg);
  EXPECT(LOAD("retry7.conf", "node_id = n1\ncycle_retry_max_ms = 5\n"
                             "cycle_retry_max_ms = 6\n", &cfg, eb) == 1);
  snprintf(expect, sizeof expect,
           "clpdaemon: %s:3: duplicate key 'cycle_retry_max_ms' (first set on "
           "line 2)\nclpdaemon: %s: cycle_retry_initial_ms (100) must not "
           "exceed cycle_retry_max_ms (5)\n", path, path);
  EXPECT(strcmp(eb, expect) == 0);
  clpd_config_free(&cfg);
}

/* instance_isolate_after (T10e): a positive integer, default 3. */
static void test_instance_isolate_after(void) {
  static const char *const bad[] = {"0", "-1", "", "1.5", "3x",
                                    "2147483648"};
  clpd_config cfg;
  char text[160], eb[512], expect[sizeof path + 160];
  size_t i;
  EXPECT(LOAD("iso0.conf", "node_id = n1\n", &cfg, eb) == 0);
  EXPECT(cfg.instance_isolate_after == 3);
  clpd_config_free(&cfg);
  EXPECT(LOAD("iso1.conf", "node_id = n1\ninstance_isolate_after = 1\n", &cfg,
              eb) == 0);
  EXPECT(strcmp(eb, "") == 0 && cfg.instance_isolate_after == 1);
  clpd_config_free(&cfg);
  EXPECT(LOAD("iso2.conf", "node_id = n1\ninstance_isolate_after = 2147483647\n",
              &cfg, eb) == 0);
  EXPECT(strcmp(eb, "") == 0 && cfg.instance_isolate_after == 2147483647);
  clpd_config_free(&cfg);
  for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    snprintf(text, sizeof text, "node_id = n1\ninstance_isolate_after = %s\n",
             bad[i]);
    EXPECT(load("iso.conf", text, strlen(text), &cfg, eb, sizeof eb) == 1);
    snprintf(expect, sizeof expect,
             "clpdaemon: %s:2: instance_isolate_after: expected a positive "
             "integer, got '%s'\n",
             path, bad[i]);
    EXPECT(strcmp(eb, expect) == 0);
    EXPECT(cfg.instance_isolate_after == 3);
    clpd_config_free(&cfg);
  }
  /* The retry keys keep their own checks and defaults. */
  EXPECT(LOAD("iso3.conf", "node_id = n1\ninstance_isolate_after = 5\n"
                           "cycle_retry_max_ms = 0\n", &cfg, eb) == 1);
  snprintf(expect, sizeof expect,
           "clpdaemon: %s:3: cycle_retry_max_ms: expected a positive integer, "
           "got '0'\n", path);
  EXPECT(strcmp(eb, expect) == 0);
  EXPECT(cfg.instance_isolate_after == 5 && cfg.cycle_retry_initial_ms == 100);
  clpd_config_free(&cfg);
}

static void test_nul_byte_rejected(void) {
  clpd_config cfg;
  char eb[512], want[sizeof path + 64];
  EXPECT(LOAD("nul.conf", "node_id = n1\ntick_interval_ms = 1\0000\n", &cfg, eb) == 1);
  snprintf(want, sizeof want, "clpdaemon: %s:2: line contains a NUL byte\n", path);
  EXPECT(strcmp(eb, want) == 0);
  EXPECT(cfg.tick_interval_ms == 5000); /* the rejected line set nothing */
  clpd_config_free(&cfg);
}

int main(void) {
  if (mkdtemp(dir) == NULL) {
    perror("config_test");
    return 2;
  }
  test_defaults();
  test_all_values();
  test_durations_and_empty_lists();
  test_crlf_and_tabs();
  test_nul_byte_rejected();
  test_admin_socket_mode();
  test_orm_allow_drop();
  test_admin_socket_none();
  test_database();
  test_cycle_retry();
  test_instance_isolate_after();
  rmdir(dir);
  if (failures != 0) {
    fprintf(stderr, "%d config checks failed\n", failures);
    return 1;
  }
  puts("config checks passed");
  return 0;
}
