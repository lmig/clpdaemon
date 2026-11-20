/* ORM DML functions (T10d, src/orm_dml.h, docs/orm.md#dml-functions).
 *
 * Each call: the template (clpd_orm_ddl_resolve_template, the DDL
 * functions' caller scope), for a write its sync = manual, the
 * <slot> <value> pairs (the arguments, or the items of the filter
 * multifield) checked against the mapping and turned into typed
 * parameters, then on PostgreSQL the current schema, then one statement.
 * The checks run in that order and the first failure is the message; a
 * refused call sends no statement. */
#define _POSIX_C_SOURCE 200809L

#include "orm_dml.h"

#include "log.h"
#include "orm_load.h"
#include "orm_sync.h"
#include "orm_tx.h"

#include <stdlib.h>
#include <string.h>

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

static void put_param(sbuf *b, size_t number) {
  char text[24];
  snprintf(text, sizeof text, "$%zu", number);
  put(b, text);
}

/* One call of a DML function. */
typedef struct {
  UDFContext *ctx;
  UDFValue *ret;
  const clpd_orm_ddl *ddl;
  const clpd_package *pkg;
  const clpd_orm_template *t;
  size_t tpl; /* index of t in pkg's mapping */
  char name[CLPD_ORM_DDL_NAME_SIZE]; /* the template argument, as shown */
  char schema[CLPD_ORM_TX_SCHEMA_SIZE + 2]; /* "<schema>." or "" */
  clpd_orm_filter *pairs; /* filters, then the slot values */
  size_t nfilters, nvalues;
  unsigned char *given; /* per column: named by a pair of the current list */
} call;

/* "clpdaemon: <udf>: template '<t>': table '<table>'<message>". */
#define FAIL(c, ...)                                                       \
  clpd_orm_ddl_fail((c)->ctx, (c)->ret, (c)->name, (c)->t, __VA_ARGS__)

static void call_free(call *c) {
  free(c->pairs);
  free(c->given);
}

/* Resolves the template; a write (db-insert, db-update, db-delete) takes
 * only a sync = manual template, so it never races the automatic
 * synchronization. Returns 0 (FALSE in ret, error logged) when it
 * cannot. */
static int begin(call *c, UDFContext *ctx, UDFValue *ret, int write) {
  memset(c, 0, sizeof *c);
  c->ctx = ctx;
  c->ret = ret;
  c->ddl = ctx->context;
  if (!clpd_orm_ddl_resolve_template(ctx, ret, &c->pkg, &c->t, c->name))
    return 0;
  c->tpl = (size_t)(c->t - c->pkg->orm->templates);
  if (write && c->t->sync == CLPD_ORM_SYNC_AUTO) {
    FAIL(c, " has sync = auto: only the automatic synchronization writes "
            "it");
    return 0;
  }
  c->given = xrealloc(NULL, c->t->column_count);
  return 1;
}

/* Copies a slot argument for a message: control bytes as '?', at most
 * CLPD_ORM_DDL_NAME_SIZE - 1 bytes. */
static void shown(const char *s, char out[CLPD_ORM_DDL_NAME_SIZE]) {
  size_t i;
  for (i = 0; s[i] != '\0' && i + 1 < CLPD_ORM_DDL_NAME_SIZE; i++)
    out[i] = (unsigned char)s[i] < 0x20 || s[i] == 0x7f ? '?' : s[i];
  out[i] = '\0';
}

static int is_key(const clpd_orm_template *t, size_t col) {
  size_t k;
  for (k = 0; k <= t->key.count; k++)
    if (t->key_columns[k] == col) return 1;
  return 0;
}

/* What a <slot> <value> list is. */
typedef enum {
  LIST_VALUES,      /* db-insert, the SET of db-update */
  LIST_FILTERS,     /* db-delete, db-load, db-count */
  LIST_KEY_FILTERS, /* db-update: key slots only */
  LIST_SET          /* db-update: no key slot */
} list_kind;

