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
 * updated_at set on every write). A committed cycle forgets the changes;
 * a rolled-back one keeps them for the retry, except those of an instance
 * whose write failed for good (clpd_orm_sync_end_cycle).
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
 * follows. Returns 0 when the cycle committed (the changes are forgotten),
 * -1 when it was rolled back (the others are kept). s may be NULL. */
int clpd_orm_sync_end_cycle(clpd_orm_syncer *s, clpd_orm_tx *tx, FILE *log);

#endif
