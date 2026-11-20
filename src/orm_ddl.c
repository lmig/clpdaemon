/* ORM DDL and sequence functions (T9b, T9d, T9c,
 * docs/orm.md#ddl-functions). The SQL text holds only names and types from
 * the mapping (identifiers checked at load, quoted here), the per-backend
 * type names and, on PostgreSQL, the current schema's name (quoted);
 * catalog lookups and db-nextval pass the names as parameters. At most
 * one CREATE or DROP per call. Statements run on the daemon's connection:
 * inside the cycle transaction when a rule calls them (T10a; after a
 * failed statement there, the rest of the cycle's statements are refused,
 * so db-nextval's lookup reports the database's message), in autocommit
 * otherwise. The DML functions (src/orm_dml.c, T10d) share the caller
 * scope and the messages through clpd_orm_ddl_resolve_template and
 * clpd_orm_ddl_fail. */
#define _POSIX_C_SOURCE 200809L

#include "orm_ddl.h"

#include "log.h"
#include "orm_dml.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define NAME_SHOWN (CLPD_ORM_DDL_NAME_SIZE - 1) /* bytes of an argument
                                                * shown in messages */

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

/* "name" with any '"' doubled (mapping identifiers never hold one; a
 * PostgreSQL schema name may). */
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

/* What the name argument of a function names: a mapped template (the
 * table functions) or a sequence alias (T9c). */
typedef enum { NAMES_TEMPLATE, NAMES_SEQUENCE } name_kind;

/* How messages call the argument and the database object it maps. */
static const char *const argument_word[] = {[NAMES_TEMPLATE] = "template",
                                            [NAMES_SEQUENCE] =
                                                "sequence alias"};
static const char *const object_word[] = {[NAMES_TEMPLATE] = "table",
                                          [NAMES_SEQUENCE] = "sequence"};

typedef struct {
  name_kind kind;
  const clpd_package *pkg;
  const clpd_orm_template *t; /* NAMES_TEMPLATE */
  const clpd_orm_sequence *s; /* NAMES_SEQUENCE */
  char name[NAME_SHOWN + 1];  /* the argument, as messages show it */
} target;

/* The table or sequence name of a resolved target. */
static const char *object_of(const target *tg) {
  return tg->kind == NAMES_TEMPLATE ? tg->t->table : tg->s->name;
}

/* Logs "clpdaemon: <udf>: [<prefix>]<message>" on the daemon log stream,
 * throws the CLIPS error and sets ret to FALSE, the value the function
 * returns. With a resolved target the prefix is "template '<t>': table
 * '<table>'" or "sequence alias '<a>': sequence '<name>'". */
static void vfail(UDFContext *ctx, UDFValue *ret, const target *tg,
                  const char *fmt, va_list ap) {
  FILE *log = clpd_log();
  fprintf(log, "clpdaemon: %s: ", UDFContextFunctionName(ctx));
  if (tg != NULL)
    fprintf(log, "%s '%s': %s '%s'", argument_word[tg->kind], tg->name,
            object_word[tg->kind], object_of(tg));
  vfprintf(log, fmt, ap);
  fputc('\n', log);
  fflush(log);
  UDFThrowError(ctx);
  ret->lexemeValue = FalseSymbol(ctx->environment);
}

static void fail(UDFContext *ctx, UDFValue *ret, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfail(ctx, ret, NULL, fmt, ap);
  va_end(ap);
}

/* fail() with the target prefix; fmt continues it (": <message>" or
 * " is not owned ..."). */
static void fail_on(UDFContext *ctx, UDFValue *ret, const target *tg,
                    const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfail(ctx, ret, tg, fmt, ap);
  va_end(ap);
}

static int is_pg(const clpd_db *db) {
  return strcmp(clpd_db_backend(db), "postgresql") == 0;
}

/* A package whose every mapping check passed. */
static int mapped(const clpd_package *p) {
  return p->orm != NULL && p->orm_templates != NULL;
}

/* The template `name` of a mapped package p, or NULL. */
static const clpd_orm_template *template_in(const clpd_package *p,
                                            const char *name) {
  return mapped(p) ? clpd_orm_template_of(p->orm, name) : NULL;
}

/* The sequence of a mapped package p whose alias is `alias`, or NULL. */
static const clpd_orm_sequence *sequence_in(const clpd_package *p,
                                            const char *alias) {
  return mapped(p) ? clpd_orm_sequence_of(p->orm, alias) : NULL;
}

