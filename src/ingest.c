#include "ingest.h"

#include "cstrnchk.h" /* ConstraintCheckDataObject: not in clips.h */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

const clpd_json_limits clpd_ingest_limits = {.max_bytes = 65536,
                                              .max_depth = 4,
                                              .max_string = 8192,
                                              .max_elements = 1024,
                                              .max_nodes = 4096};

/* --- queue ------------------------------------------------------------- */

typedef struct {
  char *text;
  size_t len;
  const void *origin;
} item;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static item ring[CLPD_INGEST_QUEUE_MAX];
static size_t head, count; /* guarded by lock, like the notify fields */
static clpd_ingest_notify_fn *notify_fn;
static void *notify_ctx;

int clpd_ingest_post(const void *origin, const char *json, size_t len) {
  clpd_json_error jerr;
  clpd_json *doc;
  char *copy;

  if (json == NULL ||
      (doc = clpd_json_parse(json, len, &clpd_ingest_limits, &jerr)) == NULL) {
    errno = EINVAL;
    return -1;
  }
  clpd_json_free(doc);
  copy = malloc(len + 1);
  if (copy == NULL) {
    errno = ENOMEM;
    return -1;
  }
  memcpy(copy, json, len);
  copy[len] = '\0';
  pthread_mutex_lock(&lock);
  if (count == CLPD_INGEST_QUEUE_MAX) {
    pthread_mutex_unlock(&lock);
    free(copy);
    errno = EAGAIN;
    return -1;
  }
  ring[(head + count) % CLPD_INGEST_QUEUE_MAX].text = copy;
  ring[(head + count) % CLPD_INGEST_QUEUE_MAX].len = len;
  ring[(head + count) % CLPD_INGEST_QUEUE_MAX].origin = origin;
  count++;
  if (notify_fn != NULL) notify_fn(notify_ctx);
  pthread_mutex_unlock(&lock);
  return 0;
}

size_t clpd_ingest_queue_length(void) {
  size_t n;
  pthread_mutex_lock(&lock);
  n = count;
  pthread_mutex_unlock(&lock);
  return n;
}

void clpd_ingest_set_notify(clpd_ingest_notify_fn *fn, void *ctx) {
  pthread_mutex_lock(&lock);
  notify_fn = fn;
  notify_ctx = ctx;
  if (fn != NULL && count > 0) fn(ctx);
  pthread_mutex_unlock(&lock);
}

/* Takes the oldest queued fact; 0 when the queue is empty. */
static int take(item *out) {
  int got = 0;
  pthread_mutex_lock(&lock);
  if (count > 0) {
    *out = ring[head];
    head = (head + 1) % CLPD_INGEST_QUEUE_MAX;
    count--;
    got = 1;
  }
  pthread_mutex_unlock(&lock);
  return got;
}

/* The oldest queued fact without taking it; 0 when the queue is empty.
 * Only the engine thread takes facts, and posts only add after the last
 * one, so the head stays the same until pop(). */
static int peek(item *out) {
  int got = 0;
  pthread_mutex_lock(&lock);
  if (count > 0) {
    *out = ring[head];
    got = 1;
  }
  pthread_mutex_unlock(&lock);
  return got;
}

/* Removes the oldest queued fact (the one peek() returned). */
static void pop(void) {
  pthread_mutex_lock(&lock);
  if (count > 0) {
    head = (head + 1) % CLPD_INGEST_QUEUE_MAX;
    count--;
  }
  pthread_mutex_unlock(&lock);
}

size_t clpd_ingest_clear(void) {
  size_t dropped = 0;
  item it;
  while (take(&it)) {
    free(it.text);
    dropped++;
  }
  return dropped;
}

/* --- validation -------------------------------------------------------- */

#define SHOWN_MAX CLPD_INGEST_SHOWN_MAX

const char *clpd_ingest_shown(const char *name, char buf[SHOWN_MAX + 4]) {
  size_t n = strlen(name), i;
  if (n > SHOWN_MAX) {
    n = SHOWN_MAX;
    while (n > 0 && ((unsigned char)name[n] & 0xC0) == 0x80) n--;
  }
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)name[i];
    buf[i] = c < 0x20 || c == 0x7f ? '?' : (char)c;
  }
  strcpy(buf + n, strlen(name) > n ? "..." : "");
  return buf;
}

typedef struct {
  Environment *env;
  char *msg;
  size_t n;
  char slot[SHOWN_MAX + 4]; /* shown name of the slot being checked */
} ctx;

static int fail(ctx *c, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(c->msg, c->n, fmt, ap);
  va_end(ap);
  return -1;
}

