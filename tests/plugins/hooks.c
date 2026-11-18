/* Test plugin that logs every hook (the cycle hooks for the first cycle
 * only), so tests can observe the hook order of several plugins. Built
 * several times with a different HOOKS_NAME and these variants:
 * HOOKS_FAIL_INIT (init registers a CLIPS function and then fails),
 * HOOKS_BIG (the descriptor is larger than sizeof(clpd_plugin), as from a
 * later contract revision), HOOKS_THREAD (init also posts a fact JSON from
 * another thread), HOOKS_POST (init also posts a valid order fact and one
 * with an unknown slot, see docs/ingest.md), HOOKS_MARKERS (shutdown, the
 * CLIPS environment cleanup and the library destructor append a line each to the file named
 * by CLPD_TEST_MARKERS), HOOKS_SLEEPER (init starts a thread that lives
 * until shutdown), HOOKS_SLOW (init sleeps 300 ms after its log line) and
 * HOOKS_LATE (init starts a thread that posts a valid order fact 200 ms
 * later; shutdown joins it and logs "shutdown late post_fact=<rc>").
 * Every variant registers <name>-answer, which returns 42; every variant
 * except HOOKS_FAIL_INIT also registers <name>-throw, which calls
 * UDFThrowError. Every variant first posts "(x)", which is not JSON, and
 * logs post_fact's result (T7b). */
#define _POSIX_C_SOURCE 200809L

#include "clpd_plugin.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(HOOKS_THREAD) || defined(HOOKS_SLEEPER) || defined(HOOKS_LATE)
#include <pthread.h>
#endif
#if defined(HOOKS_SLOW) || defined(HOOKS_LATE)
#include <time.h>
#endif

#ifndef HOOKS_NAME
#define HOOKS_NAME "hooks"
#endif

typedef struct {
  const clpd_host *host;
  int cycles, ended;
#ifdef HOOKS_SLEEPER
  pthread_t sleeper;
  pthread_mutex_t lock;
  pthread_cond_t wake;
  int stop;
#endif
#ifdef HOOKS_LATE
  pthread_t poster;
  int rc, err;
#endif
} hooks_state;

#ifdef HOOKS_LATE
/* Posts one order fact 200 ms after init, from its own thread; shutdown
 * joins it and logs post_fact's result (only post_fact may be called from
 * another thread). */
static void *post_late(void *arg) {
  hooks_state *s = arg;
  struct timespec pause = {0, 200000000L};
  while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {
  }
  errno = 0;
  s->rc = s->host->post_fact(
      "{\"template\":\"order\",\"slots\":{\"wf-id\":\"late\"}}");
  s->err = errno;
  return NULL;
}
#endif

#ifdef HOOKS_SLEEPER
/* Waits until shutdown; it never touches its signal mask, so it keeps the
 * one it inherited from the thread that ran init. */
static void *sleep_until_stop(void *arg) {
  hooks_state *s = arg;
  pthread_mutex_lock(&s->lock);
  while (!s->stop) pthread_cond_wait(&s->wake, &s->lock);
  pthread_mutex_unlock(&s->lock);
  return NULL;
}
#endif

#ifdef HOOKS_BIG
/* A descriptor with fields this daemon does not know about. */
typedef struct {
  clpd_plugin base;
  void (*later_hook)(void *state);
} big_descriptor;
static const big_descriptor descriptor;
#define SELF (&descriptor.base)
#else
static const clpd_plugin descriptor;
#define SELF (&descriptor)
#endif

#ifdef HOOKS_MARKERS
static void mark(const char *what) {
  const char *path = getenv("CLPD_TEST_MARKERS");
  FILE *f;
  if (path == NULL || (f = fopen(path, "a")) == NULL) return;
  fprintf(f, "%s\n", what);
  fclose(f);
}

static void env_cleanup(Environment *env) {
  (void)env;
  mark("env-cleanup");
}

__attribute__((destructor)) static void unloaded(void) { mark("destructor"); }
#endif

#ifdef HOOKS_THREAD
typedef struct {
  const clpd_host *host;
  int rc, err;
} post_result;

static void *post_from_thread(void *arg) {
  post_result *r = arg;
  errno = 0;
  r->rc = r->host->post_fact("{\"template\":\"x\",\"slots\":{}}");
  r->err = errno;
  return NULL;
}
#endif

static const char *errno_name(int e) {
  return e == EINVAL ? " EINVAL" : e == EAGAIN ? " EAGAIN" : "";
}

static void answer(Environment *env, UDFContext *udf, UDFValue *out) {
  (void)udf;
  out->integerValue = CreateInteger(env, 42);
}

#ifndef HOOKS_FAIL_INIT
static void throw_error(Environment *env, UDFContext *udf, UDFValue *out) {
  UDFThrowError(udf);
  out->lexemeValue = FalseSymbol(env);
}
#endif

