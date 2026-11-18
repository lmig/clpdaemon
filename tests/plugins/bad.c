/* Test plugins with one defect each, selected at compile time:
 * BAD_NOSYM (no clpd_plugin_get), BAD_ABI (abi_version 2), BAD_SIZE
 * (struct_size too small), BAD_NULL (clpd_plugin_get returns NULL),
 * BAD_NAME (invalid name), BAD_NOINIT (init is NULL), BAD_SIZE1
 * (struct_size one byte short), BAD_NAME65 (a 65-character name),
 * BAD_UNDEF (references a symbol nobody defines) and BAD_INTERNAL
 * (references clpd_log, a daemon internal the executable does not export).
 * The last two fail at dlopen (RTLD_NOW), before clpd_plugin_get. */
#include "clpd_plugin.h"

#ifdef BAD_UNDEF
int clpd_test_no_such_symbol(void);
#endif
#ifdef BAD_INTERNAL
#include <stdio.h>
FILE *clpd_log(void);
#endif

#ifdef BAD_NOSYM
int clpd_plugin_get_typo(void);
int clpd_plugin_get_typo(void) { return 0; }
#else
#ifndef BAD_NOINIT
static int bad_init(const clpd_host *host, Environment *env, void **state) {
  (void)env;
  (void)state;
#ifdef BAD_UNDEF
  if (clpd_test_no_such_symbol() != 0) return -1;
#endif
#ifdef BAD_INTERNAL
  if (clpd_log() == NULL) return -1;
#endif
  host->log(clpd_plugin_get(), "init must not be called");
  return -1;
}
#endif

static const clpd_plugin descriptor = {
#ifdef BAD_ABI
    2,
#else
    CLPD_PLUGIN_ABI,
#endif
#ifdef BAD_SIZE
    8,
#elif defined(BAD_SIZE1)
    sizeof(clpd_plugin) - 1,
#else
    sizeof(clpd_plugin),
#endif
#ifdef BAD_NAME
    "bad name",
#elif defined(BAD_NAME65)
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
#else
    "bad",
#endif
#ifdef BAD_NOINIT
    NULL,
#else
    bad_init,
#endif
    NULL, NULL, NULL, NULL, NULL};

const clpd_plugin *clpd_plugin_get(void) {
#ifdef BAD_NULL
  (void)descriptor;
  return NULL;
#else
  return &descriptor;
#endif
}
#endif
