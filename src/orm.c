/* ORM mapping file parser (T9a, docs/orm.md). CLIPS-free: the package
 * loader binds the model to the deftemplates (src/packages.c). */
#include "orm.h"

#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IDENT_RULE "[a-z_][a-z0-9_]* (at most 63 bytes)"
#define SEQ_PREFIX "seq."
#define OWNED_SUFFIX ".owned"
#define SLOT_PREFIX "slot."

static const char *const sql_names[CLPD_ORM_SQL_TYPE_COUNT] = {
    "bigint", "double", "text", "bytea", "boolean", "timestamptz", "jsonb",
    "numeric"};
/* Type names per backend (T9b). The SQLite names keep INTEGER, REAL, TEXT
 * or BLOB affinity, never NUMERIC, so values come back as they were
 * stored (L76): TIMESTAMPTZ, JSONB and NUMERIC are TEXT there. */
static const char *const pg_names[CLPD_ORM_SQL_TYPE_COUNT] = {
    "BIGINT", "DOUBLE PRECISION", "TEXT", "BYTEA", "BOOLEAN", "TIMESTAMPTZ",
    "JSONB", "NUMERIC"};
static const char *const sqlite_names[CLPD_ORM_SQL_TYPE_COUNT] = {
    "INTEGER", "REAL", "TEXT", "BLOB", "INTEGER", "TEXT", "TEXT", "TEXT"};
static const char *const clips_names[CLPD_ORM_CLIPS_TYPE_COUNT] = {
    "integer", "float", "string", "symbol"};
static const char *const null_names[CLPD_ORM_NULL_COUNT] = {"nil", "default",
                                                            "error"};

const char *clpd_orm_sql_type_name(clpd_orm_sql_type t) {
  return (unsigned)t < CLPD_ORM_SQL_TYPE_COUNT ? sql_names[t] : NULL;
}

const char *clpd_orm_clips_type_name(clpd_orm_clips_type t) {
  return (unsigned)t < CLPD_ORM_CLIPS_TYPE_COUNT ? clips_names[t] : NULL;
}

const char *clpd_orm_null_name(clpd_orm_null n) {
  return (unsigned)n < CLPD_ORM_NULL_COUNT ? null_names[n] : NULL;
}

const char *clpd_orm_backend_type(clpd_orm_sql_type t, const char *backend) {
  if ((unsigned)t >= CLPD_ORM_SQL_TYPE_COUNT || backend == NULL) return NULL;
  if (strcmp(backend, "postgresql") == 0) return pg_names[t];
  if (strcmp(backend, "sqlite") == 0) return sqlite_names[t];
  return NULL;
}

