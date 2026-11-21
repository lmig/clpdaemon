/* Cycle transaction and the workflow_instance table (T10a, src/orm_tx.h,
 * docs/database.md#cycle-transaction). */
#define _POSIX_C_SOURCE 200809L

#include "orm_tx.h"

#include "orm.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

void clpd_orm_tx_init(clpd_orm_tx *tx, clpd_db *db, const char *spec,
                      int initial_ms, int max_ms) {
  tx->db = db;
  tx->spec = spec;
  tx->initial_ms = initial_ms;
  tx->max_ms = max_ms;
  tx->retry_ms = 0;
  tx->reconnect = 0;
  tx->failure = CLPD_DB_OK;
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
  tx->failure = st;
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
  tx->failure = CLPD_DB_OK;
  return 0;
}

/* The columns of workflow_instance, in order: the CREATE and the shape
 * check (T11a) both come from this table. PostgreSQL TIMESTAMPTZ columns
 * are TEXT on SQLite, like the ORM's own SQLite types
 * (docs/orm.md#sql-types); pg_udt is the type information_schema names. */
static const struct {
  const char *name, *pg, *pg_udt;
  int not_null;
} instance_columns[] = {
    {"wf_id", "TEXT", "text", 1},
    {"definition", "TEXT", "text", 1},
    {"version", "TEXT", "text", 1},
    {"status", "TEXT", "text", 1},
    {"reason", "TEXT", "text", 0},
    {"parent_wf_id", "TEXT", "text", 0},
    {"owner_node", "TEXT", "text", 0},
    {"lease_until", "TIMESTAMPTZ", "timestamptz", 0},
    {"created_at", "TIMESTAMPTZ", "timestamptz", 1},
    {"updated_at", "TIMESTAMPTZ", "timestamptz", 1},
};
#define INSTANCE_COLUMN_COUNT                                              \
  (sizeof instance_columns / sizeof instance_columns[0])

/* " (\"wf_id\" TEXT NOT NULL, ..., PRIMARY KEY (\"wf_id\"))" appended to
 * sql, which holds n bytes. */
static void put_columns(char *sql, size_t n, int pg) {
  size_t i, len = strlen(sql);
  for (i = 0; i < INSTANCE_COLUMN_COUNT && len < n; i++)
    len += (size_t)snprintf(sql + len, n - len, "%s\"%s\" %s%s",
                            i == 0 ? " (" : ", ", instance_columns[i].name,
                            pg ? instance_columns[i].pg : "TEXT",
                            instance_columns[i].not_null ? " NOT NULL" : "");
  if (len < n) snprintf(sql + len, n - len, ", PRIMARY KEY (\"wf_id\"))");
}

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
             "CREATE TABLE IF NOT EXISTS %s.\"" CLPD_ORM_INSTANCE_TABLE "\"",
             schema);
    put_columns(sql, sizeof sql, 1);
  } else {
    snprintf(sql, sizeof sql,
             "CREATE TABLE IF NOT EXISTS \"" CLPD_ORM_INSTANCE_TABLE "\"");
    put_columns(sql, sizeof sql, 0);
  }
  if (clpd_db_exec(db, sql, NULL, 0, NULL) != CLPD_DB_OK) {
    snprintf(msg, n, "%s", clpd_db_error(db));
    return -1;
  }
  return 0;
}


/* A catalog name or type for a message: control bytes as '?', at most 200
 * bytes. */
static void catalog_text(const clpd_db_value *v, char *out, size_t n) {
  size_t i, len = v != NULL && v->type == CLPD_DB_TEXT ? v->size : 0;
  if (len > 200) len = 200;
  if (len > n - 1) len = n - 1;
  for (i = 0; i < len; i++) {
    char c = ((const char *)v->data)[i];
    out[i] = (unsigned char)c < 0x20 || c == 0x7f ? '?' : c;
  }
  out[i] = '\0';
}

static int same_name(const clpd_db_value *v, const char *name, int pg) {
  if (v == NULL || v->type != CLPD_DB_TEXT) return 0;
  return pg ? strcmp(v->data, name) == 0 : strcasecmp(v->data, name) == 0;
}