/* A symbol that reads as one token, not a variable: non-empty, no space,
 * control or delimiter byte ("()&|<~; and the quote), no leading ? or $?.
 * Number-like text such as "3" or "1e2" is accepted too: it becomes a
 * SYMBOL that prints like a number (docs/ingest.md). */
static int valid_symbol(const char *s) {
  const unsigned char *p = (const unsigned char *)s;
  if (*p == '\0' || *p == '?' || (p[0] == '$' && p[1] == '?')) return 0;
  for (; *p != '\0'; p++)
    if (*p <= 0x20 || *p == 0x7f || strchr("\"()&|<~;", *p) != NULL) return 0;
  return 1;
}

/* Converts one JSON scalar (or {"sym":...}) to a CLIPS value. */
static int scalar(ctx *c, const clpd_json *v, CLIPSValue *out) {
  const clpd_json *sym;
  switch (v->type) {
  case CLPD_JSON_STRING:
    out->lexemeValue = CreateString(c->env, v->u.str.s);
    return 0;
  case CLPD_JSON_INT:
    out->integerValue = CreateInteger(c->env, v->u.i);
    return 0;
  case CLPD_JSON_FLOAT:
    out->floatValue = CreateFloat(c->env, v->u.f);
    return 0;
  case CLPD_JSON_TRUE:
  case CLPD_JSON_FALSE:
    return fail(c, "slot '%s': booleans are not allowed", c->slot);
  case CLPD_JSON_NULL:
    return fail(c, "slot '%s': null is not allowed", c->slot);
  case CLPD_JSON_ARRAY:
    return fail(c, "slot '%s': nested arrays are not allowed", c->slot);
  case CLPD_JSON_OBJECT:
    sym = clpd_json_get(v, "sym");
    if (v->u.obj.count != 1 || sym == NULL || sym->type != CLPD_JSON_STRING)
      return fail(c, "slot '%s': expected {\"sym\":\"<symbol>\"} for an "
                     "object", c->slot);
    if (!valid_symbol(sym->u.str.s))
      return fail(c, "slot '%s': invalid symbol", c->slot);
    out->lexemeValue = CreateSymbol(c->env, sym->u.str.s);
    return 0;
  }
  return fail(c, "slot '%s': unsupported value", c->slot);
}

static TemplateSlot *find_slot(Deftemplate *t, const char *name) {
  TemplateSlot *s;
  for (s = t->slotList; s != NULL; s = s->next)
    if (strcmp(s->slotName->contents, name) == 0) return s;
  return NULL;
}

/* The deftemplate of an ingest list named `name`, or NULL. */
static Deftemplate *ingestible(const clpd_packages *p, const char *name) {
  size_t i, j;
  for (i = 0; p != NULL && i < p->count; i++)
    for (j = 0; p->items[i].ingest != NULL &&
                j < p->items[i].manifest.ingest.count; j++)
      if (strcmp(p->items[i].manifest.ingest.items[j], name) == 0)
        return p->items[i].ingest[j];
  return NULL;
}

/* Checks the value against the slot constraints (FBPutSlot checks a
 * multifield only as a whole, not its fields nor its cardinality). */
static int check_constraints(ctx *c, TemplateSlot *ts, CLIPSValue *cv) {
  UDFValue u;
  memset(&u, 0, sizeof u);
  u.value = cv->value;
  u.begin = 0;
  u.range = ts->multislot ? cv->multifieldValue->length : 1;
  switch (ConstraintCheckDataObject(c->env, &u, ts->constraints)) {
  case NO_VIOLATION: return 0;
  case TYPE_VIOLATION:
    return fail(c, "slot '%s': value type not allowed by the slot", c->slot);
  case RANGE_VIOLATION:
    return fail(c, "slot '%s': value outside the slot's range", c->slot);
  case ALLOWED_VALUES_VIOLATION:
    return fail(c, "slot '%s': value not among the slot's allowed values",
                c->slot);
  case CARDINALITY_VIOLATION:
    return fail(c, "slot '%s': value count outside the slot's cardinality",
                c->slot);
  default:
    return fail(c, "slot '%s': value rejected by the slot's constraints",
                c->slot);
  }
}

/* Puts one slot member into fb after the shape checks; the value put in
 * *put (when not NULL). */
