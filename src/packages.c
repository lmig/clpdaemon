#define _POSIX_C_SOURCE 200809L

#include "packages.h"

#include "lint.h"
#include "log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CAPTURE_ROUTER "clpd-load-capture"

/* Text CLIPS writes to its error and warning routers while loading. */
typedef struct {
  char *buf;
  size_t len, cap;
} capture;

static bool capture_query(Environment *env, const char *name, void *ctx) {
  (void)env;
  (void)ctx;
  return strcmp(name, STDERR) == 0 || strcmp(name, STDWRN) == 0;
}

static void append(capture *c, const char *str) {
  size_t n = strlen(str);
  if (c->len + n + 1 > c->cap) {
    size_t cap = c->cap == 0 ? 256 : c->cap;
    char *grown;
    while (c->len + n + 1 > cap) cap *= 2;
    grown = realloc(c->buf, cap);
    if (grown == NULL) {
      clpd_log_die_errno();
    }
    c->buf = grown;
    c->cap = cap;
  }
  memcpy(c->buf + c->len, str, n + 1);
  c->len += n;
}

static void capture_write(Environment *env, const char *name, const char *str,
                          void *ctx) {
  (void)env;
  (void)name;
  append(ctx, str);
}

/* Writes each non-blank captured line as "clpdaemon: <pkg>: clips: <line>". */
static void forward(const capture *c, const char *pkg, FILE *err) {
  const char *p = c->buf, *end;
  if (p == NULL) return;
  while (*p != '\0') {
    const char *q;
    int blank = 1;
    end = strchr(p, '\n');
    if (end == NULL) end = p + strlen(p);
    for (q = p; q < end; q++)
      if (*q != ' ' && *q != '\t' && *q != '\r') blank = 0;
    if (!blank) fprintf(err, "clpdaemon: %s: clips: %.*s\n", pkg, (int)(end - p), p);
    p = *end == '\0' ? end : end + 1;
  }
}

static size_t module_count(Environment *env) {
  size_t n = 0;
  Defmodule *m;
  for (m = GetNextDefmodule(env, NULL); m != NULL; m = GetNextDefmodule(env, m)) n++;
  return n;
}

/* Module guard (T9e, L87; T9f, L88): every construct a package file
 * defines must be in the package's module. While a file loads, the parse
 * function of every construct type is replaced by a hook that runs it and
 * then looks at what it defined, so each construct is checked where CLIPS
 * put it, not only by the current module at the end (a qualified name
 * switches the current module, and a later one can switch it back):
 *
 * - deftemplate, defrule, deffacts, deffunction, defgeneric, defmethod
 *   (its defgeneric), defclass, definstances: one parse defines one
 *   construct, and CLIPS appends a new or redefined construct to the end
 *   of its module's list. The last construct of another module whose list
 *   changed (its first or last construct) is reported; so is the last
 *   construct of the current module when the parse left another module
 *   current (a redefinition can reuse the freed address of the construct
 *   it replaced, so the list alone cannot tell). A construct built while
 *   the parse evaluates an expression goes through its own hook.
 * - defglobal: a redefinition updates the existing global in place, so
 *   the hook marks the globals the statement touches through their
 *   `inScope` bit: AddDefglobal sets it on every global it defines, and
 *   nothing else in CLIPS writes it except UpdateDefglobalScope, which
 *   only the C function GetNextDefglobalInScope runs and which leaves
 *   DefglobalData's LastModuleIndex changed (detected, fail closed). The
 *   hook clears the bit of every global before the parse and restores the
 *   untouched ones after it. The watch flags are not used, so a value
 *   that runs (watch globals) or (unwatch globals) cannot hide a global
 *   or report an untouched one, the flags stay as CLIPS sets them and the
 *   guard prints no ":==" lines; still, a defglobal value that runs watch
 *   or unwatch on globals (or all, or a computed item), or that leaves
 *   WatchGlobals changed, rejects the file (fail closed, L88). A
 *   plugin's C call that sets the flag of one global is not seen; it
 *   cannot affect the check.
 * - defmessage-handler: the handler of a class of another module (also a
 *   system class such as USER, visible from every module) that the parse
 *   added, or whose actions it replaced, is reported. A parsed handler
 *   always has actions (an empty body is the constant FALSE), allocated
 *   before the old ones are freed, so a redefinition always changes the
 *   pointer, also over a handler only declared by its defclass (NULL).
 *
 * Modules created by the file are left to the defmodule check. One load
 * runs at a time, so the hooks find their state in `guard`. */

#define GUARD_HOOKS 16 /* construct types hooked; CLIPS 6.4 has 10 */

typedef enum { DEFINES_LIST, DEFINES_GLOBALS, DEFINES_HANDLERS } guard_kind;

typedef struct {
  Construct *construct;
  bool (*parse)(Environment *, const char *); /* CLIPS's own */
  guard_kind kind;
  const char *type;  /* as messages name it: the construct name */
  unsigned list;     /* module item of the constructs (of the classes for
                        DEFINES_HANDLERS); unused for DEFINES_GLOBALS */
} guard_hook;

/* The watch and unwatch commands, wrapped while a file loads. */
typedef struct {
  FunctionDefinition *function; /* NULL: not defined in this CLIPS */
  void (*call)(Environment *, UDFContext *, UDFValue *); /* CLIPS's own */
} guard_watch;

