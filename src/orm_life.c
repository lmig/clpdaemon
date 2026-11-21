/* Workflow lifecycle functions (T11a, src/orm_life.h,
 * docs/orm.md#lifecycle-functions).
 *
 * Each call, in order (the first failure is the message, a refused call
 * writes nothing): the caller scope (clpd_orm_ddl_caller), the arguments
 * (CLIPS checks their types), an isolated instance, on PostgreSQL the
 * current schema, the instance's workflow_instance row (its definition,
 * then its status), the instance's pending synchronization changes, for
 * an instance that had no row the row the flush may have written (its
 * definition again), the final row, then the facts. Every statement is
 * parameterized; the only names are the daemon's own. */
#define _POSIX_C_SOURCE 200809L

#include "orm_life.h"

#include "log.h"
#include "orm.h"
#include "orm_sync.h"
#include "orm_tx.h"

#include <stdio.h>
#include <string.h>

#define SHOWN_MAX 200 /* bytes of a wf-id shown in a message */

/* The text of a wf-id value as workflow_instance keys it (src/orm_sync.c
 * value_text): a symbol, string or instance name as it is, an integer in
 * decimal, a float with %.17g. */
static void wf_of(const UDFValue *v, char *out, size_t n) {
  switch (v->header->type) {
  case INTEGER_TYPE:
    snprintf(out, n, "%lld", v->integerValue->contents);
    break;
  case FLOAT_TYPE:
    snprintf(out, n, "%.17g", v->floatValue->contents);
    break;
  default:
    snprintf(out, n, "%s", v->lexemeValue->contents);
    break;
  }
}

/* wf as messages show it: control bytes as '?', at most SHOWN_MAX. */
static void shown(const char *wf, char out[SHOWN_MAX + 1]) {
  size_t i;
  for (i = 0; wf[i] != '\0' && i < SHOWN_MAX; i++)
    out[i] = (unsigned char)wf[i] < 0x20 || wf[i] == 0x7f ? '?' : wf[i];
  out[i] = '\0';
}

/* The instance's row as the cycle transaction sees it. */
typedef enum { ROW_NONE, ROW_FOUND, ROW_FAILED } row_state;

typedef clpd_orm_life_row instance_row;

/* Copies a TEXT value ("" otherwise). */
static void text_of(const clpd_db_value *v, char *out, size_t n) {
  if (v != NULL && v->type == CLPD_DB_TEXT)
    snprintf(out, n, "%.*s", (int)v->size, (const char *)v->data);
  else
    snprintf(out, n, "%s", "");
}

int clpd_orm_life_read_row(clpd_db *db, const char *schema, const char *wf,
                           clpd_orm_life_row *row, clpd_db_status *st) {
  char sql[256];
  clpd_db_result *r = NULL;
  clpd_db_value p = clpd_db_text(wf);
  int found = 0;
  snprintf(sql, sizeof sql,
           "SELECT \"definition\", \"version\", \"status\" FROM "
           "%s\"" CLPD_ORM_INSTANCE_TABLE "\" WHERE \"wf_id\" = $1",
           schema);
  *st = clpd_db_exec(db, sql, &p, 1, &r);
  if (*st != CLPD_DB_OK) return -1;
  if (clpd_db_result_rows(r) > 0) {
    text_of(clpd_db_result_value(r, 0, 0), row->definition,
            sizeof row->definition);
    text_of(clpd_db_result_value(r, 0, 1), row->version,
            sizeof row->version);
    text_of(clpd_db_result_value(r, 0, 2), row->status, sizeof row->status);
    found = 1;
  }
  clpd_db_result_free(r);
  return found;
}

static row_state read_row(clpd_db *db, const char *schema, const char *wf,
                          instance_row *row) {
  clpd_db_status st;
  switch (clpd_orm_life_read_row(db, schema, wf, row, &st)) {
  case 1: return ROW_FOUND;
  case 0: return ROW_NONE;
  default: return ROW_FAILED;
  }
}

/* The final row: inserted with the caller's definition and version when
 * the instance has none, else its status, reason and updated_at set (the
 * database clock, like the synchronization's instance rows). */
static int write_final(clpd_db *db, const char *schema, const char *wf,
                       const clpd_package *pkg, const char *status,
                       const char *reason) {
  const char *now = schema[0] != '\0'
                        ? "now()"
                        : "strftime('%Y-%m-%dT%H:%M:%fZ', 'now')";
  char sql[1024];
  clpd_db_value p[5];
  snprintf(sql, sizeof sql,
           "INSERT INTO %s\"" CLPD_ORM_INSTANCE_TABLE "\" (\"wf_id\", "
           "\"definition\", \"version\", \"status\", \"reason\", "
           "\"created_at\", \"updated_at\") VALUES ($1, $2, $3, $4, $5, %s, "
           "%s) ON CONFLICT (\"wf_id\") DO UPDATE SET \"status\" = "
           "excluded.\"status\", \"reason\" = excluded.\"reason\", "
           "\"updated_at\" = excluded.\"updated_at\"",
           schema, now, now);
  p[0] = clpd_db_text(wf);
  p[1] = clpd_db_text(pkg->manifest.definition);
  p[2] = clpd_db_text(pkg->manifest.version);
  p[3] = clpd_db_text(status);
  p[4] = reason != NULL ? clpd_db_text(reason) : clpd_db_null();
  return clpd_db_exec(db, sql, p, 5, NULL) == CLPD_DB_OK;
}

