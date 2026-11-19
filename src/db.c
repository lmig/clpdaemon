/* Backend-neutral database layer (src/db.h, T8a): spec checks, SQL and
 * parameter validation, transaction state, error messages and results,
 * shared by the SQLite and PostgreSQL backends (src/db_backend.h). */
#include "db_backend.h"

#include <libpq-fe.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct clpd_db_result {
  size_t rows, columns, cap_rows;
  char **names;           /* columns entries */
  clpd_db_value *values;  /* rows * columns, row-major */
  int64_t affected;
};

static void *xrealloc(void *p, size_t n) {
  void *q = realloc(p, n == 0 ? 1 : n);
  if (q == NULL) {
    perror("clpdaemon");
    exit(1);
  }
  return q;
}

static void copy_msg(char *msg, size_t n, const char *s) {
  if (msg != NULL && n > 0) snprintf(msg, n, "%s", s);
}

void clpd_db_set_error(clpd_db *db, const char *fmt, ...) {
  va_list ap;
  size_t len;
  char *p;
  va_start(ap, fmt);
  vsnprintf(db->error, sizeof db->error, fmt, ap);
  va_end(ap);
  for (p = db->error; *p != '\0'; p++)
    if ((unsigned char)*p < 0x20 || *p == 0x7f) *p = ' ';
  len = strlen(db->error);
  while (len > 0 && db->error[len - 1] == ' ') db->error[--len] = '\0';
}

/* Values. */

clpd_db_value clpd_db_null(void) {
  clpd_db_value v;
  memset(&v, 0, sizeof v);
  v.type = CLPD_DB_NULL;
  return v;
}

clpd_db_value clpd_db_int(int64_t x) {
  clpd_db_value v = clpd_db_null();
  v.type = CLPD_DB_INTEGER;
  v.integer = x;
  return v;
}

clpd_db_value clpd_db_float(double x) {
  clpd_db_value v = clpd_db_null();
  v.type = CLPD_DB_FLOAT;
  v.real = x;
  return v;
}

clpd_db_value clpd_db_text(const char *s) {
  clpd_db_value v = clpd_db_null();
  v.type = CLPD_DB_TEXT;
  v.data = s;
  v.size = strlen(s);
  return v;
}

clpd_db_value clpd_db_blob(const void *data, size_t size) {
  clpd_db_value v = clpd_db_null();
  v.type = CLPD_DB_BLOB;
  v.data = data;
  v.size = size;
  return v;
}

/* Spec. */

/* libpq's test for a dbname that is itself a connection string. */
static int is_conninfo(const char *s) {
  return strchr(s, '=') != NULL || strncmp(s, "postgresql://", 13) == 0 ||
         strncmp(s, "postgres://", 11) == 0;
}

/* Nesting bound for dbname values that are connection strings: deeper
 * values are reported as invalid. */
#define SPEC_NESTING_MAX 8

/* Messages never include the value: it may hold credentials (NFR-2). */
int clpd_db_check_spec(const char *value, char *msg, size_t n) {
  PQconninfoOption *opts, *o;
  char *perr = NULL, *cur = NULL, *next;
  int password = 0, depth;

  if (value == NULL || *value == '\0') {
    copy_msg(msg, n, "database: expected a connection string");
    return 0;
  }
  if (strncmp(value, "sqlite", 6) == 0 &&
      (value[6] == '\0' || value[6] == ':')) {
    if (value[6] == '\0' || value[7] == '\0') {
      copy_msg(msg, n, "database: expected sqlite:<path>");
      return 0;
    }
    return 1;
  }
  /* A dbname that is a connection string can carry a password too (it is
   * echoed by "database does not exist"): check every nested level. */
  for (depth = 0;; depth++) {
    opts = depth > SPEC_NESTING_MAX
               ? NULL
               : PQconninfoParse(cur != NULL ? cur : value, &perr);
    if (perr != NULL) PQfreemem(perr); /* may quote the value: not shown */
    perr = NULL;
    free(cur);
    cur = NULL;
    if (opts == NULL) {
      copy_msg(msg, n, "database: invalid PostgreSQL connection string");
      return 0;
    }
    next = NULL;
    for (o = opts; o->keyword != NULL; o++) {
      if (o->val == NULL || o->val[0] == '\0') continue;
      if (strcmp(o->keyword, "password") == 0 ||
          strcmp(o->keyword, "sslpassword") == 0)
        password = 1;
      else if (strcmp(o->keyword, "dbname") == 0 && is_conninfo(o->val))
        next = o->val;
    }
    if (next != NULL && !password) {
      cur = xrealloc(NULL, strlen(next) + 1);
      strcpy(cur, next);
    }
    PQconninfoFree(opts);
    if (cur == NULL) break;
  }
  if (password) {
    copy_msg(msg, n, "database: the connection string must not contain a "
                     "password (use a password file, see PGPASSFILE)");
    return 0;
  }
  return 1;
}

