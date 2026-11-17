#define _POSIX_C_SOURCE 200809L

#include "config.h"

#include <errno.h>
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
  K_OPTIONAL_PATH,
  K_WATCH,
  K_PATH_LIST
} key_kind;

enum {
  KEY_NODE_ID,
  KEY_TICK,
  KEY_CYCLE_MIN,
  KEY_MAX_RULES,
  KEY_SAFETY_POLL,
  KEY_IDLE_EVICTION,
  KEY_ADMIN_SOCKET,
  KEY_TRACE_TEXT,
  KEY_TRACE_JSONL,
  KEY_WATCH,
  KEY_PACKAGES,
  KEY_PLUGINS,
  KEY_COUNT
};

static const struct {
  const char *name;
  key_kind kind;
} keys[KEY_COUNT] = {
    {"node_id", K_NODE_ID},
    {"tick_interval_ms", K_POSITIVE},
    {"cycle_min_interval_ms", K_NON_NEGATIVE},
    {"max_rules_per_cycle", K_MAX_RULES},
    {"safety_poll_interval_ms", K_POSITIVE},
    {"idle_eviction_after", K_DURATION},
    {"admin_socket", K_PATH},
    {"trace_text", K_OPTIONAL_PATH},
    {"trace_jsonl", K_OPTIONAL_PATH},
    {"watch", K_WATCH},
    {"packages", K_PATH_LIST},
    {"plugins", K_PATH_LIST},
};

typedef struct {
  const char *path;
  FILE *err;
  int errors;
  long line;
} parser;

static void line_error(parser *p, const char *fmt, const char *a,
                       const char *b) {
  fprintf(p->err, "clpdaemon: %s:%ld: ", p->path, p->line);
  fprintf(p->err, fmt, a, b);
  fputc('\n', p->err);
  p->errors++;
}

static char *trim(char *s) {
  char *end;
  while (*s == ' ' || *s == '\t') s++;
  end = s + strlen(s);
  while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' ||
                     end[-1] == '\r'))
    end--;
  *end = '\0';
  return s;
}

static char *dup_str(const char *s) {
  size_t n = strlen(s) + 1;
  char *d = malloc(n);
  if (d == NULL) {
    perror("clpdaemon");
    exit(1);
  }
  memcpy(d, s, n);
  return d;
}

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

