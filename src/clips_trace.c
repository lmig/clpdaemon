#define _POSIX_C_SOURCE 200809L

#include "clips_trace.h"
#include "clock.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

#define NAME "clpd-trace"
/* Logical name of the string builder that renders facts and slot values. */
#define PP_NAME "clpd-trace-pp"

/* Above the console router "fileio" (0) and the string routers (0) so
 * engine output reaches the trace instead of the console; below the CLIPS
 * capture routers (40: dribble, error-capture of construct parsing and
 * check-syntax), which then keep working: dribble copies and passes the
 * text on, the parser captures keep their own diagnostics. The package
 * load capture (50, src/packages.c) is gone before the trace attaches. */
#define ROUTER_PRIORITY 30

enum { STREAM_OUT, STREAM_ERR, STREAM_WRN, STREAM_COUNT };
static const char *const stream_name[STREAM_COUNT] = {
    [STREAM_OUT] = "STDOUT", [STREAM_ERR] = "STDERR", [STREAM_WRN] = "STDWRN"};

typedef struct {
  char *buf;
  size_t len, cap;
} line_buf;

/* Hooks attach installed, so a failed attach or detach removes only those. */
enum {
  HOOK_ROUTER = 1,
  HOOK_BEFORE = 2,
  HOOK_AFTER = 4,
  HOOK_ASSERT = 8,
  HOOK_RETRACT = 16,
  HOOK_MODIFY = 32
};

struct clpd_clips_trace {
  Environment *env;
  clpd_trace *trace;
  int installed; /* HOOK_* bits */
  /* Partial output line per stream, shorter than CLPD_TRACE_LINE_MAX. */
  line_buf pending[STREAM_COUNT];
  /* Scratch output of render_fact and fact_wf_id. Each use resets it, so
   * a result read from it (render_fact returns its contents) is valid only
   * until the next render_fact or fact_wf_id call: callers consume or copy
   * it first (fact_wf_id itself returns a copy). */
  StringBuilder *sb;
  /* wf_id of the firing rule (NULL outside a firing or when unknown). */
  char *rule_wf_id;
  /* The fact being modified, between the two modify callbacks; its nested
   * retract and assert are not traced (L48). The old version is kept in
   * case the modify ends without that fact (a duplicate). CLIPS 6.4.2 calls
   * both callbacks unconditionally (ReplaceFact, tmpltfun.c), and a modify
   * never spans a rule firing boundary or a cycle end, so the state is also
   * cleared there: a modify cut short (for example by a callback of
   * another component) cannot suppress later events. */
  Fact *modifying;
  /* The fact being modified is the core's clock fact (T11b): its modify
   * is not traced at all. */
  int modifying_clock;
  long long old_index;
  char *old_tmpl, *old_text, *old_wf_id;
};

/* Out of memory inside a CLIPS callback, which has no way to report an
 * error: fatal, on the daemon log stream (never into the trace pipe). */
static void *xrealloc(void *p, size_t n) {
  void *q = realloc(p, n);
  if (q == NULL) clpd_log_die_errno();
  return q;
}

static char *xstrdup(const char *s) {
  size_t n;
  if (s == NULL) return NULL;
  n = strlen(s) + 1;
  return memcpy(xrealloc(NULL, n), s, n);
}

/* --- router -------------------------------------------------------------- */

/* Index of a logical name in stream_name, -1 for any other name. CLIPS
 * calls router_write only for names router_query accepted, that is names
 * for which this returns an index. */
static int stream_of(const char *name) {
  if (strcmp(name, STDOUT) == 0) return STREAM_OUT;
  if (strcmp(name, STDERR) == 0) return STREAM_ERR;
  if (strcmp(name, STDWRN) == 0) return STREAM_WRN;
  return -1;
}

/* Writes one output line. While the sink is failing, STDERR and STDWRN
 * lines also go to the daemon log stream, so engine errors stay visible
 * (L52 R4): "clpdaemon: <STDERR|STDWRN>: <text>". */
static void emit_line(clpd_clips_trace *ct, int s, const char *text,
                      size_t len) {
  clpd_trace_output(ct->trace, ct->rule_wf_id, stream_name[s], text, len);
  if (s != STREAM_OUT && clpd_trace_failing(ct->trace)) {
    FILE *log = clpd_log();
    flockfile(log);
    fprintf(log, "clpdaemon: %s: ", stream_name[s]);
    fwrite(text, 1, len, log);
    fputc('\n', log);
    funlockfile(log);
  }
}