typedef struct {
  Environment *env;
  Defmodule *home; /* the package module */
  const char *file, *pkg;
  Defmodule **others; /* the modules before the load, without home */
  size_t other_count;
  guard_hook hooks[GUARD_HOOKS];
  size_t hook_count;
  guard_watch watch[2]; /* watch, unwatch */
  capture lines; /* the messages, in definition order */
  int errors;
  int global_depth;  /* defglobal parses running (nested by a build) */
  int watch_changed; /* a defglobal value changed the watch state */
  int watch_written; /* its message, written once per file */
  int scope_written; /* the scope message, written once per file */
  int depth; /* nested parses (a build evaluated by a defglobal) */
  const void **reported; /* by the outermost parse, never twice */
  size_t reported_count, reported_cap;
} module_guard;

static module_guard *guard;

/* calloc for count items; count + 1 so that an empty snapshot (count 0)
 * still gets a non-NULL block, which is never taken for out of memory. */
static void *guard_alloc(size_t count, size_t size) {
  void *p = calloc(count + 1, size);
  if (p == NULL) clpd_log_die_errno();
  return p;
}

/* Index of m in guard->others, or -1 (home, or created by the file). */
static long other_index(const Defmodule *m) {
  size_t i;
  for (i = 0; i < guard->other_count; i++)
    if (guard->others[i] == m) return (long)i;
  return -1;
}

static ConstructHeader *first_item(Defmodule *m, unsigned list) {
  return ((struct defmoduleItemHeader *)GetModuleItem(guard->env, m, list))
      ->firstItem;
}

static ConstructHeader *last_item(Defmodule *m, unsigned list) {
  return ((struct defmoduleItemHeader *)GetModuleItem(guard->env, m, list))
      ->lastItem;
}

/* Writes "clpdaemon: <file>: cannot check the modules of its constructs[
 * (<why>)]" once per flag. */
static void cannot_check(int *written, const char *why) {
  char line[1024];
  if (*written) return;
  *written = 1;
  snprintf(line, sizeof line, "clpdaemon: %s: cannot check the modules of "
           "its constructs%s%s%s\n", guard->file, why != NULL ? " (" : "",
           why != NULL ? why : "", why != NULL ? ")" : "");
  append(&guard->lines, line);
  guard->errors++;
}

/* Writes "clpdaemon: <file>: <type> '<module>::<name>[<suffix>]' is
 * outside the package module <pkg>" once per construct (item). */
static void report(const void *item, const char *type, Defmodule *m,
                   const char *name, const char *suffix) {
  char line[1024];
  size_t i;
  for (i = 0; i < guard->reported_count; i++)
    if (guard->reported[i] == item) return;
  if (guard->reported_count == guard->reported_cap) {
    size_t cap = guard->reported_cap == 0 ? 16 : guard->reported_cap * 2;
    const void **grown = realloc(guard->reported, cap * sizeof *grown);
    if (grown == NULL) clpd_log_die_errno();
    guard->reported = grown;
    guard->reported_cap = cap;
  }
  guard->reported[guard->reported_count++] = item;
  snprintf(line, sizeof line, "clpdaemon: %s: %s '%s::%s%s' is outside the "
           "package module %s\n", guard->file, type, DefmoduleName(m), name,
           suffix, guard->pkg);
  append(&guard->lines, line);
  guard->errors++;
}

static void report_item(const guard_hook *h, ConstructHeader *item) {
  report(item, h->type, item->whichModule->theModule, item->name->contents,
         "");
}

/* What a hooked parse may change, taken before it runs. */
typedef struct {
  /* DEFINES_LIST: the first and last construct of each list of the other
   * modules. */
  ConstructHeader **first, **last;
  /* DEFINES_GLOBALS: the globals whose inScope bit was clear, sorted by
   * address (normally none), WatchGlobals and the scope stamp. */
  Defglobal **out_of_scope;
  size_t out_of_scope_count;
  bool watch_globals;
  long scope_index;
  /* DEFINES_HANDLERS: the classes of the other modules with the actions
   * of each handler. */
  Defclass **classes;
  Expression ***actions;
  unsigned short *handler_count;
  size_t class_count;
} guard_snapshot;

static void snapshot_lists(const guard_hook *h, guard_snapshot *s) {
  size_t i;
  s->first = guard_alloc(guard->other_count, sizeof *s->first);
  s->last = guard_alloc(guard->other_count, sizeof *s->last);
  for (i = 0; i < guard->other_count; i++) {
    s->first[i] = first_item(guard->others[i], h->list);
    s->last[i] = last_item(guard->others[i], h->list);
  }
}

static void compare_lists(const guard_hook *h, const guard_snapshot *s) {
  long current = other_index(GetCurrentModule(guard->env));
  size_t i;
  for (i = 0; i < guard->other_count; i++) {
    Defmodule *m = guard->others[i];
    ConstructHeader *last = last_item(m, h->list);
    if (last != NULL &&
        ((long)i == current || last != s->last[i] ||
         first_item(m, h->list) != s->first[i]))
      report_item(h, last);
  }
}

static void free_lists(guard_snapshot *s) {
  free(s->first);
  free(s->last);
}