/* 1 when the mapped package p maps `name` (a template or an alias, per
 * kind); then, with out, sets out->pkg and out->t or out->s. */
static int maps(const clpd_package *p, name_kind kind, const char *name,
                target *out) {
  const clpd_orm_template *t = NULL;
  const clpd_orm_sequence *s = NULL;
  if (kind == NAMES_TEMPLATE)
    t = template_in(p, name);
  else
    s = sequence_in(p, name);
  if (t == NULL && s == NULL) return 0;
  if (out != NULL) {
    out->pkg = p;
    out->t = t;
    out->s = s;
  }
  return 1;
}

/* The loaded package of module m, or NULL. */
static const clpd_package *package_of(const clpd_packages *pkgs,
                                      const Defmodule *m) {
  size_t i;
  for (i = 0; i < pkgs->count; i++)
    if (m != NULL && pkgs->items[i].module == m) return &pkgs->items[i];
  return NULL;
}

/* 1 when p may be used by a caller restricted to `definition` (NULL: any
 * definition). */
static int in_scope(const clpd_package *p, const char *definition) {
  return definition == NULL || strcmp(p->manifest.definition, definition) == 0;
}

/* "<pkg>, <pkg>, ..." of the mapped packages mapping `name` (a template or
 * an alias, per kind) and, with in == 1, in the scope of `definition`
 * (in == 0: outside it). The caller frees the text. */
static char *mappers(const clpd_packages *pkgs, name_kind kind,
                     const char *name, const char *definition, int in) {
  sbuf list = {NULL, 0, 0};
  size_t i;
  for (i = 0; i < pkgs->count; i++) {
    const clpd_package *p = &pkgs->items[i];
    if (in_scope(p, definition) == in && maps(p, kind, name, NULL)) {
      if (list.len > 0) put(&list, ", ");
      put(&list, p->manifest.name);
    }
  }
  return list.s;
}

/* Resolves the first argument, a template or a sequence alias per kind:
 * <package>::<name>, or an unqualified name of `caller` (the package
 * whose module calls), else of the only package mapping it. With a
 * definition, only that definition's packages count, and a name only
 * other definitions map is an error of its own. Returns 0 (FALSE in ret,
 * error logged) when it cannot. */
static int resolve(UDFContext *ctx, UDFValue *ret, const clpd_package *caller,
                   const char *definition, name_kind kind, target *out) {
  const clpd_orm_ddl *ddl = ctx->context;
  const clpd_packages *pkgs = ddl->packages;
  const char *word = argument_word[kind];
  UDFValue arg;
  const char *name, *sep;
  size_t i, found = 0;
  int resolved = 0;

  if (!UDFFirstArgument(ctx, LEXEME_BITS, &arg)) {
    ret->lexemeValue = FalseSymbol(ctx->environment);
    return 0;
  }
  name = arg.lexemeValue->contents;
  shown(name, out->name, sizeof out->name);
  out->kind = kind;
  out->pkg = NULL;
  out->t = NULL;
  out->s = NULL;
  sep = strstr(name, "::");
  if (sep != NULL) {
    size_t n = (size_t)(sep - name);
    for (i = 0; i < pkgs->count && !resolved; i++) {
      const clpd_package *p = &pkgs->items[i];
      resolved = strlen(p->manifest.name) == n &&
                 strncmp(p->manifest.name, name, n) == 0 &&
                 maps(p, kind, sep + 2, out);
    }
    if (resolved && !in_scope(out->pkg, definition)) {
      fail(ctx, ret, "%s '%s' is not mapped by definition %s of the calling "
                     "rule (mapped by %s)", word, out->name, definition,
           out->pkg->manifest.name);
      return 0;
    }
  } else if (caller != NULL && maps(caller, kind, name, out)) {
    resolved = 1;
  } else {
    /* The first package mapping it fills out. */
    for (i = 0; i < pkgs->count; i++) {
      const clpd_package *p = &pkgs->items[i];
      if (in_scope(p, definition) &&
          maps(p, kind, name, found == 0 ? out : NULL))
        found++;
    }
    resolved = found == 1;
    if (found > 1) {
      char *list = mappers(pkgs, kind, name, definition, 1);
      fail(ctx, ret, "%s '%s' is mapped by %s: qualify it as <package>::%s",
           word, out->name, list, out->name);
      free(list);
      return 0;
    }
    if (found == 0 && definition != NULL) {
      char *list = mappers(pkgs, kind, name, definition, 0);
      if (list != NULL) {
        fail(ctx, ret, "%s '%s' is not mapped by definition %s of the "
                       "calling rule (mapped by %s)", word, out->name,
             definition, list);
        free(list);
        return 0;
      }
    }
  }
  if (!resolved) {
    fail(ctx, ret, "unknown %s '%s' (no loaded package maps it)", word,
         out->name);
    return 0;
  }
  return 1;
}

