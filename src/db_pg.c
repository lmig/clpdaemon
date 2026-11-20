/* PostgreSQL backend (src/db_backend.h, T8c) on libpq.
 *
 * - Connection: PQconnectdbParams on the conninfo db.c already checked (no
 *   password: it comes from PGPASSFILE/.pgpass), expanded over a default
 *   connect_timeout of 10 s, so a connect_timeout in the conninfo (or
 *   PGCONNECT_TIMEOUT) wins; CONNECTION_OK required, client encoding UTF8,
 *   standard_conforming_strings on (the quoting db.c's SQL check assumes)
 *   and extra_float_digits 3 (exact doubles on any server). lock_timeout
 *   5 s and statement_timeout 30 s when the session value is 0, so a value
 *   from options=-c ..., role or database settings wins; a timeout is
 *   CLPD_DB_TRANSIENT (55P03, 57014). Notices are discarded, never printed.
 * - Statements: PQexecParams with text-format parameters (INTEGER as int8,
 *   FLOAT as float8 with %.17g, TEXT untyped so the server types it from
 *   its context, e.g. timestamptz or jsonb), BLOB as a binary bytea
 *   parameter and NULL as a NULL pointer. Text-format results; column
 *   types by OID: int2/int4/int8 -> INTEGER, float4/float8 -> FLOAT,
 *   bytea -> BLOB (decoded), bool -> INTEGER 0/1, anything else TEXT.
 * - Errors: one line, "<primary message> (SQLSTATE <code>)"; the DETAIL
 *   and other fields (which may carry row values) are never shown, and in
 *   data exceptions (class 22) everything from the first to the last double
 *   quote of the whole primary message (the input value) becomes "?".
 *   SQLSTATE 23505 is CLPD_DB_UNIQUE; 40001 (serialization failure), 40P01
 *   (deadlock detected), 55P03 (lock not available) and 57014 (query
 *   canceled) are CLPD_DB_TRANSIENT (T10f); a failure that leaves the
 *   connection CONNECTION_BAD is CLPD_DB_LOST.
 * - A COMMIT the server answers with ROLLBACK (aborted transaction) fails.
 * - Out of memory exits through clpd_db_die_oom.
 * Engine thread only, like every clpd_db call. */
#include "db_backend.h"

#include <libpq-fe.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Type OIDs (pg_type.dat; stable across server versions). */
#define OID_BOOL 16
#define OID_BYTEA 17
#define OID_INT8 20
#define OID_INT2 21
#define OID_INT4 23
#define OID_FLOAT4 700
#define OID_FLOAT8 701

#define PG_MAX_PARAMS 65535 /* protocol limit (Int16 count) */

/* Defaults that bound every call on the engine thread (T8f, L80). */
#define PG_CONNECT_TIMEOUT "10"  /* seconds */
#define PG_LOCK_TIMEOUT "5s"
#define PG_STATEMENT_TIMEOUT "30s"

static PGconn *conn_of(clpd_db *db) { return (PGconn *)db->impl; }

static void *xmalloc(size_t n) {
  void *p = calloc(n == 0 ? 1 : n, 1); /* zeroed: no uninitialized slots */
  if (p == NULL) clpd_db_die_oom();
  return p;
}

static void ignore_notice(void *arg, const char *message) {
  (void)arg;
  (void)message;
}

/* The first line of libpq's connection message (it names the server and
 * the user, never the whole conninfo; a password is never in it). */
static void set_conn_error(clpd_db *db, const char *prefix) {
  const char *m = PQerrorMessage(conn_of(db));
  size_t len = strcspn(m, "\n");
  if (len == 0)
    clpd_db_set_error(db, "%s", prefix);
  else
    clpd_db_set_error(db, "%s: %.*s", prefix, (int)len, m);
}

/* The status of a server error by its SQLSTATE (NULL: none). */
static clpd_db_status status_of(const char *state) {
  static const char *const transient[] = {"40001", "40P01", "55P03",
                                          "57014"};
  size_t i;
  if (state == NULL) return CLPD_DB_ERROR;
  if (strcmp(state, "23505") == 0) return CLPD_DB_UNIQUE;
  for (i = 0; i < sizeof transient / sizeof transient[0]; i++)
    if (strcmp(state, transient[i]) == 0) return CLPD_DB_TRANSIENT;
  return CLPD_DB_ERROR;
}

