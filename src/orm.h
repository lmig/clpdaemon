/* ORM mapping file of a rule package (T9a, docs/orm.md): the CLIPS-free
 * model and its parser. The package loader (src/packages.c) binds the model
 * to the package's deftemplates. */
#ifndef CLPD_ORM_H
#define CLPD_ORM_H

#include <stddef.h>
#include <stdio.h>

#include "kvfile.h"

#define CLPD_ORM_IDENT_MAX 63 /* bytes; PostgreSQL's NAMEDATALEN - 1 */
#define CLPD_ORM_WF_ID "wf-id" /* always the first key slot */

/* Logical SQL types; each backend has its own type names (T9b). */
typedef enum {
  CLPD_ORM_BIGINT,
  CLPD_ORM_DOUBLE,
  CLPD_ORM_TEXT,
  CLPD_ORM_BYTEA,
  CLPD_ORM_BOOLEAN,
  CLPD_ORM_TIMESTAMPTZ,
  CLPD_ORM_JSONB,
  CLPD_ORM_NUMERIC,
  CLPD_ORM_SQL_TYPE_COUNT
} clpd_orm_sql_type;

typedef enum {
  CLPD_ORM_INTEGER,
  CLPD_ORM_FLOAT,
  CLPD_ORM_STRING,
  CLPD_ORM_SYMBOL,
  CLPD_ORM_CLIPS_TYPE_COUNT
} clpd_orm_clips_type;

/* What a NULL column becomes in the slot (T10): the symbol nil, the slot's
 * default, or a rejected row. Key slots are always CLPD_ORM_NULL_ERROR. */
typedef enum {
  CLPD_ORM_NULL_NIL,
  CLPD_ORM_NULL_DEFAULT,
  CLPD_ORM_NULL_ERROR,
  CLPD_ORM_NULL_COUNT
} clpd_orm_null;

/* One `<tpl>.slot.<slot> = <column> <sql-type> <clips-type> <null>` line. */
typedef struct {
  char *slot;
  char *column;
  clpd_orm_sql_type sql_type;
  clpd_orm_clips_type clips_type;
  clpd_orm_null null;
  long line;
} clpd_orm_column;

typedef struct {
  char *name;  /* deftemplate name, unqualified */
  char *table; /* identifier */
  int owned;   /* 1: the daemon may create (and, if allowed, drop) it */
  clpd_list key; /* local key slots in order, without wf-id (may be empty) */
  clpd_orm_column *columns; /* in file order */
  size_t column_count;
  /* Indexes into columns: wf-id first, then the local key in order; set
   * by clpd_orm_resolve_key (NULL until then). key.count + 1 entries. */
  size_t *key_columns;
  long line;                              /* first line naming it */
  long table_line, owned_line, key_line;  /* 0 = not set */
} clpd_orm_template;

/* `seq.<alias> = <name>` and `seq.<alias>.owned = yes|no`. */
typedef struct {
  char *alias; /* identifier used from CLIPS (T9c) */
  char *name;  /* identifier of the database sequence */
  int owned;
  long line, name_line, owned_line; /* line: first line naming it */
} clpd_orm_sequence;

typedef struct {
  char *path;
  clpd_orm_template *templates; /* in order of first appearance */
  size_t template_count;
  clpd_orm_sequence *sequences; /* in order of first appearance */
  size_t sequence_count;
} clpd_orm;

/* Reads and validates the mapping file `path` (syntax, keys, identifiers,
 * types, null policies, required keys, a column or key slot repeated in a
 * template, a table or sequence mapped twice in the file). Every error is
 * written to err, one line each. Returns 0 when valid; otherwise nonzero,
 * and orm must still be freed. */
int clpd_orm_parse(const char *path, clpd_orm *orm, FILE *err);
void clpd_orm_free(clpd_orm *orm);

/* The column mapping `slot` of t, or NULL. */
const clpd_orm_column *clpd_orm_column_of(const clpd_orm_template *t,
                                          const char *slot);

/* Fills t->key_columns. Returns 0, or -1 (and sets nothing) when wf-id or
 * a key slot has no column line. */
int clpd_orm_resolve_key(clpd_orm_template *t);

/* 1 when s matches [a-z_][a-z0-9_]* and has at most CLPD_ORM_IDENT_MAX
 * bytes. */
int clpd_orm_valid_ident(const char *s);

/* Names as written in the mapping file (bigint, integer, nil, ...). */
const char *clpd_orm_sql_type_name(clpd_orm_sql_type t);
const char *clpd_orm_clips_type_name(clpd_orm_clips_type t);
const char *clpd_orm_null_name(clpd_orm_null n);

/* Column type for `backend` ("postgresql" or "sqlite", as returned by
 * clpd_db_backend), or NULL for another backend. SQLite names never get
 * NUMERIC affinity (L76, L82). */
const char *clpd_orm_backend_type(clpd_orm_sql_type t, const char *backend);

#endif
