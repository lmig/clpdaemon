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
/* The poison retry (T10e): after n rolled-back cycles in a row attributed
 * to one instance (see clpd_orm_sync_end_cycle) it is isolated; n >= 1,
 * CLPD_ORM_SYNC_ISOLATE_AFTER after the attach. NULL is accepted. */
#define CLPD_ORM_SYNC_ISOLATE_AFTER 3
void clpd_orm_sync_set_isolate_after(clpd_orm_syncer *s, int n);
/* Idle eviction (T11c, FR-9, docs/orm.md#idle-eviction): the idle period
 * in milliseconds (ms >= 1; CLPD_ORM_SYNC_IDLE_AFTER_MS after the attach,
 * the default of idle_eviction_after) and the monotonic clock the
 * activity times come from (NULL: CLOCK_MONOTONIC; tests replace it).
 * Set both before the first fact is asserted. NULL s is accepted. */
#define CLPD_ORM_SYNC_IDLE_AFTER_MS (30LL * 60 * 1000)
typedef long long clpd_orm_sync_clock(void *ctx);
void clpd_orm_sync_set_idle_after(clpd_orm_syncer *s, long long ms);
void clpd_orm_sync_set_clock(clpd_orm_syncer *s, clpd_orm_sync_clock *clock,
                             void *ctx);
/* Removes the hooks and frees s. NULL is accepted. */
void clpd_orm_sync_detach(clpd_orm_syncer *s);

/* Loading mode: between suspend and resume (they nest) nothing is
 * recorded. NULL is accepted. */
void clpd_orm_sync_suspend(clpd_orm_syncer *s);
void clpd_orm_sync_resume(clpd_orm_syncer *s);
int clpd_orm_sync_suspended(const clpd_orm_syncer *s);

/* The DML functions (T10d, src/orm_dml.h) wrote rows of instance wf (the
 * text of its wf-id value) of a template of pkg: while a cycle is open the
 * instance counts as touched, so a rollback evicts and reloads it like an
 * instance whose facts changed. NULL is accepted. */
void clpd_orm_sync_touch(clpd_orm_syncer *s, const char *wf,
                         const clpd_package *pkg);

/* The lifecycle functions (T11a, src/orm_life.h), from a rule inside the
 * open cycle transaction.
 *
 * clpd_orm_sync_flush_instance writes the recorded changes of instance wf
 * now (the flush of clpd_orm_sync_end_cycle restricted to the entries
 * whose last committed or current row belongs to wf: its DELETEs,
 * UPDATEs, INSERTs and workflow_instance row, same statements, same order,
 * same failure lines), then forgets them and notes the instances written
 * as touched by the cycle (a rollback evicts and reloads them). Returns 0
 * (also with nothing to write, or s NULL), or -1 when nothing more may be
 * written in the cycle (a statement of it failed already, or a value
 * could not be written before) or the flush failed: a refused write or a
 * value it cannot write logs its sync: line, the instance of that write
 * is isolated when the cycle ends, and the cycle is rolled back there (a
 * value through clpd_orm_tx_abort, as for the end-of-cycle flush).
 *
 * clpd_orm_sync_finish ends instance wf in memory: every fact carrying
 * its wf-id is retracted with the recording suspended (no DELETE, its
 * rows stay), the instance counts as touched by the open cycle, and from
 * now on no fact of it is recorded (never written). A rolled-back cycle
 * forgets that it ended (the rehydration reloads it); after a commit it
 * stays ended until the syncer is detached, and a rollback never reloads
 * it. NULL is accepted (nothing happens).
 *
 * clpd_orm_sync_isolated is 1 when wf is no longer synchronized (a write
 * that failed for good, the poison retry), else 0 (also for s NULL). */
int clpd_orm_sync_flush_instance(clpd_orm_syncer *s, const char *wf,
                                 FILE *log);
void clpd_orm_sync_finish(clpd_orm_syncer *s, const char *wf);
int clpd_orm_sync_isolated(const clpd_orm_syncer *s, const char *wf);

/* The parameter for the value v in column c, by the write rules of the
 * synchronization (docs/orm.md#write-rules; the DML functions use them
 * too): the symbol nil in a null nil column is NULL; otherwise v must have
 * the column's CLIPS type (a string and a symbol for either lexeme type),
 * a boolean column takes 0 or 1. TEXT and BLOB parameters point into v's
 * lexeme. Returns 0, or -1 with the reason in why:
 *   slot '<slot>' (column '<column>'): expected <clips-type>, got <TYPE>
 *   slot '<slot>' (column '<column>'): a boolean column takes 0 or 1 */
int clpd_orm_sync_param(const CLIPSValue *v, const clpd_orm_column *c,
                        clpd_db_value *out, char *why, size_t n);

/* The name messages give a CLIPS type code (INTEGER, FLOAT, SYMBOL,
 * STRING, INSTANCE-NAME, FACT-ADDRESS, INSTANCE-ADDRESS, EXTERNAL-ADDRESS,
 * MULTIFIELD, VOID). */