/* Appends the n items of v, <slot> <value> pairs, to c->pairs (filters
 * when kind is a filter list, else values), each value the typed
 * parameter of its column. Returns 0 (FALSE in ret, error logged) at the
 * first item it refuses. */
static int read_pairs(call *c, const CLIPSValue *v, size_t n,
                      list_kind kind) {
  const int filter = kind == LIST_FILTERS || kind == LIST_KEY_FILTERS;
  const char *word = filter ? "filter " : "";
  char slot[CLPD_ORM_DDL_NAME_SIZE], why[512];
  size_t i, base = c->nfilters + c->nvalues;
  if (n % 2 != 0) {
    FAIL(c, ": the %s must come in <slot> <value> pairs",
         filter ? "filters" : "slot values");
    return 0;
  }
  memset(c->given, 0, c->t->column_count);
  c->pairs = xrealloc(c->pairs, (base + n / 2 + 1) * sizeof *c->pairs);
  for (i = 0; i < n; i += 2) {
    const clpd_orm_column *col;
    clpd_orm_filter *p = &c->pairs[base + i / 2];
    unsigned short type = v[i].header->type;
    if (type != SYMBOL_TYPE && type != STRING_TYPE) {
      FAIL(c, ": %sexpected a slot name, got %s", filter ? "filter: " : "",
           clpd_orm_sync_type_name(type));
      return 0;
    }
    shown(v[i].lexemeValue->contents, slot);
    col = clpd_orm_column_of(c->t, v[i].lexemeValue->contents);
    if (col == NULL) {
      FAIL(c, ": %sslot '%s' is not mapped", word, slot);
      return 0;
    }
    p->column = (size_t)(col - c->t->columns);
    if (kind == LIST_KEY_FILTERS && !is_key(c->t, p->column)) {
      FAIL(c, ": filter slot '%s' is not a key slot", slot);
      return 0;
    }
    if (kind == LIST_SET && is_key(c->t, p->column)) {
      FAIL(c, ": slot '%s' is a key slot, which db-update never changes",
           slot);
      return 0;
    }
    if (c->given[p->column]) {
      FAIL(c, ": %sslot '%s' is given twice", word, slot);
      return 0;
    }
    c->given[p->column] = 1;
    if (clpd_orm_sync_param(&v[i + 1], col, &p->value, why, sizeof why) !=
        0) {
      FAIL(c, ": %s%s", word, why);
      return 0;
    }
  }
  if (filter)
    c->nfilters += n / 2;
  else
    c->nvalues += n / 2;
  return 1;
}

/* The pairs of the multifield argument (the filters). */
static int read_filters(call *c, list_kind kind) {
  UDFValue mf;
  if (!UDFNextArgument(c->ctx, MULTIFIELD_BIT, &mf)) {
    c->ret->lexemeValue = FalseSymbol(c->ctx->environment);
    return 0;
  }
  return read_pairs(c, mf.multifieldValue->contents + mf.begin, mf.range,
                    kind);
}

/* The pairs of the remaining arguments (the slot values). */
static int read_values(call *c, list_kind kind) {
  CLIPSValue *v = NULL;
  size_t n = 0;
  int ok;
  while (UDFHasNextArgument(c->ctx)) {
    UDFValue a;
    if (!UDFNextArgument(c->ctx, ANY_TYPE_BITS, &a)) {
      free(v);
      c->ret->lexemeValue = FalseSymbol(c->ctx->environment);
      return 0;
    }
    v = xrealloc(v, (n + 1) * sizeof *v);
    v[n++].value = a.value;
  }
  ok = read_pairs(c, v, n, kind);
  free(v);
  return ok;
}

/* Refuses filters that name neither wf-id nor a local key slot (an empty
 * list included): a delete or a load always selects by instance or key. */
static int need_key_filter(call *c) {
  size_t i;
  for (i = 0; i < c->nfilters; i++)
    if (is_key(c->t, c->pairs[i].column)) return 1;
  FAIL(c, ": the filters must name wf-id or a key slot");
  return 0;
}

