/* ORM DDL functions (T9b, docs/orm.md#ddl-functions). The SQL text holds
 * only names and types from the mapping (identifiers checked at load,
 * quoted here) and the per-backend type names; catalog lookups pass the
 * table name as a parameter. One autocommit statement per call. */
#define _POSIX_C_SOURCE 200809L

#include "orm_ddl.h"

#include "log.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define NAME_SHOWN 200 /* bytes of a template argument shown in messages */

typedef struct {
  char *s;
  size_t len, cap;
} sbuf;

static void put(sbuf *b, const char *s) {
  size_t n = strlen(s);
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap == 0 ? 256 : b->cap;
    char *grown;
    while (b->len + n + 1 > cap) cap *= 2;
    grown = realloc(b->s, cap);
    if (grown == NULL) clpd_log_die_errno();
    b->s = grown;
    b->cap = cap;
  }
  memcpy(b->s + b->len, s, n + 1);
  b->len += n;
}

/* "name" with any '"' doubled (mapping identifiers never hold one). */
static void put_ident(sbuf *b, const char *id) {
  char one[2] = {0, 0};
  put(b, "\"");
  for (; *id != '\0'; id++) {
    one[0] = *id;
    put(b, *id == '"' ? "\"\"" : one);
  }
  put(b, "\"");
}

/* Copies s for a one-line message: control bytes become '?', and a long
 * argument is cut. */
static void shown(const char *s, char *out, size_t n) {
  size_t i;
  for (i = 0; s[i] != '\0' && i + 1 < n; i++)
    out[i] = (unsigned char)s[i] < 0x20 || s[i] == 0x7f ? '?' : s[i];
  out[i] = '\0';
}

/* Logs "clpdaemon: <udf>: <message>", throws the CLIPS error and returns
 * FALSE. */
static void fail(UDFContext *ctx, UDFValue *ret, const char *fmt, ...) {
  FILE *log = clpd_log();
  va_list ap;
  fprintf(log, "clpdaemon: %s: ", UDFContextFunctionName(ctx));
  va_start(ap, fmt);
  vfprintf(log, fmt, ap);
  va_end(ap);
  fputc('\n', log);
  fflush(log);
  UDFThrowError(ctx);
  ret->lexemeValue = FalseSymbol(ctx->environment);
}

typedef struct {
  const clpd_package *pkg;
  const clpd_orm_template *t;
  char name[NAME_SHOWN + 1]; /* the argument, as messages show it */
} target;

/* A package whose every mapping check passed. */
static int mapped(const clpd_package *p) {
  return p->orm != NULL && p->orm_templates != NULL;
}

/* Resolves the first argument: <package>::<template>, or an unqualified
 * template of the current module's package, else of the only package
 * mapping it. Returns 0 (FALSE in ret, error logged) when it cannot. */
static int resolve(UDFContext *ctx, UDFValue *ret, target *out) {
  const clpd_orm_ddl *ddl = ctx->context;
  const clpd_packages *pkgs = ddl->packages;
  UDFValue arg;
  const char *name, *sep;
  size_t i, found = 0;

  if (!UDFFirstArgument(ctx, LEXEME_BITS, &arg)) {
    ret->lexemeValue = FalseSymbol(ctx->environment);
    return 0;
  }
  name = arg.lexemeValue->contents;
  shown(name, out->name, sizeof out->name);
  out->pkg = NULL;
  out->t = NULL;
  sep = strstr(name, "::");
  if (sep != NULL) {
    size_t n = (size_t)(sep - name);
    for (i = 0; i < pkgs->count && out->t == NULL; i++) {
      const clpd_package *p = &pkgs->items[i];
      if (mapped(p) && strlen(p->manifest.name) == n &&
          strncmp(p->manifest.name, name, n) == 0 &&
          (out->t = clpd_orm_template_of(p->orm, sep + 2)) != NULL)
        out->pkg = p;
    }
  } else {
    Defmodule *current = GetCurrentModule(ctx->environment);
    for (i = 0; i < pkgs->count; i++) {
      const clpd_package *p = &pkgs->items[i];
      if (mapped(p) && p->module == current &&
          (out->t = clpd_orm_template_of(p->orm, name)) != NULL) {
        out->pkg = p;
        return 1;
      }
    }
    for (i = 0; i < pkgs->count; i++) {
      const clpd_package *p = &pkgs->items[i];
      const clpd_orm_template *t;
      if (mapped(p) && (t = clpd_orm_template_of(p->orm, name)) != NULL) {
        if (found++ == 0) {
          out->pkg = p;
          out->t = t;
        }
      }
    }
    if (found > 1) {
      sbuf list = {NULL, 0, 0};
      for (i = 0; i < pkgs->count; i++) {
        const clpd_package *p = &pkgs->items[i];
        if (mapped(p) && clpd_orm_template_of(p->orm, name) != NULL) {
          if (list.len > 0) put(&list, ", ");
          put(&list, p->manifest.name);
        }
      }
      fail(ctx, ret, "template '%s' is mapped by %s: qualify it as "
                     "<package>::%s", out->name, list.s, out->name);
      free(list.s);
      return 0;
    }
  }
  if (out->t == NULL) {
    fail(ctx, ret, "unknown template '%s' (no loaded package maps it)",
         out->name);
    return 0;
  }
  return 1;
}