static int put_slot(ctx *c, FactBuilder *fb, TemplateSlot *ts,
                    const clpd_json *v, CLIPSValue *put) {
  CLIPSValue cv;
  if (!ts->multislot) {
    if (v->type == CLPD_JSON_ARRAY)
      return fail(c, "slot '%s': array for a single-field slot", c->slot);
    if (scalar(c, v, &cv) != 0) return -1;
  } else {
    MultifieldBuilder *mb;
    size_t i;
    if (v->type != CLPD_JSON_ARRAY)
      return fail(c, "slot '%s': expected an array for a multislot", c->slot);
    mb = CreateMultifieldBuilder(c->env, v->u.arr.count);
    for (i = 0; i < v->u.arr.count; i++) {
      if (scalar(c, &v->u.arr.items[i], &cv) != 0) {
        MBDispose(mb);
        return -1;
      }
      MBAppend(mb, &cv);
    }
    cv.multifieldValue = MBCreate(mb);
    MBDispose(mb);
  }
  if (check_constraints(c, ts, &cv) != 0) return -1;
  if (FBPutSlot(fb, ts->slotName->contents, &cv) != PSE_NO_ERROR)
    return fail(c, "slot '%s': value rejected by the slot's constraints",
                c->slot);
  if (put != NULL) *put = cv;
  return 0;
}

#ifndef NDEBUG
/* 1 when no decoded string or key of v holds a NUL before its end. */
static int no_inner_nul(const clpd_json *v) {
  size_t i;
  switch (v->type) {
  case CLPD_JSON_STRING: return strlen(v->u.str.s) == v->u.str.len;
  case CLPD_JSON_ARRAY:
    for (i = 0; i < v->u.arr.count; i++)
      if (!no_inner_nul(&v->u.arr.items[i])) return 0;
    return 1;
  case CLPD_JSON_OBJECT:
    for (i = 0; i < v->u.obj.count; i++)
      if (strlen(v->u.obj.members[i].key) != v->u.obj.members[i].key_len ||
          !no_inner_nul(&v->u.obj.members[i].value))
        return 0;
    return 1;
  default: return 1;
  }
}
#endif

#define WF_ID "wf-id" /* the instance slot (src/orm.h CLPD_ORM_WF_ID) */

/* The value the wf-id slot of the fact will hold: the one put (has_wf),
 * else the slot's default (into *wf); NULL without a wf-id slot or
 * default. */
static const CLIPSValue *admitted_wf(Deftemplate *tpl, int has_wf,
                                     CLIPSValue *wf) {
  if (has_wf) return wf;
  if (!DeftemplateSlotExistP(tpl, WF_ID) ||
      !DeftemplateSlotDefaultValue(tpl, WF_ID, wf))
    return NULL;
  return wf;
}

/* The admission of the drain (T11d), NULL: none. */
typedef struct {
  clpd_ingest_admit_fn *fn;
  void *ctx;
  int verdict; /* what fn returned; CLPD_INGEST_ASSERT without fn */
} admission;

/* Checks the document and fills a fact builder for its template, then
 * asserts it when do_assert is set, after the admission a (if not NULL)
 * agreed. Order of the checks (docs/ingest.md): JSON, top-level keys in
 * document order, 'template', 'slots', the whitelist, each slot in
 * document order, then the required slots. A fact the admission dropped
 * or stopped returns 1, with a->verdict set. */
