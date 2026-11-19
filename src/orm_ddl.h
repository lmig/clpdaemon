/* ORM DDL functions (T9b, docs/orm.md#ddl-functions): db-create-table,
 * db-check-schema, db-drop-table and the sequence functions
 * db-create-sequence and db-nextval (T9c), registered by the core only
 * when a database is configured. Every SQL text comes from the mapping
 * (quoted identifiers, per-backend type names); each statement runs in
 * autocommit through clpd_db_exec, never BEGIN/COMMIT, never ALTER.
 * db-create-table, db-drop-table, db-create-sequence and db-nextval act
 * only from the actions of a rule of a loaded package, on the templates
 * and sequence aliases of that package's definition (T9d, L84); on
 * PostgreSQL they name the table or sequence in current_schema(). */
#ifndef CLPD_ORM_DDL_H
#define CLPD_ORM_DDL_H

#include "clips.h"
#include "db.h"
#include "packages.h"

/* Context of the functions, owned by the caller and alive as long as the
 * environment. packages may still be loading when it is registered: the
 * functions see the mapped templates of the packages whose every mapping
 * check passed (orm_templates set) at the time of the call. */
typedef struct {
  clpd_db *db;
  const clpd_packages *packages;
  int allow_drop; /* orm_allow_drop = yes */
} clpd_orm_ddl;

/* Defines the five functions in env with ddl as their context. Returns 0,
 * or -1 when CLIPS refuses a definition (for example a name already
 * defined). */
int clpd_orm_ddl_register(Environment *env, clpd_orm_ddl *ddl);

#endif