/* PostgreSQL: c->schema = the quoted current_schema() and '.', as the
 * DDL functions and the synchronization name the tables; SQLite "". */
static int read_schema(call *c) {
  clpd_db *db = c->ddl->db;
  char msg[512];
  clpd_db_status st;
  if (strcmp(clpd_db_backend(db), "postgresql") != 0) return 1;
  if (clpd_orm_tx_schema_status(db, c->schema, msg, sizeof msg, &st) != 0) {
    FAIL(c, ": %s", msg);
    return 0;
  }
  strcat(c->schema, ".");
  return 1;
}

/* " WHERE "<column>" = $n AND "<column>" IS NULL ..." with the filters in
 * argument order; their non-NULL values appended to params. */
static void put_where(const call *c, sbuf *b, clpd_db_value *params,
                      size_t *np) {
  size_t i;
  for (i = 0; i < c->nfilters; i++) {
    const clpd_orm_filter *f = &c->pairs[i];
    put(b, i == 0 ? " WHERE " : " AND ");
    put_ident(b, c->t->columns[f->column].column);
    if (f->value.type == CLPD_DB_NULL) {
      put(b, " IS NULL");
      continue;
    }
    params[(*np)++] = f->value;
    put(b, " = ");
    put_param(b, *np);
  }
}

/* The text of a wf-id value as workflow_instance and the rehydration key
 * an instance (src/orm_sync.c value_text): text as it is, an integer in
 * decimal, a float with %.17g; NULL for NULL. A new string. */
static char *instance_text(const clpd_db_value *v) {
  char text[32], *s;
  switch (v->type) {
  case CLPD_DB_INTEGER:
    snprintf(text, sizeof text, "%lld", (long long)v->integer);
    break;
  case CLPD_DB_FLOAT:
    snprintf(text, sizeof text, "%.17g", v->real);
    break;
  case CLPD_DB_TEXT:
  case CLPD_DB_BLOB:
    s = xrealloc(NULL, v->size + 1);
    if (v->size > 0) memcpy(s, v->data, v->size);
    s[v->size] = '\0';
    return s;
  default:
    return NULL;
  }
  s = xrealloc(NULL, strlen(text) + 1);
  strcpy(s, text);
  return s;
}

static void touch(const call *c, const clpd_db_value *wf) {
  char *text = instance_text(wf);
  if (text == NULL) return;
  clpd_orm_sync_touch(c->ddl->sync, text, c->pkg);
  free(text);
}

/* Runs the statement; with r, keeps its result. Returns 0 (FALSE in ret,
 * error logged) when the database refuses it. */
static int run(call *c, const char *sql, const clpd_db_value *params,
               size_t np, clpd_db_result **r) {
  clpd_db *db = c->ddl->db;
  if (clpd_db_exec(db, sql, params, np, r) == CLPD_DB_OK) return 1;
  FAIL(c, ": %s", clpd_db_error(db));
  return 0;
}

/* The rows a statement ending in RETURNING "<wf_id>" changed: each row's
 * instance touched; returns their number. */
static long long touch_rows(const call *c, const clpd_db_result *r) {
  size_t i, n = clpd_db_result_rows(r);
  for (i = 0; i < n; i++) touch(c, clpd_db_result_value(r, i, 0));
  return (long long)n;
}

static void put_returning(const call *c, sbuf *b) {
  put(b, " RETURNING ");
  put_ident(b, c->t->columns[c->t->key_columns[0]].column);
}

/* (db-insert <tpl> <slot> <value> ...): INSERT of the given columns, in
 * mapping order; every key slot needs a value, an omitted slot gets the
 * column's default (NULL without one). */
