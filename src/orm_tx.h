/* The cycle transaction (T10a, FR-26, D7, D8, NFR-3;
 * docs/database.md#cycle-transaction): with a database, each cycle runs
 * inside one all-or-nothing transaction (clpd_db_begin_atomic) opened
 * before the ingest drain and committed after Run; the first failing
 * statement, or a failed commit, rolls the cycle back and the next cycle
 * waits for an exponential backoff (cycle_retry_initial_ms doubled up to
 * cycle_retry_max_ms, indefinitely; reset by a committed cycle); after
 * CLPD_DB_LOST the connection is reopened with the same spec before the
 * next attempt. The wait itself belongs to the event loop
 * (clpd_loop_defer), so signals and the admin socket are served meanwhile.
 * Also the daemon's workflow_instance table (CLPD_ORM_INSTANCE_TABLE). */
#ifndef CLPD_ORM_TX_H
#define CLPD_ORM_TX_H

#include <stddef.h>
#include <stdio.h>

#include "db.h"
#include "orm.h"

typedef struct {
  clpd_db *db;      /* NULL: no database; begin and end do nothing */
  const char *spec; /* the database spec, for the reconnect; never shown */
  int initial_ms, max_ms;
  int retry_ms;  /* the wait after the last failure; 0 after a commit */
  int reconnect; /* a status was CLPD_DB_LOST: reconnect before begin */
} clpd_orm_tx;

/* db and spec stay owned by the caller and must outlive tx; 1 <= initial_ms
 * <= max_ms (the configuration enforces it). */
void clpd_orm_tx_init(clpd_orm_tx *tx, clpd_db *db, const char *spec,
                      int initial_ms, int max_ms);

/* Reconnects when the last failure lost the connection (nothing to do
 * otherwise). Returns 0 (also without a database), or -1 when the
 * reconnect failed: the line of clpd_orm_tx_begin is logged and retry_ms
 * set, as for a failed begin. The rehydration of src/orm_sync.h runs
 * between it and the begin. */
int clpd_orm_tx_connect(clpd_orm_tx *tx, FILE *log);

/* Opens the cycle transaction, reconnecting first when the last failure
 * lost the connection. Returns 0 (also without a database), or -1 when
 * the reconnect or the begin failed: the cycle must not run, the line
 *   clpdaemon: cycle transaction failed: <message>; retrying in <n> ms
 * is written to log and retry_ms is the wait (<n>) before the next try. */
int clpd_orm_tx_begin(clpd_orm_tx *tx, FILE *log);

/* Commits the cycle transaction. Returns 0 when it committed (retry_ms
 * back to 0; also without a database), or -1 when the cycle was rolled
 * back: a statement of the cycle failed (<message> is the first failure's)
 * or the commit itself did; the same line is logged and retry_ms set. */
int clpd_orm_tx_end(clpd_orm_tx *tx, FILE *log);

/* A failed attempt outside the cycle transaction (a rehydration whose
 * SELECT lost the connection, src/orm_sync.h): the same line as a failed
 * begin with message as <message>, the same wait, and a reconnect before
 * the next attempt when st is CLPD_DB_LOST. Returns -1 (0 without a
 * database). */
int clpd_orm_tx_fail(clpd_orm_tx *tx, FILE *log, clpd_db_status st,
                     const char *message);

/* Rolls the open cycle transaction back for a reason that is no failed
 * statement (a value the synchronization cannot write, src/orm_sync.h):
 * the same line as a failure, with message as <message>, and the same
 * wait. Returns -1 (0 without a database). */
int clpd_orm_tx_abort(clpd_orm_tx *tx, FILE *log, const char *message);

/* The wait after a failure: initial_ms after a commit (prev 0), otherwise
 * prev doubled, never above max_ms. */
int clpd_orm_tx_next_delay(int prev, int initial_ms, int max_ms);

/* PostgreSQL: current_schema() quoted ("name", any '"' doubled) into out,
 * which holds CLPD_ORM_TX_SCHEMA_SIZE bytes; the schema the DDL functions,
 * workflow_instance and the synchronization name. Returns 0, or -1 with a
 * one-line message in msg (the query failed, no schema of the search_path
 * exists, or the name is too long). */
#define CLPD_ORM_TX_SCHEMA_SIZE (2 * CLPD_ORM_IDENT_MAX + 3)
int clpd_orm_tx_current_schema(clpd_db *db, char *out, char *msg, size_t n);
/* The same, with the status of the query in *st (CLPD_DB_ERROR for a
 * missing or too long schema name), so a caller can tell a lost
 * connection. */
int clpd_orm_tx_schema_status(clpd_db *db, char *out, char *msg, size_t n,
                              clpd_db_status *st);

/* CREATE TABLE IF NOT EXISTS for workflow_instance (one row per workflow
 * instance, written by the synchronization, src/orm_sync.h), in autocommit; on PostgreSQL qualified
 * with current_schema() like the DDL functions (docs/orm.md). Columns, in
 * order (PostgreSQL / SQLite types):
 *   wf_id TEXT NOT NULL (the primary key), definition TEXT NOT NULL,
 *   version TEXT NOT NULL, status TEXT NOT NULL, parent_wf_id TEXT,
 *   owner_node TEXT, lease_until TIMESTAMPTZ / TEXT,
 *   created_at TIMESTAMPTZ / TEXT NOT NULL,
 *   updated_at TIMESTAMPTZ / TEXT NOT NULL.
 * Returns 0, or -1 with a one-line message in msg. */
int clpd_orm_tx_create_instance_table(clpd_db *db, char *msg, size_t n);

#endif
