/* Example clpdaemon plugin (docs/plugins.md).
 *
 * CLIPS functions:
 *   (clpd-example-add <integer> <integer>)  -> their sum
 *   (clpd-example-greet <string-or-symbol>) -> "hello, <name>"
 *   (clpd-example-cycles)                   -> engine cycles begun so far
 *
 * Log lines show the hook order: init, after_load, the first cycle_begin,
 * every cycle_end whose ok differs from the previous cycle's (the first
 * included), and shutdown. */
#include "clpd_plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const clpd_host *host;
  long long cycles;
  int last_ok; /* -1 before the first cycle_end */
} example_state;

static const clpd_plugin descriptor;

static void add(Environment *env, UDFContext *udf, UDFValue *out) {
  UDFValue a, b;
  if (!UDFFirstArgument(udf, INTEGER_BIT, &a)) return;
  if (!UDFNextArgument(udf, INTEGER_BIT, &b)) return;
  out->integerValue = CreateInteger(env, a.integerValue->contents +
                                             b.integerValue->contents);
}

static void greet(Environment *env, UDFContext *udf, UDFValue *out) {
  UDFValue name;
  const char *prefix = "hello, ";
  char *text;
  size_t n;
  if (!UDFFirstArgument(udf, LEXEME_BITS, &name)) return;
  n = strlen(prefix) + strlen(name.lexemeValue->contents) + 1;
  text = malloc(n);
  if (text == NULL) {
    UDFThrowError(udf);
    out->lexemeValue = CreateString(env, "");
    return;
  }
  snprintf(text, n, "%s%s", prefix, name.lexemeValue->contents);
  out->lexemeValue = CreateString(env, text);
  free(text);
}

static void cycles(Environment *env, UDFContext *udf, UDFValue *out) {
  example_state *s = udf->context;
  out->integerValue = CreateInteger(env, s->cycles);
}

static int init(const clpd_host *host, Environment *env, void **state) {
  example_state *s = calloc(1, sizeof *s);
  if (s == NULL) return -1;
  s->host = host;
  s->last_ok = -1;
  /* Return types "l" (integer) and "s" (string); argument types: two
   * integers, one symbol or string. */
  if (AddUDF(env, "clpd-example-add", "l", 2, 2, "l", add, "add", NULL) !=
          AUE_NO_ERROR ||
      AddUDF(env, "clpd-example-greet", "s", 1, 1, "sy", greet, "greet",
             NULL) != AUE_NO_ERROR ||
      AddUDF(env, "clpd-example-cycles", "l", 0, 0, NULL, cycles, "cycles",
             s) != AUE_NO_ERROR) {
    host->log(&descriptor, "cannot register the CLIPS functions");
    free(s);
    return -1;
  }
  *state = s;
  host->log(&descriptor, "init");
  return 0;
}

static void after_load(void *state) {
  example_state *s = state;
  s->host->log(&descriptor, "after_load");
}

static void cycle_begin(void *state) {
  example_state *s = state;
  if (++s->cycles == 1) s->host->log(&descriptor, "cycle_begin 1");
}

static void cycle_end(void *state, int ok) {
  example_state *s = state;
  if (ok != s->last_ok)
    s->host->log(&descriptor, "cycle_end %lld ok=%d", s->cycles, ok);
  s->last_ok = ok;
}

static void example_shutdown(void *state) {
  example_state *s = state;
  s->host->log(&descriptor, "shutdown");
  free(s);
}

static const clpd_plugin descriptor = {
    CLPD_PLUGIN_ABI, sizeof(clpd_plugin), "example", init, after_load,
    cycle_begin,     cycle_end,           NULL,      example_shutdown};

const clpd_plugin *clpd_plugin_get(void) { return &descriptor; }