/* Calls fn on every global of every module, in module order. */
static void each_global(void (*fn)(Defglobal *, void *), void *ctx) {
  unsigned list = DefglobalData(guard->env)->DefglobalModuleIndex;
  Defmodule *m;
  ConstructHeader *c, *next;
  for (m = GetNextDefmodule(guard->env, NULL); m != NULL;
       m = GetNextDefmodule(guard->env, m))
    for (c = first_item(m, list); c != NULL; c = next) {
      next = c->next;
      fn((Defglobal *)c, ctx);
    }
}

static int by_address(const void *a, const void *b) {
  const Defglobal *x = *(Defglobal *const *)a, *y = *(Defglobal *const *)b;
  return x < y ? -1 : x > y;
}

static void count_out_of_scope(Defglobal *g, void *ctx) {
  if (!g->inScope) ++*(size_t *)ctx;
}

static void clear_scope(Defglobal *g, void *ctx) {
  guard_snapshot *s = ctx;
  if (!g->inScope) s->out_of_scope[s->out_of_scope_count++] = g;
  g->inScope = false;
}

static void snapshot_globals(guard_snapshot *s) {
  size_t n = 0;
  each_global(count_out_of_scope, &n);
  s->out_of_scope = guard_alloc(n, sizeof *s->out_of_scope);
  each_global(clear_scope, s);
  qsort(s->out_of_scope, s->out_of_scope_count, sizeof *s->out_of_scope,
        by_address);
  s->watch_globals = DefglobalData(guard->env)->WatchGlobals;
  s->scope_index = DefglobalData(guard->env)->LastModuleIndex;
}

typedef struct {
  guard_snapshot *s;
  int parsed;
} globals_check;

/* A global with the bit set was defined by the statement: reported when
 * it is in another module. One with the bit clear was not touched: its
 * bit is restored (set, unless it was clear before). */
static void check_global(Defglobal *g, void *ctx) {
  globals_check *k = ctx;
  if (g->inScope) {
    Defmodule *m = g->header.whichModule->theModule;
    if (k->parsed && other_index(m) >= 0) {
      char name[256];
      snprintf(name, sizeof name, "?*%s*", g->header.name->contents);
      report(g, "defglobal", m, name, "");
    }
  } else if (k->s->out_of_scope_count == 0 ||
             bsearch(&g, k->s->out_of_scope, k->s->out_of_scope_count,
                     sizeof g, by_address) == NULL) {
    g->inScope = true;
  }
}

/* Reports the globals the parse defined in another module, also after a
 * failed parse restores the inScope bits, and writes the fail-closed
 * messages (watch state changed, scopes recomputed). */
static void compare_globals(guard_snapshot *s, int parsed) {
  globals_check k;
  if (DefglobalData(guard->env)->WatchGlobals != s->watch_globals)
    guard->watch_changed = 1;
  if (guard->watch_changed)
    cannot_check(&guard->watch_written,
                 "a defglobal value changed the watch state of globals");
  if (DefglobalData(guard->env)->LastModuleIndex != s->scope_index) {
    /* UpdateDefglobalScope ran: the bits say nothing about the
     * statement, and CLIPS's values stay. */
    cannot_check(&guard->scope_written, NULL);
  } else {
    k.s = s;
    k.parsed = parsed;
    each_global(check_global, &k);
  }
  free(s->out_of_scope);
}

/* watch and unwatch while a file loads: inside a defglobal parse, a call
 * whose item is globals or all, or is computed, changes (or may change)
 * the watch state of globals. The command then runs as usual. */
static void guarded_watch(Environment *env, UDFContext *ctx,
                          UDFValue *ret) {
  const guard_watch *w =
      &guard->watch[ctx->theFunction == guard->watch[0].function ? 0 : 1];
  const Expression *item = ctx->lastArg; /* the first argument */
  if (guard->global_depth > 0 && item != NULL &&
      (item->type != SYMBOL_TYPE ||
       strcmp(item->lexemeValue->contents, "globals") == 0 ||
       strcmp(item->lexemeValue->contents, "all") == 0))
    guard->watch_changed = 1;
  w->call(env, ctx, ret);
}

static void snapshot_handlers(const guard_hook *h, guard_snapshot *s) {
  size_t i, n = 0;
  unsigned short k;
  ConstructHeader *c;
  for (i = 0; i < guard->other_count; i++)
    for (c = first_item(guard->others[i], h->list); c != NULL; c = c->next)
      n++;
  s->classes = guard_alloc(n, sizeof *s->classes);
  s->actions = guard_alloc(n, sizeof *s->actions);
  s->handler_count = guard_alloc(n, sizeof *s->handler_count);
  for (n = 0, i = 0; i < guard->other_count; i++)
    for (c = first_item(guard->others[i], h->list); c != NULL;
         c = c->next, n++) {
      Defclass *cls = (Defclass *)c;
      s->classes[n] = cls;
      s->handler_count[n] = cls->handlerCount;
      s->actions[n] = guard_alloc(cls->handlerCount, sizeof **s->actions);
      for (k = 0; k < cls->handlerCount; k++)
        s->actions[n][k] = cls->handlers[k].actions;
    }
  s->class_count = n;
}

