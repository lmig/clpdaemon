/* Loading stored rows into facts (T10c, FR-26; docs/orm.md#loading-rows):
 * the reusable loader of the rehydration after a rolled-back cycle
 * (src/orm_sync.h), and later of the on-demand rehydration (T11), the
 * failover (T22) and db-load (T10d).
 *
 * A row becomes one fact of its mapped template, every column through the
 * column's declared CLIPS type and null policy; the facts are asserted
 * with the automatic synchronization suspended (echo suppression), so they
 * are never written back. Engine thread only, never during Run's pattern
 * matching (CLIPS refuses an assert there). */
#ifndef CLPD_ORM_LOAD_H
#define CLPD_ORM_LOAD_H

#include <stddef.h>
#include <stdio.h>

#include "clips.h"
#include "db.h"
#include "orm_sync.h"
#include "packages.h"

/* One equality filter: column (an index into the template's columns)
 * equals value; a NULL value is IS NULL. TEXT and BLOB values point to
 * memory the caller keeps until the load returns. */
typedef struct {
  size_t column;
  clpd_db_value value;
} clpd_orm_filter;

/* Selects the rows of the mapped template tpl of pkg (an index into
 * pkg->orm->templates; the package loaded cleanly) that match every
 * filter (nf >= 1), in primary key order:
 *   SELECT "<column>", ... FROM <schema>"<table>"
 *     WHERE "<column>" = $1 AND ... ORDER BY "<wf_id>", "<key>", ...
 * (every mapped column in mapping order), schema being the quoted current
 * schema followed by '.' on PostgreSQL (clpd_orm_tx_current_schema) and ""
 * on SQLite. Then asserts one fact per row, with sync suspended (sync may
 * be NULL): a NULL column becomes the symbol nil (null policy nil) or the
 * slot's default (default); any other value is converted to the column's
 * CLIPS type (docs/orm.md#loading-rows). A row that cannot become a fact (a
 * NULL in a column of null policy error, a value of another type, a value
 * the slot's constraints refuse, a slot without default) is not asserted
 * and one line is logged:
 *   clpdaemon: load: <pkg>::<template>: table '<table>': instance '<wf-id>':
 *   row rejected: <reason>
 * ("instance '<wf-id>': " left out when the row's wf_id is NULL; control
 * characters of <wf-id> shown as '?', at most 200 bytes). Returns
 * CLPD_DB_OK with *loaded the facts asserted, or the status of the failed
 * SELECT (its message in clpd_db_error(db)) with nothing asserted. */
clpd_db_status clpd_orm_load(Environment *env, clpd_db *db,
                             clpd_orm_syncer *sync, const clpd_package *pkg,
                             size_t tpl, const char *schema,
                             const clpd_orm_filter *filters, size_t nf,
                             FILE *log, size_t *loaded);

/* Every mapped template of pkg (sync auto and manual, in mapping order)
 * for instance wf (the text of a wf-id value, src/orm_sync.h):
 * clpd_orm_load with the filter wf_id = wf, typed per the wf-id column (an
 * integer column takes wf as a decimal integer, a float column as a
 * number; a wf that is no such number matches no row and that template is
 * skipped). A template whose table is mapped by one of the packages in
 * done[0..ndone) (versions of one definition sharing a table, already
 * loaded for wf) is skipped. Returns CLPD_DB_OK with *loaded the facts
 * asserted, or the status of the first failed SELECT with *failed set to
 * its template (the facts of the templates before it stay asserted). */
clpd_db_status clpd_orm_load_instance(Environment *env, clpd_db *db,
                                      clpd_orm_syncer *sync,
                                      const clpd_package *pkg,
                                      const char *schema, const char *wf,
                                      const clpd_package *const *done,
                                      size_t ndone, FILE *log,
                                      size_t *loaded,
                                      const clpd_orm_template **failed);

#endif
