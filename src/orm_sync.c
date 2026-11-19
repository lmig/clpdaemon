/* Automatic synchronization of mapped facts (T10b, src/orm_sync.h,
 * docs/orm.md#automatic-synchronization).
 *
 * The hooks record, per fact index, the row the fact had at the last
 * commit (base: none when the fact is newer) and the row it has now (cur:
 * none once it is retracted). A modify retracts and asserts the same fact
 * index (CLIPS 6.4.2 ReplaceFact), so the assert finds the entry its
 * retract left and only the current row changes. Invariant: an entry is
 * linked under its index while it may still pair with an assert of that
 * index; a live fact of a mapped template has a linked entry with a cur
 * row exactly when it changed since the last commit. The flush compares
 * base and cur: none/row INSERT, row/none DELETE, same template and key
 * UPDATE of the columns that differ, otherwise DELETE + INSERT. Values
 * are copied out of the facts, so nothing points into working memory.
 *
 * Rehydration (T10c): while a cycle is open the same hooks also note the
 * instance (the text of the wf-id slot) of every fact asserted or
 * retracted, of any template, with the package of its template. When the
 * cycle is rolled back those instances, and the one the failed write
 * isolated, are evicted: their facts retracted and their recorded changes
 * dropped; the ones still synchronized wait in `pending` until the next
 * cycle starts, which reloads them (src/orm_load.h) before its begin. */
#define _POSIX_C_SOURCE 200809L

#include "orm_sync.h"

#include "log.h"
#include "orm.h"
#include "orm_load.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NAME "clpd-orm-sync"
#define BUCKETS_MIN 64
#define SHOWN_MAX 200 /* bytes of a wf-id shown in a rehydration line */

/* An instance (wf-id text) with a package whose template carried it (NULL:
 * a template of no package). */
typedef struct {
  char *wf;
  const clpd_package *pkg;
} wfpkg;

typedef struct {
  wfpkg *v;
  size_t n, cap;
} wfset;

/* A mapped template whose facts are synchronized (sync = auto). */
typedef struct {
  Deftemplate *d;
  const clpd_package *pkg;
  const clpd_orm_template *t;
  size_t aggregate; /* index of its definition, in package order */
} tmap;

/* One slot value as the fact held it. */
typedef struct {
  unsigned short type; /* CLIPS type code */
  long long i;
  double f;
  char *s; /* SYMBOL, STRING, INSTANCE_NAME contents; NULL otherwise */
} sval;

typedef struct {
  const tmap *m; /* NULL: no row */
  sval *v;       /* m->t->column_count values, in column order */
} row;

typedef struct {
  long long index; /* fact index */
  row base, cur;
  long next; /* next linked entry of the bucket, -1 */
  int linked;
} entry;

struct clpd_orm_syncer {
  Environment *env;
  clpd_db *db;
  tmap *maps;
  size_t map_count;
  size_t aggregates;
  entry *e;
  size_t n, cap;
  long *buckets;
  size_t nb; /* a power of two */
  int suspended;
  int installed; /* 1: assert hook, 2: retract hook */
  char **isolated; /* instances no longer synchronized */
  size_t isolated_n;
  char failed[4096]; /* the instance of the write that failed, or "" */
  const clpd_packages *p;
  int cycle_open; /* between a begin and the end of its cycle */
  wfset touched;  /* what the open cycle asserted or retracted */
  wfset pending;  /* evicted instances to reload when the next cycle starts */
};

static void *xrealloc(void *p, size_t n) {
  void *q = realloc(p, n);
  if (q == NULL) clpd_log_die_errno();
  return q;
}

static char *xstrdup(const char *s);

/* --- instances ---------------------------------------------------------- */

/* Adds (wf, pkg) unless it is there. */
static void wfset_add(wfset *w, const char *wf, const clpd_package *pkg) {
  size_t i;
  for (i = 0; i < w->n; i++)
    if (w->v[i].pkg == pkg && strcmp(w->v[i].wf, wf) == 0) return;
  if (w->n == w->cap) {
    w->cap = w->cap == 0 ? 8 : 2 * w->cap;
    w->v = xrealloc(w->v, w->cap * sizeof *w->v);
  }
  w->v[w->n].wf = xstrdup(wf);
  w->v[w->n++].pkg = pkg;
}

/* Removes every pair of instance wf. */
static void wfset_remove(wfset *w, const char *wf) {
  size_t i, k = 0;
  char *drop = NULL;
  for (i = 0; i < w->n; i++) {
    if (strcmp(w->v[i].wf, wf) == 0 && w->v[i].wf != wf) {
      free(w->v[i].wf);
    } else if (strcmp(w->v[i].wf, wf) == 0) {
      drop = w->v[i].wf; /* wf itself: freed last */
    } else {
      w->v[k++] = w->v[i];
    }
  }
  w->n = k;
  free(drop);
}

static void wfset_clear(wfset *w) {
  while (w->n > 0) free(w->v[--w->n].wf);
}

static void wfset_free(wfset *w) {
  wfset_clear(w);
  free(w->v);
  w->v = NULL;
  w->cap = 0;
}

/* The text of a wf-id value as workflow_instance keys it (wf_text): a
 * symbol, string or instance name as it is, an integer in decimal, a float
 * with %.17g; NULL for any other type. */