/* db-check-schema: any loaded package; an unqualified name prefers the
 * current module's package. */
static int resolve_any(UDFContext *ctx, UDFValue *ret, target *out) {
  const clpd_orm_ddl *ddl = ctx->context;
  return resolve(ctx, ret,
                 package_of(ddl->packages, GetCurrentModule(ctx->environment)),
                 NULL, NAMES_TEMPLATE, out);
}

/* The functions that change the database (db-create-table, db-drop-table,
 * db-create-sequence, db-nextval; L84, T9c): only a rule whose actions are
 * running, of a loaded package, and only the names its definition maps
 * (any version); an unqualified name prefers the rule's own package.
 * Anything else (the top level, a deffacts at Reset, a plugin hook, a rule
 * of another module) is refused. */
static int resolve_own(UDFContext *ctx, UDFValue *ret, name_kind kind,
                       target *out) {
  const clpd_orm_ddl *ddl = ctx->context;
  Environment *env = ctx->environment;
  Defrule *rule = EngineData(env)->ExecutingRule;
  const clpd_package *caller;
  char rule_name[NAME_SHOWN + 1], module_name[NAME_SHOWN + 1];

  if (rule == NULL || !rule->executing) {
    fail(ctx, ret, "only a rule of a rule package may call it (no rule is "
                   "executing)");
    return 0;
  }
  caller = package_of(ddl->packages, rule->header.whichModule->theModule);
  if (caller == NULL) {
    shown(DefruleName(rule), rule_name, sizeof rule_name);
    shown(DefmoduleName(rule->header.whichModule->theModule), module_name,
          sizeof module_name);
    fail(ctx, ret, "only a rule of a rule package may call it (rule '%s' is "
                   "in module '%s')", rule_name, module_name);
    return 0;
  }
  return resolve(ctx, ret, caller, caller->manifest.definition, kind, out);
}

/* Runs one generated statement; on failure logs it with the target. */
static int run(UDFContext *ctx, UDFValue *ret, const target *tg,
               const char *sql, const clpd_db_value *params, size_t n) {
  clpd_db *db = ((const clpd_orm_ddl *)ctx->context)->db;
  if (clpd_db_exec(db, sql, params, n, NULL) == CLPD_DB_OK) return 1;
  fail_on(ctx, ret, tg, ": %s", clpd_db_error(db));
  return 0;
}

/* Appends `verb` and the table or sequence name to b: on PostgreSQL
 * qualified with current_schema(), the schema db-check-schema inspects, so
 * a same-named object of a later search_path schema is never the one
 * created, dropped or advanced (L84); on SQLite unqualified. Returns 0
 * (FALSE in ret, error logged) when PostgreSQL has no current schema or
 * the query fails. */
static int put_object(UDFContext *ctx, UDFValue *ret, const target *tg,
                      sbuf *b, const char *verb) {
  clpd_db *db = ((const clpd_orm_ddl *)ctx->context)->db;
  clpd_db_result *r = NULL;
  const clpd_db_value *v;
  put(b, verb);
  if (is_pg(db)) {
    if (clpd_db_exec(db, "SELECT current_schema()", NULL, 0, &r) !=
        CLPD_DB_OK) {
      fail_on(ctx, ret, tg, ": %s", clpd_db_error(db));
      return 0;
    }
    v = clpd_db_result_rows(r) == 1 ? clpd_db_result_value(r, 0, 0) : NULL;
    if (v == NULL || v->type != CLPD_DB_TEXT) {
      clpd_db_result_free(r);
      fail_on(ctx, ret, tg, ": no current schema (the search_path names no "
                            "existing schema)");
      return 0;
    }
    put_ident(b, v->data);
    put(b, ".");
    clpd_db_result_free(r);
  }
  put_ident(b, object_of(tg));
  return 1;
}

/* CREATE TABLE IF NOT EXISTS [<schema>.]"<table>" ("<column>" <type>[ NOT
 * NULL], ..., PRIMARY KEY ("wf_id", <local key columns>)), columns in file
 * order. */