const char *clpd_orm_sync_type_name(unsigned short type);

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
 * A SELECT that lost the connection or failed with transient contention
 * (CLPD_DB_TRANSIENT, T10f) logs the same line, retracts what that
 * instance loaded and then logs the cycle transaction line
 * (clpd_orm_tx_fail): the cycle must not run, that instance and the ones
 * not reloaded yet stay pending for the next start, and nothing is
 * isolated. On PostgreSQL a failed SELECT current_schema() logs
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
 * src/orm_tx.h follows. When the refused write (neither CLPD_DB_LOST nor
 * CLPD_DB_TRANSIENT, T10f) or the value belongs to instance <wf-id>, its
 * changes are dropped, its facts are no longer recorded until the syncer
 * is detached, and
 *   clpdaemon: sync: instance '<wf-id>' is no longer synchronized: <reason>
 * follows. Then (T10c) every instance the cycle touched, and that one, is
 * evicted (see above); an isolated one logs
 *   clpdaemon: rehydrate: instance '<wf-id>' is not reloaded: it is no
 *   longer synchronized
 * instead of waiting for a reload. Poison retry (T10e,
 * docs/database.md#poison-retry): a rolled-back cycle whose first failure
 * came from a statement of a rule is attributed to the rule's instance
 * (the wf-id of its first basis fact that has one; none: not attributed),
 * unless that failure was CLPD_DB_LOST or CLPD_DB_TRANSIENT (T10f: such a
 * cycle is retried, counted for nobody, and resets no count); an instance
 * with isolate_after attributed cycles since the last commit is isolated
 * and evicted the same way, its line's <reason> being
 * "<message> (<n> rolled-back cycles in a row)" (a long <message> is cut,
 * never the suffix). A commit resets the counts. Every <wf-id> is shown
 * with control characters as '?', at most 200 bytes. Returns 0 when the
 * cycle committed (the changes are forgotten), -1 when it was rolled back
 * (the changes of the instances it did not touch are kept). s may be
 * NULL. */
int clpd_orm_sync_end_cycle(clpd_orm_syncer *s, clpd_orm_tx *tx, FILE *log);

/* The idle eviction (T11c), at the start of a cycle, before
 * clpd_orm_sync_begin_cycle (outside the cycle transaction; it sends no
 * statement). The syncer keeps the time of the last activity of each
 * instance: an assert, modify or retract of a fact carrying its wf-id
 * (any template; facts loaded with the recording suspended, by db-load or
 * the rehydration, included) and the firing of a rule whose rule wf-id
 * (see clpd_orm_sync_end_cycle) is the instance. An instance idle for the
 * idle period is evicted when every fact carrying its wf-id is of a
 * template mapped with sync = auto, it has no recorded change waiting to
 * be written, and it is neither isolated nor waiting for a reload: its
 * facts are retracted with the recording suspended (nothing is written,
 * its rows stay), its activity is forgotten, and
 *   clpdaemon: evict: instance '<wf-id>' after <n> s without activity
 * is logged (<n> whole seconds since its last activity; <wf-id> shown
 * like the rehydrate: lines), oldest activity first. An idle instance
 * that does not qualify is checked again one idle period later; one with
 * no fact left is forgotten without a line. Instances that a lifecycle
 * function ended or that were isolated have no activity kept. Returns the
 * number of instances evicted; 0 for s NULL (no database) or while a
 * cycle is open. */
int clpd_orm_sync_evict_idle(clpd_orm_syncer *s, FILE *log);

/* The on-demand rehydration (T11d, FR-9;
 * docs/orm.md#on-demand-rehydration): the admission of the ingest drain
 * (src/ingest.h clpd_ingest_admit_fn, whose CLPD_INGEST_* values it
 * returns), inside the open cycle transaction. wf_id is the
 * value the wf-id slot of the fact about to be asserted will hold (NULL:
 * no wf-id slot). Returns CLPD_INGEST_ASSERT at once for s NULL, no cycle
 * open, a value that names no instance (src/orm_sync.c value_text), an
 * instance with a fact in working memory (a count the hooks keep, O(1))
 * or an isolated one (never reloaded). Otherwise reads the instance's
 * workflow_instance row:
 *   no row: CLPD_INGEST_ASSERT (a new instance);
 *   status completed or failed: logs
 *     clpdaemon: ingest: instance '<wf-id>' is <status>; fact dropped
 *   and returns CLPD_INGEST_DROP;
 *   any other status, the package definition@version of the row not
 *   loaded: logs
 *     clpdaemon: ingest: instance '<wf-id>' belongs to package
 *     <definition>@<version>, which is not loaded; fact dropped
 *   and returns CLPD_INGEST_DROP;
 *   otherwise loads the instance with clpd_orm_load_instance (that
 *   package, without echo) and returns CLPD_INGEST_ASSERT.
 * A failed statement logs
 *   clpdaemon: rehydrate: instance '<wf-id>': table 'workflow_instance':
 *   <message>
 * (the row) or the rehydrate: line of clpd_orm_sync_begin_cycle (a
 * SELECT of the load; what the instance loaded is retracted), or
 * "clpdaemon: rehydrate: <message>" (PostgreSQL current_schema()), and
 * returns CLPD_INGEST_STOP: the cycle can no longer commit, and
 * clpd_orm_sync_end_cycle rolls it back (its cycle transaction line
 * follows). A failure that is neither CLPD_DB_LOST nor CLPD_DB_TRANSIENT
 * (T10f) also isolates the instance (the isolation line of
 * clpd_orm_sync_end_cycle follows at once). <wf-id>, <definition> and
 * <version> are shown with control characters as '?', at most 200
 * bytes. */
int clpd_orm_sync_admit(clpd_orm_syncer *s, const CLIPSValue *wf_id,
                        FILE *log);

#endif
