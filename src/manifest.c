#include "manifest.h"

#include "config.h"

#include <stdlib.h>
#include <string.h>

enum {
  KEY_DEFINITION,
  KEY_VERSION,
  KEY_FILES,
  KEY_LINT_EXEMPT,
  KEY_INGEST,
  KEY_ORM,
  KEY_COUNT
};

static const char *const key_names[KEY_COUNT] = {
    "definition", "version", "files", "lint_exempt", "ingest", "orm"};

typedef struct {
  clpd_manifest *m;
  const char *dir;
  long seen[KEY_COUNT];
} load_state;

static char *join(const char *dir, const char *rel) {
  size_t a = strlen(dir), b = strlen(rel);
  char *p = malloc(a + b + 2);
  if (p == NULL) {
    perror("clpdaemon");
    exit(1);
  }
  memcpy(p, dir, a);
  p[a] = '/';
  memcpy(p + a + 1, rel, b + 1);
  return p;
}

static int relative_path(clpd_kv *kv, const char *key, const char *item,
                         void *ctx) {
  (void)ctx;
  if (item[0] == '/') {
    clpd_kv_error(kv, "%s: '%s' must be relative to the package directory",
                  key, item);
    return 0;
  }
  return 1;
}

/* Returns the name after "template:" or "rule:", or NULL. */
static const char *exempt_name(const char *item, int *is_rule) {
  static const char tpl[] = "template:", rule[] = "rule:";
  const char *name = NULL;
  *is_rule = 0; /* set on every path: -O2 flags it as maybe-uninitialized */
  if (strncmp(item, tpl, sizeof tpl - 1) == 0) {
    name = item + sizeof tpl - 1;
  } else if (strncmp(item, rule, sizeof rule - 1) == 0) {
    name = item + sizeof rule - 1;
    *is_rule = 1;
  }
  return name != NULL && *name != '\0' ? name : NULL;
}

static int exempt_item(clpd_kv *kv, const char *key, const char *item,
                       void *ctx) {
  int is_rule;
  (void)ctx;
  if (exempt_name(item, &is_rule) == NULL) {
    clpd_kv_error(kv, "%s: expected template:<name> or rule:<name>, got '%s'",
                  key, item);
    return 0;
  }
  return 1;
}

/* Rejects a name already in the list being built (ctx). */
static int ingest_item(clpd_kv *kv, const char *key, const char *item,
                       void *ctx) {
  clpd_list *seen = ctx;
  size_t i;
  for (i = 0; i < seen->count; i++) {
    if (strcmp(seen->items[i], item) == 0) {
      clpd_kv_error(kv, "%s: '%s' listed more than once", key, item);
      return 0;
    }
  }
  /* Only the address of the item is kept: the list owns the strings. */
  {
    char **grown = realloc(seen->items, (seen->count + 1) * sizeof *grown);
    if (grown == NULL) {
      perror("clpdaemon");
      exit(1);
    }
    seen->items = grown;
    seen->items[seen->count++] = (char *)item;
  }
  return 1;
}

static void set_name(clpd_kv *kv, const char *key, const char *value,
                     char **slot) {
  if (!clpd_valid_name(value, CLPD_PACKAGE_NAME_MAX)) {
    clpd_kv_error(kv, "%s: expected 1-64 characters from [A-Za-z0-9._-], "
                      "got '%s'", key, value);
    return;
  }
  free(*slot);
  *slot = clpd_strdup(value);
}

static void on_pair(clpd_kv *kv, char *key, char *value, void *ctx) {
  load_state *st = ctx;
  clpd_manifest *m = st->m;
  clpd_list list = {NULL, 0};
  size_t i;
  int k = clpd_kv_key(kv, key_names, KEY_COUNT, st->seen, key);

  switch (k) {
  case KEY_DEFINITION: set_name(kv, key, value, &m->definition); break;
  case KEY_VERSION: set_name(kv, key, value, &m->version); break;
  case KEY_FILES:
    if (*value == '\0') {
      clpd_kv_error(kv, "files: expected at least one file");
      break;
    }
    if (!clpd_kv_list(kv, key, value, &list, relative_path, NULL)) break;
    clpd_list_free(&m->files);
    for (i = 0; i < list.count; i++) {
      char *full = join(st->dir, list.items[i]);
      free(list.items[i]);
      list.items[i] = full;
    }
    m->files = list;
    break;
  case KEY_LINT_EXEMPT:
    if (!clpd_kv_list(kv, key, value, &list, exempt_item, NULL)) break;
    for (i = 0; i < list.count; i++) {
      int is_rule;
      const char *name = exempt_name(list.items[i], &is_rule);
      clpd_list *dst = is_rule ? &m->exempt_rules : &m->exempt_templates;
      char **grown = realloc(dst->items, (dst->count + 1) * sizeof *grown);
      if (grown == NULL) {
        perror("clpdaemon");
        exit(1);
      }
      dst->items = grown;
      dst->items[dst->count++] = clpd_strdup(name);
    }
    clpd_list_free(&list);
    break;
  case KEY_INGEST: {
    clpd_list seen = {NULL, 0};
    int ok = clpd_kv_list(kv, key, value, &list, ingest_item, &seen);
    free(seen.items);
    if (!ok) break;
    clpd_list_free(&m->ingest);
    m->ingest = list;
    break;
  }
  case KEY_ORM:
    /* One path: a comma is part of it, not a list separator. */
    if (*value == '\0') {
      clpd_kv_error(kv, "orm: expected a file");
      break;
    }
    if (!relative_path(kv, key, value, NULL)) break;
    free(m->orm);
    m->orm = join(st->dir, value);
    break;
  default: break;
  }
}

int clpd_manifest_load(const char *dir, clpd_manifest *m, FILE *err) {
  load_state st;
  clpd_kv kv;
  char *path = join(dir, CLPD_MANIFEST_FILE);
  int k, errors;

  memset(m, 0, sizeof *m);
  memset(&st, 0, sizeof st);
  st.m = m;
  st.dir = dir;
  if (clpd_kv_read(&kv, path, err, on_pair, &st) != 0) {
    free(path);
    return 1;
  }
  errors = kv.errors;
  for (k = KEY_DEFINITION; k <= KEY_FILES; k++) {
    if (st.seen[k] == 0) {
      fprintf(err, "clpdaemon: %s: missing required key '%s'\n", path,
              key_names[k]);
      errors++;
    }
  }
  free(path);
  if (errors == 0) {
    size_t a = strlen(m->definition), b = strlen(m->version);
    m->name = malloc(a + b + 2);
    if (m->name == NULL) {
      perror("clpdaemon");
      exit(1);
    }
    memcpy(m->name, m->definition, a);
    m->name[a] = '@';
    memcpy(m->name + a + 1, m->version, b + 1);
  }
  return errors != 0;
}

void clpd_manifest_free(clpd_manifest *m) {
  free(m->definition);
  free(m->version);
  free(m->name);
  clpd_list_free(&m->files);
  clpd_list_free(&m->exempt_templates);
  clpd_list_free(&m->exempt_rules);
  clpd_list_free(&m->ingest);
  free(m->orm);
  memset(m, 0, sizeof *m);
}
