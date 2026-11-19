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

/* The wait after a failure: initial_ms after a commit (prev 0), otherwise
 * prev doubled, never above max_ms. */
int clpd_orm_tx_next_delay(int prev, int initial_ms, int max_ms);

/* CREATE TABLE IF NOT EXISTS for workflow_instance (one row per workflow
 * instance, written from T10b on), in autocommit; on PostgreSQL qualified
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
