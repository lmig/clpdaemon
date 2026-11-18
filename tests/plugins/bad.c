/* Test plugins with one defect each, selected at compile time:
 * BAD_NOSYM (no clpd_plugin_get), BAD_ABI (abi_version 2), BAD_SIZE
 * (struct_size too small), BAD_NULL (clpd_plugin_get returns NULL),
 * BAD_NAME (invalid name), BAD_NOINIT (init is NULL). */
#include "clpd_plugin.h"

#ifdef BAD_NOSYM
int clpd_plugin_get_typo(void);
int clpd_plugin_get_typo(void) { return 0; }
#else
#ifndef BAD_NOINIT
static int bad_init(const clpd_host *host, Environment *env, void **state) {
  (void)env;
  (void)state;
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
#else
    sizeof(clpd_plugin),
#endif
#ifdef BAD_NAME
    "bad name",
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
