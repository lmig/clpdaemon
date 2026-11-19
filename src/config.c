#include "config.h"

#include "db.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  K_NODE_ID,
  K_POSITIVE,
  K_NON_NEGATIVE,
  K_MAX_RULES,
  K_DURATION,
  K_PATH,
  K_MODE,
  K_OPTIONAL_PATH,
  K_WATCH,
  K_PATH_LIST,
  K_DATABASE,
  K_YES_NO
} key_kind;

enum {
  KEY_NODE_ID,
  KEY_TICK,
  KEY_CYCLE_MIN,
  KEY_MAX_RULES,
  KEY_SAFETY_POLL,
  KEY_IDLE_EVICTION,
  KEY_ADMIN_SOCKET,
  KEY_ADMIN_SOCKET_MODE,
  KEY_TRACE_TEXT,
  KEY_TRACE_JSONL,
  KEY_WATCH,
  KEY_PACKAGES,
  KEY_PLUGINS,
  KEY_DATABASE,
  KEY_ORM_ALLOW_DROP,
  KEY_COUNT
};

/* key_names and key_kinds are indexed by the KEY_* enum above: keep the
 * three in the same order. */
static const char *const key_names[KEY_COUNT] = {
    "node_id",          "tick_interval_ms",        "cycle_min_interval_ms",
    "max_rules_per_cycle", "safety_poll_interval_ms", "idle_eviction_after",
    "admin_socket",     "admin_socket_mode",       "trace_text",
    "trace_jsonl",
    "watch",            "packages",                "plugins",
    "database",         "orm_allow_drop",
};

static const key_kind key_kinds[KEY_COUNT] = {
    K_NODE_ID,     K_POSITIVE,      K_NON_NEGATIVE,  K_MAX_RULES,
    K_POSITIVE,    K_DURATION,      K_PATH,          K_MODE,
    K_OPTIONAL_PATH, K_OPTIONAL_PATH, K_WATCH,       K_PATH_LIST,
    K_PATH_LIST,   K_DATABASE,      K_YES_NO,
};

typedef struct {
  clpd_config *cfg;
  long seen[KEY_COUNT];
  int valid[KEY_COUNT];
} load_state;

/* Parses a non-empty run of decimal digits no greater than INT_MAX. */
static int parse_digits(const char *s, const char **end, long *out) {
  long v = 0;
  if (*s < '0' || *s > '9') return 0;
  for (; *s >= '0' && *s <= '9'; s++) {
    v = v * 10 + (*s - '0');
    if (v > INT_MAX) return 0;
  }
  *end = s;
  *out = v;
  return 1;
}

static int parse_int(const char *s, long *out) {
  const char *end;
  return parse_digits(s, &end, out) && *end == '\0';
}