/* Maps a failed result (res may be NULL) to a status and a message. */
static clpd_db_status fail_result(clpd_db *db, const PGresult *res,
                                  const char *what) {
  const char *state, *primary, *first, *last;

  if (PQstatus(conn_of(db)) == CONNECTION_BAD) {
    set_conn_error(db, "connection to the database lost");
    return CLPD_DB_LOST;
  }
  state = res != NULL ? PQresultErrorField(res, PG_DIAG_SQLSTATE) : NULL;
  primary = res != NULL ? PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY)
                        : NULL;
  if (primary == NULL || *primary == '\0') {
    set_conn_error(db, what);
    return CLPD_DB_ERROR;
  }
  /* Data exceptions quote the offending input, e.g. invalid input syntax
   * for type bigint: "<value>"; the value may contain quotes itself, so
   * everything from the first to the last quote goes. The quotes are found
   * on the whole message, before any truncation to the error buffer, so no
   * part of a long value can remain. */
  first = last = NULL;
  if (state != NULL && strncmp(state, "22", 2) == 0) {
    first = strchr(primary, '"');
    last = strrchr(primary, '"');
  }
  if (first != NULL) {
    size_t head = (size_t)(first - primary);
    if (head > CLPD_DB_ERROR_MAX) head = CLPD_DB_ERROR_MAX;
    clpd_db_set_error(db, "%.*s\"?\"%s (SQLSTATE %s)", (int)head, primary,
                      last != first ? last + 1 : "", state);
  } else if (state != NULL && *state != '\0') {
    clpd_db_set_error(db, "%s (SQLSTATE %s)", primary, state);
  } else {
    clpd_db_set_error(db, "%s", primary);
  }
  return status_of(state);
}

/* Ends a COPY the statement started (unsupported: nothing is copied) and
 * drains the remaining results. Returns the final status. */
static clpd_db_status refuse_copy(clpd_db *db, PGresult *res) {
  PGconn *c = conn_of(db);
  ExecStatusType st = PQresultStatus(res);
  PGresult *next;
  char *buf;

  PQclear(res);
  if (st == PGRES_COPY_IN) {
    PQputCopyEnd(c, "COPY is not supported");
  } else if (st == PGRES_COPY_OUT) {
    buf = NULL;
    while (PQgetCopyData(c, &buf, 0) > 0) {
      PQfreemem(buf);
      buf = NULL;
    }
  }
  while ((next = PQgetResult(c)) != NULL) PQclear(next);
  if (PQstatus(c) == CONNECTION_BAD) {
    set_conn_error(db, "connection to the database lost");
    return CLPD_DB_LOST;
  }
  clpd_db_set_error(db, "COPY is not supported");
  return CLPD_DB_ERROR;
}

/* Runs a statement without parameters or rows (session setup and
 * transaction control); `tag` receives the command tag when not NULL. */
static clpd_db_status simple(clpd_db *db, const char *sql, char *tag,
                             size_t tag_size) {
  PGresult *res = PQexec(conn_of(db), sql);
  clpd_db_status st = CLPD_DB_OK;
  if (PQresultStatus(res) != PGRES_COMMAND_OK)
    st = fail_result(db, res, "statement failed");
  else if (tag != NULL)
    snprintf(tag, tag_size, "%s", PQcmdStatus(res));
  PQclear(res);
  return st;
}

/* Sets a timeout setting to `value` when the session has none (0), so a
 * value from options=-c ..., role or database settings is kept. */
static clpd_db_status default_timeout(clpd_db *db, const char *name,
                                      const char *value) {
  char sql[64];
  PGresult *res;
  clpd_db_status st;
  int zero;

  snprintf(sql, sizeof sql, "SHOW %s", name);
  res = PQexec(conn_of(db), sql);
  if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) != 1 ||
      PQnfields(res) != 1) {
    st = fail_result(db, res, "cannot read the session timeouts");
    PQclear(res);
    return st;
  }
  zero = strcmp(PQgetvalue(res, 0, 0), "0") == 0;
  PQclear(res);
  if (!zero) return CLPD_DB_OK;
  snprintf(sql, sizeof sql, "SET %s = '%s'", name, value);
  return simple(db, sql, NULL, 0);
}

