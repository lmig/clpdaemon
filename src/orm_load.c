/* Loading stored rows into facts (T10c, src/orm_load.h,
 * docs/orm.md#loading-rows).
 *
 * The rows are read first, whole (clpd_db_exec copies them), then each
 * becomes a fact through a FactBuilder: so a failed SELECT asserts
 * nothing, and nothing points into the result after the load. */
#define _POSIX_C_SOURCE 200809L

#include "orm_load.h"

#include "log.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SHOWN_MAX 200 /* bytes of a wf-id shown in a log line */

static void *xrealloc(void *p, size_t n) {
  void *q = realloc(p, n);
  if (q == NULL) clpd_log_die_errno();
  return q;
}

typedef struct {
  char *s;
  size_t len, cap;
} sbuf;

static void put(sbuf *b, const char *s) {
  size_t n = strlen(s);
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap == 0 ? 256 : b->cap;
    while (b->len + n + 1 > cap) cap *= 2;
    b->s = xrealloc(b->s, cap);
    b->cap = cap;
  }
  memcpy(b->s + b->len, s, n + 1);
  b->len += n;
}

/* "name": mapping identifiers never hold a '"' (checked at load). */
static void put_ident(sbuf *b, const char *id) {
  put(b, "\"");
  put(b, id);
  put(b, "\"");
}

static const char *db_type_name(clpd_db_type t) {
  switch (t) {
  case CLPD_DB_NULL: return "NULL";
  case CLPD_DB_INTEGER: return "INTEGER";
  case CLPD_DB_FLOAT: return "FLOAT";
  case CLPD_DB_TEXT: return "TEXT";
  default: return "BLOB";
  }
}

/* Why the slot refused a value (FBPutSlot). */
static const char *refusal(PutSlotError e) {
  switch (e) {
  case PSE_TYPE_ERROR:
    return "the slot's type constraint does not allow the value";
  case PSE_RANGE_ERROR:
    return "the value is outside the slot's range";
  case PSE_ALLOWED_VALUES_ERROR:
    return "the slot's allowed values do not include the value";
  case PSE_CARDINALITY_ERROR:
    return "the slot's cardinality does not allow the value";
  case PSE_ALLOWED_CLASSES_ERROR:
    return "the slot's allowed classes do not include the value";
  default:
    return "CLIPS refused the value";
  }
}

/* A NUL-terminated copy of a TEXT or BLOB value; NULL (and *nul set) when
 * it holds a NUL byte, which no CLIPS lexeme can. */
static char *lexeme_copy(const clpd_db_value *v, int *nul) {
  char *s;
  *nul = v->size > 0 && memchr(v->data, '\0', v->size) != NULL;
  if (*nul) return NULL;
  s = xrealloc(NULL, v->size + 1);
  if (v->size > 0) memcpy(s, v->data, v->size);
  s[v->size] = '\0';
  return s;
}

/* A whole TEXT value read as a finite number. */
static int text_number(const clpd_db_value *v, double *out) {
  char *end;
  if (v->size == 0 || memchr(v->data, '\0', v->size) != NULL) return 0;
  *out = strtod(v->data, &end);
  return end == (const char *)v->data + v->size && isfinite(*out);
}

/* Puts the value v of column c into the fact builder, per the column's
 * CLIPS type and null policy: 0 done (or the slot keeps its default), -1
 * with the reason in why. */