/* A handler the parse added (InsertHandlerHeader appends it) or whose
 * actions it replaced (the new actions are parsed while the old ones are
 * still allocated, so their address differs). */
static void compare_handlers(guard_snapshot *s, int parsed) {
  size_t i;
  unsigned short k;
  for (i = 0; i < s->class_count; i++) {
    Defclass *cls = s->classes[i];
    for (k = 0; parsed && k < cls->handlerCount; k++) {
      DefmessageHandler *hnd = &cls->handlers[k];
      char suffix[256];
      if (k < s->handler_count[i] && hnd->actions == s->actions[i][k])
        continue;
      snprintf(suffix, sizeof suffix, " %s %s", hnd->header.name->contents,
               MessageHandlerData(guard->env)->hndquals[hnd->type]);
      /* Keyed by the actions: the handler array moves when it grows. */
      report(hnd->actions, "defmessage-handler",
             cls->header.whichModule->theModule, cls->header.name->contents,
             suffix);
    }
    free(s->actions[i]);
  }
  free(s->classes);
  free(s->actions);
  free(s->handler_count);
}

static bool guarded_parse(Environment *env, const char *source, size_t i) {
  const guard_hook *h = &guard->hooks[i];
  guard_snapshot s;
  bool failed;
  memset(&s, 0, sizeof s);
  if (guard->depth++ == 0) guard->reported_count = 0;
  if (h->kind == DEFINES_LIST) {
    snapshot_lists(h, &s);
  } else if (h->kind == DEFINES_GLOBALS) {
    snapshot_globals(&s);
    guard->global_depth++;
  } else {
    snapshot_handlers(h, &s);
  }
  failed = h->parse(env, source);
  if (h->kind == DEFINES_LIST) {
    if (!failed) compare_lists(h, &s);
    free_lists(&s);
  } else if (h->kind == DEFINES_GLOBALS) {
    guard->global_depth--;
    compare_globals(&s, !failed);
  } else {
    compare_handlers(&s, !failed);
  }
  guard->depth--;
  return failed;
}

#define HOOK(i)                                                        \
  static bool hook_##i(Environment *env, const char *source) {        \
    return guarded_parse(env, source, i);                              \
  }
HOOK(0) HOOK(1) HOOK(2) HOOK(3) HOOK(4) HOOK(5) HOOK(6) HOOK(7)
HOOK(8) HOOK(9) HOOK(10) HOOK(11) HOOK(12) HOOK(13) HOOK(14) HOOK(15)
static bool (*const hooks[GUARD_HOOKS])(Environment *, const char *) = {
    hook_0,  hook_1,  hook_2,  hook_3,  hook_4,  hook_5,  hook_6,  hook_7,
    hook_8,  hook_9,  hook_10, hook_11, hook_12, hook_13, hook_14, hook_15};

/* Hooks every construct type but defmodule, and wraps watch and unwatch.
 * Returns 0, or -1 (nothing hooked) when a type cannot be checked. */
static int guard_install(module_guard *g, Environment *env,
                         const clpd_package *pkg, const char *file) {
  static const char *const watch_names[2] = {"watch", "unwatch"};
  Construct *c;
  Defmodule *m;
  size_t n = 0;
  memset(g, 0, sizeof *g);
  g->env = env;
  g->home = pkg->module;
  g->file = file;
  g->pkg = pkg->manifest.name;
  for (m = GetNextDefmodule(env, NULL); m != NULL; m = GetNextDefmodule(env, m))
    n++;
  g->others = guard_alloc(n, sizeof *g->others);
  for (m = GetNextDefmodule(env, NULL); m != NULL; m = GetNextDefmodule(env, m))
    if (m != g->home) g->others[g->other_count++] = m;
  for (c = ConstructData(env)->ListOfConstructs; c != NULL; c = c->next) {
    guard_hook *h;
    const char *item_name = c->constructName; /* its module item */
    ModuleItem *item;
    if (strcmp(c->constructName, "defmodule") == 0) continue;
    if (g->hook_count == GUARD_HOOKS) {
      free(g->others);
      return -1;
    }
    h = &g->hooks[g->hook_count];
    h->construct = c;
    h->parse = c->parseFunction;
    h->type = c->constructName;
    if (strcmp(item_name, "defglobal") == 0) {
      h->kind = DEFINES_GLOBALS;
    } else {
      h->kind = DEFINES_LIST;
      if (strcmp(item_name, "defmethod") == 0) item_name = "defgeneric";
      if (strcmp(item_name, "defmessage-handler") == 0) {
        h->kind = DEFINES_HANDLERS;
        item_name = "defclass";
      }
      if ((item = FindModuleItem(env, item_name)) == NULL) {
        free(g->others);
        return -1;
      }
      h->list = item->moduleIndex;
    }
    g->hook_count++;
  }
  for (n = 0; n < g->hook_count; n++)
    g->hooks[n].construct->parseFunction = hooks[n];
  for (n = 0; n < 2; n++) {
    g->watch[n].function = FindFunction(env, watch_names[n]);
    if (g->watch[n].function == NULL) continue;
    g->watch[n].call = g->watch[n].function->functionPointer;
    g->watch[n].function->functionPointer = guarded_watch;
  }
  guard = g;
  return 0;
}

