/* SQLite backend (src/db_backend.h, T8b, L74). PostgreSQL DDL type names
 * are accepted as SQLite declared types (BIGINT -> INTEGER affinity,
 * DOUBLE PRECISION -> REAL, BYTEA and TIMESTAMPTZ/JSONB -> NUMERIC);
 * parameters keep their storage class (BLOB stays BLOB), and a column
 * declared TIMESTAMPTZ or JSONB is always read back as TEXT, the type the
 * PostgreSQL backend returns for them.
 *
 * SQLITE_BUSY and SQLITE_LOCKED ("database is locked", any extended code;
 * after the busy timeout) are CLPD_DB_TRANSIENT (T10f).
 *
 * Placeholders are SQLite named parameters "$1".."$n", bound by name.
 * Error messages come from sqlite3_errmsg/sqlite3_errstr, which never
 * contain bound parameter values or the database path. */
#include "db_backend.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Waits this long for a lock held by another connection. */
#define SQ_BUSY_TIMEOUT_MS 5000

static sqlite3 *handle(clpd_db *db) { return (sqlite3 *)db->impl; }

static clpd_db_status sq_fail(clpd_db *db, int rc) {
  sqlite3 *h = handle(db);
  clpd_db_set_error(db, "%s", h != NULL ? sqlite3_errmsg(h) : sqlite3_errstr(rc));
  if (rc == SQLITE_CONSTRAINT_UNIQUE || rc == SQLITE_CONSTRAINT_PRIMARYKEY)
    return CLPD_DB_UNIQUE;
  if ((rc & 0xff) == SQLITE_BUSY || (rc & 0xff) == SQLITE_LOCKED)
    return CLPD_DB_TRANSIENT;
  return CLPD_DB_ERROR;
}

/* Runs SQL without parameters or rows (connection setup, transactions). */
static clpd_db_status sq_simple(clpd_db *db, const char *sql) {
  int rc = sqlite3_exec(handle(db), sql, NULL, NULL, NULL);
  return rc == SQLITE_OK ? CLPD_DB_OK : sq_fail(db, rc);
}