static void insert(Environment *env, UDFContext *ctx, UDFValue *ret) {
  call c;
  sbuf sql = {NULL, 0, 0};
  clpd_db_value *params = NULL;
  size_t i, k, np = 0;
  int ok = 0;
  if (!begin(&c, ctx, ret, 1)) {
    call_free(&c);
    return;
  }
  if (!read_values(&c, LIST_VALUES)) goto done;
  for (k = 0; k <= c.t->key.count; k++)
    if (!c.given[c.t->key_columns[k]]) {
      FAIL(&c, ": key slot '%s' needs a value",
           c.t->columns[c.t->key_columns[k]].slot);
      goto done;
    }
  if (!read_schema(&c)) goto done;
  params = xrealloc(NULL, (c.nvalues + 1) * sizeof *params);
  put(&sql, "INSERT INTO ");
  put(&sql, c.schema);
  put_ident(&sql, c.t->table);
  for (i = 0; i < c.t->column_count; i++) {
    if (!c.given[i]) continue;
    put(&sql, np == 0 ? " (" : ", ");
    put_ident(&sql, c.t->columns[i].column);
    for (k = 0; c.pairs[k].column != i; k++) {
    }
    params[np++] = c.pairs[k].value;
  }
  put(&sql, ") VALUES (");
  for (i = 1; i <= np; i++) {
    if (i > 1) put(&sql, ", ");
    put_param(&sql, i);
  }
  put(&sql, ")");
  if (run(&c, sql.s, params, np, NULL)) {
    for (k = 0; c.pairs[k].column != c.t->key_columns[0]; k++) {
    }
    touch(&c, &c.pairs[k].value);
    ok = 1;
  }
done:
  free(sql.s);
  free(params);
  call_free(&c);
  if (ok) ret->lexemeValue = TrueSymbol(env);
}

/* (db-update <tpl> (create$ <key filters>) <slot> <value> ...): UPDATE of
 * the given non-key columns, in mapping order, of the rows the key
 * filters select; returns the number of rows changed. */
static void update(Environment *env, UDFContext *ctx, UDFValue *ret) {
  call c;
  sbuf sql = {NULL, 0, 0};
  clpd_db_value *params = NULL;
  clpd_db_result *r = NULL;
  size_t i, k, np = 0;
  if (!begin(&c, ctx, ret, 1)) {
    call_free(&c);
    return;
  }
  /* The documented order: both lists, then the filter rule, then the
   * slot count. */
  if (!read_filters(&c, LIST_KEY_FILTERS) || !read_values(&c, LIST_SET) ||
      !need_key_filter(&c))
    goto done;
  if (c.nvalues == 0) {
    FAIL(&c, ": no slot to set");
    goto done;
  }
  if (!read_schema(&c)) goto done;
  params = xrealloc(NULL, (c.nfilters + c.nvalues + 1) * sizeof *params);
  put(&sql, "UPDATE ");
  put(&sql, c.schema);
  put_ident(&sql, c.t->table);
  for (i = 0; i < c.t->column_count; i++) {
    if (!c.given[i]) continue; /* given: the SET list, read last */
    put(&sql, np == 0 ? " SET " : ", ");
    put_ident(&sql, c.t->columns[i].column);
    put(&sql, " = ");
    for (k = c.nfilters; c.pairs[k].column != i; k++) {
    }
    params[np++] = c.pairs[k].value;
    put_param(&sql, np);
  }
  put_where(&c, &sql, params, &np);
  put_returning(&c, &sql);
  if (run(&c, sql.s, params, np, &r))
    ret->integerValue = CreateInteger(env, touch_rows(&c, r));
done:
  clpd_db_result_free(r);
  free(sql.s);
  free(params);
  call_free(&c);
}

/* (db-delete <tpl> (create$ <filters>)): DELETE of the rows the filters
 * select (at least one on wf-id or a key slot); returns their number. */