static void flush_pending(clpd_clips_trace *ct) {
  int s;
  for (s = 0; s < STREAM_COUNT; s++) {
    if (ct->pending[s].len == 0) continue;
    emit_line(ct, s, ct->pending[s].buf, ct->pending[s].len);
    ct->pending[s].len = 0;
  }
}

static void append(line_buf *b, const char *p, size_t n) {
  if (n == 0) return;
  if (b->len + n > b->cap) {
    size_t cap = b->cap == 0 ? 256 : b->cap;
    while (b->len + n > cap) cap *= 2;
    b->buf = xrealloc(b->buf, cap);
    b->cap = cap;
  }
  memcpy(b->buf + b->len, p, n);
  b->len += n;
}

static bool router_query(Environment *env, const char *name, void *ctx) {
  (void)env;
  (void)ctx;
  return stream_of(name) >= 0;
}

/* Writes the pending line of stream s followed by n bytes of p as one
 * line, and empties the pending line. */
static void emit_joined(clpd_clips_trace *ct, int s, const char *p,
                        size_t n) {
  line_buf *b = &ct->pending[s];
  if (b->len == 0) {
    emit_line(ct, s, p, n);
    return;
  }
  append(b, p, n);
  emit_line(ct, s, b->buf, b->len);
  b->len = 0;
}

/* Emits every complete line; the rest waits for more text or a flush. A
 * line that reaches CLPD_TRACE_LINE_MAX bytes without a newline is written
 * as one event of exactly that size and the rest continues (the fd capture
 * rule), so the pending line stays shorter than the cap. */
static void router_write(Environment *env, const char *name, const char *str,
                         void *ctx) {
  clpd_clips_trace *ct = ctx;
  int s = stream_of(name);
  line_buf *b;
  (void)env;
  if (s < 0) return; /* not reached, see stream_of */
  b = &ct->pending[s];
  for (;;) {
    const char *nl = strchr(str, '\n');
    size_t n = nl != NULL ? (size_t)(nl - str) : strlen(str);
    while (b->len + n >= CLPD_TRACE_LINE_MAX) {
      size_t k = CLPD_TRACE_LINE_MAX - b->len;
      emit_joined(ct, s, str, k);
      str += k;
      n -= k;
    }
    if (nl == NULL) {
      append(b, str, n);
      return;
    }
    emit_joined(ct, s, str, n);
    str = nl + 1;
  }
}

/* --- facts --------------------------------------------------------------- */

/* Renders the fact into ct->sb on one line; see the ct->sb contract. */
static const char *render_fact(clpd_clips_trace *ct, Fact *f) {
  SBReset(ct->sb);
  OpenStringBuilderDestination(ct->env, PP_NAME, ct->sb);
  PrintFact(ct->env, PP_NAME, f, false, false, NULL);
  CloseStringBuilderDestination(ct->env, PP_NAME);
  return ct->sb->contents;
}

/* Copy of the wf-id slot of f: symbol or string contents, a number as
 * CLIPS prints it; NULL for ordered facts, templates without the slot, the
 * symbol nil (the slot default: not set) and other values. Uses ct->sb for
 * numbers, so it invalidates an earlier render_fact result. */
static char *fact_wf_id(clpd_clips_trace *ct, Fact *f) {
  Deftemplate *d = FactDeftemplate(f);
  CLIPSValue v;
  if (d->implied || !DeftemplateSlotExistP(d, "wf-id") ||
      GetFactSlot(f, "wf-id", &v) != GSE_NO_ERROR)
    return NULL;
  switch (v.header->type) {
  case SYMBOL_TYPE:
    if (strcmp(v.lexemeValue->contents, "nil") == 0) return NULL;
    return xstrdup(v.lexemeValue->contents);
  case STRING_TYPE:
    return xstrdup(v.lexemeValue->contents);
  case INTEGER_TYPE:
  case FLOAT_TYPE:
    SBReset(ct->sb);
    OpenStringBuilderDestination(ct->env, PP_NAME, ct->sb);
    WriteCLIPSValue(ct->env, PP_NAME, &v);
    CloseStringBuilderDestination(ct->env, PP_NAME);
    return xstrdup(ct->sb->contents);
  default:
    return NULL;
  }
}