/* SQL validation: one statement, placeholders exactly $1..$n. */

static int ident_char(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

/* Skips a quoted token starting at p (the opening quote). Doubled quotes
 * escape; with `backslash` a backslash escapes the next byte (E'...').
 * Returns the byte after the closing quote, or NULL when unterminated. */
static const char *skip_quoted(const char *p, int backslash) {
  char q = *p++;
  for (; *p != '\0'; p++) {
    if (backslash && *p == '\\') {
      if (*++p == '\0') return NULL;
    } else if (*p == q) {
      if (p[1] != q) return p + 1;
      p++;
    }
  }
  return NULL;
}

#define PLACEHOLDER_CAP 100000UL

static int check_sql(clpd_db *db, const char *sql, size_t n) {
  unsigned char *used = calloc(n == 0 ? 1 : n, 1);
  const char *p = sql, *q;
  int ended = 0, tokens = 0, ok = 0;
  size_t i;

  if (used == NULL) {
    perror("clpdaemon");
    exit(1);
  }
  while (*p != '\0') {
    char c = *p;
    if (is_space(c)) {
      p++;
      continue;
    }
    if (c == '-' && p[1] == '-') {
      while (*p != '\0' && *p != '\n' && *p != '\r') p++;
      continue;
    }
    if (c == '/' && p[1] == '*') {
      q = strstr(p + 2, "*/");
      if (q == NULL) {
        clpd_db_set_error(db, "unterminated comment in the SQL statement");
        goto out;
      }
      p = q + 2;
      continue;
    }
    if (ended) {
      clpd_db_set_error(db, "only one SQL statement is allowed");
      goto out;
    }
    if (c == ';') {
      ended = 1;
      p++;
      continue;
    }
    tokens = 1;
    if (c == '\'' || c == '"') {
      q = skip_quoted(p, 0);
      if (q == NULL) {
        clpd_db_set_error(db, "unterminated %s in the SQL statement",
                          c == '\'' ? "string" : "quoted identifier");
        goto out;
      }
      p = q;
    } else if (c == '$') {
      unsigned long num = 0;
      if (!is_digit(p[1]) || p[1] == '0') {
        clpd_db_set_error(db, "invalid placeholder (use $1..$n)");
        goto out;
      }
      for (p++; is_digit(*p); p++)
        if (num <= PLACEHOLDER_CAP) num = num * 10 + (unsigned long)(*p - '0');
      if (ident_char((unsigned char)*p) || *p == '$') {
        clpd_db_set_error(db, "invalid placeholder (use $1..$n)");
        goto out;
      }
      if (num > n) {
        clpd_db_set_error(db, "placeholder $%lu has no parameter (%lu given)",
                          num > PLACEHOLDER_CAP ? PLACEHOLDER_CAP : num,
                          (unsigned long)n);
        goto out;
      }
      used[num - 1] = 1;
    } else if (c == '?' || ((c == ':' || c == '@') &&
                            ident_char((unsigned char)p[1]))) {
      clpd_db_set_error(db, "invalid placeholder (use $1..$n)");
      goto out;
    } else if (c == ':' && p[1] == ':') {
      p += 2; /* PostgreSQL cast */
    } else if (ident_char((unsigned char)c)) {
      q = p;
      while (ident_char((unsigned char)*p) || *p == '$') p++;
      if (p - q == 1 && (*q == 'E' || *q == 'e') && *p == '\'') {
        q = skip_quoted(p, 1); /* PostgreSQL E'...' */
        if (q == NULL) {
          clpd_db_set_error(db, "unterminated string in the SQL statement");
          goto out;
        }
        p = q;
      }
    } else {
      p++;
    }
  }
  if (!tokens) {
    clpd_db_set_error(db, "empty SQL statement");
    goto out;
  }
  for (i = 0; i < n; i++) {
    if (!used[i]) {
      clpd_db_set_error(db, "parameter $%lu is not used in the SQL statement",
                        (unsigned long)(i + 1));
      goto out;
    }
  }
  ok = 1;
out:
  free(used);
  return ok;
}

/* 1 when s[0..n) is valid UTF-8 (no overlongs, surrogates or values above
 * U+10FFFF) without NUL bytes. */
static int valid_utf8(const unsigned char *s, size_t n) {
  size_t i = 0;
  while (i < n) {
    unsigned char c = s[i];
    size_t len, k;
    unsigned long cp;
    if (c == 0) return 0;
    if (c < 0x80) {
      i++;
      continue;
    }
    if (c >= 0xc2 && c <= 0xdf) len = 2, cp = c & 0x1f;
    else if (c >= 0xe0 && c <= 0xef) len = 3, cp = c & 0x0f;
    else if (c >= 0xf0 && c <= 0xf4) len = 4, cp = c & 0x07;
    else return 0;
    if (n - i < len) return 0;
    for (k = 1; k < len; k++) {
      if ((s[i + k] & 0xc0) != 0x80) return 0;
      cp = (cp << 6) | (s[i + k] & 0x3f);
    }
    if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
        (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff)
      return 0;
    i += len;
  }
  return 1;
}

static int check_params(clpd_db *db, const clpd_db_value *params, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    const clpd_db_value *v = &params[i];
    unsigned long k = (unsigned long)(i + 1);
    switch (v->type) {
    case CLPD_DB_NULL:
    case CLPD_DB_INTEGER:
      break;
    case CLPD_DB_FLOAT:
      if (isnan(v->real)) {
        clpd_db_set_error(db, "parameter $%lu: FLOAT must not be NaN", k);
        return 0;
      }
      break;
    case CLPD_DB_TEXT:
    case CLPD_DB_BLOB:
      if (v->data == NULL && v->size != 0) {
        clpd_db_set_error(db, "parameter $%lu: no data", k);
        return 0;
      }
      if (v->type == CLPD_DB_TEXT && !valid_utf8(v->data, v->size)) {
        clpd_db_set_error(db, "parameter $%lu: TEXT must be UTF-8 without "
                              "NUL bytes", k);
        return 0;
      }
      break;
    default:
      clpd_db_set_error(db, "parameter $%lu: unknown value type", k);
      return 0;
    }
  }
  return 1;
}

