/* Automatic synchronization of mapped facts (T10b, FR-25, D7;
 * docs/orm.md#automatic-synchronization).
 *
 * Always-on assert and retract hooks (independent of the trace and of
 * watch) record the net change of every fact of a template mapped with
 * `sync = auto` by a loaded package, since the last committed cycle; a
 * modify is the retract and assert of one fact index (CLIPS 6.4.2
 * ReplaceFact keeps it), so it needs no hook of its own. At the end of the
 * cycle, inside the cycle transaction (src/orm_tx.h), the changes are
 * written by the primary key (wf_id column, then the local key), one
 * aggregate (workflow definition, in package order) after the other: its
 * DELETEs, UPDATEs and INSERTs, then the workflow_instance row of each
 * instance written (inserted on its first write with status active, its
 * updated_at set on every write). A committed cycle forgets the changes.
 *
 * Rehydration (T10c, FR-26; docs/orm.md#rehydration): while a cycle is
 * open the hooks also note the instance (wf-id) of every fact of any
 * template asserted or retracted (the ingest drain and modifies
 * included). A rolled-back cycle evicts those instances: every fact
 * carrying their wf-id is retracted, mapped or not, and their recorded
 * changes are dropped; the changes of the instances it did not touch are
 * kept for the retry. The next cycle's start (clpd_orm_sync_begin_cycle)
 * reloads each evicted instance from the database before its begin
 * (src/orm_load.h). An instance isolated by a write that failed for good
 * is evicted and never reloaded.
 *
 * Facts asserted or retracted while the recording is suspended (loading
 * from the database: db-load, rehydration; T10c, T10d) are not recorded,
 * and a suspended retract also drops what was recorded for that fact.
 * Engine thread only. */
#ifndef CLPD_ORM_SYNC_H
#define CLPD_ORM_SYNC_H

#include <stdio.h>

#include "clips.h"
#include "db.h"
#include "orm_tx.h"
#include "packages.h"

typedef struct clpd_orm_syncer clpd_orm_syncer;

/* Installs the hooks in env for the mapped sync = auto templates of the
 * loaded packages p (every mapping check passed), writing through db. p
 * and db stay owned by the caller and must outlive the syncer; attach once
 * per environment (CLIPS 6.4.2 accepts a second hook of the same name, and
 * both would record). Returns 0 with *out set, or -1 (nothing installed,
 * *out NULL) when CLIPS refuses a hook. */
int clpd_orm_sync_attach(Environment *env, clpd_db *db,
                         const clpd_packages *p, clpd_orm_syncer **out);
/* Removes the hooks and frees s. NULL is accepted. */
void clpd_orm_sync_detach(clpd_orm_syncer *s);

/* Loading mode: between suspend and resume (they nest) nothing is
 * recorded. NULL is accepted. */
void clpd_orm_sync_suspend(clpd_orm_syncer *s);
void clpd_orm_sync_resume(clpd_orm_syncer *s);
int clpd_orm_sync_suspended(const clpd_orm_syncer *s);

/* Forgets every recorded change (they will not be written). NULL is
 * accepted. */
void clpd_orm_sync_clear(clpd_orm_syncer *s);

/* The start of a cycle, in place of clpd_orm_tx_begin (T10c): when a
 * rolled-back cycle left instances to reload, reconnects if needed
 * (clpd_orm_tx_connect), retracts whatever carries their wf-id and loads
 * every mapped template of each package whose facts carried them
 * (clpd_orm_load_instance, outside the cycle transaction, without echo; a
 * rejected row logs its line), then opens the cycle transaction
 * (clpd_orm_tx_begin) and starts noting the instances the cycle touches.
 * A SELECT the database refuses logs
 *   clpdaemon: rehydrate: instance '<wf-id>': <pkg>::<template>:
 *   table '<table>': <message>
 * retracts what that instance loaded and isolates it (the line of
 * clpd_orm_sync_end_cycle follows); the others go on and the cycle runs.
 * A SELECT that lost the connection logs the same line and then the cycle
 * transaction line (clpd_orm_tx_fail): the cycle must not run, the
 * instances not reloaded yet stay pending for the next start. On
 * PostgreSQL a failed SELECT current_schema() logs
 * "clpdaemon: rehydrate: <message>" and fails the start the same way.
 * Returns 0 when the cycle may run, -1 when it must wait tx->retry_ms. s
 * may be NULL (only the begin). */
int clpd_orm_sync_begin_cycle(clpd_orm_syncer *s, clpd_orm_tx *tx, FILE *log);

/* The end of a cycle whose transaction tx opened (clpd_orm_tx_begin
 * returned 0): writes the recorded changes, unless a statement of the
 * cycle already failed, then commits with clpd_orm_tx_end. A write the
 * database refuses logs
 *   clpdaemon: sync: <pkg>::<template>: table '<table>': <message>
 * (or "clpdaemon: sync: table 'workflow_instance': <message>") and rolls
 * the cycle back; a value that cannot be written (docs/orm.md) logs the
 * same line with its reason instead of <message> and rolls the cycle back
 * through clpd_orm_tx_abort. Either way the cycle transaction line of
 * src/orm_tx.h follows. When the refused write (not CLPD_DB_LOST) or the
 * value belongs to instance <wf-id>, its changes are dropped, its facts are
 * no longer recorded until the syncer is detached, and
 *   clpdaemon: sync: instance '<wf-id>' is no longer synchronized: <reason>
 * follows. Then (T10c) every instance the cycle touched, and that one, is
 * evicted (see above); an isolated one logs
 *   clpdaemon: rehydrate: instance '<wf-id>' is not reloaded: it is no
 *   longer synchronized
 * instead of waiting for a reload. Returns 0 when the cycle committed (the
 * changes are forgotten), -1 when it was rolled back (the changes of the
 * instances it did not touch are kept). s may be NULL. */
int clpd_orm_sync_end_cycle(clpd_orm_syncer *s, clpd_orm_tx *tx, FILE *log);

#endif