static void trace_fact(clpd_clips_trace *ct, clpd_trace_fact_kind kind,
                       Fact *f) {
  char *wf_id = fact_wf_id(ct, f); /* before render_fact: both use ct->sb */
  clpd_trace_fact(ct->trace, kind, wf_id, FactIndex(f),
                  DeftemplateName(FactDeftemplate(f)), render_fact(ct, f));
  free(wf_id);
}

static void on_assert(Environment *env, void *fact, void *ctx) {
  clpd_clips_trace *ct = ctx;
  (void)env;
  if (fact != ct->modifying) trace_fact(ct, CLPD_TRACE_ASSERT, fact);
}

static void on_retract(Environment *env, void *fact, void *ctx) {
  clpd_clips_trace *ct = ctx;
  (void)env;
  if (fact != ct->modifying) trace_fact(ct, CLPD_TRACE_RETRACT, fact);
}

static void clear_modify(clpd_clips_trace *ct) {
  ct->modifying = NULL;
  ct->modifying_clock = 0;
  free(ct->old_tmpl);
  free(ct->old_text);
  free(ct->old_wf_id);
  ct->old_tmpl = ct->old_text = ct->old_wf_id = NULL;
}

/* Called with (old, NULL) before the change and (NULL, new) after it
 * (tmpltfun.c). The modified fact keeps its address and index; a modify
 * into an existing fact (duplicate) or a failed assert returns another fact
 * or NULL, and the old version was then only retracted. A modify of the
 * clock fact (the core's per-cycle update, or any other) writes nothing,
 * its nested retract and assert included (T11b): it would add one event
 * per cycle. */
static void on_modify(Environment *env, Fact *old, Fact *new_fact, void *ctx) {
  clpd_clips_trace *ct = ctx;
  (void)env;
  if (old != NULL) {
    clear_modify(ct);
    ct->modifying = old;
    if (clpd_clock_is_template(FactDeftemplate(old))) {
      ct->modifying_clock = 1;
      return;
    }
    ct->old_index = FactIndex(old);
    ct->old_tmpl = xstrdup(DeftemplateName(FactDeftemplate(old)));
    ct->old_text = xstrdup(render_fact(ct, old));
    ct->old_wf_id = fact_wf_id(ct, old);
    return;
  }
  if (ct->modifying == NULL) return;
  if (ct->modifying_clock) {
    clear_modify(ct); /* not traced */
    return;
  }
  if (new_fact == ct->modifying)
    trace_fact(ct, CLPD_TRACE_MODIFY, new_fact);
  else
    clpd_trace_fact(ct->trace, CLPD_TRACE_RETRACT, ct->old_wf_id,
                    ct->old_index, ct->old_tmpl, ct->old_text);
  clear_modify(ct);
}

/* --- rules --------------------------------------------------------------- */

/* Sets the rule context: wf_id of the first basis fact that has one. With
 * trace_fire, traces the firing with the fact entities of its basis in
 * pattern order (not CEs have no entity; instances are skipped). Pending
 * partial lines end before the context changes. */
static void start_rule(clpd_clips_trace *ct, Activation *act, int trace_fire) {
  PartialMatch *basis = act->basis;
  Defrule *rule = act->theRule;
  long long *facts = NULL;
  size_t n = 0;
  unsigned short i;

  flush_pending(ct);
  clear_modify(ct);
  free(ct->rule_wf_id);
  ct->rule_wf_id = NULL;
  if (trace_fire && basis->bcount > 0)
    facts = xrealloc(NULL, basis->bcount * sizeof *facts);
  for (i = 0; i < basis->bcount; i++) {
    PatternEntity *e;
    if (basis->binds[i].gm.theMatch == NULL) continue;
    e = basis->binds[i].gm.theMatch->matchingItem;
    if (e == NULL || e->header.type != FACT_ADDRESS_TYPE) continue;
    if (ct->rule_wf_id == NULL) ct->rule_wf_id = fact_wf_id(ct, (Fact *)e);
    if (facts != NULL) facts[n++] = FactIndex((Fact *)e);
  }
  if (trace_fire)
    clpd_trace_fire(ct->trace, ct->rule_wf_id, DefruleModule(rule),
                    DefruleName(rule), facts, n);
  free(facts);
}