/* Runs one generated statement; on failure logs it with the table. */
static int run(UDFContext *ctx, UDFValue *ret, const target *tg,
               const char *sql) {
  clpd_db *db = ((const clpd_orm_ddl *)ctx->context)->db;
  if (clpd_db_exec(db, sql, NULL, 0, NULL) == CLPD_DB_OK) return 1;
  fail(ctx, ret, "template '%s': table '%s': %s", tg->name, tg->t->table,
       clpd_db_error(db));
  return 0;
}

/* CREATE TABLE IF NOT EXISTS "<table>" ("<column>" <type>[ NOT NULL], ...,
 * PRIMARY KEY ("wf_id", <local key columns>)), columns in file order. */
static char *create_sql(const clpd_orm_template *t, const char *backend) {
  sbuf b = {NULL, 0, 0};
  size_t i;
  put(&b, "CREATE TABLE IF NOT EXISTS ");
  put_ident(&b, t->table);
  put(&b, " (");
  for (i = 0; i < t->column_count; i++) {
    const clpd_orm_column *c = &t->columns[i];
    put_ident(&b, c->column);
    put(&b, " ");
    put(&b, clpd_orm_backend_type(c->sql_type, backend));
    if (c->null == CLPD_ORM_NULL_ERROR) put(&b, " NOT NULL");
    put(&b, ", ");
  }
  put(&b, "PRIMARY KEY (");
  for (i = 0; i <= t->key.count; i++) {
    if (i > 0) put(&b, ", ");
    put_ident(&b, t->columns[t->key_columns[i]].column);
  }
  put(&b, "))");
  return b.s;
}

static void create_table(Environment *env, UDFContext *ctx, UDFValue *ret) {
  const clpd_orm_ddl *ddl = ctx->context;
  target tg;
  char *sql;
  int ok;
  if (!resolve(ctx, ret, &tg)) return;
  if (!tg.t->owned) {
    fail(ctx, ret, "template '%s': table '%s' is not owned (owned = no), so "
                   "it is never created", tg.name, tg.t->table);
    return;
  }
  sql = create_sql(tg.t, clpd_db_backend(ddl->db));
  ok = run(ctx, ret, &tg, sql);
  free(sql);
  if (ok) ret->lexemeValue = TrueSymbol(env);
}

static void drop_table(Environment *env, UDFContext *ctx, UDFValue *ret) {
  const clpd_orm_ddl *ddl = ctx->context;
  target tg;
  sbuf sql = {NULL, 0, 0};
  int ok;
  if (!resolve(ctx, ret, &tg)) return;
  if (!ddl->allow_drop) {
    fail(ctx, ret, "dropping tables is disabled (orm_allow_drop = no)");
    return;
  }
  if (!tg.t->owned) {
    fail(ctx, ret, "template '%s': table '%s' is not owned (owned = no), so "
                   "it is never dropped", tg.name, tg.t->table);
    return;
  }
  put(&sql, "DROP TABLE IF EXISTS ");
  put_ident(&sql, tg.t->table);
  ok = run(ctx, ret, &tg, sql.s);
  free(sql.s);
  if (ok) ret->lexemeValue = TrueSymbol(env);
}

/* SQLite column affinity of a declared type (the rules of "Datatypes In
 * SQLite", section 3.1): 'I'nteger, 'T'ext, 'B'lob, 'R'eal, 'N'umeric. */
static char affinity(const char *decl) {
  char up[256];
  size_t i;
  for (i = 0; decl[i] != '\0' && i + 1 < sizeof up; i++)
    up[i] = decl[i] >= 'a' && decl[i] <= 'z' ? (char)(decl[i] - 'a' + 'A')
                                             : decl[i];
  up[i] = '\0';
  if (strstr(up, "INT") != NULL) return 'I';
  if (strstr(up, "CHAR") != NULL || strstr(up, "CLOB") != NULL ||
      strstr(up, "TEXT") != NULL)
    return 'T';
  if (strstr(up, "BLOB") != NULL || up[0] == '\0') return 'B';
  if (strstr(up, "REAL") != NULL || strstr(up, "FLOA") != NULL ||
      strstr(up, "DOUB") != NULL)
    return 'R';
  return 'N';
}

/* PostgreSQL udt_name values a column of each sql type may have. */
static const char *const pg_compatible[CLPD_ORM_SQL_TYPE_COUNT][4] = {
    {"int8", "int4", "int2", NULL}, /* bigint */
    {"float8", "float4", NULL},     /* double */
    {"text", "varchar", NULL},      /* text */
    {"bytea", NULL},                /* bytea */
    {"bool", NULL},                 /* boolean */
    {"timestamptz", NULL},          /* timestamptz */
    {"jsonb", NULL},                /* jsonb */
    {"numeric", NULL}};             /* numeric */