static char *value_text(const CLIPSValue *v) {
  char text[32];
  switch (v->header->type) {
  case SYMBOL_TYPE:
  case STRING_TYPE:
  case INSTANCE_NAME_TYPE:
    return xstrdup(v->lexemeValue->contents);
  case INTEGER_TYPE:
    snprintf(text, sizeof text, "%lld", v->integerValue->contents);
    return xstrdup(text);
  case FLOAT_TYPE:
    snprintf(text, sizeof text, "%.17g", v->floatValue->contents);
    return xstrdup(text);
  default:
    return NULL;
  }
}

/* The instance of f (a new string), or NULL when its template has no
 * wf-id slot or the value is no instance. */
static char *fact_wf(Fact *f) {
  CLIPSValue v;
  if (!DeftemplateSlotExistP(FactDeftemplate(f), CLPD_ORM_WF_ID) ||
      GetFactSlot(f, CLPD_ORM_WF_ID, &v) != GSE_NO_ERROR)
    return NULL;
  return value_text(&v);
}

/* The loaded package whose module holds d, or NULL. */
static const clpd_package *package_of(const clpd_orm_syncer *s,
                                      Deftemplate *d) {
  const char *module = DeftemplateModule(d);
  size_t i;
  for (i = 0; module != NULL && i < s->p->count; i++)
    if (s->p->items[i].module != NULL &&
        strcmp(DefmoduleName(s->p->items[i].module), module) == 0)
      return &s->p->items[i];
  return NULL;
}

/* An open cycle (not loading) asserted or retracted f. */
static void track(clpd_orm_syncer *s, Fact *f) {
  char *wf;
  if (!s->cycle_open || s->suspended) return;
  if ((wf = fact_wf(f)) == NULL) return;
  wfset_add(&s->touched, wf, package_of(s, FactDeftemplate(f)));
  free(wf);
}