static int hooks_init(const clpd_host *host, Environment *env, void **state) {
  int rc;
  if (*state != NULL || host->struct_size < sizeof(clpd_host)) return -1;
  errno = 0;
  rc = host->post_fact("(x)");
  host->log(SELF, "init node_id=%s post_fact=%d%s", host->node_id, rc,
            rc != 0 ? errno_name(errno) : "");
#ifdef HOOKS_POST
  {
    int rc_valid = host->post_fact(
        "{\"template\":\"order\",\"slots\":{\"wf-id\":\"w1\",\"qty\":3,"
        "\"tags\":[\"a\",{\"sym\":\"b\"}]}}");
    int rc_unknown_slot = host->post_fact(
        "{\"template\":\"order\",\"slots\":{\"bogus\":1}}");
    host->log(SELF, "posted %d %d%s", rc_valid, rc_unknown_slot,
              rc_unknown_slot != 0 ? errno_name(errno) : "");
  }
#endif
#ifdef HOOKS_SLOW
  {
    struct timespec pause = {0, 300000000L};
    while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {
    }
  }
#endif
#ifdef HOOKS_THREAD
  {
    post_result r = {NULL, 0, 0};
    pthread_t t;
    r.host = host;
    if (pthread_create(&t, NULL, post_from_thread, &r) != 0 ||
        pthread_join(t, NULL) != 0)
      return -1;
    host->log(SELF, "thread post_fact=%d%s", r.rc,
              r.rc != 0 ? errno_name(r.err) : "");
  }
#endif
#ifdef HOOKS_FAIL_INIT
  AddUDF(env, HOOKS_NAME "-answer", "l", 0, 0, NULL, answer, "answer", NULL);
  return -1;
#else
  {
    hooks_state *s;
    if (AddUDF(env, HOOKS_NAME "-answer", "l", 0, 0, NULL, answer, "answer",
               NULL) != AUE_NO_ERROR ||
        AddUDF(env, HOOKS_NAME "-throw", "b", 0, 0, NULL, throw_error,
               "throw_error", NULL) != AUE_NO_ERROR)
      return -1;
#ifdef HOOKS_MARKERS
    if (!AddEnvironmentCleanupFunction(env, "hooks-markers", env_cleanup, 0))
      return -1;
#endif
    s = calloc(1, sizeof *s);
    if (s == NULL) return -1;
    s->host = host;
#ifdef HOOKS_SLEEPER
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->wake, NULL);
    if (pthread_create(&s->sleeper, NULL, sleep_until_stop, s) != 0) {
      free(s);
      return -1;
    }
#endif
#ifdef HOOKS_LATE
    if (pthread_create(&s->poster, NULL, post_late, s) != 0) {
      free(s);
      return -1;
    }
#endif
    *state = s;
    return 0;
  }
#endif
}

static void hooks_after_load(void *state) {
  hooks_state *s = state;
  s->host->log(SELF, "after_load");
}

static void hooks_cycle_begin(void *state) {
  hooks_state *s = state;
  if (s->cycles++ == 0) s->host->log(SELF, "cycle_begin");
}

static void hooks_cycle_end(void *state, int ok) {
  hooks_state *s = state;
  if (s->ended++ == 0) s->host->log(SELF, "cycle_end ok=%d", ok);
}

static void hooks_shutdown(void *state) {
  hooks_state *s = state;
#ifdef HOOKS_LATE
  pthread_join(s->poster, NULL);
  s->host->log(SELF, "shutdown late post_fact=%d%s", s->rc,
               s->rc != 0 ? errno_name(s->err) : "");
#else
  s->host->log(SELF, "shutdown");
#endif
#ifdef HOOKS_SLEEPER
  pthread_mutex_lock(&s->lock);
  s->stop = 1;
  pthread_cond_signal(&s->wake);
  pthread_mutex_unlock(&s->lock);
  pthread_join(s->sleeper, NULL);
  pthread_cond_destroy(&s->wake);
  pthread_mutex_destroy(&s->lock);
#endif
#ifdef HOOKS_MARKERS
  mark("shutdown");
#endif
  free(s);
}

#define HOOKS_DESCRIPTOR(size)                                               \
  {CLPD_PLUGIN_ABI,  (size),           HOOKS_NAME,      hooks_init,          \
   hooks_after_load, hooks_cycle_begin, hooks_cycle_end, NULL,               \
   hooks_shutdown}

#ifdef HOOKS_BIG
static const big_descriptor descriptor = {
    HOOKS_DESCRIPTOR(sizeof(big_descriptor)), NULL};
#else
static const clpd_plugin descriptor = HOOKS_DESCRIPTOR(sizeof(clpd_plugin));
#endif

const clpd_plugin *clpd_plugin_get(void) { return SELF; }