static int ingest(Environment *env, const clpd_packages *p, const char *json,
                  size_t len, char *msg, size_t n, int do_assert,
                  admission *a) {
  char tname[SHOWN_MAX + 4];
  clpd_json_error jerr;
  clpd_json *doc;
  const clpd_json *t, *slots;
  Deftemplate *tpl;
  TemplateSlot *ts;
  Defmodule *saved;
  FactBuilder *fb;
  GCBlock gcb;
  ctx c;
  CLIPSValue wf;
  int has_wf = 0;
  size_t i;
  int rc = -1;

  c.env = env;
  c.msg = msg;
  c.n = n;
  msg[0] = '\0';
  doc = clpd_json_parse(json, len, &clpd_ingest_limits, &jerr);
  if (doc == NULL) return fail(&c, "invalid JSON: %s", jerr.message);
  /* Names and values are used as C strings below: the parser never decodes
   * a NUL into a string or key (src/json.h), checked in debug builds. */
  assert(no_inner_nul(doc));
  if (doc->type != CLPD_JSON_OBJECT) {
    clpd_json_free(doc);
    return fail(&c, "expected a JSON object");
  }
  for (i = 0; i < doc->u.obj.count; i++) {
    const char *key = doc->u.obj.members[i].key;
    if (strcmp(key, "template") != 0 && strcmp(key, "slots") != 0) {
      fail(&c, "unknown key '%s'", clpd_ingest_shown(key, tname));
      goto done;
    }
  }
  t = clpd_json_get(doc, "template");
  slots = clpd_json_get(doc, "slots");
  if (t == NULL) {
    fail(&c, "missing key 'template'");
    goto done;
  }
  if (slots == NULL) {
    fail(&c, "missing key 'slots'");
    goto done;
  }
  if (t->type != CLPD_JSON_STRING) {
    fail(&c, "'template' must be a string");
    goto done;
  }
  if (slots->type != CLPD_JSON_OBJECT) {
    fail(&c, "'slots' must be an object");
    goto done;
  }
  clpd_ingest_shown(t->u.str.s, tname);
  tpl = ingestible(p, t->u.str.s);
  if (tpl == NULL) {
    fail(&c, "template '%s' is not ingestible", tname);
    goto done;
  }

  /* CreateFactBuilder takes a name: the package module (which imports
   * nothing) resolves it to exactly tpl. */
  saved = GetCurrentModule(env);
  SetCurrentModule(env, FindDefmodule(env, DeftemplateModule(tpl)));
  fb = CreateFactBuilder(env, DeftemplateName(tpl));
  SetCurrentModule(env, saved);
  if (fb == NULL || fb->fbDeftemplate != tpl) {
    FBDispose(fb);
    fail(&c, "cannot assert the fact");
    goto done;
  }
  GCBlockStart(env, &gcb);
  for (i = 0; i < slots->u.obj.count; i++) {
    const clpd_json_member *m = &slots->u.obj.members[i];
    clpd_ingest_shown(m->key, c.slot);
    if ((ts = find_slot(tpl, m->key)) == NULL) {
      fail(&c, "template '%s' has no slot '%s'", tname, c.slot);
      break;
    }
    if (put_slot(&c, fb, ts, &m->value,
                 strcmp(m->key, WF_ID) == 0 ? &wf : NULL) != 0)
      break;
    if (strcmp(m->key, WF_ID) == 0) has_wf = 1;
  }
  if (i == slots->u.obj.count) {
    for (ts = tpl->slotList; ts != NULL; ts = ts->next)
      if (ts->noDefault && clpd_json_get(slots, ts->slotName->contents) == NULL)
        break;
    if (ts != NULL)
      fail(&c, "slot '%s' requires a value",
           clpd_ingest_shown(ts->slotName->contents, c.slot));
    else if (!do_assert)
      rc = 0;
    else if (a != NULL && a->fn != NULL &&
             (a->verdict = a->fn(a->ctx, admitted_wf(tpl, has_wf, &wf))) !=
                 CLPD_INGEST_ASSERT)
      rc = 1;
    else if (FBAssert(fb) == NULL || FBError(env) != FBE_NO_ERROR)
      fail(&c, "cannot assert the fact");
    else
      rc = 0;
  }
  FBDispose(fb);
  GCBlockEnd(env, &gcb);
done:
  clpd_json_free(doc);
  return rc;
}

int clpd_ingest_check(Environment *env, const clpd_packages *p,
                      const char *json, size_t len, char *msg, size_t n) {
  return ingest(env, p, json, len, msg, n, 0, NULL);
}

int clpd_ingest_assert(Environment *env, const clpd_packages *p,
                       const char *json, size_t len, char *msg, size_t n) {
  return ingest(env, p, json, len, msg, n, 1, NULL);
}

size_t clpd_ingest_drain(Environment *env, const clpd_packages *p,
                         clpd_ingest_reject_fn *reject, void *rctx) {
  size_t asserted = 0;
  clpd_ingest_drain_admit(env, p, reject, rctx, NULL, NULL, &asserted);
  return asserted;
}

int clpd_ingest_drain_admit(Environment *env, const clpd_packages *p,
                            clpd_ingest_reject_fn *reject, void *rctx,
                            clpd_ingest_admit_fn *admit, void *actx,
                            size_t *asserted) {
  char msg[CLPD_INGEST_MESSAGE_MAX];
  size_t todo = clpd_ingest_queue_length(), n = 0;
  admission a;
  int rc;
  item it;
  a.fn = admit;
  a.ctx = actx;
  /* Only the facts queued at the start: a post during the drain waits for
   * the next cycle. */
  while (todo-- > 0 && peek(&it)) {
    a.verdict = CLPD_INGEST_ASSERT;
    rc = ingest(env, p, it.text, it.len, msg, sizeof msg, 1, &a);
    if (rc == 1 && a.verdict == CLPD_INGEST_STOP) {
      if (asserted != NULL) *asserted = n;
      return -1; /* it stays queued, first */
    }
    if (rc == 0)
      n++;
    else if (rc < 0 && reject != NULL)
      reject(rctx, it.origin, msg);
    pop();
    free(it.text);
  }
  if (asserted != NULL) *asserted = n;
  return 0;
}