static void before_fire(Environment *env, Activation *act, void *ctx) {
  (void)env;
  start_rule(ctx, act, 0);
}

static void before_fire_watched(Environment *env, Activation *act, void *ctx) {
  (void)env;
  start_rule(ctx, act, 1);
}

/* Also called once with NULL by a Run that fired nothing. */
static void after_fire(Environment *env, Activation *act, void *ctx) {
  clpd_clips_trace *ct = ctx;
  (void)env;
  if (act == NULL) return;
  flush_pending(ct);
  clear_modify(ct);
  free(ct->rule_wf_id);
  ct->rule_wf_id = NULL;
}

/* --- attach / detach ----------------------------------------------------- */

/* Removes the hooks attach installed, and only those: a router or callback
 * of the same name that belongs to someone else stays. */
static void remove_hooks(clpd_clips_trace *ct) {
  if (ct->installed & HOOK_ROUTER) DeleteRouter(ct->env, NAME);
  if (ct->installed & HOOK_BEFORE) RemoveBeforeRuleFiresFunction(ct->env, NAME);
  if (ct->installed & HOOK_AFTER) RemoveAfterRuleFiresFunction(ct->env, NAME);
  if (ct->installed & HOOK_ASSERT) RemoveAssertFunction(ct->env, NAME);
  if (ct->installed & HOOK_RETRACT) RemoveRetractFunction(ct->env, NAME);
  if (ct->installed & HOOK_MODIFY) RemoveModifyFunction(ct->env, NAME);
  ct->installed = 0;
}

/* Records hook bit when ok; returns ok. */
static bool installed(clpd_clips_trace *ct, int hook, bool ok) {
  if (ok) ct->installed |= hook;
  return ok;
}

static void free_ct(clpd_clips_trace *ct) {
  int s;
  for (s = 0; s < STREAM_COUNT; s++) free(ct->pending[s].buf);
  clear_modify(ct);
  free(ct->rule_wf_id);
  if (ct->sb != NULL) SBDispose(ct->sb);
  free(ct);
}

int clpd_clips_trace_attach(Environment *env, clpd_trace *t, int watch_facts,
                            int watch_rules, clpd_clips_trace **out) {
  clpd_clips_trace *ct;
  bool ok;

  *out = NULL;
  if (!clpd_trace_active(t)) return 0;
  ct = calloc(1, sizeof *ct);
  if (ct == NULL) return -1;
  ct->env = env;
  ct->trace = t;
  ct->sb = CreateStringBuilder(env, 256);
  ok = ct->sb != NULL &&
       installed(ct, HOOK_ROUTER,
                 AddRouter(env, NAME, ROUTER_PRIORITY, router_query,
                           router_write, NULL, NULL, NULL, ct));
  ok = ok && installed(ct, HOOK_BEFORE,
                       AddBeforeRuleFiresFunction(
                           env, NAME,
                           watch_rules ? before_fire_watched : before_fire, 0,
                           ct));
  ok = ok && installed(ct, HOOK_AFTER,
                       AddAfterRuleFiresFunction(env, NAME, after_fire, 0, ct));
  if (ok && watch_facts)
    ok = installed(ct, HOOK_ASSERT,
                   AddAssertFunction(env, NAME, on_assert, 0, ct)) &&
         installed(ct, HOOK_RETRACT,
                   AddRetractFunction(env, NAME, on_retract, 0, ct)) &&
         installed(ct, HOOK_MODIFY,
                   AddModifyFunction(env, NAME, on_modify, 0, ct));
  if (!ok) {
    remove_hooks(ct);
    free_ct(ct);
    return -1;
  }
  *out = ct;
  return 0;
}

void clpd_clips_trace_flush(clpd_clips_trace *ct) {
  if (ct == NULL) return;
  flush_pending(ct);
  clear_modify(ct);
}

void clpd_clips_trace_detach(clpd_clips_trace *ct) {
  if (ct == NULL) return;
  flush_pending(ct);
  remove_hooks(ct);
  free_ct(ct);
}