int clpd_valid_name(const char *s, size_t max) {
  size_t n = strlen(s), i;
  if (n == 0 || n > max) return 0;
  for (i = 0; i < n; i++) {
    char c = s[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
      return 0;
  }
  return 1;
}

/* Accepts "facts" and "rules" and records them in the configuration. */
static int watch_item(clpd_kv *kv, const char *key, const char *item,
                      void *ctx) {
  clpd_config *cfg = ctx;
  (void)key;
  if (strcmp(item, "facts") == 0) {
    cfg->watch_facts = 1;
  } else if (strcmp(item, "rules") == 0) {
    cfg->watch_rules = 1;
  } else {
    clpd_kv_error(kv, "watch: unknown item '%s' (expected facts, rules)", item);
    return 0;
  }
  return 1;
}

static void set_path(char **slot, const char *value) {
  free(*slot);
  *slot = *value == '\0' ? NULL : clpd_strdup(value);
}

/* Applies one key; returns 1 when the value is valid. */
static int apply(clpd_kv *kv, int key, char *value, clpd_config *cfg) {
  const char *name = key_names[key];
  long v = 0;
  const char *end;

  switch (key_kinds[key]) {
  case K_NODE_ID:
    if (!clpd_valid_name(value, CLPD_NODE_ID_MAX)) {
      clpd_kv_error(kv, "node_id: expected 1-64 characters from "
                        "[A-Za-z0-9._-], got '%s'", value);
      return 0;
    }
    strcpy(cfg->node_id, value);
    return 1;
  case K_POSITIVE:
    if (!parse_int(value, &v) || v == 0) {
      clpd_kv_error(kv, "%s: expected a positive integer, got '%s'", name,
                    value);
      return 0;
    }
    break;
  case K_NON_NEGATIVE:
    if (!parse_int(value, &v)) {
      clpd_kv_error(kv, "%s: expected a non-negative integer, got '%s'", name,
                    value);
      return 0;
    }
    break;
  case K_MAX_RULES:
    if (strcmp(value, "-1") == 0) {
      v = -1;
    } else if (!parse_int(value, &v) || v == 0) {
      clpd_kv_error(kv, "max_rules_per_cycle: expected -1 or a positive "
                        "integer, got '%s'", value);
      return 0;
    }
    break;
  case K_DURATION: {
    long unit = 0;
    if (parse_digits(value, &end, &v) && v > 0 && end[0] != '\0' &&
        end[1] == '\0')
      unit = end[0] == 's' ? 1 : end[0] == 'm' ? 60 : end[0] == 'h' ? 3600 : 0;
    if (unit == 0) {
      clpd_kv_error(kv, "idle_eviction_after: expected a duration such as "
                        "30s, 30m or 2h, got '%s'", value);
      return 0;
    }
    cfg->idle_eviction_after_s = v * unit;
    return 1;
  }
  case K_PATH:
    if (*value == '\0') {
      clpd_kv_error(kv, "%s: expected a path", name);
      return 0;
    }
    /* none disables the admin socket (L71); ./none names a file. */
    if (strcmp(value, "none") == 0) value = "";
    set_path(&cfg->admin_socket, value);
    return 1;
  case K_MODE: {
    /* 3 or 4 octal digits, a 4-digit one starting with 0; owner and group
     * bits only, so the admin socket is never open to every local user. */
    size_t n = strlen(value), i;
    int ok = (n == 3 || (n == 4 && value[0] == '0'));
    for (i = 0; ok && i < n; i++) {
      if (value[i] < '0' || value[i] > '7') ok = 0;
      else v = v * 8 + (value[i] - '0');
    }
    if (!ok || (v & 07) != 0) {
      clpd_kv_error(kv, "admin_socket_mode: expected an octal mode such as "
                        "0600 or 0660, with no permission bits for others, "
                        "got '%s'", value);
      return 0;
    }
    cfg->admin_socket_mode = (int)v;
    return 1;
  }
  case K_OPTIONAL_PATH:
    set_path(key == KEY_TRACE_TEXT ? &cfg->trace_text : &cfg->trace_jsonl,
             value);
    return 1;
  case K_WATCH: {
    int facts = cfg->watch_facts, rules = cfg->watch_rules;
    cfg->watch_facts = cfg->watch_rules = 0;
    if (!clpd_kv_list(kv, name, value, NULL, watch_item, cfg)) {
      cfg->watch_facts = facts;
      cfg->watch_rules = rules;
      return 0;
    }
    return 1;
  }
  case K_PATH_LIST:
    return clpd_kv_list(kv, name, value,
                        key == KEY_PACKAGES ? &cfg->packages : &cfg->plugins,
                        NULL, NULL);
  case K_DATABASE: {
    /* The value may hold credentials: the message never echoes it. */
    char msg[160];
    if (!clpd_db_check_spec(value, msg, sizeof msg)) {
      clpd_kv_error(kv, "%s", msg);
      return 0;
    }
    free(cfg->database);
    cfg->database = clpd_strdup(value);
    return 1;
  }
  case K_YES_NO:
    if (strcmp(value, "yes") != 0 && strcmp(value, "no") != 0) {
      clpd_kv_error(kv, "%s: expected yes or no, got '%s'", name, value);
      return 0;
    }
    v = strcmp(value, "yes") == 0;
    break; /* stored by the per-key switch below */
  }

  switch (key) {
  case KEY_TICK: cfg->tick_interval_ms = (int)v; break;
  case KEY_CYCLE_MIN: cfg->cycle_min_interval_ms = (int)v; break;
  case KEY_MAX_RULES: cfg->max_rules_per_cycle = (int)v; break;
  case KEY_SAFETY_POLL: cfg->safety_poll_interval_ms = (int)v; break;
  case KEY_ORM_ALLOW_DROP: cfg->orm_allow_drop = (int)v; break;
  default: break;
  }
  return 1;
}

static void on_pair(clpd_kv *kv, char *key, char *value, void *ctx) {
  load_state *st = ctx;
  int k = clpd_kv_key(kv, key_names, KEY_COUNT, st->seen, key);
  if (k >= 0) st->valid[k] = apply(kv, k, value, st->cfg);
}

static void set_defaults(clpd_config *cfg) {
  memset(cfg, 0, sizeof *cfg);
  cfg->tick_interval_ms = CLPD_DEFAULT_TICK_INTERVAL_MS;
  cfg->cycle_min_interval_ms = CLPD_DEFAULT_CYCLE_MIN_INTERVAL_MS;
  cfg->max_rules_per_cycle = CLPD_DEFAULT_MAX_RULES_PER_CYCLE;
  cfg->safety_poll_interval_ms = CLPD_DEFAULT_SAFETY_POLL_INTERVAL_MS;
  cfg->idle_eviction_after_s = CLPD_DEFAULT_IDLE_EVICTION_AFTER_S;
  cfg->admin_socket = clpd_strdup(CLPD_DEFAULT_ADMIN_SOCKET);
  cfg->admin_socket_mode = CLPD_DEFAULT_ADMIN_SOCKET_MODE;
  cfg->watch_facts = 1;
  cfg->watch_rules = 1;
}

int clpd_config_load(const char *path, clpd_config *cfg, FILE *err) {
  load_state st;
  clpd_kv kv;
  int k, errors;

  set_defaults(cfg);
  memset(&st, 0, sizeof st);
  st.cfg = cfg;
  for (k = 0; k < KEY_COUNT; k++) st.valid[k] = 1;
  if (clpd_kv_read(&kv, path, err, on_pair, &st) != 0) return 1;
  errors = kv.errors;

  if (st.seen[KEY_NODE_ID] == 0) {
    fprintf(err, "clpdaemon: %s: missing required key 'node_id'\n", path);
    errors++;
  }
  if (st.valid[KEY_TICK] && st.valid[KEY_CYCLE_MIN] &&
      cfg->cycle_min_interval_ms > cfg->tick_interval_ms) {
    fprintf(err,
            "clpdaemon: %s: cycle_min_interval_ms (%d) must not exceed "
            "tick_interval_ms (%d)\n",
            path, cfg->cycle_min_interval_ms, cfg->tick_interval_ms);
    errors++;
  }
  return errors != 0;
}

void clpd_config_free(clpd_config *cfg) {
  free(cfg->admin_socket);
  free(cfg->trace_text);
  free(cfg->trace_jsonl);
  free(cfg->database);
  clpd_list_free(&cfg->packages);
  clpd_list_free(&cfg->plugins);
  memset(cfg, 0, sizeof *cfg);
}