/* Connections. */

clpd_db_status clpd_db_open(const char *spec, clpd_db **out, char *msg,
                            size_t n) {
  clpd_db *db;
  clpd_db_status st;
  const char *target;

  *out = NULL;
  if (!clpd_db_check_spec(spec, msg, n)) return CLPD_DB_ERROR;
  db = xrealloc(NULL, sizeof *db);
  memset(db, 0, sizeof *db);
  if (strncmp(spec, "sqlite:", 7) == 0) {
    db->ops = &clpd_db_sqlite_ops;
    target = spec + 7;
  } else {
    db->ops = &clpd_db_pg_ops;
    target = spec;
  }
  st = db->ops->open(db, target);
  if (st != CLPD_DB_OK) {
    copy_msg(msg, n, db->error[0] != '\0' ? db->error
                                          : "cannot open the database");
    free(db);
    return st == CLPD_DB_LOST ? CLPD_DB_LOST : CLPD_DB_ERROR;
  }
  db->error[0] = '\0';
  copy_msg(msg, n, "");
  *out = db;
  return CLPD_DB_OK;
}

void clpd_db_close(clpd_db *db) {
  if (db == NULL) return;
  if (db->in_tx) db->ops->rollback(db);
  db->ops->close(db);
  free(db);
}

const char *clpd_db_backend(const clpd_db *db) { return db->ops->name; }

const char *clpd_db_error(const clpd_db *db) {
  return db != NULL ? db->error : "";
}

int clpd_db_in_transaction(const clpd_db *db) { return db->in_tx; }

static clpd_db_result *result_new(void) {
  clpd_db_result *r = xrealloc(NULL, sizeof *r);
  memset(r, 0, sizeof *r);
  return r;
}

clpd_db_status clpd_db_exec(clpd_db *db, const char *sql,
                            const clpd_db_value *params, size_t n,
                            clpd_db_result **result) {
  clpd_db_result *r;
  clpd_db_status st;

  if (result != NULL) *result = NULL;
  db->error[0] = '\0';
  if (n > 0 && params == NULL) {
    clpd_db_set_error(db, "%lu parameters expected, none given",
                      (unsigned long)n);
    return CLPD_DB_ERROR;
  }
  if (!check_sql(db, sql, n) || !check_params(db, params, n))
    return CLPD_DB_ERROR;
  r = result_new();
  st = db->ops->exec(db, sql, params, n, r);
  if (st == CLPD_DB_LOST) db->in_tx = 0;
  if (st != CLPD_DB_OK || result == NULL) {
    clpd_db_result_free(r);
    return st;
  }
  *result = r;
  return CLPD_DB_OK;
}

