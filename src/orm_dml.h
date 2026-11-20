/* ORM DML functions (T10d, FR-24, docs/orm.md#dml-functions): db-insert,
 * db-update and db-delete on the templates mapped with sync = manual,
 * db-load and db-count on any mapped template. Registered with the DDL
 * functions (clpd_orm_ddl_register, src/orm_ddl.h) only when a database
 * is configured, and with the same caller scope: only the actions of a
 * rule of a loaded package, only the templates of that package's
 * definition (T9d, L84). Every statement is one parameterized
 * clpd_db_exec on the daemon's connection, named from the mapping only
 * (quoted identifiers; on PostgreSQL the table qualified with
 * current_schema()), each value typed per its column's mapping (the
 * synchronization's write rules, clpd_orm_sync_param; nil in a null nil
 * column is NULL, IS NULL in a filter); every parameter sits where its
 * column types it (a value, a comparison with a column), never in a
 * typeless context. A rule calls them inside the cycle transaction
 * (T10a), so a database error rolls the cycle back; the instances they
 * write count as touched by the cycle (clpd_orm_sync_touch). db-load
 * asserts through the loader (src/orm_load.h) with the synchronization
 * suspended (echo suppression). */
#ifndef CLPD_ORM_DML_H
#define CLPD_ORM_DML_H

#include "clips.h"
#include "orm_ddl.h"

/* Defines db-insert, db-update, db-delete, db-load and db-count in env
 * with ddl as their context (ddl->sync may be set later, before the
 * first rule runs). Returns 0, or -1 when CLIPS refuses a definition. */
int clpd_orm_dml_register(Environment *env, clpd_orm_ddl *ddl);

#endif