static clpd_db_status pg_open(clpd_db *db, const char *target) {
  /* The target is expanded over the earlier keywords (expand_dbname), so
   * its own connect_timeout overrides the default; PGCONNECT_TIMEOUT only
   * applies to an unset option, so the default is left out when it is
   * set. */
  static const char *const keys[] = {"connect_timeout", "dbname", NULL};
  const char *values[] = {PG_CONNECT_TIMEOUT, NULL, NULL};
  const char *env = getenv("PGCONNECT_TIMEOUT");
  int skip = env != NULL && *env != '\0';
  PGconn *c;
  clpd_db_status st;

  values[1] = target;
  c = PQconnectdbParams(keys + skip, values + skip, 1);
  if (c == NULL) {
    clpd_db_set_error(db, "cannot connect to PostgreSQL: out of memory");
    return CLPD_DB_ERROR;
  }
  db->impl = c;
  if (PQstatus(c) != CONNECTION_OK) {
    set_conn_error(db, "cannot connect to PostgreSQL");
    PQfinish(c);
    db->impl = NULL;
    return CLPD_DB_ERROR;
  }
  PQsetNoticeProcessor(c, ignore_notice, NULL);
  if (PQsetClientEncoding(c, "UTF8") != 0) {
    set_conn_error(db, "cannot set the client encoding to UTF8");
    PQfinish(c);
    db->impl = NULL;
    return CLPD_DB_ERROR;
  }
  st = simple(db, "SET standard_conforming_strings = on", NULL, 0);
  if (st == CLPD_DB_OK)
    st = simple(db, "SET extra_float_digits = 3", NULL, 0);
  if (st == CLPD_DB_OK)
    st = default_timeout(db, "lock_timeout", PG_LOCK_TIMEOUT);
  if (st == CLPD_DB_OK)
    st = default_timeout(db, "statement_timeout", PG_STATEMENT_TIMEOUT);
  if (st != CLPD_DB_OK) {
    PQfinish(c);
    db->impl = NULL;
    return CLPD_DB_ERROR;
  }
  return CLPD_DB_OK;
}

static void pg_close(clpd_db *db) {
  if (db->impl != NULL) PQfinish(conn_of(db));
  db->impl = NULL;
}

/* Text form of a double that float8in reads back exactly. */
static void format_double(char *buf, size_t n, double x) {
  if (isinf(x))
    snprintf(buf, n, "%s", x > 0 ? "Infinity" : "-Infinity");
  else
    snprintf(buf, n, "%.17g", x);
}

static void store_value(clpd_db_result *result, size_t row, size_t col,
                        Oid type, const char *text, int len) {
  clpd_db_value v;
  unsigned char *bytes;
  size_t size = 0;

  memset(&v, 0, sizeof v);
  switch (type) {
  case OID_INT2:
  case OID_INT4:
  case OID_INT8:
    v.type = CLPD_DB_INTEGER;
    v.integer = strtoll(text, NULL, 10);
    break;
  case OID_FLOAT4:
  case OID_FLOAT8:
    v.type = CLPD_DB_FLOAT;
    v.real = strtod(text, NULL);
    break;
  case OID_BOOL:
    v.type = CLPD_DB_INTEGER;
    v.integer = text[0] == 't';
    break;
  case OID_BYTEA:
    /* NULL is out of memory (the text is the server's valid bytea
     * output); libpq leaves errno unspecified, so none is shown. */
    bytes = PQunescapeBytea((const unsigned char *)text, &size);
    if (bytes == NULL) clpd_db_die_oom();
    v.type = CLPD_DB_BLOB;
    v.data = bytes;
    v.size = size;
    clpd_db_result_set(result, row, col, &v);
    PQfreemem(bytes);
    return;
  default:
    v.type = CLPD_DB_TEXT;
    v.data = text;
    v.size = (size_t)len;
    break;
  }
  clpd_db_result_set(result, row, col, &v);
}

static void fill_result(clpd_db_result *result, const PGresult *res) {
  int cols = PQnfields(res), rows = PQntuples(res), i, j;
  const char *tag = PQcmdStatus((PGresult *)res);

  clpd_db_result_init_columns(result, (size_t)cols);
  for (j = 0; j < cols; j++) {
    const char *name = PQfname(res, j);
    clpd_db_result_set_name(result, (size_t)j, name != NULL ? name : "");
  }
  for (i = 0; i < rows; i++) {
    size_t row = clpd_db_result_add_row(result);
    for (j = 0; j < cols; j++) {
      if (PQgetisnull(res, i, j)) continue; /* rows start NULL */
      store_value(result, row, (size_t)j, PQftype(res, j),
                  PQgetvalue(res, i, j), PQgetlength(res, i, j));
    }
  }
  if (strncmp(tag, "INSERT ", 7) == 0 || strncmp(tag, "UPDATE ", 7) == 0 ||
      strncmp(tag, "DELETE ", 7) == 0)
    clpd_db_result_set_affected(result,
                                strtoll(PQcmdTuples((PGresult *)res), NULL,
                                        10));
}

