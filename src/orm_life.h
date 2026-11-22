/* Workflow lifecycle functions (T11a, FR-8, S10, D4, L107;
 * docs/orm.md#lifecycle-functions): (wf-complete ?wf-id) and
 * (wf-fail ?wf-id ?reason) end a workflow instance. Registered with the
 * DDL and DML functions (clpd_orm_ddl_register, src/orm_ddl.h) only when
 * a database is configured, with their caller scope: only the actions of
 * a rule of a loaded package (clpd_orm_ddl_caller); the instance's
 * workflow_instance row, when it has one, must belong to that rule's
 * definition.
 *
 * Inside the cycle transaction (T10a) a call: reads the instance's
 * workflow_instance row, writes the instance's pending synchronization
 * changes (clpd_orm_sync_flush_instance), writes the row with status
 * completed or failed, reason (NULL, or ?reason for wf-fail) and
 * updated_at (inserting it when the instance has none yet), then retracts
 * every fact carrying the instance's wf-id without writing a DELETE
 * (clpd_orm_sync_finish): its mapped rows stay as history. Facts asserted
 * for it afterwards are never written. A rolled-back cycle undoes all of
 * it (the rehydration of T10c reloads the instance). */
#ifndef CLPD_ORM_LIFE_H
#define CLPD_ORM_LIFE_H

#include "clips.h"
#include "db.h"
#include "orm_ddl.h"

/* Defines wf-complete and wf-fail in env with ddl as their context
 * (ddl->sync is set before the first rule runs). Returns 0, or -1 when
 * CLIPS refuses a definition. */
int clpd_orm_life_register(Environment *env, clpd_orm_ddl *ddl);

/* The workflow_instance row of instance wf (the text of its wf-id) as the
 * open transaction sees it, read with
 *   SELECT "definition", "version", "status" FROM <schema>"workflow_instance"
 *     WHERE "wf_id" = $1
 * (schema: the quoted current schema and '.' on PostgreSQL, "" on
 * SQLite); the lifecycle functions and the on-demand rehydration (T11d,
 * src/orm_sync.h) use it. Returns 1 with *row filled (TEXT values, cut to
 * the buffers; "" for any other type), 0 when there is no row, or -1 when
 * the SELECT failed (its message in clpd_db_error(db)); *st is the
 * status of the SELECT. */
typedef struct {
  char definition[1024], version[1024], status[64];
} clpd_orm_life_row;
int clpd_orm_life_read_row(clpd_db *db, const char *schema, const char *wf,
                           clpd_orm_life_row *row, clpd_db_status *st);

#endif