int clpd_orm_valid_ident(const char *s) {
  size_t n = strlen(s), i;
  if (n == 0 || n > CLPD_ORM_IDENT_MAX) return 0;
  if (!((s[0] >= 'a' && s[0] <= 'z') || s[0] == '_')) return 0;
  for (i = 1; i < n; i++) {
    char c = s[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
      return 0;
  }
  return 1;
}

/* Index of `name` in names[0..count), or -1. */
static int lookup(const char *const *names, int count, const char *name) {
  int i;
  for (i = 0; i < count; i++)
    if (strcmp(names[i], name) == 0) return i;
  return -1;
}

static void *grow(void *p, size_t count, size_t size) {
  void *q = realloc(p, (count + 1) * size);
  if (q == NULL) clpd_log_die_errno();
  return q;
}

const clpd_orm_column *clpd_orm_column_of(const clpd_orm_template *t,
                                          const char *slot) {
  size_t i;
  for (i = 0; i < t->column_count; i++)
    if (strcmp(t->columns[i].slot, slot) == 0) return &t->columns[i];
  return NULL;
}

int clpd_orm_resolve_key(clpd_orm_template *t) {
  size_t i, *idx;
  const clpd_orm_column *c = clpd_orm_column_of(t, CLPD_ORM_WF_ID);
  if (c == NULL) return -1;
  idx = malloc((t->key.count + 1) * sizeof *idx);
  if (idx == NULL) clpd_log_die_errno();
  idx[0] = (size_t)(c - t->columns);
  for (i = 0; i < t->key.count; i++) {
    c = clpd_orm_column_of(t, t->key.items[i]);
    if (c == NULL) {
      free(idx);
      return -1;
    }
    idx[i + 1] = (size_t)(c - t->columns);
  }
  free(t->key_columns);
  t->key_columns = idx;
  return 0;
}

/* Template `name`, added at `line` when new. */
static clpd_orm_template *template_of(clpd_orm *orm, const char *name,
                                      long line) {
  clpd_orm_template *t;
  size_t i;
  for (i = 0; i < orm->template_count; i++)
    if (strcmp(orm->templates[i].name, name) == 0) return &orm->templates[i];
  orm->templates = grow(orm->templates, orm->template_count, sizeof *t);
  t = &orm->templates[orm->template_count++];
  memset(t, 0, sizeof *t);
  t->name = clpd_strdup(name);
  t->line = line;
  return t;
}

static clpd_orm_sequence *sequence_of(clpd_orm *orm, const char *alias,
                                      long line) {
  clpd_orm_sequence *s;
  size_t i;
  for (i = 0; i < orm->sequence_count; i++)
    if (strcmp(orm->sequences[i].alias, alias) == 0) return &orm->sequences[i];
  orm->sequences = grow(orm->sequences, orm->sequence_count, sizeof *s);
  s = &orm->sequences[orm->sequence_count++];
  memset(s, 0, sizeof *s);
  s->alias = clpd_strdup(alias);
  s->line = line;
  return s;
}

/* Reports a repeated key; returns 1 when `*seen` was already set,
 * otherwise records the current line. */
static int duplicate(clpd_kv *kv, const char *key, long *seen) {
  if (*seen != 0) {
    clpd_kv_error(kv, "duplicate key '%s' (first set on line %ld)", key,
                  *seen);
    return 1;
  }
  *seen = kv->line;
  return 0;
}

static int ident(clpd_kv *kv, const char *key, const char *what,
                 const char *value) {
  if (clpd_orm_valid_ident(value)) return 1;
  clpd_kv_error(kv, "%s: %sexpected an identifier matching " IDENT_RULE
                    ", got '%s'", key, what, value);
  return 0;
}

static int yes_no(clpd_kv *kv, const char *key, const char *value, int *out) {
  if (strcmp(value, "yes") == 0) {
    *out = 1;
  } else if (strcmp(value, "no") == 0) {
    *out = 0;
  } else {
    clpd_kv_error(kv, "%s: expected yes or no, got '%s'", key, value);
    return 0;
  }
  return 1;
}

/* Splits value into exactly n blank-separated fields (in place). */
static int fields(char *value, char **out, int n) {
  int count = 0;
  char *p = value;
  for (;;) {
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') break;
    if (count == n) return 0;
    out[count++] = p;
    while (*p != '\0' && *p != ' ' && *p != '\t') p++;
    if (*p != '\0') *p++ = '\0';
  }
  return count == n;
}

static void on_slot(clpd_kv *kv, clpd_orm_template *t,
                    const char *key, const char *slot, char *value) {
  char *f[4];
  int sql, clips, null;
  size_t i;
  clpd_orm_column *c;

  for (i = 0; i < t->slot_line_count; i++) {
    if (strcmp(t->slot_lines[i].slot, slot) == 0) {
      clpd_kv_error(kv, "duplicate key '%s' (first set on line %ld)", key,
                    t->slot_lines[i].line);
      return;
    }
  }
  /* Recorded before validation: a repeat of a rejected line is still a
   * duplicate, like the other keys (duplicate() runs first there too). */
  t->slot_lines = grow(t->slot_lines, t->slot_line_count, sizeof *t->slot_lines);
  t->slot_lines[t->slot_line_count].slot = clpd_strdup(slot);
  t->slot_lines[t->slot_line_count++].line = kv->line;
  {
    /* fields() writes NULs into value: keep the text for the message. */
    char *copy = clpd_strdup(value);
    int ok = fields(value, f, 4);
    if (!ok)
      clpd_kv_error(kv, "%s: expected '<column> <sql-type> <clips-type> "
                        "<null>', got '%s'", key, copy);
    free(copy);
    if (!ok) return;
  }
  if (!ident(kv, key, "column: ", f[0])) return;
  if ((sql = lookup(sql_names, CLPD_ORM_SQL_TYPE_COUNT, f[1])) < 0) {
    clpd_kv_error(kv, "%s: unknown sql type '%s' (expected bigint, double, "
                      "text, bytea, boolean, timestamptz, jsonb, numeric)",
                  key, f[1]);
    return;
  }
  if ((clips = lookup(clips_names, CLPD_ORM_CLIPS_TYPE_COUNT, f[2])) < 0) {
    clpd_kv_error(kv, "%s: unknown clips type '%s' (expected integer, float, "
                      "string, symbol)", key, f[2]);
    return;
  }
  if ((null = lookup(null_names, CLPD_ORM_NULL_COUNT, f[3])) < 0) {
    clpd_kv_error(kv, "%s: unknown null policy '%s' (expected nil, default, "
                      "error)", key, f[3]);
    return;
  }
  for (i = 0; i < t->column_count; i++) {
    if (strcmp(t->columns[i].column, f[0]) == 0) {
      clpd_kv_error(kv, "%s: column '%s' is already mapped by slot '%s' on "
                        "line %ld", key, f[0], t->columns[i].slot,
                    t->columns[i].line);
      return;
    }
  }
  t->columns = grow(t->columns, t->column_count, sizeof *c);
  c = &t->columns[t->column_count++];
  c->slot = clpd_strdup(slot);
  c->column = clpd_strdup(f[0]);
  c->sql_type = (clpd_orm_sql_type)sql;
  c->clips_type = (clpd_orm_clips_type)clips;
  c->null = (clpd_orm_null)null;
  c->line = kv->line;
}

/* Rejects wf-id and a slot already in the list being built (ctx). */
static int key_item(clpd_kv *kv, const char *key, const char *item,
                    void *ctx) {
  clpd_list *seen = ctx;
  size_t i;
  if (strcmp(item, CLPD_ORM_WF_ID) == 0) {
    clpd_kv_error(kv, "%s: '%s' is always the first key slot and must not be "
                      "listed", key, item);
    return 0;
  }
  for (i = 0; i < seen->count; i++) {
    if (strcmp(seen->items[i], item) == 0) {
      clpd_kv_error(kv, "%s: '%s' listed more than once", key, item);
      return 0;
    }
  }
  /* Only the address is kept: the list being built owns the strings. */
  seen->items = grow(seen->items, seen->count, sizeof *seen->items);
  seen->items[seen->count++] = (char *)item;
  return 1;
}

static void on_template(clpd_kv *kv, clpd_orm *orm, char *key, char *value) {
  char *dot = strchr(key, '.');
  const char *attr;
  clpd_orm_template *t;
  size_t i;

  if (dot == NULL || dot == key) {
    clpd_kv_error(kv, "unknown key '%s'", key);
    return;
  }
  attr = dot + 1;
  if (strcmp(attr, "table") != 0 && strcmp(attr, "owned") != 0 &&
      strcmp(attr, "key") != 0 &&
      !(strncmp(attr, SLOT_PREFIX, sizeof SLOT_PREFIX - 1) == 0 &&
        attr[sizeof SLOT_PREFIX - 1] != '\0')) {
    clpd_kv_error(kv, "unknown key '%s'", key);
    return;
  }
  *dot = '\0';
  t = template_of(orm, key, kv->line);
  *dot = '.';
  if (strcmp(attr, "table") == 0) {
    if (duplicate(kv, key, &t->table_line) || !ident(kv, key, "", value))
      return;
    for (i = 0; i < orm->template_count; i++) {
      const clpd_orm_template *o = &orm->templates[i];
      if (o != t && o->table != NULL && strcmp(o->table, value) == 0) {
        clpd_kv_error(kv, "%s: table '%s' is already mapped by template '%s' "
                          "on line %ld", key, value, o->name, o->table_line);
        return;
      }
    }
    t->table = clpd_strdup(value);
  } else if (strcmp(attr, "owned") == 0) {
    if (duplicate(kv, key, &t->owned_line)) return;
    yes_no(kv, key, value, &t->owned);
  } else if (strcmp(attr, "key") == 0) {
    clpd_list seen = {NULL, 0}, list = {NULL, 0};
    int ok;
    if (duplicate(kv, key, &t->key_line)) return;
    ok = clpd_kv_list(kv, key, value, &list, key_item, &seen);
    free(seen.items);
    if (ok) {
      clpd_list_free(&t->key);
      t->key = list;
    }
  } else {
    on_slot(kv, t, key, attr + sizeof SLOT_PREFIX - 1, value);
  }
}

static void on_sequence(clpd_kv *kv, clpd_orm *orm, char *key, char *value) {
  char *alias = key + sizeof SEQ_PREFIX - 1;
  size_t n = strlen(alias), suffix = sizeof OWNED_SUFFIX - 1;
  int owned = n > suffix && strcmp(alias + n - suffix, OWNED_SUFFIX) == 0;
  clpd_orm_sequence *s;
  size_t i;

  if (n == 0 || (n == suffix && strcmp(alias, OWNED_SUFFIX) == 0)) {
    clpd_kv_error(kv, "unknown key '%s'", key);
    return;
  }
  if (owned) alias[n - suffix] = '\0';
  if (!clpd_orm_valid_ident(alias)) {
    clpd_kv_error(kv, "%s%s%s: sequence alias: expected an identifier "
                      "matching " IDENT_RULE ", got '%s'",
                  SEQ_PREFIX, alias, owned ? OWNED_SUFFIX : "", alias);
    return;
  }
  s = sequence_of(orm, alias, kv->line);
  if (owned) {
    alias[n - suffix] = '.';
    if (duplicate(kv, key, &s->owned_line)) return;
    yes_no(kv, key, value, &s->owned);
    return;
  }
  if (duplicate(kv, key, &s->name_line) || !ident(kv, key, "", value)) return;
  for (i = 0; i < orm->sequence_count; i++) {
    const clpd_orm_sequence *o = &orm->sequences[i];
    if (o != s && o->name != NULL && strcmp(o->name, value) == 0) {
      clpd_kv_error(kv, "%s: sequence '%s' is already mapped by alias '%s' on "
                        "line %ld", key, value, o->alias, o->name_line);
      return;
    }
  }
  s->name = clpd_strdup(value);
}

static void on_pair(clpd_kv *kv, char *key, char *value, void *ctx) {
  if (strncmp(key, SEQ_PREFIX, sizeof SEQ_PREFIX - 1) == 0)
    on_sequence(kv, ctx, key, value);
  else
    on_template(kv, ctx, key, value);
}

static void missing(FILE *err, const char *path, const char *a,
                    const char *b, int *errors) {
  fprintf(err, "clpdaemon: %s: missing required key '%s%s'\n", path, a, b);
  (*errors)++;
}

static int is_key_slot(const clpd_orm_template *t, const char *slot) {
  size_t i;
  if (strcmp(slot, CLPD_ORM_WF_ID) == 0) return 1;
  for (i = 0; i < t->key.count; i++)
    if (strcmp(t->key.items[i], slot) == 0) return 1;
  return 0;
}

/* File-level checks after every line: required keys, key slots with a
 * null policy other than error. Returns the error count. */
static int check_file(const clpd_orm *orm, FILE *err) {
  size_t i, j;
  int errors = 0;
  for (i = 0; i < orm->template_count; i++) {
    const clpd_orm_template *t = &orm->templates[i];
    if (t->table_line == 0) missing(err, orm->path, t->name, ".table", &errors);
    if (t->owned_line == 0) missing(err, orm->path, t->name, ".owned", &errors);
    for (j = 0; j < t->column_count; j++) {
      const clpd_orm_column *c = &t->columns[j];
      if (c->null != CLPD_ORM_NULL_ERROR && is_key_slot(t, c->slot)) {
        fprintf(err, "clpdaemon: %s:%ld: %s.slot.%s: a key slot needs null "
                     "policy error, got '%s'\n", orm->path, c->line, t->name,
                c->slot, null_names[c->null]);
        errors++;
      }
    }
  }
  for (i = 0; i < orm->sequence_count; i++) {
    const clpd_orm_sequence *s = &orm->sequences[i];
    if (s->name_line == 0) missing(err, orm->path, SEQ_PREFIX, s->alias, &errors);
    if (s->owned_line == 0) {
      fprintf(err, "clpdaemon: %s: missing required key '" SEQ_PREFIX
                   "%s" OWNED_SUFFIX "'\n", orm->path, s->alias);
      errors++;
    }
  }
  return errors;
}

int clpd_orm_parse(const char *path, clpd_orm *orm, FILE *err) {
  clpd_kv kv;
  memset(orm, 0, sizeof *orm);
  orm->path = clpd_strdup(path);
  if (clpd_kv_read(&kv, path, err, on_pair, orm) != 0) return 1;
  return kv.errors + check_file(orm, err) != 0;
}

void clpd_orm_free(clpd_orm *orm) {
  size_t i, j;
  for (i = 0; i < orm->template_count; i++) {
    clpd_orm_template *t = &orm->templates[i];
    for (j = 0; j < t->column_count; j++) {
      free(t->columns[j].slot);
      free(t->columns[j].column);
    }
    free(t->columns);
    for (j = 0; j < t->slot_line_count; j++) free(t->slot_lines[j].slot);
    free(t->slot_lines);
    free(t->key_columns);
    clpd_list_free(&t->key);
    free(t->name);
    free(t->table);
  }
  for (i = 0; i < orm->sequence_count; i++) {
    free(orm->sequences[i].alias);
    free(orm->sequences[i].name);
  }
  free(orm->templates);
  free(orm->sequences);
  free(orm->path);
  memset(orm, 0, sizeof *orm);
}

/* The CLIPS types each sql type holds (L83), one bit per
 * clpd_orm_clips_type: the only source of both clpd_orm_type_fits and the
 * lists messages show. */
#define FIT(c) (1u << (c))
static const unsigned fits[CLPD_ORM_SQL_TYPE_COUNT] = {
    FIT(CLPD_ORM_INTEGER),                                       /* bigint */
    FIT(CLPD_ORM_FLOAT),                                         /* double */
    FIT(CLPD_ORM_STRING) | FIT(CLPD_ORM_SYMBOL),                 /* text */
    FIT(CLPD_ORM_STRING),                                        /* bytea */
    FIT(CLPD_ORM_INTEGER),                                       /* boolean */
    FIT(CLPD_ORM_STRING) | FIT(CLPD_ORM_SYMBOL),                 /* timestamptz */
    FIT(CLPD_ORM_STRING) | FIT(CLPD_ORM_SYMBOL),                 /* jsonb */
    FIT(CLPD_ORM_FLOAT) | FIT(CLPD_ORM_STRING) | FIT(CLPD_ORM_SYMBOL)}; /* numeric */

int clpd_orm_type_fits(clpd_orm_sql_type t, clpd_orm_clips_type c) {
  if ((unsigned)t >= CLPD_ORM_SQL_TYPE_COUNT ||
      (unsigned)c >= CLPD_ORM_CLIPS_TYPE_COUNT)
    return 0;
  return (fits[t] & FIT(c)) != 0;
}

const char *clpd_orm_type_fits_list(clpd_orm_sql_type t, char *buf,
                                    size_t n) {
  size_t len = 0, left = 0;
  int c;
  if ((unsigned)t >= CLPD_ORM_SQL_TYPE_COUNT || n == 0) return NULL;
  buf[0] = '\0';
  for (c = 0; c < CLPD_ORM_CLIPS_TYPE_COUNT; c++)
    if ((fits[t] & FIT(c)) != 0) left++;
  for (c = 0; c < CLPD_ORM_CLIPS_TYPE_COUNT; c++) {
    if ((fits[t] & FIT(c)) == 0) continue;
    left--;
    snprintf(buf + len, n - len, "%s%s", clips_names[c],
             left > 1 ? ", " : left == 1 ? " or " : "");
    len += strlen(buf + len);
  }
  return buf;
}

const clpd_orm_template *clpd_orm_template_of(const clpd_orm *orm,
                                              const char *name) {
  size_t i;
  for (i = 0; i < orm->template_count; i++)
    if (strcmp(orm->templates[i].name, name) == 0) return &orm->templates[i];
  return NULL;
}

const clpd_orm_sequence *clpd_orm_sequence_of(const clpd_orm *orm,
                                              const char *alias) {
  size_t i;
  for (i = 0; i < orm->sequence_count; i++)
    if (strcmp(orm->sequences[i].alias, alias) == 0)
      return &orm->sequences[i];
  return NULL;
}

/* Column of key position i of t (0: wf-id), or NULL when that slot has no
 * column line. */
static const char *key_column(const clpd_orm_template *t, size_t i) {
  const clpd_orm_column *c = clpd_orm_column_of(
      t, i == 0 ? CLPD_ORM_WF_ID : t->key.items[i - 1]);
  return c != NULL ? c->column : NULL;
}

static const clpd_orm_column *column_named(const clpd_orm_template *t,
                                           const char *column) {
  size_t i;
  for (i = 0; i < t->column_count; i++)
    if (strcmp(t->columns[i].column, column) == 0) return &t->columns[i];
  return NULL;
}

/* 1 when the two lines give a column the same table definition: sql type,
 * and NOT NULL (null policy error) or not. */
static int same_column(const clpd_orm_column *a, const clpd_orm_column *b) {
  return b != NULL && a->sql_type == b->sql_type &&
         (a->null == CLPD_ORM_NULL_ERROR) == (b->null == CLPD_ORM_NULL_ERROR);
}

int clpd_orm_table_difference(const clpd_orm_template *a,
                              const clpd_orm_template *b, char *buf,
                              size_t n) {
  size_t i;
  if (a->owned != b->owned) {
    snprintf(buf, n, "owned");
    return 1;
  }
  if (a->key.count != b->key.count) {
    snprintf(buf, n, "key");
    return 1;
  }
  for (i = 0; i <= a->key.count; i++) {
    const char *x = key_column(a, i), *y = key_column(b, i);
    if (x == NULL || y == NULL || strcmp(x, y) != 0) {
      snprintf(buf, n, "key");
      return 1;
    }
  }
  for (i = 0; i < a->column_count; i++) {
    if (!same_column(&a->columns[i], column_named(b, a->columns[i].column))) {
      snprintf(buf, n, "column '%s'", a->columns[i].column);
      return 1;
    }
  }
  for (i = 0; i < b->column_count; i++) {
    if (column_named(a, b->columns[i].column) == NULL) {
      snprintf(buf, n, "column '%s'", b->columns[i].column);
      return 1;
    }
  }
  return 0;
}
