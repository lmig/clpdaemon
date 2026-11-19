/* ORM DDL functions (T9b, docs/orm.md#ddl-functions): db-create-table,
 * db-check-schema and db-drop-table, registered by the core only when a
 * database is configured. Every SQL text comes from the mapping (quoted
 * identifiers, per-backend type names); each call is one autocommit
 * statement through clpd_db_exec, never BEGIN/COMMIT, never ALTER. */
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

/* Defines the three functions in env with ddl as their context. Returns 0,
 * or -1 when CLIPS refuses a definition (for example a name already
 * defined). */
int clpd_orm_ddl_register(Environment *env, clpd_orm_ddl *ddl);

#endif