static void delete_rows(Environment *env, UDFContext *ctx, UDFValue *ret) {
  call c;
  sbuf sql = {NULL, 0, 0};
  clpd_db_value *params = NULL;
  clpd_db_result *r = NULL;
  size_t np = 0;
  if (!begin(&c, ctx, ret, 1)) {
    call_free(&c);
    return;
  }
  if (!read_filters(&c, LIST_FILTERS) || !need_key_filter(&c) ||
      !read_schema(&c))
    goto done;
  params = xrealloc(NULL, (c.nfilters + 1) * sizeof *params);
  put(&sql, "DELETE FROM ");
  put(&sql, c.schema);
  put_ident(&sql, c.t->table);
  put_where(&c, &sql, params, &np);
  put_returning(&c, &sql);
  if (run(&c, sql.s, params, np, &r))
    ret->integerValue = CreateInteger(env, touch_rows(&c, r));
done:
  clpd_db_result_free(r);
  free(sql.s);
  free(params);
  call_free(&c);
}

/* (db-load <tpl> (create$ <filters>)): the matching rows asserted as
 * facts of the template, without echo (src/orm_load.h); returns the
 * number of facts asserted (a row the loader rejects logs its line and
 * is not counted). */
static void load(Environment *env, UDFContext *ctx, UDFValue *ret) {
  call c;
  size_t loaded = 0;
  if (!begin(&c, ctx, ret, 0)) {
    call_free(&c);
    return;
  }
  if (read_filters(&c, LIST_FILTERS) && need_key_filter(&c) &&
      read_schema(&c)) {
    if (clpd_orm_load(env, c.ddl->db, c.ddl->sync, c.pkg, c.tpl, c.schema,
                      c.pairs, c.nfilters, clpd_log(), &loaded) == CLPD_DB_OK)
      ret->integerValue = CreateInteger(env, (long long)loaded);
    else
      FAIL(&c, ": %s", clpd_db_error(c.ddl->db));
  }
  call_free(&c);
}

/* (db-count <tpl> (create$ <filters>)): SELECT count(*) of the rows the
 * filters select (none: every row of the table). */
static void count(Environment *env, UDFContext *ctx, UDFValue *ret) {
  call c;
  sbuf sql = {NULL, 0, 0};
  clpd_db_value *params = NULL;
  clpd_db_result *r = NULL;
  const clpd_db_value *v;
  size_t np = 0;
  if (!begin(&c, ctx, ret, 0)) {
    call_free(&c);
    return;
  }
  if (!read_filters(&c, LIST_FILTERS) || !read_schema(&c)) goto done;
  params = xrealloc(NULL, (c.nfilters + 1) * sizeof *params);
  put(&sql, "SELECT count(*) FROM ");
  put(&sql, c.schema);
  put_ident(&sql, c.t->table);
  put_where(&c, &sql, params, &np);
  if (!run(&c, sql.s, params, np, &r)) goto done;
  v = clpd_db_result_rows(r) == 1 ? clpd_db_result_value(r, 0, 0) : NULL;
  if (v != NULL && v->type == CLPD_DB_INTEGER)
    ret->integerValue = CreateInteger(env, v->integer);
  else
    FAIL(&c, ": the database returned no INTEGER");
done:
  clpd_db_result_free(r);
  free(sql.s);
  free(params);
  call_free(&c);
}

int clpd_orm_dml_register(Environment *env, clpd_orm_ddl *ddl) {
  if (AddUDF(env, "db-insert", "b", 1, UNBOUNDED, "*;sy", insert,
             "clpd_orm_dml_insert", ddl) != AUE_NO_ERROR ||
      AddUDF(env, "db-update", "bl", 2, UNBOUNDED, "*;sy;m", update,
             "clpd_orm_dml_update", ddl) != AUE_NO_ERROR ||
      AddUDF(env, "db-delete", "bl", 2, 2, "*;sy;m", delete_rows,
             "clpd_orm_dml_delete", ddl) != AUE_NO_ERROR ||
      AddUDF(env, "db-load", "bl", 2, 2, "*;sy;m", load, "clpd_orm_dml_load",
             ddl) != AUE_NO_ERROR ||
      AddUDF(env, "db-count", "bl", 2, 2, "*;sy;m", count,
             "clpd_orm_dml_count", ddl) != AUE_NO_ERROR)
    return -1;
  return 0;
}