static clpd_db_status pg_exec(clpd_db *db, const char *sql,
                              const clpd_db_value *params, size_t n,
                              clpd_db_result *result) {
  enum { SLOT = 32 }; /* fits any int64 and %.17g double */
  Oid *types;
  const char **values;
  int *lengths, *formats;
  char *numbers, **owned;
  PGresult *res;
  ExecStatusType es;
  clpd_db_status st;
  size_t i;

  if (n > PG_MAX_PARAMS) {
    clpd_db_set_error(db, "too many parameters (%lu, at most %d)",
                      (unsigned long)n, PG_MAX_PARAMS);
    return CLPD_DB_ERROR;
  }
  types = xmalloc(n * sizeof *types);
  values = xmalloc(n * sizeof *values);
  lengths = xmalloc(n * sizeof *lengths);
  formats = xmalloc(n * sizeof *formats);
  numbers = xmalloc(n * SLOT);
  owned = xmalloc(n * sizeof *owned);
  for (i = 0; i < n; i++) {
    const clpd_db_value *v = &params[i];
    char *slot = numbers + i * SLOT;
    types[i] = 0;
    values[i] = NULL;
    lengths[i] = 0;
    formats[i] = 0;
    owned[i] = NULL;
    switch (v->type) {
    case CLPD_DB_INTEGER:
      types[i] = OID_INT8;
      snprintf(slot, SLOT, "%lld", (long long)v->integer);
      values[i] = slot;
      break;
    case CLPD_DB_FLOAT:
      types[i] = OID_FLOAT8;
      format_double(slot, SLOT, v->real);
      values[i] = slot;
      break;
    case CLPD_DB_TEXT: /* text format reads a C string */
      owned[i] = xmalloc(v->size + 1);
      if (v->size != 0) memcpy(owned[i], v->data, v->size);
      owned[i][v->size] = '\0';
      values[i] = owned[i];
      break;
    case CLPD_DB_BLOB: /* a NULL pointer would be SQL NULL */
      types[i] = OID_BYTEA;
      values[i] = v->size != 0 ? (const char *)v->data : "";
      lengths[i] = (int)v->size;
      formats[i] = 1;
      break;
    default: /* CLPD_DB_NULL */
      break;
    }
  }
  res = PQexecParams(conn_of(db), sql, (int)n, types, values, lengths,
                     formats, 0);
  for (i = 0; i < n; i++) free(owned[i]);
  free(owned);
  free(numbers);
  free(formats);
  free(lengths);
  free(values);
  free(types);

  es = PQresultStatus(res);
  if (es == PGRES_TUPLES_OK || es == PGRES_COMMAND_OK) {
    fill_result(result, res);
    st = CLPD_DB_OK;
  } else if (es == PGRES_COPY_IN || es == PGRES_COPY_OUT ||
             es == PGRES_COPY_BOTH) {
    return refuse_copy(db, res);
  } else {
    st = fail_result(db, res, "statement failed");
  }
  PQclear(res);
  return st;
}

static clpd_db_status pg_begin(clpd_db *db) {
  return simple(db, "BEGIN", NULL, 0);
}

static clpd_db_status pg_commit(clpd_db *db) {
  char tag[64] = "";
  clpd_db_status st = simple(db, "COMMIT", tag, sizeof tag);
  if (st == CLPD_DB_OK && strcmp(tag, "COMMIT") != 0) {
    clpd_db_set_error(db, "the transaction was aborted by an earlier error "
                          "and rolled back");
    return CLPD_DB_ERROR;
  }
  return st;
}

static clpd_db_status pg_rollback(clpd_db *db) {
  return simple(db, "ROLLBACK", NULL, 0);
}

const clpd_db_backend_ops clpd_db_pg_ops = {
    "postgresql", pg_open, pg_close, pg_exec, pg_begin, pg_commit, pg_rollback,
};
