/* Cycle transaction and the workflow_instance table (T10a, src/orm_tx.h,
 * docs/database.md#cycle-transaction). */
#include "orm_tx.h"

#include "orm.h"

#include <stdio.h>
#include <string.h>

void clpd_orm_tx_init(clpd_orm_tx *tx, clpd_db *db, const char *spec,
                      int initial_ms, int max_ms) {
  tx->db = db;
  tx->spec = spec;
  tx->initial_ms = initial_ms;
  tx->max_ms = max_ms;
  tx->retry_ms = 0;
  tx->reconnect = 0;
}

int clpd_orm_tx_next_delay(int prev, int initial_ms, int max_ms) {
  long long next = prev <= 0 ? initial_ms : 2LL * prev;
  return next > max_ms ? max_ms : (int)next;
}

/* A failed attempt: the next wait, one log line, and a reconnect before
 * the next begin when the connection was lost. Returns -1. */
static int failed(clpd_orm_tx *tx, FILE *log, clpd_db_status st,
                  const char *message) {
  if (st == CLPD_DB_LOST) tx->reconnect = 1;
  tx->retry_ms = clpd_orm_tx_next_delay(tx->retry_ms, tx->initial_ms,
                                        tx->max_ms);
  fprintf(log, "clpdaemon: cycle transaction failed: %s; retrying in %d ms\n",
          message, tx->retry_ms);
  fflush(log);
  return -1;
}

int clpd_orm_tx_fail(clpd_orm_tx *tx, FILE *log, clpd_db_status st,
                     const char *message) {
  if (tx->db == NULL) return 0;
  return failed(tx, log, st, message);
}

int clpd_orm_tx_connect(clpd_orm_tx *tx, FILE *log) {
  if (tx->db == NULL) return 0;
  if (tx->reconnect) {
    /* The message never holds the spec (src/db.h). */
    char msg[512];
    if (clpd_db_reconnect(tx->db, tx->spec, msg, sizeof msg) != CLPD_DB_OK)
      return failed(tx, log, CLPD_DB_LOST, msg);
    tx->reconnect = 0;
  }
  return 0;
}

int clpd_orm_tx_begin(clpd_orm_tx *tx, FILE *log) {
  clpd_db_status st;
  if (tx->db == NULL) return 0;
  if (clpd_orm_tx_connect(tx, log) != 0) return -1;
  st = clpd_db_begin_atomic(tx->db);
  if (st != CLPD_DB_OK) return failed(tx, log, st, clpd_db_error(tx->db));
  return 0;
}

int clpd_orm_tx_abort(clpd_orm_tx *tx, FILE *log, const char *message) {
  clpd_db_status st;
  if (tx->db == NULL) return 0;
  st = clpd_db_rollback(tx->db);
  return failed(tx, log, st == CLPD_DB_LOST ? CLPD_DB_LOST : CLPD_DB_ERROR,
                message);
}

int clpd_orm_tx_end(clpd_orm_tx *tx, FILE *log) {
  clpd_db_status st;
  if (tx->db == NULL) return 0;
  /* The status and message of the cycle's first failing statement, or of
   * the commit itself; either way the transaction is over. */
  st = clpd_db_commit(tx->db);
  if (st != CLPD_DB_OK) return failed(tx, log, st, clpd_db_error(tx->db));
  tx->retry_ms = 0;
  return 0;
}

/* Column definitions after the quoted name, per backend: PostgreSQL
 * TIMESTAMPTZ columns are TEXT on SQLite, like the ORM's own SQLite types
 * (docs/orm.md#sql-types). */
#define INSTANCE_COLUMNS(ts)                                                \
  " (\"wf_id\" TEXT NOT NULL, \"definition\" TEXT NOT NULL, \"version\" "  \
  "TEXT NOT NULL, \"status\" TEXT NOT NULL, \"parent_wf_id\" TEXT, "       \
  "\"owner_node\" TEXT, \"lease_until\" " ts ", \"created_at\" " ts        \
  " NOT NULL, \"updated_at\" " ts " NOT NULL, PRIMARY KEY (\"wf_id\"))"

/* s quoted, any '"' doubled, into out, which holds 2 * strlen(s) + 3
 * bytes. */
static void quote(const char *s, char *out) {
  *out++ = '"';
  for (; *s != '\0'; s++) {
    if (*s == '"') *out++ = '"';
    *out++ = *s;
  }
  *out++ = '"';
  *out = '\0';
}

int clpd_orm_tx_current_schema(clpd_db *db, char *out, char *msg,
                               size_t n) {
  clpd_db_status st;
  return clpd_orm_tx_schema_status(db, out, msg, n, &st);
}

int clpd_orm_tx_schema_status(clpd_db *db, char *out, char *msg, size_t n,
                              clpd_db_status *st) {
  clpd_db_result *r = NULL;
  const clpd_db_value *v;
  *st = clpd_db_exec(db, "SELECT current_schema()", NULL, 0, &r);
  if (*st != CLPD_DB_OK) {
    snprintf(msg, n, "%s", clpd_db_error(db));
    return -1;
  }
  *st = CLPD_DB_ERROR; /* the failures below */
  v = clpd_db_result_rows(r) == 1 ? clpd_db_result_value(r, 0, 0) : NULL;
  if (v == NULL || v->type != CLPD_DB_TEXT) {
    clpd_db_result_free(r);
    snprintf(msg, n, "no current schema (the search_path names no existing "
                     "schema)");
    return -1;
  }
  /* PostgreSQL names have at most NAMEDATALEN - 1 bytes; out holds that
   * many with every byte a doubled quote. */
  if (v->size > CLPD_ORM_IDENT_MAX) {
    clpd_db_result_free(r);
    snprintf(msg, n, "the current schema name is too long");
    return -1;
  }
  quote(v->data, out);
  clpd_db_result_free(r);
  *st = CLPD_DB_OK;
  return 0;
}

int clpd_orm_tx_create_instance_table(clpd_db *db, char *msg, size_t n) {
  char sql[1024], schema[CLPD_ORM_TX_SCHEMA_SIZE];
  snprintf(msg, n, "%s", "");
  if (strcmp(clpd_db_backend(db), "postgresql") == 0) {
    /* The schema db-check-schema and the DDL functions use, so a table of
     * that name in a later search_path schema is never the one kept. */
    if (clpd_orm_tx_current_schema(db, schema, msg, n) != 0) return -1;
    snprintf(sql, sizeof sql,
             "CREATE TABLE IF NOT EXISTS %s.\"" CLPD_ORM_INSTANCE_TABLE
             "\"" INSTANCE_COLUMNS("TIMESTAMPTZ"), schema);
  } else {
    snprintf(sql, sizeof sql,
             "CREATE TABLE IF NOT EXISTS \"" CLPD_ORM_INSTANCE_TABLE
             "\"" INSTANCE_COLUMNS("TEXT"));
  }
  if (clpd_db_exec(db, sql, NULL, 0, NULL) != CLPD_DB_OK) {
    snprintf(msg, n, "%s", clpd_db_error(db));
    return -1;
  }
  return 0;
}