static int put_column(Environment *env, FactBuilder *fb, Deftemplate *d,
                      const clpd_orm_column *c, const clpd_db_value *v,
                      char *why, size_t n) {
  CLIPSValue cv;
  PutSlotError pe;
  char *text = NULL;
  int nul = 0;
  double f;
  if (v->type == CLPD_DB_NULL) {
    switch (c->null) {
    case CLPD_ORM_NULL_NIL:
      cv.lexemeValue = CreateSymbol(env, "nil");
      break;
    case CLPD_ORM_NULL_DEFAULT: {
      /* The slot's default: a slot without one (?NONE) needs a value. */
      struct templateSlot *ts;
      for (ts = d->slotList; ts != NULL; ts = ts->next)
        if (strcmp(ts->slotName->contents, c->slot) == 0) break;
      if (ts != NULL && ts->noDefault) {
        snprintf(why, n, "column '%s' (slot '%s') is NULL and the slot has "
                         "no default", c->column, c->slot);
        return -1;
      }
      return 0;
    }
    default:
      snprintf(why, n, "column '%s' (slot '%s') is NULL and its null policy "
                       "is error", c->column, c->slot);
      return -1;
    }
  } else {
    switch (c->clips_type) {
    case CLPD_ORM_INTEGER:
      if (v->type != CLPD_DB_INTEGER) goto mismatch;
      cv.integerValue = CreateInteger(env, v->integer);
      break;
    case CLPD_ORM_FLOAT:
      if (v->type == CLPD_DB_FLOAT) {
        cv.floatValue = CreateFloat(env, v->real);
      } else if (v->type == CLPD_DB_INTEGER) {
        cv.floatValue = CreateFloat(env, (double)v->integer);
      } else if (v->type == CLPD_DB_TEXT) {
        /* numeric: PostgreSQL returns it as text, SQLite stores it so. */
        if (!text_number(v, &f)) {
          snprintf(why, n, "column '%s' (slot '%s'): the text is not a "
                           "number", c->column, c->slot);
          return -1;
        }
        cv.floatValue = CreateFloat(env, f);
      } else {
        goto mismatch;
      }
      break;
    default: /* string, symbol: text, or the bytes of a bytea */
      if (v->type != CLPD_DB_TEXT && v->type != CLPD_DB_BLOB) goto mismatch;
      text = lexeme_copy(v, &nul);
      if (nul) {
        snprintf(why, n, "column '%s' (slot '%s'): the value holds a NUL "
                         "byte", c->column, c->slot);
        return -1;
      }
      cv.lexemeValue = c->clips_type == CLPD_ORM_SYMBOL
                           ? CreateSymbol(env, text)
                           : CreateString(env, text);
      free(text);
      break;
    }
  }
  pe = FBPutSlot(fb, c->slot, &cv);
  if (pe != PSE_NO_ERROR) {
    snprintf(why, n, "column '%s' (slot '%s'): %s", c->column, c->slot,
             refusal(pe));
    return -1;
  }
  return 0;
mismatch:
  snprintf(why, n, "column '%s' (slot '%s'): expected %s, got %s", c->column,
           c->slot, clpd_orm_clips_type_name(c->clips_type),
           db_type_name(v->type));
  return -1;
}

/* The wf_id of a row as a log line shows it: control bytes as '?', at
 * most SHOWN_MAX bytes; "" when it is NULL. */
static void row_instance(const clpd_db_value *v, char *out, size_t n) {
  size_t i, len;
  out[0] = '\0';
  switch (v->type) {
  case CLPD_DB_INTEGER:
    snprintf(out, n, "%lld", (long long)v->integer);
    return;
  case CLPD_DB_FLOAT:
    snprintf(out, n, "%.17g", v->real);
    return;
  case CLPD_DB_TEXT:
  case CLPD_DB_BLOB:
    len = v->size < n - 1 ? v->size : n - 1;
    for (i = 0; i < len; i++) {
      unsigned char ch = ((const unsigned char *)v->data)[i];
      out[i] = ch < 0x20 || ch == 0x7f ? '?' : (char)ch;
    }
    out[len] = '\0';
    return;
  default:
    return;
  }
}

/* One row as a fact of d; 0 asserted, -1 rejected (logged). */
static int assert_row(Environment *env, const clpd_package *pkg,
                      const clpd_orm_template *t, Deftemplate *d,
                      const clpd_db_result *r, size_t row, FILE *log) {
  char why[512], wf[SHOWN_MAX + 1];
  Defmodule *saved;
  FactBuilder *fb;
  GCBlock gcb;
  size_t i;
  int rc = 0;
  why[0] = '\0';
  /* CreateFactBuilder takes a name: the package module (which imports
   * nothing) resolves it to exactly d. */
  saved = GetCurrentModule(env);
  SetCurrentModule(env, pkg->module);
  fb = CreateFactBuilder(env, DeftemplateName(d));
  SetCurrentModule(env, saved);
  if (fb == NULL || fb->fbDeftemplate != d) {
    FBDispose(fb);
    snprintf(why, sizeof why, "CLIPS cannot build a fact of the template");
    rc = -1;
  } else {
    GCBlockStart(env, &gcb);
    for (i = 0; rc == 0 && i < t->column_count; i++)
      rc = put_column(env, fb, d, &t->columns[i],
                      clpd_db_result_value(r, row, i), why, sizeof why);
    if (rc == 0 && (FBAssert(fb) == NULL || FBError(env) != FBE_NO_ERROR)) {
      snprintf(why, sizeof why, "CLIPS could not assert the fact");
      rc = -1;
    }
    FBDispose(fb);
    GCBlockEnd(env, &gcb);
  }
  if (rc != 0) {
    const clpd_db_value *id = clpd_db_result_value(r, row, t->key_columns[0]);
    if (id->type == CLPD_DB_NULL) {
      fprintf(log, "clpdaemon: load: %s::%s: table '%s': row rejected: %s\n",
              pkg->manifest.name, t->name, t->table, why);
    } else {
      row_instance(id, wf, sizeof wf);
      fprintf(log, "clpdaemon: load: %s::%s: table '%s': instance '%s': row "
                   "rejected: %s\n",
              pkg->manifest.name, t->name, t->table, wf, why);
    }
    fflush(log);
  }
  return rc;
}