/* wf-complete (reason NULL, status completed) and wf-fail (status failed,
 * the second argument as reason). */
static void finish(UDFContext *ctx, UDFValue *ret, const char *status,
                   int with_reason) {
  const clpd_orm_ddl *ddl = ctx->context;
  clpd_db *db = ddl->db;
  const clpd_package *pkg;
  UDFValue a, why;
  char wf[4096], name[SHOWN_MAX + 1], schema[CLPD_ORM_TX_SCHEMA_SIZE + 2] = "",
                                       msg[512];
  instance_row row;
  row_state state;

  if (!clpd_orm_ddl_caller(ctx, ret, &pkg)) return;
  if (!UDFFirstArgument(ctx, LEXEME_BITS | NUMBER_BITS | INSTANCE_NAME_BIT,
                        &a) ||
      (with_reason && !UDFNextArgument(ctx, LEXEME_BITS, &why))) {
    ret->lexemeValue = FalseSymbol(ctx->environment);
    return;
  }
  wf_of(&a, wf, sizeof wf);
  shown(wf, name);
  if (clpd_orm_sync_isolated(ddl->sync, wf)) {
    clpd_orm_ddl_fail(ctx, ret, NULL, NULL,
                      "instance '%s' is no longer synchronized", name);
    return;
  }
  if (strcmp(clpd_db_backend(db), "postgresql") == 0) {
    clpd_db_status st;
    if (clpd_orm_tx_schema_status(db, schema, msg, sizeof msg, &st) != 0) {
      clpd_orm_ddl_fail(ctx, ret, NULL, NULL, "instance '%s': %s", name, msg);
      return;
    }
    strcat(schema, ".");
  }
  state = read_row(db, schema, wf, &row);
  if (state == ROW_FAILED) {
    clpd_orm_ddl_fail(ctx, ret, NULL, NULL, "instance '%s': %s", name,
                      clpd_db_error(db));
    return;
  }
  if (state == ROW_FOUND &&
      strcmp(row.definition, pkg->manifest.definition) != 0) {
    clpd_orm_ddl_fail(ctx, ret, NULL, NULL,
                      "instance '%s' belongs to definition %s, not to "
                      "definition %s of the calling rule",
                      name, row.definition, pkg->manifest.definition);
    return;
  }
  if (state == ROW_FOUND && (strcmp(row.status, "completed") == 0 ||
                             strcmp(row.status, "failed") == 0)) {
    clpd_orm_ddl_fail(ctx, ret, NULL, NULL, "instance '%s' is already %s",
                      name, row.status);
    return;
  }
  /* The changes made before the call come first. */
  if (clpd_orm_sync_flush_instance(ddl->sync, wf, clpd_log()) != 0) {
    clpd_orm_ddl_fail(ctx, ret, NULL, NULL,
                      "instance '%s': its pending changes could not be "
                      "written",
                      name);
    return;
  }
  if (state == ROW_NONE) {
    /* The flush may have inserted it from facts of another definition. */
    state = read_row(db, schema, wf, &row);
    if (state == ROW_FAILED) {
      clpd_orm_ddl_fail(ctx, ret, NULL, NULL, "instance '%s': %s", name,
                        clpd_db_error(db));
      return;
    }
    if (state == ROW_FOUND &&
        strcmp(row.definition, pkg->manifest.definition) != 0) {
      clpd_orm_ddl_fail(ctx, ret, NULL, NULL,
                        "instance '%s' belongs to definition %s, not to "
                        "definition %s of the calling rule",
                        name, row.definition, pkg->manifest.definition);
      return;
    }
  }
  if (!write_final(db, schema, wf, pkg, status,
                   with_reason ? why.lexemeValue->contents : NULL)) {
    clpd_orm_ddl_fail(ctx, ret, NULL, NULL, "instance '%s': %s", name,
                      clpd_db_error(db));
    return;
  }
  clpd_orm_sync_finish(ddl->sync, wf);
  ret->lexemeValue = TrueSymbol(ctx->environment);
}

static void complete(Environment *env, UDFContext *ctx, UDFValue *ret) {
  (void)env;
  finish(ctx, ret, "completed", 0);
}

static void fail(Environment *env, UDFContext *ctx, UDFValue *ret) {
  (void)env;
  finish(ctx, ret, "failed", 1);
}

int clpd_orm_life_register(Environment *env, clpd_orm_ddl *ddl) {
  if (AddUDF(env, "wf-complete", "b", 1, 1, "syldn", complete,
             "clpd_orm_life_complete", ddl) != AUE_NO_ERROR ||
      AddUDF(env, "wf-fail", "b", 2, 2, "syldn;syldn;sy", fail,
             "clpd_orm_life_fail", ddl) != AUE_NO_ERROR)
    return -1;
  return 0;
}