static void guard_remove(module_guard *g) {
  size_t i;
  for (i = 0; i < g->hook_count; i++)
    g->hooks[i].construct->parseFunction = g->hooks[i].parse;
  for (i = 0; i < 2; i++)
    if (g->watch[i].function != NULL)
      g->watch[i].function->functionPointer = g->watch[i].call;
  guard = NULL;
  free(g->others);
  free(g->reported);
}

/* Loads one file into the current (package) module. Returns 0 on success. */
static int load_file(Environment *env, clpd_package *pkg, const char *file,
                     FILE *err) {
  capture cap = {NULL, 0, 0};
  size_t modules = module_count(env);
  module_guard g;
  LoadError rc;
  struct stat st;
  int failed = 0;
  FILE *probe = fopen(file, "r");

  /* Load reports nothing for a file it cannot open, and fopen succeeds on a
   * directory, which Load would then read as an empty file. */
  if (probe == NULL) {
    fprintf(err, "clpdaemon: %s: cannot open: %s\n", file, strerror(errno));
    return 1;
  }
  if (fstat(fileno(probe), &st) == 0 && S_ISDIR(st.st_mode)) {
    fclose(probe);
    fprintf(err, "clpdaemon: %s: cannot open: %s\n", file, strerror(EISDIR));
    return 1;
  }
  fclose(probe);
  if (guard_install(&g, env, pkg, file) != 0) {
    fprintf(err, "clpdaemon: %s: cannot check the modules of its constructs\n",
            file);
    return 1;
  }
  if (!AddRouter(env, CAPTURE_ROUTER, 50, capture_query, capture_write, NULL,
                 NULL, NULL, &cap)) {
    guard_remove(&g);
    fprintf(err, "clpdaemon: %s: cannot capture CLIPS messages\n", file);
    return 1;
  }
  rc = Load(env, file);
  DeleteRouter(env, CAPTURE_ROUTER);
  guard_remove(&g);
  forward(&cap, pkg->manifest.name, err);
  free(cap.buf);
  if (rc != LE_NO_ERROR) {
    fprintf(err, "clpdaemon: %s: load failed\n", file);
    failed = 1;
  } else if (module_count(env) != modules) {
    /* A file that defines a defmodule and switches back still grows the
     * module count; the constructs of the new module are not reported. */
    fprintf(err, "clpdaemon: %s: defmodule is not allowed in package files\n",
            file);
    failed = 1;
  } else if (g.errors == 0 && GetCurrentModule(env) != pkg->module) {
    /* Every construct is checked where CLIPS put it, so a file that ends
     * in another module without a guard line defined nothing there: only
     * a statement that names a module and defines nothing does that
     * (`(defglobal MAIN)`). The next file starts in the package module
     * again, but the file is rejected anyway (fail closed, L88). */
    fprintf(err, "clpdaemon: %s: the file ends in module %s, not the package "
                 "module %s\n", file, DefmoduleName(GetCurrentModule(env)),
            pkg->manifest.name);
    failed = 1;
  }
  if (g.lines.buf != NULL) fputs(g.lines.buf, err);
  free(g.lines.buf);
  return failed || g.errors != 0;
}

/* The non-implied deftemplate `name` of the current module, or NULL. */
static Deftemplate *own_template(Environment *env, const char *name) {
  Deftemplate *t;
  for (t = GetNextDeftemplate(env, NULL); t != NULL;
       t = GetNextDeftemplate(env, t))
    if (!t->implied && strcmp(DeftemplateName(t), name) == 0) return t;
  return NULL;
}

/* Resolves the ingest list of the last package of out to its own
 * deftemplates (T7b, docs/ingest.md): a name an earlier package lists, or
 * that is not a deftemplate of this package, is an error. Sets pkg->ingest
 * only when every name resolved. Returns the error count. */
static int check_ingest(Environment *env, clpd_packages *out, FILE *err) {
  clpd_package *pkg = &out->items[out->count - 1];
  const clpd_list *names = &pkg->manifest.ingest;
  Deftemplate **found = calloc(names->count + 1, sizeof *found);
  size_t i, j, k;
  int errors = 0;

  if (found == NULL) clpd_log_die_errno();
  SetCurrentModule(env, pkg->module);
  for (i = 0; i < names->count; i++) {
    const char *other = NULL;
    for (j = 0; j + 1 < out->count && other == NULL; j++)
      for (k = 0; k < out->items[j].manifest.ingest.count; k++)
        if (strcmp(out->items[j].manifest.ingest.items[k], names->items[i]) ==
            0) {
          other = out->items[j].manifest.name;
          break;
        }
    if (other != NULL) {
      fprintf(err, "clpdaemon: %s: ingest names template '%s', already "
                   "listed by %s\n", pkg->manifest.name, names->items[i], other);
      errors++;
    } else if ((found[i] = own_template(env, names->items[i])) == NULL) {
      fprintf(err, "clpdaemon: %s: ingest names unknown template '%s'\n",
              pkg->manifest.name, names->items[i]);
      errors++;
    }
  }
  if (errors == 0)
    pkg->ingest = found;
  else
    free(found);
  return errors;
}

#define ORM_PREFIX "clpdaemon: %s: orm: "

