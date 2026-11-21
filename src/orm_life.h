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
#include "orm_ddl.h"

/* Defines wf-complete and wf-fail in env with ddl as their context
 * (ddl->sync is set before the first rule runs). Returns 0, or -1 when
 * CLIPS refuses a definition. */
int clpd_orm_life_register(Environment *env, clpd_orm_ddl *ddl);

#endif