int clpd_orm_tx_check_instance_table(clpd_db *db, char *msg, size_t n) {
  static const char *const pg_columns =
      "SELECT column_name, udt_name, CASE is_nullable WHEN 'NO' THEN 1 ELSE "
      "0 END, 0 FROM information_schema.columns WHERE table_schema = "
      "current_schema() AND table_name = $1 ORDER BY ordinal_position";
  static const char *const pg_key =
      "SELECT k.column_name FROM information_schema.table_constraints c JOIN "
      "information_schema.key_column_usage k ON k.constraint_schema = "
      "c.constraint_schema AND k.constraint_name = c.constraint_name AND "
      "k.table_name = c.table_name WHERE c.constraint_type = 'PRIMARY KEY' "
      "AND c.table_schema = current_schema() AND c.table_name = $1 ORDER BY "
      "k.ordinal_position";
  /* pk: the column's position in the primary key, 0 outside it. */
  static const char *const lite_columns =
      "SELECT name, type, \"notnull\", pk FROM pragma_table_info($1) ORDER "
      "BY cid";
  int pg = strcmp(clpd_db_backend(db), "postgresql") == 0, rc = 0;
  clpd_db_value table = clpd_db_text(CLPD_ORM_INSTANCE_TABLE);
  clpd_db_result *r = NULL, *k = NULL;
  char a[256], b[256], key[1024] = "";
  size_t i, j, rows, keys = 0;

  snprintf(msg, n, "%s", "");
  if (clpd_db_exec(db, pg ? pg_columns : lite_columns, &table, 1, &r) !=
          CLPD_DB_OK ||
      (pg && clpd_db_exec(db, pg_key, &table, 1, &k) != CLPD_DB_OK)) {
    snprintf(msg, n, "%s", clpd_db_error(db));
    clpd_db_result_free(r);
    return -1;
  }
  rows = clpd_db_result_rows(r);
  for (i = 0; rc == 0 && i < INSTANCE_COLUMN_COUNT; i++) {
    const char *want = pg ? instance_columns[i].pg_udt : "TEXT";
    const clpd_db_value *type, *nn;
    for (j = 0; j < rows; j++)
      if (same_name(clpd_db_result_value(r, j, 0), instance_columns[i].name,
                    pg))
        break;
    if (j == rows) {
      snprintf(msg, n, "missing column '%s'", instance_columns[i].name);
      rc = 1;
      break;
    }
    type = clpd_db_result_value(r, j, 1);
    nn = clpd_db_result_value(r, j, 2);
    if (type == NULL || type->type != CLPD_DB_TEXT ||
        (pg ? strcmp(type->data, want) : strcasecmp(type->data, want)) != 0) {
      catalog_text(type, a, sizeof a);
      snprintf(msg, n, "column '%s' has type '%s', expected %s",
               instance_columns[i].name, a,
               pg ? instance_columns[i].pg : "TEXT");
      rc = 1;
    } else if ((nn != NULL && nn->type == CLPD_DB_INTEGER && nn->integer) !=
               instance_columns[i].not_null) {
      snprintf(msg, n, "column '%s' is %s, expected %s",
               instance_columns[i].name,
               instance_columns[i].not_null ? "nullable" : "NOT NULL",
               instance_columns[i].not_null ? "NOT NULL" : "nullable");
      rc = 1;
    }
  }
  for (j = 0; rc == 0 && j < rows; j++) {
    const clpd_db_value *name = clpd_db_result_value(r, j, 0);
    for (i = 0; i < INSTANCE_COLUMN_COUNT; i++)
      if (same_name(name, instance_columns[i].name, pg)) break;
    if (i == INSTANCE_COLUMN_COUNT) {
      catalog_text(name, a, sizeof a);
      snprintf(msg, n, "unexpected column '%s'", a);
      rc = 1;
    }
  }
  if (rc == 0) {
    /* The key columns in key order: PostgreSQL from k, SQLite from pk. */
    size_t total = pg ? clpd_db_result_rows(k) : rows, pos;
    int only_wf = 1;
    for (pos = 1; pos <= (pg ? total : rows); pos++) {
      const clpd_db_value *name = NULL;
      if (pg) {
        name = clpd_db_result_value(k, pos - 1, 0);
      } else {
        for (j = 0; j < rows; j++) {
          const clpd_db_value *p = clpd_db_result_value(r, j, 3);
          if (p != NULL && p->type == CLPD_DB_INTEGER &&
              p->integer == (int64_t)pos)
            name = clpd_db_result_value(r, j, 0);
        }
        if (name == NULL) break;
      }
      catalog_text(name, b, sizeof b);
      snprintf(key + strlen(key), sizeof key - strlen(key), "%s%s",
               keys > 0 ? ", " : "", b);
      if (!same_name(name, "wf_id", pg)) only_wf = 0;
      keys++;
    }
    if (keys == 0) {
      snprintf(msg, n, "there is no primary key, expected (wf_id)");
      rc = 1;
    } else if (keys != 1 || !only_wf) {
      snprintf(msg, n, "the primary key is (%s), expected (wf_id)", key);
      rc = 1;
    }
  }
  clpd_db_result_free(r);
  clpd_db_result_free(k);
  return rc;
}