clpd_db_status clpd_orm_load(Environment *env, clpd_db *db,
                             clpd_orm_syncer *sync, const clpd_package *pkg,
                             size_t tpl, const char *schema,
                             const clpd_orm_filter *filters, size_t nf,
                             FILE *log, size_t *loaded) {
  const clpd_orm_template *t = &pkg->orm->templates[tpl];
  Deftemplate *d = pkg->orm_templates[tpl];
  clpd_db_value *params = xrealloc(NULL, (nf + 1) * sizeof *params);
  clpd_db_result *r = NULL;
  clpd_db_status st;
  size_t i, np = 0, k;
  sbuf sql = {NULL, 0, 0};
  char number[24];
  *loaded = 0;
  put(&sql, "SELECT ");
  for (i = 0; i < t->column_count; i++) {
    if (i > 0) put(&sql, ", ");
    put_ident(&sql, t->columns[i].column);
  }
  put(&sql, " FROM ");
  put(&sql, schema);
  put_ident(&sql, t->table);
  for (i = 0; i < nf; i++) {
    put(&sql, i == 0 ? " WHERE " : " AND ");
    put_ident(&sql, t->columns[filters[i].column].column);
    if (filters[i].value.type == CLPD_DB_NULL) {
      put(&sql, " IS NULL");
      continue;
    }
    params[np++] = filters[i].value;
    snprintf(number, sizeof number, " = $%zu", np);
    put(&sql, number);
  }
  for (k = 0; k <= t->key.count; k++) {
    put(&sql, k == 0 ? " ORDER BY " : ", ");
    put_ident(&sql, t->columns[t->key_columns[k]].column);
  }
  st = clpd_db_exec(db, sql.s, params, np, &r);
  free(sql.s);
  free(params);
  if (st != CLPD_DB_OK) return st;
  /* Echo suppression: the facts mirror stored rows (src/orm_sync.h). */
  clpd_orm_sync_suspend(sync);
  for (i = 0; i < clpd_db_result_rows(r); i++)
    if (assert_row(env, pkg, t, d, r, i, log) == 0) (*loaded)++;
  clpd_orm_sync_resume(sync);
  clpd_db_result_free(r);
  return CLPD_DB_OK;
}

/* 1 when the table of t is mapped by a template of one of the packages
 * done[0..ndone). */
static int table_done(const clpd_orm_template *t,
                      const clpd_package *const *done, size_t ndone) {
  size_t i, j;
  for (i = 0; i < ndone; i++)
    for (j = 0; done[i]->orm != NULL && j < done[i]->orm->template_count; j++)
      if (strcmp(done[i]->orm->templates[j].table, t->table) == 0) return 1;
  return 0;
}

/* The filter value of the wf-id column c for the instance text wf: TEXT
 * for a string or symbol column, the number for an integer or float one.
 * Returns 0, or -1 when wf is no such number. */
static int wf_value(const clpd_orm_column *c, const char *wf,
                    clpd_db_value *out) {
  char *end;
  switch (c->clips_type) {
  case CLPD_ORM_INTEGER: {
    long long v;
    errno = 0;
    v = strtoll(wf, &end, 10);
    if (*wf == '\0' || *end != '\0' || errno != 0) return -1;
    *out = clpd_db_int(v);
    return 0;
  }
  case CLPD_ORM_FLOAT: {
    double v = strtod(wf, &end);
    if (*wf == '\0' || *end != '\0' || !isfinite(v)) return -1;
    *out = clpd_db_float(v);
    return 0;
  }
  default:
    *out = clpd_db_text(wf);
    return 0;
  }
}

clpd_db_status clpd_orm_load_instance(Environment *env, clpd_db *db,
                                      clpd_orm_syncer *sync,
                                      const clpd_package *pkg,
                                      const char *schema, const char *wf,
                                      const clpd_package *const *done,
                                      size_t ndone, FILE *log,
                                      size_t *loaded,
                                      const clpd_orm_template **failed) {
  size_t j, n;
  clpd_db_status st;
  *loaded = 0;
  if (pkg->orm == NULL || pkg->orm_templates == NULL) return CLPD_DB_OK;
  for (j = 0; j < pkg->orm->template_count; j++) {
    const clpd_orm_template *t = &pkg->orm->templates[j];
    clpd_orm_filter f;
    if (table_done(t, done, ndone)) continue;
    f.column = t->key_columns[0];
    if (wf_value(&t->columns[f.column], wf, &f.value) != 0) continue;
    st = clpd_orm_load(env, db, sync, pkg, j, schema, &f, 1, log, &n);
    if (st != CLPD_DB_OK) {
      if (failed != NULL) *failed = t;
      return st;
    }
    *loaded += n;
  }
  return CLPD_DB_OK;
}
