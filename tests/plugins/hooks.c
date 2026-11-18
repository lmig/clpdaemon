/* Test plugin that logs every hook (the cycle hooks for the first cycle
 * only), so tests can observe the hook order of several plugins. Built
 * several times with a different HOOKS_NAME; with HOOKS_FAIL_INIT, init
 * registers a CLIPS function and then fails. */
#include "clpd_plugin.h"

#include <errno.h>
#include <stdlib.h>

#ifndef HOOKS_NAME
#define HOOKS_NAME "hooks"
#endif

typedef struct {
  const clpd_host *host;
  int cycles, ended;
} hooks_state;

static const clpd_plugin descriptor;

static void answer(Environment *env, UDFContext *udf, UDFValue *out) {
  (void)udf;
  out->integerValue = CreateInteger(env, 42);
}

static int hooks_init(const clpd_host *host, Environment *env, void **state) {
  int rc;
  if (*state != NULL || host->struct_size < sizeof(clpd_host)) return -1;
  errno = 0;
  rc = host->post_fact("(x)");
  host->log(&descriptor, "init node_id=%s post_fact=%d%s", host->node_id, rc,
            errno == ENOSYS ? " ENOSYS" : "");
#ifdef HOOKS_FAIL_INIT
  AddUDF(env, HOOKS_NAME "-answer", "l", 0, 0, NULL, answer, "answer", NULL);
  return -1;
#else
  {
    hooks_state *s;
    if (AddUDF(env, HOOKS_NAME "-answer", "l", 0, 0, NULL, answer, "answer",
               NULL) != AUE_NO_ERROR)
      return -1;
    s = calloc(1, sizeof *s);
    if (s == NULL) return -1;
    s->host = host;
    *state = s;
    return 0;
  }
#endif
}

static void hooks_after_load(void *state) {
  hooks_state *s = state;
  s->host->log(&descriptor, "after_load");
}

static void hooks_cycle_begin(void *state) {
  hooks_state *s = state;
  if (s->cycles++ == 0) s->host->log(&descriptor, "cycle_begin");
}

static void hooks_cycle_end(void *state, int ok) {
  hooks_state *s = state;
  if (s->ended++ == 0) s->host->log(&descriptor, "cycle_end ok=%d", ok);
}

static void hooks_shutdown(void *state) {
  hooks_state *s = state;
  s->host->log(&descriptor, "shutdown");
  free(s);
}

static const clpd_plugin descriptor = {
    CLPD_PLUGIN_ABI,   sizeof(clpd_plugin), HOOKS_NAME,      hooks_init,
    hooks_after_load,  hooks_cycle_begin,   hooks_cycle_end, NULL,
    hooks_shutdown};

const clpd_plugin *clpd_plugin_get(void) { return &descriptor; }