static clpd_db_status sq_open(clpd_db *db, const char *target) {
  sqlite3 *h = NULL;
  int rc = sqlite3_open_v2(target, &h,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
  if (rc != SQLITE_OK) {
    clpd_db_set_error(db, "cannot open the SQLite database: %s",
                      h != NULL ? sqlite3_errmsg(h) : sqlite3_errstr(rc));
    sqlite3_close_v2(h); /* NULL is accepted */
    db->impl = NULL;
    return CLPD_DB_ERROR;
  }
  db->impl = h;
  sqlite3_extended_result_codes(h, 1);
  sqlite3_busy_timeout(h, SQ_BUSY_TIMEOUT_MS);
  /* Reading the schema fails now, not on the first statement, when the
   * file is not a database. */
  if (sq_simple(db, "PRAGMA foreign_keys = ON") != CLPD_DB_OK ||
      sq_simple(db, "SELECT count(*) FROM sqlite_master") != CLPD_DB_OK) {
    char saved[CLPD_DB_ERROR_MAX];
    memcpy(saved, db->error, sizeof saved);
    clpd_db_set_error(db, "cannot open the SQLite database: %s", saved);
    sqlite3_close_v2(h);
    db->impl = NULL;
    return CLPD_DB_ERROR;
  }
  return CLPD_DB_OK;
}

static void sq_close(clpd_db *db) {
  sqlite3_close_v2(handle(db));
  db->impl = NULL;
}

/* 1 when only whitespace and comments remain (prepare reports no
 * statement for them). */
static int empty_tail(sqlite3 *h, const char *tail) {
  sqlite3_stmt *st = NULL;
  int rc;
  if (tail == NULL || *tail == '\0') return 1;
  rc = sqlite3_prepare_v2(h, tail, -1, &st, NULL);
  if (st != NULL) {
    sqlite3_finalize(st);
    return 0;
  }
  return rc == SQLITE_OK;
}

/* Number of a "$<digits>" parameter name, or 0. */
static size_t placeholder_number(const char *name) {
  size_t num = 0;
  const char *p;
  if (name == NULL || name[0] != '$' || name[1] < '1' || name[1] > '9')
    return 0;
  for (p = name + 1; *p >= '0' && *p <= '9'; p++) {
    if (num > ((size_t)-1 - 9) / 10) return 0;
    num = num * 10 + (size_t)(*p - '0');
  }
  return *p == '\0' ? num : 0;
}

static int bind_value(sqlite3_stmt *st, int idx, const clpd_db_value *v) {
  switch (v->type) {
  case CLPD_DB_INTEGER:
    return sqlite3_bind_int64(st, idx, (sqlite3_int64)v->integer);
  case CLPD_DB_FLOAT:
    return sqlite3_bind_double(st, idx, v->real);
  case CLPD_DB_TEXT:
    return sqlite3_bind_text64(st, idx, v->size != 0 ? v->data : "",
                               (sqlite3_uint64)v->size, SQLITE_TRANSIENT,
                               SQLITE_UTF8);
  case CLPD_DB_BLOB:
    /* A NULL pointer would bind SQL NULL: an empty BLOB is a zero-length
     * BLOB. */
    if (v->size == 0) return sqlite3_bind_zeroblob(st, idx, 0);
    return sqlite3_bind_blob64(st, idx, v->data, (sqlite3_uint64)v->size,
                               SQLITE_TRANSIENT);
  case CLPD_DB_NULL:
  default:
    return sqlite3_bind_null(st, idx);
  }
}

static clpd_db_status bind_all(clpd_db *db, sqlite3_stmt *st,
                               const clpd_db_value *params, size_t n) {
  int count = sqlite3_bind_parameter_count(st), idx, rc;
  for (idx = 1; idx <= count; idx++) {
    size_t num = placeholder_number(sqlite3_bind_parameter_name(st, idx));
    if (num == 0 || num > n) {
      clpd_db_set_error(db, "invalid placeholder (use $1..$n)");
      return CLPD_DB_ERROR;
    }
    rc = bind_value(st, idx, &params[num - 1]);
    if (rc != SQLITE_OK) {
      clpd_db_set_error(db, "parameter $%lu: %s", (unsigned long)num,
                        sqlite3_errstr(rc));
      return CLPD_DB_ERROR;
    }
  }
  return CLPD_DB_OK;
}

/* Declared types PostgreSQL returns as text but SQLite gives NUMERIC
 * affinity (a numeric-looking value would come back as a number). */
static int text_decltype(const char *decl) {
  return decl != NULL &&
         (strcasecmp(decl, "TIMESTAMPTZ") == 0 || strcasecmp(decl, "JSONB") == 0);
}

static void read_row(sqlite3_stmt *st, clpd_db_result *result, int cols) {
  size_t row = clpd_db_result_add_row(result);
  int c;
  for (c = 0; c < cols; c++) {
    clpd_db_value v = clpd_db_null();
    int type = sqlite3_column_type(st, c);
    if ((type == SQLITE_INTEGER || type == SQLITE_FLOAT) &&
        text_decltype(sqlite3_column_decltype(st, c)))
      type = SQLITE_TEXT;
    switch (type) {
    case SQLITE_INTEGER:
      v = clpd_db_int((int64_t)sqlite3_column_int64(st, c));
      break;
    case SQLITE_FLOAT:
      v = clpd_db_float(sqlite3_column_double(st, c));
      break;
    case SQLITE_TEXT:
      v.type = CLPD_DB_TEXT;
      v.data = sqlite3_column_text(st, c);
      v.size = (size_t)sqlite3_column_bytes(st, c);
      break;
    case SQLITE_BLOB:
      v.type = CLPD_DB_BLOB;
      v.data = sqlite3_column_blob(st, c);
      v.size = (size_t)sqlite3_column_bytes(st, c);
      break;
    default:
      break;
    }
    clpd_db_result_set(result, row, (size_t)c, &v);
  }
}

/* First keyword of sql after whitespace and comments, upper-cased into
 * buf (empty when it is not a word). */
static void first_keyword(const char *sql, char *buf, size_t n) {
  const char *p = sql;
  size_t i = 0;
  for (;;) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
           *p == '\f' || *p == '\v')
      p++;
    if (p[0] == '-' && p[1] == '-') {
      while (*p != '\0' && *p != '\n') p++;
    } else if (p[0] == '/' && p[1] == '*') {
      const char *q = strstr(p + 2, "*/");
      p = q != NULL ? q + 2 : p + strlen(p);
    } else {
      break;
    }
  }
  while (i + 1 < n && ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))) {
    buf[i++] = (char)(*p >= 'a' ? *p - 'a' + 'A' : *p);
    p++;
  }
  buf[i] = '\0';
}