static void put_create(sbuf *b, const clpd_orm_template *t,
                       const char *backend) {
  size_t i;
  put(b, " (");
  for (i = 0; i < t->column_count; i++) {
    const clpd_orm_column *c = &t->columns[i];
    put_ident(b, c->column);
    put(b, " ");
    put(b, clpd_orm_backend_type(c->sql_type, backend));
    if (c->null == CLPD_ORM_NULL_ERROR) put(b, " NOT NULL");
    put(b, ", ");
  }
  put(b, "PRIMARY KEY (");
  for (i = 0; i <= t->key.count; i++) {
    if (i > 0) put(b, ", ");
    put_ident(b, t->columns[t->key_columns[i]].column);
  }
  put(b, "))");
}

static void create_table(Environment *env, UDFContext *ctx, UDFValue *ret) {
  const clpd_orm_ddl *ddl = ctx->context;
  target tg;
  sbuf sql = {NULL, 0, 0};
  int ok;
  if (!resolve_own(ctx, ret, NAMES_TEMPLATE, &tg)) return;
  if (!tg.t->owned) {
    fail_on(ctx, ret, &tg, " is not owned (owned = no), so it is never "
                           "created");
    return;
  }
  ok = put_object(ctx, ret, &tg, &sql, "CREATE TABLE IF NOT EXISTS ");
  if (ok) {
    put_create(&sql, tg.t, clpd_db_backend(ddl->db));
    ok = run(ctx, ret, &tg, sql.s, NULL, 0);
  }
  free(sql.s);
  if (ok) ret->lexemeValue = TrueSymbol(env);
}

static void drop_table(Environment *env, UDFContext *ctx, UDFValue *ret) {
  const clpd_orm_ddl *ddl = ctx->context;
  target tg;
  sbuf sql = {NULL, 0, 0};
  int ok;
  if (!resolve_own(ctx, ret, NAMES_TEMPLATE, &tg)) return;
  if (!ddl->allow_drop) {
    fail(ctx, ret, "dropping tables is disabled (orm_allow_drop = no)");
    return;
  }
  if (!tg.t->owned) {
    fail_on(ctx, ret, &tg, " is not owned (owned = no), so it is never "
                           "dropped");
    return;
  }
  ok = put_object(ctx, ret, &tg, &sql, "DROP TABLE IF EXISTS ") &&
       run(ctx, ret, &tg, sql.s, NULL, 0);
  free(sql.s);
  if (ok) ret->lexemeValue = TrueSymbol(env);
}

/* SQLite stands in for sequences with one counter row per sequence name
 * (T9c) in the table CLPD_ORM_SEQUENCE_TABLE, a name no mapping may use
 * (T9e): value is the last value db-nextval returned, 0 before the first,
 * like a PostgreSQL sequence with its defaults (START 1, INCREMENT 1). */
#define LITE_TABLE "\"" CLPD_ORM_SEQUENCE_TABLE "\""
#define LITE_COUNTERS                                                      \
  "CREATE TABLE IF NOT EXISTS " LITE_TABLE " (\"name\" TEXT PRIMARY KEY, " \
  "\"value\" INTEGER NOT NULL)"
#define LITE_COUNTER_ROW                                                   \
  "INSERT INTO " LITE_TABLE " (\"name\", \"value\") VALUES ($1, 0) ON "    \
  "CONFLICT (\"name\") DO NOTHING"
/* One statement; the guard leaves a row unchanged and returns no row when
 * its value is no INTEGER (written by someone else: a REAL would be
 * advanced, TEXT compares above every number) or is at the INT64 maximum
 * (SQLite would turn the sum into a REAL). */
#define LITE_NEXTVAL                                                       \
  "UPDATE " LITE_TABLE " SET \"value\" = \"value\" + 1 WHERE \"name\" = $1 " \
  "AND typeof(\"value\") = 'integer' AND \"value\" < 9223372036854775807 " \
  "RETURNING \"value\""