static TemplateSlot *slot_of(Deftemplate *t, const char *name) {
  TemplateSlot *s;
  for (s = t->slotList; s != NULL; s = s->next)
    if (strcmp(s->slotName->contents, name) == 0) return s;
  return NULL;
}

/* 1 when the slot's type constraint allows the CLIPS type (an
 * unconstrained slot allows every type). */
static int type_allowed(Deftemplate *t, const char *slot,
                        clpd_orm_clips_type type) {
  static const char *const symbols[CLPD_ORM_CLIPS_TYPE_COUNT] = {
      [CLPD_ORM_INTEGER] = "INTEGER", [CLPD_ORM_FLOAT] = "FLOAT",
      [CLPD_ORM_STRING] = "STRING", [CLPD_ORM_SYMBOL] = "SYMBOL"};
  CLIPSValue v;
  size_t i;
  if (!DeftemplateSlotTypes(t, slot, &v)) return 0;
  for (i = 0; i < v.multifieldValue->length; i++) {
    CLIPSValue *e = &v.multifieldValue->contents[i];
    if (e->header->type == SYMBOL_TYPE &&
        strcmp(e->lexemeValue->contents, symbols[type]) == 0)
      return 1;
  }
  return 0;
}

/* Checks one mapped template against its deftemplate, in this order: no
 * wf-id, the slot lines in file order (a clips type the sql type cannot
 * hold, unknown slot, a clips type the slot does not allow, null policy
 * nil on a slot that does not allow SYMBOL), key slots that are no slot of
 * the template, then the template's slots (multislot, unmapped). Returns
 * the error count. */
static int bind_template(Deftemplate *d, clpd_orm_template *t,
                         const char *pkg, FILE *err) {
  TemplateSlot *s;
  size_t i;
  int errors = 0, has_wf_id = slot_of(d, CLPD_ORM_WF_ID) != NULL;

  if (!has_wf_id) {
    fprintf(err, ORM_PREFIX "template '%s' has no wf-id slot\n", pkg, t->name);
    errors++;
  }
  for (i = 0; i < t->column_count; i++) {
    const clpd_orm_column *c = &t->columns[i];
    if (!clpd_orm_type_fits(c->sql_type, c->clips_type)) {
      char fit[CLPD_ORM_TYPE_LIST_SIZE];
      fprintf(err, ORM_PREFIX "template '%s': slot '%s': sql type %s cannot "
                   "hold clips type %s (expected %s)\n", pkg, t->name,
              c->slot, clpd_orm_sql_type_name(c->sql_type),
              clpd_orm_clips_type_name(c->clips_type),
              clpd_orm_type_fits_list(c->sql_type, fit, sizeof fit));
      errors++;
    }
    if ((s = slot_of(d, c->slot)) == NULL) {
      if (has_wf_id || strcmp(c->slot, CLPD_ORM_WF_ID) != 0) {
        fprintf(err, ORM_PREFIX "template '%s' has no slot '%s'\n", pkg,
                t->name, c->slot);
        errors++;
      }
      continue;
    }
    if (s->multislot) continue; /* reported with the template's slots */
    if (!type_allowed(d, c->slot, c->clips_type)) {
      fprintf(err, ORM_PREFIX "template '%s': slot '%s' is mapped as %s, "
                   "which its type constraint does not allow\n", pkg, t->name,
              c->slot, clpd_orm_clips_type_name(c->clips_type));
      errors++;
    }
    /* A NULL column becomes the symbol nil in the slot (T10). */
    if (c->null == CLPD_ORM_NULL_NIL &&
        !type_allowed(d, c->slot, CLPD_ORM_SYMBOL)) {
      fprintf(err, ORM_PREFIX "template '%s': slot '%s' has null policy nil, "
                   "but its type constraint does not allow SYMBOL\n", pkg,
              t->name, c->slot);
      errors++;
    }
  }
  for (i = 0; i < t->key.count; i++) {
    if (clpd_orm_column_of(t, t->key.items[i]) == NULL &&
        slot_of(d, t->key.items[i]) == NULL) {
      fprintf(err, ORM_PREFIX "template '%s' has no slot '%s'\n", pkg,
              t->name, t->key.items[i]);
      errors++;
    }
  }
  for (s = d->slotList; s != NULL; s = s->next) {
    if (s->multislot) {
      fprintf(err, ORM_PREFIX "template '%s': slot '%s' is a multislot, "
                   "which cannot be mapped\n", pkg, t->name,
              s->slotName->contents);
      errors++;
    } else if (clpd_orm_column_of(t, s->slotName->contents) == NULL) {
      fprintf(err, ORM_PREFIX "template '%s': slot '%s' is not mapped\n", pkg,
              t->name, s->slotName->contents);
      errors++;
    }
  }
  /* Cannot fail here: wf-id and every key slot are mapped slots. */
  if (errors == 0 && clpd_orm_resolve_key(t) != 0) errors++;
  return errors;
}

/* What a mapping name is: PostgreSQL keeps tables and sequences in one
 * namespace, so the checks compare names of both kinds. */
typedef enum { ORM_TABLE, ORM_SEQUENCE } orm_name_kind;

/* 1 when mapping o names `name` as a table or sequence (kind). check_orm
 * stores a package's orm only after clpd_orm_parse succeeded, and a valid
 * mapping has a table for every template and a name for every sequence,
 * so strcmp never sees NULL. */