/* INSERT, UPDATE or DELETE (REPLACE is SQLite's INSERT OR REPLACE; a WITH
 * prefix is DML when the statement writes). */
static int is_dml(sqlite3_stmt *st, const char *sql) {
  char kw[16];
  first_keyword(sql, kw, sizeof kw);
  if (strcmp(kw, "INSERT") == 0 || strcmp(kw, "UPDATE") == 0 ||
      strcmp(kw, "DELETE") == 0 || strcmp(kw, "REPLACE") == 0)
    return 1;
  return strcmp(kw, "WITH") == 0 && !sqlite3_stmt_readonly(st);
}

static clpd_db_status sq_exec(clpd_db *db, const char *sql,
                              const clpd_db_value *params, size_t n,
                              clpd_db_result *result) {
  sqlite3 *h = handle(db);
  sqlite3_stmt *st = NULL;
  const char *tail = NULL;
  clpd_db_status status = CLPD_DB_OK;
  int rc, cols, c;

  rc = sqlite3_prepare_v2(h, sql, -1, &st, &tail);
  if (rc != SQLITE_OK) return sq_fail(db, rc);
  if (st == NULL) {
    clpd_db_set_error(db, "empty SQL statement");
    return CLPD_DB_ERROR;
  }
  if (!empty_tail(h, tail)) { /* db.c checks this too */
    clpd_db_set_error(db, "only one SQL statement is allowed");
    sqlite3_finalize(st);
    return CLPD_DB_ERROR;
  }
  status = bind_all(db, st, params, n);
  if (status != CLPD_DB_OK) {
    sqlite3_finalize(st);
    return status;
  }
  cols = sqlite3_column_count(st);
  clpd_db_result_init_columns(result, (size_t)cols);
  for (c = 0; c < cols; c++) {
    const char *name = sqlite3_column_name(st, c);
    clpd_db_result_set_name(result, (size_t)c, name != NULL ? name : "");
  }
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) read_row(st, result, cols);
  if (rc != SQLITE_DONE) {
    status = sq_fail(db, sqlite3_extended_errcode(h));
  } else if (is_dml(st, sql)) {
#if SQLITE_VERSION_NUMBER >= 3037000
    clpd_db_result_set_affected(result, (int64_t)sqlite3_changes64(h));
#else
    clpd_db_result_set_affected(result, (int64_t)sqlite3_changes(h));
#endif
  }
  sqlite3_finalize(st);
  return status;
}

/* IMMEDIATE takes the write lock now (waiting up to the busy timeout), so
 * a later write cannot fail on a lock upgrade. */
static clpd_db_status sq_begin(clpd_db *db) {
  return sq_simple(db, "BEGIN IMMEDIATE");
}

static clpd_db_status sq_commit(clpd_db *db) {
  if (sqlite3_get_autocommit(handle(db))) {
    clpd_db_set_error(db, "the transaction was rolled back by an earlier "
                          "error");
    return CLPD_DB_ERROR;
  }
  return sq_simple(db, "COMMIT");
}

/* SQLite may already have rolled back after some errors (SQLITE_FULL,
 * SQLITE_IOERR, ...): nothing is left to undo. */
static clpd_db_status sq_rollback(clpd_db *db) {
  if (sqlite3_get_autocommit(handle(db))) return CLPD_DB_OK;
  return sq_simple(db, "ROLLBACK");
}

const clpd_db_backend_ops clpd_db_sqlite_ops = {
    "sqlite", sq_open, sq_close, sq_exec, sq_begin, sq_commit, sq_rollback,
};