static void create_sequence(Environment *env, UDFContext *ctx,
                            UDFValue *ret) {
  const clpd_orm_ddl *ddl = ctx->context;
  target tg;
  sbuf sql = {NULL, 0, 0};
  clpd_db_value name;
  int ok;
  if (!resolve_own(ctx, ret, NAMES_SEQUENCE, &tg)) return;
  if (!tg.s->owned) {
    fail_on(ctx, ret, &tg, " is not owned (owned = no), so it is never "
                           "created");
    return;
  }
  if (is_pg(ddl->db)) {
    ok = put_object(ctx, ret, &tg, &sql, "CREATE SEQUENCE IF NOT EXISTS ") &&
         run(ctx, ret, &tg, sql.s, NULL, 0);
  } else {
    name = clpd_db_text(tg.s->name);
    ok = run(ctx, ret, &tg, LITE_COUNTERS, NULL, 0) &&
         run(ctx, ret, &tg, LITE_COUNTER_ROW, &name, 1);
  }
  free(sql.s);
  if (ok) ret->lexemeValue = TrueSymbol(env);
}

/* What the lookup after a db-nextval without a value finds. */
typedef enum {
  SEQUENCE_LOOKUP_FAILED,
  SEQUENCE_MISSING,
  SEQUENCE_EXISTS,
  SEQUENCE_NOT_INTEGER /* SQLite: the counter row holds no INTEGER */
} sequence_state;

/* Looks the target's sequence up: PostgreSQL, a sequence (relkind 'S',
 * like the tests) of that name in current_schema(); SQLite, the counter
 * table and its row, and whether the row's value is an INTEGER. */
static sequence_state sequence_lookup(clpd_db *db, const target *tg) {
  static const char *const pg_sequence =
      "SELECT 1 FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON "
      "n.oid = c.relnamespace WHERE n.nspname = current_schema() AND "
      "c.relname = $1 AND c.relkind = 'S'";
  static const char *const lite_counters =
      "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = '"
      CLPD_ORM_SEQUENCE_TABLE "' COLLATE NOCASE";
  static const char *const lite_row =
      "SELECT typeof(\"value\") = 'integer' FROM " LITE_TABLE " WHERE "
      "\"name\" = $1";
  clpd_db_value name = clpd_db_text(tg->s->name);
  clpd_db_result *r = NULL;
  sequence_state state;
  if (is_pg(db)) {
    if (clpd_db_exec(db, pg_sequence, &name, 1, &r) != CLPD_DB_OK)
      return SEQUENCE_LOOKUP_FAILED;
    state = clpd_db_result_rows(r) > 0 ? SEQUENCE_EXISTS : SEQUENCE_MISSING;
  } else {
    if (clpd_db_exec(db, lite_counters, NULL, 0, &r) != CLPD_DB_OK)
      return SEQUENCE_LOOKUP_FAILED;
    state = clpd_db_result_rows(r) > 0 ? SEQUENCE_EXISTS : SEQUENCE_MISSING;
    clpd_db_result_free(r);
    r = NULL;
    if (state == SEQUENCE_MISSING) return state;
    if (clpd_db_exec(db, lite_row, &name, 1, &r) != CLPD_DB_OK)
      return SEQUENCE_LOOKUP_FAILED;
    if (clpd_db_result_rows(r) == 0)
      state = SEQUENCE_MISSING;
    else if (clpd_db_result_value(r, 0, 0)->integer == 0)
      state = SEQUENCE_NOT_INTEGER;
  }
  clpd_db_result_free(r);
  return state;
}

/* db-nextval: one statement advances the sequence and returns the value;
 * PostgreSQL nextval($1) with the schema-qualified quoted name as a TEXT
 * parameter, SQLite LITE_NEXTVAL. A failure or no value is explained by
 * looking the sequence up: missing, the database's message, a failed
 * lookup, a SQLite counter that is no INTEGER, or the SQLite maximum. */
