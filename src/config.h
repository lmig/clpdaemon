/* Configuration file loading and validation (docs/configuration.md). */
#ifndef CLPD_CONFIG_H
#define CLPD_CONFIG_H

#include <stddef.h>
#include <stdio.h>

#include "kvfile.h"

#define CLPD_NODE_ID_MAX 64

/* Defaults for optional keys (docs/configuration.md). */
#define CLPD_DEFAULT_TICK_INTERVAL_MS 5000
#define CLPD_DEFAULT_CYCLE_MIN_INTERVAL_MS 200
#define CLPD_DEFAULT_MAX_RULES_PER_CYCLE 10000
#define CLPD_DEFAULT_SAFETY_POLL_INTERVAL_MS 5000
#define CLPD_DEFAULT_IDLE_EVICTION_AFTER_S (30L * 60)
#define CLPD_DEFAULT_ADMIN_SOCKET "/run/clpdaemon/admin.sock"
#define CLPD_DEFAULT_ADMIN_SOCKET_MODE 0600

typedef struct {
  char node_id[CLPD_NODE_ID_MAX + 1];
  int tick_interval_ms;
  int cycle_min_interval_ms;
  int max_rules_per_cycle; /* -1 = unlimited */
  int safety_poll_interval_ms;
  long idle_eviction_after_s;
  char *admin_socket; /* NULL: disabled (admin_socket = none) */
  int admin_socket_mode; /* permission bits, no bits for others */
  char *trace_text;  /* NULL = disabled */
  char *trace_jsonl; /* NULL = disabled */
  int watch_facts;
  int watch_rules;
  char *database; /* NULL = no database; checked by clpd_db_check_spec */
  clpd_list packages;
  clpd_list plugins;
} clpd_config;

/* Loads and validates `path` into `cfg`. Every error is written to `err`,
 * one line each, prefixed with "clpdaemon: ". Returns 0 when the
 * configuration is valid; otherwise nonzero, and `cfg` must still be freed. */
int clpd_config_load(const char *path, clpd_config *cfg, FILE *err);

void clpd_config_free(clpd_config *cfg);

/* 1 when s has 1..max characters from [A-Za-z0-9._-]. */
int clpd_valid_name(const char *s, size_t max);

#endif