static int names(const clpd_orm *o, const char *name, orm_name_kind kind) {
  size_t j;
  if (kind == ORM_SEQUENCE) {
    for (j = 0; j < o->sequence_count; j++)
      if (strcmp(o->sequences[j].name, name) == 0) return 1;
  } else {
    for (j = 0; j < o->template_count; j++)
      if (strcmp(o->templates[j].table, name) == 0) return 1;
  }
  return 0;
}

/* The first of the first n packages of out whose mapping names `name`
 * (kind), skipping the packages of definition `skip` when it is not NULL;
 * or NULL. */
static const char *first_naming(const clpd_packages *out, size_t n,
                                const char *name, orm_name_kind kind,
                                const char *skip) {
  size_t i;
  for (i = 0; i < n; i++) {
    const clpd_package *o = &out->items[i];
    if (o->orm == NULL ||
        (skip != NULL && strcmp(o->manifest.definition, skip) == 0))
      continue;
    if (names(o->orm, name, kind)) return o->manifest.name;
  }
  return NULL;
}

/* The first earlier package of another definition than the last package
 * of out that maps `name` (kind), or NULL. */
static const char *mapped_by_other_definition(const clpd_packages *out,
                                              const char *name,
                                              orm_name_kind kind) {
  return first_naming(out, out->count - 1, name, kind,
                      out->items[out->count - 1].manifest.definition);
}

/* The first earlier package (any definition) that maps `name` (kind), or
 * NULL. */
static const char *named_by_earlier(const clpd_packages *out,
                                    const char *name, orm_name_kind kind) {
  return first_naming(out, out->count - 1, name, kind, NULL);
}

/* Like named_by_earlier, then the last package of out itself. */
static const char *named_by_any(const clpd_packages *out, const char *name,
                                orm_name_kind kind) {
  return first_naming(out, out->count, name, kind, NULL);
}

/* Versions of one definition that share a table map it identically (T9b,
 * L83): t against the first earlier version mapping the same table.
 * Returns the error count. */
static int check_shared_table(const clpd_packages *out,
                              const clpd_orm_template *t, FILE *err) {
  const clpd_package *pkg = &out->items[out->count - 1];
  char what[CLPD_ORM_IDENT_MAX + 16];
  size_t i, j;
  for (i = 0; i + 1 < out->count; i++) {
    const clpd_package *o = &out->items[i];
    if (o->orm == NULL ||
        strcmp(o->manifest.definition, pkg->manifest.definition) != 0)
      continue;
    for (j = 0; j < o->orm->template_count; j++) {
      const clpd_orm_template *ot = &o->orm->templates[j];
      if (strcmp(ot->table, t->table) != 0) continue;
      if (clpd_orm_table_difference(ot, t, what, sizeof what) == 0) return 0;
      fprintf(err, ORM_PREFIX "table '%s' is mapped differently by %s (%s)\n",
              pkg->manifest.name, t->table, o->manifest.name, what);
      return 1;
    }
  }
  return 0;
}

/* Reads the mapping file of the last package of out and checks it (T9a,
 * docs/orm.md): the file, a configured database, the package's own
 * deftemplates, then tables and sequences an earlier package of another
 * definition maps, a sequence and a table with one name (T9b), and tables
 * an earlier version maps differently (T9b). Sets pkg->orm when the file is valid (so later packages
 * see its tables) and pkg->orm_templates when every check passed. Returns
 * the error count. */
static int check_orm(Environment *env, clpd_packages *out, int have_database,
                     FILE *err) {
  clpd_package *pkg = &out->items[out->count - 1];
  const char *name = pkg->manifest.name, *other;
  clpd_orm *orm;
  Deftemplate **found;
  size_t i;
  int errors = 0;

  if (pkg->manifest.orm == NULL) return 0;
  orm = malloc(sizeof *orm);
  if (orm == NULL) clpd_log_die_errno();
  if (clpd_orm_parse(pkg->manifest.orm, orm, err) != 0) {
    clpd_orm_free(orm);
    free(orm);
    orm = NULL;
    errors++;
  }
  if (!have_database) {
    fprintf(err, ORM_PREFIX "a mapping needs a database (the configuration "
                 "sets no database)\n", name);
    errors++;
  }
  if (orm == NULL) return errors;
  pkg->orm = orm;
  found = calloc(orm->template_count + 1, sizeof *found);
  if (found == NULL) clpd_log_die_errno();
  SetCurrentModule(env, pkg->module);
  for (i = 0; i < orm->template_count; i++) {
    clpd_orm_template *t = &orm->templates[i];
    if ((found[i] = own_template(env, t->name)) == NULL) {
      fprintf(err, ORM_PREFIX "unknown template '%s'\n", name, t->name);
      errors++;
    } else {
      errors += bind_template(found[i], t, name, err);
    }
  }
  for (i = 0; i < orm->template_count; i++) {
    if ((other = mapped_by_other_definition(out, orm->templates[i].table,
                                            ORM_TABLE)) != NULL) {
      fprintf(err, ORM_PREFIX "table '%s' is already mapped by %s\n", name,
              orm->templates[i].table, other);
      errors++;
    }
  }
  for (i = 0; i < orm->sequence_count; i++) {
    if ((other = mapped_by_other_definition(out, orm->sequences[i].name,
                                            ORM_SEQUENCE)) != NULL) {
      fprintf(err, ORM_PREFIX "sequence '%s' is already mapped by %s\n", name,
              orm->sequences[i].name, other);
      errors++;
    }
  }
  for (i = 0; i < orm->sequence_count; i++) {
    if ((other = named_by_any(out, orm->sequences[i].name, ORM_TABLE)) !=
        NULL) {
      fprintf(err, ORM_PREFIX "sequence '%s' has the name of a table mapped "
                   "by %s\n", name, orm->sequences[i].name, other);
      errors++;
    }
  }
  for (i = 0; i < orm->template_count; i++) {
    if ((other = named_by_earlier(out, orm->templates[i].table,
                                  ORM_SEQUENCE)) != NULL) {
      fprintf(err, ORM_PREFIX "table '%s' has the name of a sequence mapped "
                   "by %s\n", name, orm->templates[i].table, other);
      errors++;
    }
  }
  for (i = 0; i < orm->template_count; i++)
    errors += check_shared_table(out, &orm->templates[i], err);
  if (errors == 0)
    pkg->orm_templates = found;
  else
    free(found);
  return errors;
}