static void next_value(Environment *env, UDFContext *ctx, UDFValue *ret) {
  const clpd_orm_ddl *ddl = ctx->context;
  int pg = is_pg(ddl->db);
  target tg;
  sbuf qualified = {NULL, 0, 0};
  clpd_db_result *r = NULL;
  const clpd_db_value *v = NULL;
  clpd_db_value param;
  clpd_db_status st;
  char error[1024];
  sequence_state state;

  if (!resolve_own(ctx, ret, NAMES_SEQUENCE, &tg)) return;
  if (pg) {
    if (!put_object(ctx, ret, &tg, &qualified, "")) {
      free(qualified.s);
      return;
    }
    param = clpd_db_text(qualified.s);
    st = clpd_db_exec(ddl->db, "SELECT nextval($1)", &param, 1, &r);
  } else {
    param = clpd_db_text(tg.s->name);
    st = clpd_db_exec(ddl->db, LITE_NEXTVAL, &param, 1, &r);
  }
  free(qualified.s);
  if (st == CLPD_DB_OK && clpd_db_result_rows(r) == 1)
    v = clpd_db_result_value(r, 0, 0);
  if (v != NULL && v->type == CLPD_DB_INTEGER) {
    ret->integerValue = CreateInteger(env, v->integer);
    clpd_db_result_free(r);
    return;
  }
  clpd_db_result_free(r);
  if (v != NULL) {
    fail_on(ctx, ret, &tg, ": the database returned no INTEGER");
    return;
  }
  snprintf(error, sizeof error, "%s", clpd_db_error(ddl->db));
  state = sequence_lookup(ddl->db, &tg);
  if (state == SEQUENCE_MISSING)
    fail_on(ctx, ret, &tg, " does not exist");
  else if (st != CLPD_DB_OK)
    fail_on(ctx, ret, &tg, ": %s", error);
  else if (state == SEQUENCE_LOOKUP_FAILED)
    fail_on(ctx, ret, &tg, ": %s", clpd_db_error(ddl->db));
  else if (state == SEQUENCE_NOT_INTEGER)
    fail_on(ctx, ret, &tg, ": the counter value is not an INTEGER");
  else
    fail_on(ctx, ret, &tg, " has reached its maximum value");
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
    [CLPD_ORM_BIGINT] = {"int8", "int4", "int2", NULL},
    [CLPD_ORM_DOUBLE] = {"float8", "float4", NULL},
    [CLPD_ORM_TEXT] = {"text", "varchar", NULL},
    [CLPD_ORM_BYTEA] = {"bytea", NULL},
    [CLPD_ORM_BOOLEAN] = {"bool", NULL},
    [CLPD_ORM_TIMESTAMPTZ] = {"timestamptz", NULL},
    [CLPD_ORM_JSONB] = {"jsonb", NULL},
    [CLPD_ORM_NUMERIC] = {"numeric", NULL}};

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
      "name = $1 COLLATE NOCASE";
  static const char *const lite_columns =
      "SELECT name, type, \"notnull\" FROM pragma_table_info($1)";
  const clpd_orm_ddl *ddl = ctx->context;
  int pg = strcmp(clpd_db_backend(ddl->db), "postgresql") == 0;
  clpd_db_result *r = NULL;
  clpd_db_value param;
  MultifieldBuilder *mb;
  target tg;
  size_t i, j;

  if (!resolve_any(ctx, ret, &tg)) return;
  param = clpd_db_text(tg.t->table);
  if (clpd_db_exec(ddl->db, pg ? pg_table : lite_table, &param, 1, &r) !=
      CLPD_DB_OK) {
    fail_on(ctx, ret, &tg, ": %s", clpd_db_error(ddl->db));
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
      fail_on(ctx, ret, &tg, ": %s", clpd_db_error(ddl->db));
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
             "clpd_orm_drop_table", ddl) != AUE_NO_ERROR ||
      AddUDF(env, "db-create-sequence", "b", 1, 1, "sy", create_sequence,
             "clpd_orm_create_sequence", ddl) != AUE_NO_ERROR ||
      AddUDF(env, "db-nextval", "bl", 1, 1, "sy", next_value,
             "clpd_orm_nextval", ddl) != AUE_NO_ERROR)
    return -1;
  return clpd_orm_dml_register(env, ddl);
}

int clpd_orm_ddl_resolve_template(UDFContext *ctx, UDFValue *ret,
                                  const clpd_package **pkg,
                                  const clpd_orm_template **t,
                                  char name[CLPD_ORM_DDL_NAME_SIZE]) {
  target tg;
  if (!resolve_own(ctx, ret, NAMES_TEMPLATE, &tg)) return 0;
  *pkg = tg.pkg;
  *t = tg.t;
  memcpy(name, tg.name, sizeof tg.name);
  return 1;
}

void clpd_orm_ddl_fail(UDFContext *ctx, UDFValue *ret, const char *name,
                       const clpd_orm_template *t, const char *fmt, ...) {
  target tg;
  va_list ap;
  memset(&tg, 0, sizeof tg);
  if (t != NULL) {
    tg.kind = NAMES_TEMPLATE;
    tg.t = t;
    snprintf(tg.name, sizeof tg.name, "%s", name);
  }
  va_start(ap, fmt);
  vfail(ctx, ret, t != NULL ? &tg : NULL, fmt, ap);
  va_end(ap);
}