/* 1 when an existing column of catalog type `type` holds sql type t: on
 * PostgreSQL one of the udt_names above; on SQLite the declared type has
 * the affinity of the generated one (so never NUMERIC, L76). */
static int compatible(clpd_orm_sql_type t, const char *type, int pg) {
  size_t i;
  if (!pg) return affinity(type) == affinity(clpd_orm_backend_type(t, "sqlite"));
  for (i = 0; pg_compatible[t][i] != NULL; i++)
    if (strcmp(pg_compatible[t][i], type) == 0) return 1;
  return 0;
}

static void add(MultifieldBuilder *mb, const char *fmt, ...) {
  char text[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof text, fmt, ap);
  va_end(ap);
  MBAppendString(mb, text);
}

static void check_schema(Environment *env, UDFContext *ctx, UDFValue *ret) {
  static const char *const pg_table =
      "SELECT 1 FROM information_schema.tables WHERE table_schema = "
      "current_schema() AND table_name = $1";
  static const char *const pg_columns =
      "SELECT column_name, udt_name, CASE is_nullable WHEN 'NO' THEN 1 ELSE "
      "0 END FROM information_schema.columns WHERE table_schema = "
      "current_schema() AND table_name = $1";
  static const char *const lite_table =
      "SELECT 1 FROM sqlite_master WHERE type IN ('table', 'view') AND "
      "name = $1";
  static const char *const lite_columns =
      "SELECT name, type, \"notnull\" FROM pragma_table_info($1)";
  const clpd_orm_ddl *ddl = ctx->context;
  int pg = strcmp(clpd_db_backend(ddl->db), "postgresql") == 0;
  clpd_db_result *r = NULL;
  clpd_db_value param;
  MultifieldBuilder *mb;
  target tg;
  size_t i, j;

  if (!resolve(ctx, ret, &tg)) return;
  param = clpd_db_text(tg.t->table);
  if (clpd_db_exec(ddl->db, pg ? pg_table : lite_table, &param, 1, &r) !=
      CLPD_DB_OK) {
    fail(ctx, ret, "template '%s': table '%s': %s", tg.name, tg.t->table,
         clpd_db_error(ddl->db));
    return;
  }
  mb = CreateMultifieldBuilder(env, 4);
  if (clpd_db_result_rows(r) == 0) {
    add(mb, "table '%s' does not exist", tg.t->table);
  } else {
    clpd_db_result_free(r);
    r = NULL;
    if (clpd_db_exec(ddl->db, pg ? pg_columns : lite_columns, &param, 1, &r) !=
        CLPD_DB_OK) {
      MBDispose(mb);
      fail(ctx, ret, "template '%s': table '%s': %s", tg.name, tg.t->table,
           clpd_db_error(ddl->db));
      return;
    }
    for (i = 0; i < tg.t->column_count; i++) {
      const clpd_orm_column *c = &tg.t->columns[i];
      const char *type = NULL;
      int notnull = 0;
      for (j = 0; j < clpd_db_result_rows(r); j++) {
        const clpd_db_value *n = clpd_db_result_value(r, j, 0),
                            *ty = clpd_db_result_value(r, j, 1),
                            *nn = clpd_db_result_value(r, j, 2);
        if (n->type != CLPD_DB_TEXT ||
            (pg ? strcmp(n->data, c->column) : strcasecmp(n->data, c->column)) != 0)
          continue;
        type = ty->type == CLPD_DB_TEXT ? ty->data : "";
        notnull = nn->type == CLPD_DB_INTEGER && nn->integer != 0;
        break;
      }
      if (type == NULL) {
        add(mb, "column '%s' does not exist", c->column);
        continue;
      }
      if (!compatible(c->sql_type, type, pg))
        add(mb, "column '%s' has type '%s', expected %s (%s)", c->column, type,
            clpd_orm_sql_type_name(c->sql_type),
            clpd_orm_backend_type(c->sql_type, clpd_db_backend(ddl->db)));
      if (notnull && c->null != CLPD_ORM_NULL_ERROR)
        add(mb, "column '%s' is NOT NULL, expected nullable (null policy %s)",
            c->column, clpd_orm_null_name(c->null));
    }
  }
  clpd_db_result_free(r);
  ret->multifieldValue = MBCreate(mb);
  MBDispose(mb);
}

int clpd_orm_ddl_register(Environment *env, clpd_orm_ddl *ddl) {
  if (AddUDF(env, "db-create-table", "b", 1, 1, "sy", create_table,
             "clpd_orm_create_table", ddl) != AUE_NO_ERROR ||
      AddUDF(env, "db-check-schema", "bm", 1, 1, "sy", check_schema,
             "clpd_orm_check_schema", ddl) != AUE_NO_ERROR ||
      AddUDF(env, "db-drop-table", "b", 1, 1, "sy", drop_table,
             "clpd_orm_drop_table", ddl) != AUE_NO_ERROR)
    return -1;
  return 0;
}