static int cmp_text(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Retracts, with the recording suspended, every fact whose instance is
 * one of wfs[0..n) (sorted with cmp_text), in one pass over working
 * memory; with pk, adds the instance and package of each such fact. */
static void retract_instances(clpd_orm_syncer *s, char **wfs, size_t n,
                              wfset *pk) {
  Fact *f, **v = NULL;
  size_t i, count = 0, cap = 0;
  if (n == 0) return;
  for (f = GetNextFact(s->env, NULL); f != NULL;
       f = GetNextFact(s->env, f)) {
    char *wf = fact_wf(f), **hit;
    if (wf == NULL) continue;
    hit = bsearch(&wf, wfs, n, sizeof *wfs, cmp_text);
    if (hit != NULL) {
      if (count == cap) {
        cap = cap == 0 ? 16 : 2 * cap;
        v = xrealloc(v, cap * sizeof *v);
      }
      RetainFact(f); /* kept valid until every retract is done */
      v[count++] = f;
      if (pk != NULL) wfset_add(pk, *hit, package_of(s, FactDeftemplate(f)));
    }
    free(wf);
  }
  s->suspended++;
  for (i = 0; i < count; i++) Retract(v[i]);
  s->suspended--;
  for (i = 0; i < count; i++) ReleaseFact(v[i]);
  free(v);
}

/* wf as a log line shows it: control bytes as '?', at most SHOWN_MAX. */
static const char *shown(const char *wf, char out[SHOWN_MAX + 1]) {
  size_t i;
  for (i = 0; wf[i] != '\0' && i < SHOWN_MAX; i++)
    out[i] = (unsigned char)wf[i] < 0x20 || wf[i] == 0x7f ? '?' : wf[i];
  out[i] = '\0';
  return out;
}

/* --- recording ---------------------------------------------------------- */

static const tmap *map_of(const clpd_orm_syncer *s, const Deftemplate *d) {
  size_t i;
  for (i = 0; i < s->map_count; i++)
    if (s->maps[i].d == d) return &s->maps[i];
  return NULL;
}

static size_t bucket_of(const clpd_orm_syncer *s, long long index) {
  return (size_t)(((uint64_t)index * UINT64_C(11400714819323198485)) >> 32) &
         (s->nb - 1);
}

static long find(const clpd_orm_syncer *s, long long index) {
  long pos;
  for (pos = s->buckets[bucket_of(s, index)]; pos >= 0; pos = s->e[pos].next)
    if (s->e[pos].index == index) return pos;
  return -1;
}

static void link_entry(clpd_orm_syncer *s, long pos) {
  size_t b = bucket_of(s, s->e[pos].index);
  s->e[pos].next = s->buckets[b];
  s->e[pos].linked = 1;
  s->buckets[b] = pos;
}

static void unlink_entry(clpd_orm_syncer *s, long pos) {
  long *p = &s->buckets[bucket_of(s, s->e[pos].index)];
  while (*p >= 0 && *p != pos) p = &s->e[*p].next;
  if (*p == pos) *p = s->e[pos].next;
  s->e[pos].next = -1;
  s->e[pos].linked = 0;
}

static void reset_buckets(clpd_orm_syncer *s, size_t nb) {
  size_t i;
  s->buckets = xrealloc(s->buckets, nb * sizeof *s->buckets);
  s->nb = nb;
  for (i = 0; i < nb; i++) s->buckets[i] = -1;
}

/* A new linked entry for index, both rows none. */
static long add_entry(clpd_orm_syncer *s, long long index) {
  long pos;
  size_t i;
  if (s->n == s->cap) {
    s->cap = s->cap == 0 ? 64 : 2 * s->cap;
    s->e = xrealloc(s->e, s->cap * sizeof *s->e);
  }
  if (s->n + 1 > 2 * s->nb) {
    reset_buckets(s, 2 * s->nb);
    for (i = 0; i < s->n; i++)
      if (s->e[i].linked) link_entry(s, (long)i);
  }
  pos = (long)s->n++;
  memset(&s->e[pos], 0, sizeof s->e[pos]);
  s->e[pos].index = index;
  link_entry(s, pos);
  return pos;
}

static void row_free(row *r) {
  size_t i;
  if (r->m != NULL)
    for (i = 0; i < r->m->t->column_count; i++) free(r->v[i].s);
  free(r->v);
  r->m = NULL;
  r->v = NULL;
}

static char *xstrdup(const char *s) {
  size_t n = strlen(s) + 1;
  return memcpy(xrealloc(NULL, n), s, n);
}

/* r = the mapped slots of f. */
static void snapshot(row *r, const tmap *m, Fact *f) {
  size_t i, n = m->t->column_count;
  row_free(r);
  r->m = m;
  r->v = xrealloc(NULL, n * sizeof *r->v);
  memset(r->v, 0, n * sizeof *r->v);
  for (i = 0; i < n; i++) {
    sval *v = &r->v[i];
    CLIPSValue cv;
    if (GetFactSlot(f, m->t->columns[i].slot, &cv) != GSE_NO_ERROR) {
      v->type = VOID_TYPE;
      continue;
    }
    v->type = cv.header->type;
    switch (v->type) {
    case INTEGER_TYPE:
      v->i = cv.integerValue->contents;
      break;
    case FLOAT_TYPE:
      v->f = cv.floatValue->contents;
      break;
    case SYMBOL_TYPE:
    case STRING_TYPE:
    case INSTANCE_NAME_TYPE:
      v->s = xstrdup(cv.lexemeValue->contents);
      break;
    default:
      break;
    }
  }
}

static void wf_text(const row *r, char *out, size_t n);

/* 1 when the wf-id of r is wf. */
static int in_instance(const row *r, const char *wf) {
  char text[4096];
  if (r->m == NULL) return 0;
  wf_text(r, text, sizeof text);
  return strcmp(text, wf) == 0;
}

/* 1 when f belongs to an instance no longer synchronized. */
static int isolated(const clpd_orm_syncer *s, const tmap *m, Fact *f) {
  row r = {NULL, NULL};
  size_t i;
  int hit = 0;
  if (s->isolated_n == 0) return 0;
  snapshot(&r, m, f);
  for (i = 0; !hit && i < s->isolated_n; i++)
    hit = in_instance(&r, s->isolated[i]);
  row_free(&r);
  return hit;
}

static void on_assert(Environment *env, void *fact, void *ctx) {
  clpd_orm_syncer *s = ctx;
  const tmap *m = map_of(s, FactDeftemplate(fact));
  long long index;
  long pos;
  (void)env;
  track(s, fact);
  if (m == NULL) return;
  index = FactIndex(fact);
  pos = find(s, index);
  if (s->suspended) {
    /* The fact mirrors a stored row. An entry of that index belongs to an
     * earlier fact (after a reset): it keeps its rows, unlinked. */
    if (pos >= 0) unlink_entry(s, pos);
    return;
  }
  if (isolated(s, m, fact)) return;
  if (pos < 0) pos = add_entry(s, index);
  snapshot(&s->e[pos].cur, m, fact);
}

static void on_retract(Environment *env, void *fact, void *ctx) {
  clpd_orm_syncer *s = ctx;
  const tmap *m = map_of(s, FactDeftemplate(fact));
  long pos;
  (void)env;
  track(s, fact);
  if (m == NULL) return;
  pos = find(s, FactIndex(fact));
  if (s->suspended) {
    /* Discarded for a reload: what was recorded for it is dropped. */
    if (pos >= 0) {
      row_free(&s->e[pos].base);
      row_free(&s->e[pos].cur);
      unlink_entry(s, pos);
    }
    return;
  }
  if (isolated(s, m, fact)) return;
  if (pos >= 0) {
    row_free(&s->e[pos].cur); /* stays linked: a modify asserts it next */
    return;
  }
  pos = add_entry(s, FactIndex(fact));
  snapshot(&s->e[pos].base, m, fact);
}

/* --- values ------------------------------------------------------------- */

static int same_value(const sval *a, const sval *b) {
  if (a->type != b->type) return 0;
  switch (a->type) {
  case INTEGER_TYPE:
    return a->i == b->i;
  case FLOAT_TYPE:
    return a->f == b->f;
  case SYMBOL_TYPE:
  case STRING_TYPE:
  case INSTANCE_NAME_TYPE:
    return strcmp(a->s, b->s) == 0;
  default:
    return 0; /* never writable: the write reports it */
  }
}

static const char *type_name(unsigned short type) {
  switch (type) {
  case INTEGER_TYPE: return "INTEGER";
  case FLOAT_TYPE: return "FLOAT";
  case SYMBOL_TYPE: return "SYMBOL";
  case STRING_TYPE: return "STRING";
  case INSTANCE_NAME_TYPE: return "INSTANCE-NAME";
  case FACT_ADDRESS_TYPE: return "FACT-ADDRESS";
  case INSTANCE_ADDRESS_TYPE: return "INSTANCE-ADDRESS";
  case EXTERNAL_ADDRESS_TYPE: return "EXTERNAL-ADDRESS";
  case MULTIFIELD_TYPE: return "MULTIFIELD";
  default: return "VOID";
  }
}

/* The parameter for v in column c (docs/orm.md#write-rules): nil in a
 * null nil column is NULL; otherwise the value must have the declared
 * CLIPS type (a string or a symbol for either lexeme type): INTEGER as an
 * integer (a boolean column takes 0 or 1, sent as the text "0" or "1",
 * which PostgreSQL reads as false or true and SQLite stores as 0 or 1),
 * FLOAT as a float, a lexeme as text (bytea: its bytes). Returns 0, or -1
 * with the reason in why. */
static int to_param(const sval *v, const clpd_orm_column *c,
                    clpd_db_value *out, char *why, size_t n) {
  if (v->type == SYMBOL_TYPE && c->null == CLPD_ORM_NULL_NIL &&
      strcmp(v->s, "nil") == 0) {
    *out = clpd_db_null();
    return 0;
  }
  switch (c->clips_type) {
  case CLPD_ORM_INTEGER:
    if (v->type != INTEGER_TYPE) break;
    if (c->sql_type == CLPD_ORM_BOOLEAN) {
      if (v->i != 0 && v->i != 1) {
        snprintf(why, n, "slot '%s' (column '%s'): a boolean column takes 0 "
                         "or 1", c->slot, c->column);
        return -1;
      }
      *out = clpd_db_text(v->i != 0 ? "1" : "0");
      return 0;
    }
    *out = clpd_db_int(v->i);
    return 0;
  case CLPD_ORM_FLOAT:
    if (v->type != FLOAT_TYPE) break;
    *out = clpd_db_float(v->f);
    return 0;
  default: /* string, symbol */
    if (v->type != STRING_TYPE && v->type != SYMBOL_TYPE) break;
    *out = c->sql_type == CLPD_ORM_BYTEA ? clpd_db_blob(v->s, strlen(v->s))
                                         : clpd_db_text(v->s);
    return 0;
  }
  snprintf(why, n, "slot '%s' (column '%s'): expected %s, got %s", c->slot,
           c->column, clpd_orm_clips_type_name(c->clips_type),
           type_name(v->type));
  return -1;
}

static int is_key(const clpd_orm_template *t, size_t col) {
  size_t k;
  for (k = 0; k <= t->key.count; k++)
    if (t->key_columns[k] == col) return 1;
  return 0;
}

static int same_key(const row *a, const row *b) {
  const clpd_orm_template *t = a->m->t;
  size_t k;
  for (k = 0; k <= t->key.count; k++)
    if (!same_value(&a->v[t->key_columns[k]], &b->v[t->key_columns[k]]))
      return 0;
  return 1;
}

/* What an entry writes. */
enum { W_NONE = 0, W_DELETE = 1, W_UPDATE = 2, W_INSERT = 4 };

static int writes(const entry *e) {
  const clpd_orm_template *t;
  size_t i;
  if (e->base.m == NULL) return e->cur.m != NULL ? W_INSERT : W_NONE;
  if (e->cur.m == NULL) return W_DELETE;
  if (e->base.m != e->cur.m || !same_key(&e->base, &e->cur))
    return W_DELETE | W_INSERT;
  t = e->cur.m->t;
  for (i = 0; i < t->column_count; i++)
    if (!is_key(t, i) && !same_value(&e->base.v[i], &e->cur.v[i]))
      return W_UPDATE;
  return W_NONE;
}

/* --- statements --------------------------------------------------------- */

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

/* One flush: the statement text, its parameters, the schema prefix. */
typedef struct {
  clpd_orm_syncer *s;
  FILE *log;
  char schema[CLPD_ORM_TX_SCHEMA_SIZE + 1]; /* "<schema>." or "" */
  sbuf sql;
  clpd_db_value *params;
  size_t np, cap;
  char *why; /* the reason of a value that cannot be written */
  size_t why_n;
  const char *wf; /* the instance of the workflow_instance write */
} flush_ctx;

/* Outcome of a flush step. */
enum { F_OK = 0, F_DB = -1, F_ABORT = -2 };

static void add_param(flush_ctx *fc, clpd_db_value v) {
  if (fc->np == fc->cap) {
    fc->cap = fc->cap == 0 ? 16 : 2 * fc->cap;
    fc->params = xrealloc(fc->params, fc->cap * sizeof *fc->params);
  }
  fc->params[fc->np++] = v;
}

static void start(flush_ctx *fc, const char *verb, const char *table) {
  fc->sql.len = 0;
  if (fc->sql.s != NULL) fc->sql.s[0] = '\0';
  fc->np = 0;
  put(&fc->sql, verb);
  put(&fc->sql, fc->schema);
  put_ident(&fc->sql, table);
}

/* "clpdaemon: sync: <what>: <message>" on the log. */
static void report(flush_ctx *fc, const row *r, const char *message) {
  if (r != NULL)
    fprintf(fc->log, "clpdaemon: sync: %s::%s: table '%s': %s\n",
            r->m->pkg->manifest.name, r->m->t->name, r->m->t->table, message);
  else
    fprintf(fc->log, "clpdaemon: sync: table '" CLPD_ORM_INSTANCE_TABLE
                     "': %s\n", message);
  fflush(fc->log);
}

/* The instance of r (NULL: of the instance row) failed for good. */
static void mark(flush_ctx *fc, const row *r) {
  if (r != NULL) wf_text(r, fc->s->failed, sizeof fc->s->failed);
  else snprintf(fc->s->failed, sizeof fc->s->failed, "%s", fc->wf);
}

/* Adds the parameter of column col of r; F_ABORT (reported) when the value
 * cannot be written. */
static int value_param(flush_ctx *fc, const row *r, size_t col) {
  clpd_db_value v;
  if (to_param(&r->v[col], &r->m->t->columns[col], &v, fc->why, fc->why_n) !=
      0) {
    report(fc, r, fc->why);
    mark(fc, r);
    return F_ABORT;
  }
  add_param(fc, v);
  return F_OK;
}

/* Runs the statement built in fc; F_DB (reported) when it fails. With
 * affected, the number of rows it changed. */
static int run(flush_ctx *fc, const row *r, int64_t *affected) {
  clpd_db_result *res = NULL;
  clpd_db_status st = clpd_db_exec(fc->s->db, fc->sql.s, fc->params, fc->np,
                                   affected != NULL ? &res : NULL);
  if (st != CLPD_DB_OK) {
    report(fc, r, clpd_db_error(fc->s->db));
    if (st != CLPD_DB_LOST) mark(fc, r);
    return F_DB;
  }
  if (affected != NULL) *affected = clpd_db_result_affected(res);
  clpd_db_result_free(res);
  return F_OK;
}

/* " WHERE "<key>" = $n AND ..." with the key values of r. */
static int put_where(flush_ctx *fc, const row *r) {
  const clpd_orm_template *t = r->m->t;
  size_t k;
  int rc;
  for (k = 0; k <= t->key.count; k++) {
    put(&fc->sql, k == 0 ? " WHERE " : " AND ");
    put_ident(&fc->sql, t->columns[t->key_columns[k]].column);
    put(&fc->sql, " = ");
    if ((rc = value_param(fc, r, t->key_columns[k])) != F_OK) return rc;
    put_param(&fc->sql, fc->np);
  }
  return F_OK;
}

static int write_delete(flush_ctx *fc, const row *r) {
  int rc;
  start(fc, "DELETE FROM ", r->m->t->table);
  if ((rc = put_where(fc, r)) != F_OK) return rc;
  return run(fc, r, NULL);
}

static int write_insert(flush_ctx *fc, const row *r) {
  const clpd_orm_template *t = r->m->t;
  size_t i;
  int rc;
  start(fc, "INSERT INTO ", t->table);
  for (i = 0; i < t->column_count; i++) {
    put(&fc->sql, i == 0 ? " (" : ", ");
    put_ident(&fc->sql, t->columns[i].column);
  }
  put(&fc->sql, ") VALUES (");
  for (i = 0; i < t->column_count; i++) {
    if (i > 0) put(&fc->sql, ", ");
    if ((rc = value_param(fc, r, i)) != F_OK) return rc;
    put_param(&fc->sql, fc->np);
  }
  put(&fc->sql, ")");
  return run(fc, r, NULL);
}

/* UPDATE of the non-key columns that differ; when no row has the key
 * (another writer deleted it, or it was loaded without being stored), the
 * row is inserted instead. */
static int write_update(flush_ctx *fc, const entry *e) {
  const row *r = &e->cur;
  const clpd_orm_template *t = r->m->t;
  size_t i, set = 0;
  int64_t affected = 0;
  int rc;
  start(fc, "UPDATE ", t->table);
  for (i = 0; i < t->column_count; i++) {
    if (is_key(t, i) || same_value(&e->base.v[i], &r->v[i])) continue;
    put(&fc->sql, set++ == 0 ? " SET " : ", ");
    put_ident(&fc->sql, t->columns[i].column);
    put(&fc->sql, " = ");
    if ((rc = value_param(fc, r, i)) != F_OK) return rc;
    put_param(&fc->sql, fc->np);
  }
  if ((rc = put_where(fc, r)) != F_OK || (rc = run(fc, r, &affected)) != F_OK)
    return rc;
  return affected == 0 ? write_insert(fc, r) : F_OK;
}

/* The workflow_instance key of a written row: the text of its wf-id. */
static void wf_text(const row *r, char *out, size_t n) {
  const sval *v = &r->v[r->m->t->key_columns[0]];
  if (v->s != NULL)
    snprintf(out, n, "%s", v->s);
  else if (v->type == INTEGER_TYPE)
    snprintf(out, n, "%lld", v->i);
  else
    snprintf(out, n, "%.17g", v->f);
}

/* The instances an aggregate wrote, in the order of the changes. */
typedef struct {
  char **wf;
  const clpd_package **pkg;
  size_t n, cap;
} instances;

static void touch(instances *in, const row *r) {
  char text[4096];
  size_t i;
  wf_text(r, text, sizeof text);
  for (i = 0; i < in->n; i++)
    if (strcmp(in->wf[i], text) == 0) return;
  if (in->n == in->cap) {
    in->cap = in->cap == 0 ? 8 : 2 * in->cap;
    in->wf = xrealloc(in->wf, in->cap * sizeof *in->wf);
    in->pkg = xrealloc(in->pkg, in->cap * sizeof *in->pkg);
  }
  in->wf[in->n] = xstrdup(text);
  in->pkg[in->n++] = r->m->pkg;
}

static void instances_free(instances *in) {
  size_t i;
  for (i = 0; i < in->n; i++) free(in->wf[i]);
  free(in->wf);
  free(in->pkg);
  memset(in, 0, sizeof *in);
}

/* The instance row: inserted on the first write (status active, the
 * definition and version of the package of its first change), updated_at
 * set on every write; both timestamps from the database clock: PostgreSQL
 * now() (the start of the cycle transaction), SQLite the UTC time as
 * YYYY-MM-DDTHH:MM:SS.mmmZ. */
static int write_instance(flush_ctx *fc, const char *wf,
                          const clpd_package *pkg) {
  const char *now = fc->schema[0] != '\0'
                        ? "now()"
                        : "strftime('%Y-%m-%dT%H:%M:%fZ', 'now')";
  start(fc, "INSERT INTO ", CLPD_ORM_INSTANCE_TABLE);
  put(&fc->sql, " (\"wf_id\", \"definition\", \"version\", \"status\", "
                "\"created_at\", \"updated_at\") VALUES ($1, $2, $3, "
                "'active', ");
  put(&fc->sql, now);
  put(&fc->sql, ", ");
  put(&fc->sql, now);
  put(&fc->sql, ") ON CONFLICT (\"wf_id\") DO UPDATE SET \"updated_at\" = "
                "excluded.\"updated_at\"");
  add_param(fc, clpd_db_text(wf));
  add_param(fc, clpd_db_text(pkg->manifest.definition));
  add_param(fc, clpd_db_text(pkg->manifest.version));
  fc->wf = wf;
  return run(fc, NULL, NULL);
}

static int in_aggregate(const row *r, size_t a) {
  return r->m != NULL && r->m->aggregate == a;
}

/* One aggregate: its DELETEs, UPDATEs and INSERTs, each in the order of
 * the changes, then the rows of the instances it wrote, in the order of
 * the changes. */
static int flush_aggregate(flush_ctx *fc, size_t a) {
  clpd_orm_syncer *s = fc->s;
  instances in = {NULL, NULL, 0, 0};
  size_t i;
  int rc = F_OK;
  for (i = 0; rc == F_OK && i < s->n; i++) {
    entry *e = &s->e[i];
    if ((writes(e) & W_DELETE) && in_aggregate(&e->base, a))
      rc = write_delete(fc, &e->base);
  }
  for (i = 0; rc == F_OK && i < s->n; i++) {
    entry *e = &s->e[i];
    if ((writes(e) & W_UPDATE) && in_aggregate(&e->cur, a))
      rc = write_update(fc, e);
  }
  for (i = 0; rc == F_OK && i < s->n; i++) {
    entry *e = &s->e[i];
    if ((writes(e) & W_INSERT) && in_aggregate(&e->cur, a))
      rc = write_insert(fc, &e->cur);
  }
  for (i = 0; rc == F_OK && i < s->n; i++) {
    const entry *e = &s->e[i];
    int w = writes(e);
    if ((w & W_DELETE) && in_aggregate(&e->base, a)) touch(&in, &e->base);
    if ((w & (W_UPDATE | W_INSERT)) && in_aggregate(&e->cur, a))
      touch(&in, &e->cur);
  }
  for (i = 0; rc == F_OK && i < in.n; i++)
    rc = write_instance(fc, in.wf[i], in.pkg[i]);
  instances_free(&in);
  return rc;
}

static int flush(clpd_orm_syncer *s, FILE *log, char *why, size_t n) {
  flush_ctx fc;
  size_t i, a;
  int rc = F_OK;
  s->failed[0] = '\0';
  for (i = 0; i < s->n && writes(&s->e[i]) == W_NONE; i++) {
  }
  if (i == s->n) return F_OK;
  memset(&fc, 0, sizeof fc);
  fc.s = s;
  fc.log = log;
  fc.why = why;
  fc.why_n = n;
  if (strcmp(clpd_db_backend(s->db), "postgresql") == 0) {
    /* Where the DDL functions and workflow_instance put the tables. */
    if (clpd_orm_tx_current_schema(s->db, fc.schema, why, n) != 0) {
      fprintf(log, "clpdaemon: sync: %s\n", why);
      fflush(log);
      return clpd_db_atomic_failed(s->db) ? F_DB : F_ABORT;
    }
    strcat(fc.schema, ".");
  }
  for (a = 0; rc == F_OK && a < s->aggregates; a++) rc = flush_aggregate(&fc, a);
  free(fc.sql.s);
  free(fc.params);
  return rc;
}

/* --- interface ---------------------------------------------------------- */

/* 1 when package i is the first of its definition. */
static int first_of_definition(const clpd_packages *p, size_t i) {
  size_t k;
  for (k = 0; k < i; k++)
    if (strcmp(p->items[k].manifest.definition,
               p->items[i].manifest.definition) == 0)
      return 0;
  return 1;
}

int clpd_orm_sync_attach(Environment *env, clpd_db *db,
                         const clpd_packages *p, clpd_orm_syncer **out) {
  clpd_orm_syncer *s;
  size_t i, j, k, aggregate;
  *out = NULL;
  s = xrealloc(NULL, sizeof *s);
  memset(s, 0, sizeof *s);
  s->env = env;
  s->db = db;
  s->p = p;
  reset_buckets(s, BUCKETS_MIN);
  for (i = 0; i < p->count; i++) {
    const clpd_package *pkg = &p->items[i];
    if (first_of_definition(p, i)) s->aggregates++;
    if (pkg->orm == NULL || pkg->orm_templates == NULL) continue;
    /* The aggregate: the index of the definition, in package order. */
    for (aggregate = 0, k = 0; k < i; k++) {
      if (!first_of_definition(p, k)) continue;
      if (strcmp(p->items[k].manifest.definition,
                 pkg->manifest.definition) == 0)
        break;
      aggregate++;
    }
    for (j = 0; j < pkg->orm->template_count; j++) {
      const clpd_orm_template *t = &pkg->orm->templates[j];
      tmap *m;
      if (t->sync != CLPD_ORM_SYNC_AUTO) continue;
      s->maps = xrealloc(s->maps, (s->map_count + 1) * sizeof *s->maps);
      m = &s->maps[s->map_count++];
      m->d = pkg->orm_templates[j];
      m->pkg = pkg;
      m->t = t;
      m->aggregate = aggregate;
    }
  }
  if (AddAssertFunction(env, NAME, on_assert, 0, s)) s->installed |= 1;
  if (AddRetractFunction(env, NAME, on_retract, 0, s)) s->installed |= 2;
  if (s->installed != 3) {
    clpd_orm_sync_detach(s);
    return -1;
  }
  *out = s;
  return 0;
}

void clpd_orm_sync_clear(clpd_orm_syncer *s) {
  size_t i;
  if (s == NULL) return;
  for (i = 0; i < s->n; i++) {
    row_free(&s->e[i].base);
    row_free(&s->e[i].cur);
  }
  s->n = 0;
  reset_buckets(s, BUCKETS_MIN);
}

void clpd_orm_sync_detach(clpd_orm_syncer *s) {
  if (s == NULL) return;
  if (s->installed & 1) RemoveAssertFunction(s->env, NAME);
  if (s->installed & 2) RemoveRetractFunction(s->env, NAME);
  clpd_orm_sync_clear(s);
  while (s->isolated_n > 0) free(s->isolated[--s->isolated_n]);
  free(s->isolated);
  wfset_free(&s->touched);
  wfset_free(&s->pending);
  free(s->e);
  free(s->buckets);
  free(s->maps);
  free(s);
}

void clpd_orm_sync_suspend(clpd_orm_syncer *s) {
  if (s != NULL) s->suspended++;
}

void clpd_orm_sync_resume(clpd_orm_syncer *s) {
  if (s != NULL && s->suspended > 0) s->suspended--;
}

int clpd_orm_sync_suspended(const clpd_orm_syncer *s) {
  return s != NULL && s->suspended > 0;
}

/* Drops the recorded changes of instance wf. */
static void drop_instance(clpd_orm_syncer *s, const char *wf) {
  size_t i;
  for (i = 0; i < s->n; i++) {
    entry *e = &s->e[i];
    if (!in_instance(&e->base, wf) && !in_instance(&e->cur, wf)) continue;
    row_free(&e->base);
    row_free(&e->cur);
    if (e->linked) unlink_entry(s, (long)i);
  }
}

static int is_isolated(const clpd_orm_syncer *s, const char *wf) {
  size_t i;
  for (i = 0; i < s->isolated_n; i++)
    if (strcmp(s->isolated[i], wf) == 0) return 1;
  return 0;
}

/* Instance wf stops being synchronized: its recorded changes are dropped
 * and later changes of its facts are not recorded. */
static void isolate_instance(clpd_orm_syncer *s, const char *wf,
                             const char *why, FILE *log) {
  if (!is_isolated(s, wf)) {
    s->isolated = xrealloc(s->isolated,
                           (s->isolated_n + 1) * sizeof(char *));
    s->isolated[s->isolated_n++] = xstrdup(wf);
  }
  drop_instance(s, wf);
  fprintf(log, "clpdaemon: sync: instance '%s' is no longer synchronized: "
               "%s\n", wf, why);
  fflush(log);
}

/* After a rolled-back cycle: the instance whose write failed for good, if
 * any, is isolated; then every instance the cycle touched, and that one,
 * is evicted (T10c): its facts retracted (mapped or not) and its recorded
 * changes dropped; an isolated one is not reloaded (logged), the others
 * wait in pending, with the packages whose templates carried them, for
 * the next cycle's start. */
static void rolled_back(clpd_orm_syncer *s, const char *why, FILE *log) {
  char shown_wf[SHOWN_MAX + 1], **wfs;
  wfset ev = {NULL, 0, 0}, pk = {NULL, 0, 0};
  size_t i, n = 0;
  if (s == NULL) return;
  s->cycle_open = 0;
  for (i = 0; i < s->touched.n; i++)
    wfset_add(&ev, s->touched.v[i].wf, s->touched.v[i].pkg);
  wfset_clear(&s->touched);
  if (s->failed[0] != '\0') {
    wfset_add(&ev, s->failed, NULL);
    isolate_instance(s, s->failed, why, log);
    s->failed[0] = '\0';
  }
  if (ev.n == 0) return;
  /* The distinct instances, sorted for one pass over working memory. */
  wfs = xrealloc(NULL, ev.n * sizeof *wfs);
  for (i = 0; i < ev.n; i++) {
    size_t k;
    for (k = 0; k < n && strcmp(wfs[k], ev.v[i].wf) != 0; k++) {
    }
    if (k == n) wfs[n++] = ev.v[i].wf;
  }
  qsort(wfs, n, sizeof *wfs, cmp_text);
  for (i = 0; i < ev.n; i++) wfset_add(&pk, ev.v[i].wf, ev.v[i].pkg);
  retract_instances(s, wfs, n, &pk);
  free(wfs);
  /* In the order the cycle touched them. */
  for (i = 0; i < ev.n; i++) {
    const char *wf = ev.v[i].wf;
    size_t k;
    for (k = 0; k < i && strcmp(ev.v[k].wf, wf) != 0; k++) {
    }
    if (k < i) continue; /* done with its first pair */
    drop_instance(s, wf);
    if (is_isolated(s, wf)) {
      fprintf(log, "clpdaemon: rehydrate: instance '%s' is not reloaded: it "
                   "is no longer synchronized\n",
              shown(wf, shown_wf));
      fflush(log);
      continue;
    }
    for (k = 0; k < pk.n; k++)
      if (strcmp(pk.v[k].wf, wf) == 0 && pk.v[k].pkg != NULL &&
          pk.v[k].pkg->orm != NULL && pk.v[k].pkg->orm_templates != NULL)
        wfset_add(&s->pending, wf, pk.v[k].pkg);
  }
  wfset_free(&ev);
  wfset_free(&pk);
}

/* Reloads the pending instances, in order, outside the cycle transaction
 * (autocommit): their facts retracted first, then every mapped template of
 * each package that carried them (src/orm_load.h). A SELECT that fails
 * with a lost connection ends the attempt (that instance and the rest stay
 * pending, nothing of it loaded); any other failure isolates the instance
 * (its partial load retracted) and the next one goes on. Returns 0, or -1
 * after the cycle transaction line (clpd_orm_tx_fail). */
static int rehydrate(clpd_orm_syncer *s, clpd_orm_tx *tx, FILE *log) {
  char schema[CLPD_ORM_TX_SCHEMA_SIZE + 2] = "", msg[1024],
       shown_wf[SHOWN_MAX + 1], **wfs;
  const clpd_package **done;
  clpd_db_status st;
  size_t i, n = 0;
  if (strcmp(clpd_db_backend(s->db), "postgresql") == 0) {
    if (clpd_orm_tx_schema_status(s->db, schema, msg, sizeof msg, &st) != 0) {
      fprintf(log, "clpdaemon: rehydrate: %s\n", msg);
      fflush(log);
      return clpd_orm_tx_fail(tx, log, st, msg);
    }
    strcat(schema, ".");
  }
  /* What was asserted for them since the eviction goes first. */
  wfs = xrealloc(NULL, s->pending.n * sizeof *wfs);
  for (i = 0; i < s->pending.n; i++) {
    size_t k;
    for (k = 0; k < n && strcmp(wfs[k], s->pending.v[i].wf) != 0; k++) {
    }
    if (k == n) wfs[n++] = s->pending.v[i].wf;
  }
  qsort(wfs, n, sizeof *wfs, cmp_text);
  retract_instances(s, wfs, n, NULL);
  free(wfs);
  done = xrealloc(NULL, s->pending.n * sizeof *done);
  while (s->pending.n > 0) {
    char *wf = s->pending.v[0].wf;
    size_t nd = 0;
    for (i = 0; i < s->pending.n; i++) {
      const clpd_package *pkg = s->pending.v[i].pkg;
      const clpd_orm_template *t = NULL;
      size_t loaded;
      if (strcmp(s->pending.v[i].wf, wf) != 0) continue;
      st = clpd_orm_load_instance(s->env, s->db, s, pkg, schema, wf, done, nd,
                                  log, &loaded, &t);
      if (st == CLPD_DB_OK) {
        done[nd++] = pkg;
        continue;
      }
      snprintf(msg, sizeof msg, "%s", clpd_db_error(s->db));
      fprintf(log, "clpdaemon: rehydrate: instance '%s': %s::%s: table "
                   "'%s': %s\n",
              shown(wf, shown_wf), pkg->manifest.name, t->name, t->table, msg);
      fflush(log);
      retract_instances(s, &wf, 1, NULL);
      if (st == CLPD_DB_LOST) {
        free(done);
        return clpd_orm_tx_fail(tx, log, st, msg);
      }
      isolate_instance(s, wf, msg, log);
      break;
    }
    wfset_remove(&s->pending, wf);
  }
  free(done);
  return 0;
}

int clpd_orm_sync_begin_cycle(clpd_orm_syncer *s, clpd_orm_tx *tx,
                              FILE *log) {
  if (s != NULL && s->pending.n > 0) {
    if (clpd_orm_tx_connect(tx, log) != 0) return -1;
    if (rehydrate(s, tx, log) != 0) return -1;
  }
  if (clpd_orm_tx_begin(tx, log) != 0) return -1;
  if (s != NULL) {
    wfset_clear(&s->touched);
    s->cycle_open = 1;
  }
  return 0;
}

int clpd_orm_sync_end_cycle(clpd_orm_syncer *s, clpd_orm_tx *tx, FILE *log) {
  char why[1024];
  int rc;
  if (s != NULL && tx->db != NULL && !clpd_db_atomic_failed(tx->db) &&
      flush(s, log, why, sizeof why) == F_ABORT) {
    clpd_orm_tx_abort(tx, log, why);
    rolled_back(s, why, log);
    return -1;
  }
  /* After a failed statement (the flush's or an earlier one) the commit
   * returns that failure. */
  rc = clpd_orm_tx_end(tx, log);
  if (rc != 0) {
    snprintf(why, sizeof why, "%s", clpd_db_error(tx->db));
    rolled_back(s, why, log);
  } else if (s != NULL) {
    clpd_orm_sync_clear(s);
    wfset_clear(&s->touched);
    s->cycle_open = 0;
  }
  return rc;
}