clpd_db_status clpd_db_begin(clpd_db *db) {
  clpd_db_status st;
  db->error[0] = '\0';
  if (db->in_tx) {
    clpd_db_set_error(db, "a transaction is already open");
    return CLPD_DB_ERROR;
  }
  st = db->ops->begin(db);
  db->in_tx = st == CLPD_DB_OK;
  return st;
}

clpd_db_status clpd_db_commit(clpd_db *db) {
  clpd_db_status st;
  char saved[CLPD_DB_ERROR_MAX];
  db->error[0] = '\0';
  if (!db->in_tx) {
    clpd_db_set_error(db, "no transaction is open");
    return CLPD_DB_ERROR;
  }
  st = db->ops->commit(db);
  if (st != CLPD_DB_OK && st != CLPD_DB_LOST) {
    memcpy(saved, db->error, sizeof saved);
    db->ops->rollback(db); /* a failed commit ends the transaction */
    memcpy(db->error, saved, sizeof saved);
  }
  db->in_tx = 0;
  return st;
}

clpd_db_status clpd_db_rollback(clpd_db *db) {
  clpd_db_status st;
  db->error[0] = '\0';
  if (!db->in_tx) {
    clpd_db_set_error(db, "no transaction is open");
    return CLPD_DB_ERROR;
  }
  st = db->ops->rollback(db);
  db->in_tx = 0;
  return st;
}

/* Results. */

size_t clpd_db_result_rows(const clpd_db_result *r) { return r->rows; }

size_t clpd_db_result_columns(const clpd_db_result *r) { return r->columns; }

const char *clpd_db_result_column_name(const clpd_db_result *r, size_t col) {
  if (col >= r->columns) return NULL;
  return r->names[col] != NULL ? r->names[col] : "";
}

const clpd_db_value *clpd_db_result_value(const clpd_db_result *r, size_t row,
                                          size_t col) {
  if (row >= r->rows || col >= r->columns) return NULL;
  return &r->values[row * r->columns + col];
}

int64_t clpd_db_result_affected(const clpd_db_result *r) {
  return r->affected;
}

static void free_value(clpd_db_value *v) {
  if (v->type == CLPD_DB_TEXT || v->type == CLPD_DB_BLOB)
    free((void *)v->data);
}

void clpd_db_result_free(clpd_db_result *r) {
  size_t i;
  if (r == NULL) return;
  for (i = 0; i < r->columns; i++) free(r->names[i]);
  for (i = 0; i < r->rows * r->columns; i++) free_value(&r->values[i]);
  free(r->names);
  free(r->values);
  free(r);
}

void clpd_db_result_init_columns(clpd_db_result *r, size_t columns) {
  size_t i;
  if (r->columns != 0 || r->rows != 0) return; /* declared once */
  r->names = xrealloc(NULL, columns * sizeof *r->names);
  for (i = 0; i < columns; i++) r->names[i] = NULL;
  r->columns = columns;
}

void clpd_db_result_set_name(clpd_db_result *r, size_t col, const char *name) {
  size_t n;
  if (col >= r->columns) return;
  free(r->names[col]);
  n = strlen(name) + 1;
  r->names[col] = xrealloc(NULL, n);
  memcpy(r->names[col], name, n);
}

size_t clpd_db_result_add_row(clpd_db_result *r) {
  size_t i, base = r->rows * r->columns;
  if (r->rows == r->cap_rows) {
    r->cap_rows = r->cap_rows == 0 ? 16 : r->cap_rows * 2;
    r->values = xrealloc(r->values,
                         r->cap_rows * r->columns * sizeof *r->values);
  }
  for (i = 0; i < r->columns; i++) r->values[base + i] = clpd_db_null();
  return r->rows++;
}

void clpd_db_result_set(clpd_db_result *r, size_t row, size_t col,
                        const clpd_db_value *v) {
  clpd_db_value *slot;
  char *buf;
  if (row >= r->rows || col >= r->columns) return;
  slot = &r->values[row * r->columns + col];
  free_value(slot);
  *slot = *v;
  if (v->type == CLPD_DB_TEXT || v->type == CLPD_DB_BLOB) {
    buf = xrealloc(NULL, v->size + 1);
    if (v->size != 0) memcpy(buf, v->data, v->size);
    buf[v->size] = '\0';
    slot->data = buf;
  } else {
    slot->data = NULL;
    slot->size = 0;
  }
}

void clpd_db_result_set_affected(clpd_db_result *r, int64_t affected) {
  r->affected = affected;
}
