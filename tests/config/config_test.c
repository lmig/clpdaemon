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
  rmdir(dir);
  if (failures != 0) {
    fprintf(stderr, "%d config checks failed\n", failures);
    return 1;
  }
  puts("config checks passed");
  return 0;
}