static int valid_node_id(const char *s) {
  size_t n = strlen(s), i;
  if (n == 0 || n > CLPD_NODE_ID_MAX) return 0;
  for (i = 0; i < n; i++) {
    char c = s[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
      return 0;
  }
  return 1;
}

static void list_free(clpd_list *l) {
  size_t i;
  for (i = 0; i < l->count; i++) free(l->items[i]);
  free(l->items);
  l->items = NULL;
  l->count = 0;
}

/* Splits a comma-separated list; reports an empty item. With `watch`
 * set, items must be "facts" or "rules" and are not stored. */
static int parse_list(parser *p, const char *key, char *value, clpd_list *out,
                      clpd_config *cfg) {
  char *item = value, *comma;
  clpd_list list = {NULL, 0};
  int facts = 0, rules = 0;

  if (*value == '\0') goto done;
  for (;;) {
    comma = strchr(item, ',');
    if (comma != NULL) *comma = '\0';
    item = trim(item);
    if (*item == '\0') {
      line_error(p, "%s: empty list item", key, NULL);
      list_free(&list);
      return 0;
    }
    if (cfg != NULL) {
      if (strcmp(item, "facts") == 0) {
        facts = 1;
      } else if (strcmp(item, "rules") == 0) {
        rules = 1;
      } else {
        line_error(p, "watch: unknown item '%s' (expected facts, rules)", item,
                   NULL);
        return 0;
      }
    } else {
      char **grown = realloc(list.items, (list.count + 1) * sizeof *grown);
      if (grown == NULL) {
        perror("clpdaemon");
        exit(1);
      }
      list.items = grown;
      list.items[list.count++] = dup_str(item);
    }
    if (comma == NULL) break;
    item = comma + 1;
  }
done:
  if (cfg != NULL) {
    cfg->watch_facts = facts;
    cfg->watch_rules = rules;
  } else {
    list_free(out);
    *out = list;
  }
  return 1;
}

static void set_path(char **slot, const char *value) {
  free(*slot);
  *slot = *value == '\0' ? NULL : dup_str(value);
}

/* Applies one key; returns 1 when the value is valid. */
static int apply(parser *p, int key, char *value, clpd_config *cfg) {
  const char *name = keys[key].name;
  long v = 0;
  const char *end;

  switch (keys[key].kind) {
  case K_NODE_ID:
    if (!valid_node_id(value)) {
      line_error(p, "node_id: expected 1-64 characters from [A-Za-z0-9._-], "
                    "got '%s'%s", value, "");
      return 0;
    }
    strcpy(cfg->node_id, value);
    return 1;
  case K_POSITIVE:
    if (!parse_int(value, &v) || v == 0) {
      line_error(p, "%s: expected a positive integer, got '%s'", name, value);
      return 0;
    }
    break;
  case K_NON_NEGATIVE:
    if (!parse_int(value, &v)) {
      line_error(p, "%s: expected a non-negative integer, got '%s'", name,
                 value);
      return 0;
    }
    break;
  case K_MAX_RULES:
    if (strcmp(value, "-1") == 0) {
      v = -1;
    } else if (!parse_int(value, &v) || v == 0) {
      line_error(p, "max_rules_per_cycle: expected -1 or a positive integer, "
                    "got '%s'%s", value, "");
      return 0;
    }
    break;
  case K_DURATION: {
    long unit = 0;
    if (parse_digits(value, &end, &v) && v > 0 && end[0] != '\0' &&
        end[1] == '\0')
      unit = end[0] == 's' ? 1 : end[0] == 'm' ? 60 : end[0] == 'h' ? 3600 : 0;
    if (unit == 0) {
      line_error(p, "idle_eviction_after: expected a duration such as 30s, "
                    "30m or 2h, got '%s'%s", value, "");
      return 0;
    }
    cfg->idle_eviction_after_s = v * unit;
    return 1;
  }
  case K_PATH:
    if (*value == '\0') {
      line_error(p, "%s: expected a path%s", name, "");
      return 0;
    }
    set_path(&cfg->admin_socket, value);
    return 1;
  case K_OPTIONAL_PATH:
    set_path(key == KEY_TRACE_TEXT ? &cfg->trace_text : &cfg->trace_jsonl,
             value);
    return 1;
  case K_WATCH:
    return parse_list(p, name, value, NULL, cfg);
  case K_PATH_LIST:
    return parse_list(p, name, value,
                      key == KEY_PACKAGES ? &cfg->packages : &cfg->plugins,
                      NULL);
  }

  switch (key) {
  case KEY_TICK: cfg->tick_interval_ms = (int)v; break;
  case KEY_CYCLE_MIN: cfg->cycle_min_interval_ms = (int)v; break;
  case KEY_MAX_RULES: cfg->max_rules_per_cycle = (int)v; break;
  case KEY_SAFETY_POLL: cfg->safety_poll_interval_ms = (int)v; break;
  default: break;
  }
  return 1;
}

static void set_defaults(clpd_config *cfg) {
  memset(cfg, 0, sizeof *cfg);
  cfg->tick_interval_ms = 5000;
  cfg->cycle_min_interval_ms = 200;
  cfg->max_rules_per_cycle = 10000;
  cfg->safety_poll_interval_ms = 5000;
  cfg->idle_eviction_after_s = 30 * 60;
  cfg->admin_socket = dup_str("/run/clpdaemon/admin.sock");
  cfg->watch_facts = 1;
  cfg->watch_rules = 1;
}

int clpd_config_load(const char *path, clpd_config *cfg, FILE *err) {
  parser p = {path, err, 0, 0};
  long seen[KEY_COUNT] = {0};
  int valid[KEY_COUNT];
  char *buf = NULL;
  size_t cap = 0;
  FILE *f;
  int k;

  set_defaults(cfg);
  for (k = 0; k < KEY_COUNT; k++) valid[k] = 1;

  f = fopen(path, "r");
  if (f == NULL) {
    fprintf(err, "clpdaemon: %s: cannot open: %s\n", path, strerror(errno));
    return 1;
  }

  while (getline(&buf, &cap, f) != -1) {
    char *line = trim(buf), *eq, *key, *value;
    p.line++;
    if (*line == '\0' || *line == '#') continue;
    eq = strchr(line, '=');
    if (eq == NULL) {
      line_error(&p, "expected 'key = value'%s%s", "", "");
      continue;
    }
    *eq = '\0';
    key = trim(line);
    value = trim(eq + 1);
    if (*key == '\0') {
      line_error(&p, "expected 'key = value'%s%s", "", "");
      continue;
    }
    for (k = 0; k < KEY_COUNT && strcmp(keys[k].name, key) != 0; k++) {
    }
    if (k == KEY_COUNT) {
      line_error(&p, "unknown key '%s'%s", key, "");
      continue;
    }
    if (seen[k] != 0) {
      char first[32];
      sprintf(first, "%ld", seen[k]);
      line_error(&p, "duplicate key '%s' (first set on line %s)", key, first);
      continue;
    }
    seen[k] = p.line;
    valid[k] = apply(&p, k, value, cfg);
  }
  free(buf);
  if (ferror(f)) {
    int saved = errno;
    fclose(f);
    fprintf(err, "clpdaemon: %s: cannot read: %s\n", path, strerror(saved));
    return 1;
  }
  fclose(f);

  if (seen[KEY_NODE_ID] == 0) {
    fprintf(err, "clpdaemon: %s: missing required key 'node_id'\n", path);
    p.errors++;
  }
  if (valid[KEY_TICK] && valid[KEY_CYCLE_MIN] &&
      cfg->cycle_min_interval_ms > cfg->tick_interval_ms) {
    fprintf(err,
            "clpdaemon: %s: cycle_min_interval_ms (%d) must not exceed "
            "tick_interval_ms (%d)\n",
            path, cfg->cycle_min_interval_ms, cfg->tick_interval_ms);
    p.errors++;
  }
  return p.errors != 0;
}

void clpd_config_free(clpd_config *cfg) {
  free(cfg->admin_socket);
  free(cfg->trace_text);
  free(cfg->trace_jsonl);
  list_free(&cfg->packages);
  list_free(&cfg->plugins);
  memset(cfg, 0, sizeof *cfg);
}