/* Loads one package already appended to out; returns its error count. */
static int load_package(Environment *env, clpd_packages *out,
                        int have_database, FILE *err) {
  clpd_package *pkg = &out->items[out->count - 1];
  char build[CLPD_PACKAGE_NAME_MAX * 2 + 16];
  clpd_lint_exempt exempt;
  size_t i;
  int errors;

  for (i = 0; i + 1 < out->count; i++) {
    if (strcmp(out->items[i].manifest.name, pkg->manifest.name) == 0) {
      fprintf(err, "clpdaemon: %s: package %s is already loaded from %s\n",
              pkg->dir, pkg->manifest.name, out->items[i].dir);
      return 1;
    }
  }
  snprintf(build, sizeof build, "(defmodule %s)", pkg->manifest.name);
  if (Build(env, build) != BE_NO_ERROR) {
    fprintf(err, "clpdaemon: %s: cannot create module %s\n", pkg->dir,
            pkg->manifest.name);
    return 1;
  }
  pkg->module = FindDefmodule(env, pkg->manifest.name);
  for (i = 0; i < pkg->manifest.files.count; i++) {
    SetCurrentModule(env, pkg->module);
    if (load_file(env, pkg, pkg->manifest.files.items[i], err) != 0) return 1;
  }
  exempt.templates = (const char *const *)pkg->manifest.exempt_templates.items;
  exempt.template_count = pkg->manifest.exempt_templates.count;
  exempt.rules = (const char *const *)pkg->manifest.exempt_rules.items;
  exempt.rule_count = pkg->manifest.exempt_rules.count;
  errors = clpd_lint_package(env, pkg->module, pkg->manifest.name, &exempt, err);
  errors += check_ingest(env, out, err);
  return errors + check_orm(env, out, have_database, err);
}

int clpd_packages_load_ex(Environment *env, const clpd_list *dirs,
                          int have_database, clpd_packages *out, FILE *err) {
  Defmodule *main_module = FindDefmodule(env, "MAIN");
  size_t i;
  int errors = 0;

  out->items = NULL;
  out->count = 0;
  for (i = 0; dirs != NULL && i < dirs->count; i++) {
    clpd_manifest m;
    clpd_package *grown;
    if (clpd_manifest_load(dirs->items[i], &m, err) != 0) {
      clpd_manifest_free(&m);
      errors++;
      continue;
    }
    grown = realloc(out->items, (out->count + 1) * sizeof *grown);
    if (grown == NULL) {
      clpd_log_die_errno();
    }
    out->items = grown;
    out->items[out->count].dir = clpd_strdup(dirs->items[i]);
    out->items[out->count].manifest = m;
    out->items[out->count].module = NULL;
    out->items[out->count].ingest = NULL;
    out->items[out->count].orm = NULL;
    out->items[out->count].orm_templates = NULL;
    out->count++;
    errors += load_package(env, out, have_database, err);
    SetCurrentModule(env, main_module);
  }
  return errors != 0;
}

int clpd_packages_load(Environment *env, const clpd_list *dirs,
                       clpd_packages *out, FILE *err) {
  return clpd_packages_load_ex(env, dirs, 0, out, err);
}

void clpd_packages_focus(Environment *env, const clpd_packages *p) {
  size_t i;
  ClearFocusStack(env);
  Focus(FindDefmodule(env, "MAIN"));
  for (i = p->count; i > 0; i--)
    if (p->items[i - 1].module != NULL) Focus(p->items[i - 1].module);
}

void clpd_packages_free(clpd_packages *p) {
  size_t i;
  for (i = 0; i < p->count; i++) {
    free(p->items[i].dir);
    free(p->items[i].ingest);
    free(p->items[i].orm_templates);
    if (p->items[i].orm != NULL) clpd_orm_free(p->items[i].orm);
    free(p->items[i].orm);
    clpd_manifest_free(&p->items[i].manifest);
  }
  free(p->items);
  p->items = NULL;
  p->count = 0;
}
