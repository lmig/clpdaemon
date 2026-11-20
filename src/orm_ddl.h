/* ORM DDL functions (T9b, docs/orm.md#ddl-functions): db-create-table,
 * db-check-schema, db-drop-table and the sequence functions
 * db-create-sequence and db-nextval (T9c), registered by the core only
 * when a database is configured. Every SQL text comes from the mapping
 * (quoted identifiers, per-backend type names); each statement runs
 * through clpd_db_exec on the daemon's connection, never BEGIN/COMMIT,
 * never ALTER: from a rule, inside the cycle transaction (T10a), so a
 * rolled-back cycle undoes it; elsewhere (db-check-schema at load) in
 * autocommit.
 * db-create-table, db-drop-table, db-create-sequence and db-nextval act
 * only from the actions of a rule of a loaded package, on the templates
 * and sequence aliases of that package's definition (T9d, L84); on
 * PostgreSQL they name the table or sequence in current_schema(). */
#ifndef CLPD_ORM_DDL_H
#define CLPD_ORM_DDL_H

#include "clips.h"
#include "db.h"
#include "orm_sync.h"
#include "packages.h"

/* Context of the functions, owned by the caller and alive as long as the
 * environment. packages may still be loading when it is registered: the
 * functions see the mapped templates of the packages whose every mapping
 * check passed (orm_templates set) at the time of the call. */
typedef struct {
  clpd_db *db;
  const clpd_packages *packages;
  int allow_drop; /* orm_allow_drop = yes */
  /* The DML functions' echo suppression and touched instances (T10d,
   * src/orm_dml.h): NULL until the caller attaches the synchronization
   * and sets it. */
  clpd_orm_syncer *sync;
} clpd_orm_ddl;

/* Defines the five functions, and the five DML functions of src/orm_dml.h
 * (T10d), in env with ddl as their context. Returns 0, or -1 when CLIPS
 * refuses a definition (for example a name already defined). */
int clpd_orm_ddl_register(Environment *env, clpd_orm_ddl *ddl);

/* For the DML functions (src/orm_dml.c), which share the DDL functions'
 * caller scope and messages. */
#define CLPD_ORM_DDL_NAME_SIZE 201 /* an argument as messages show it */

/* Resolves the first argument of the calling function (its context a
 * clpd_orm_ddl) as a template, with the caller scope of db-create-table:
 * only from the actions of a rule of a loaded package, only a template of
 * that package's definition. Returns 1 with *pkg, *t and name (the
 * argument as messages show it: control characters as '?', at most 200
 * bytes) set, or 0 with the error logged, the CLIPS error thrown and
 * FALSE in ret. */
int clpd_orm_ddl_resolve_template(UDFContext *ctx, UDFValue *ret,
                                  const clpd_package **pkg,
                                  const clpd_orm_template **t,
                                  char name[CLPD_ORM_DDL_NAME_SIZE]);

/* Logs "clpdaemon: <udf>: template '<name>': table '<table>'<message>"
 * (t NULL: "clpdaemon: <udf>: <message>") on the daemon log stream, fmt
 * giving <message>; throws the CLIPS error and sets ret to FALSE. */
void clpd_orm_ddl_fail(UDFContext *ctx, UDFValue *ret, const char *name,
                       const clpd_orm_template *t, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

#endif
