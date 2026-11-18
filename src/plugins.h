/* Plugin loader and lifecycle (FR-17, FR-18, FR-19; docs/plugins.md). The
 * contract plugins implement is include/clpd_plugin.h. Engine thread only. */
#ifndef CLPD_PLUGINS_H
#define CLPD_PLUGINS_H

#include <stdio.h>

#include "clips.h"
#include "kvfile.h"

typedef struct clpd_plugins clpd_plugins;

/* Loads each path in order (dlopen, clpd_plugin_get, descriptor checks) and
 * calls its init with env. Stops at the first failure: writes one
 * "clpdaemon: <path>: ..." message to err, shuts down the plugins already
 * initialized (reverse order) and returns nonzero. *out is always set and
 * must be closed with clpd_plugins_close after the environment is
 * destroyed. node_id is given to plugins through the host API and must
 * outlive *out. */
int clpd_plugins_load(Environment *env, const clpd_list *paths,
                      const char *node_id, clpd_plugins **out, FILE *err);
/* Hooks, in configured order; no-ops after clpd_plugins_shutdown. */
void clpd_plugins_after_load(clpd_plugins *p);
void clpd_plugins_cycle_begin(clpd_plugins *p);
void clpd_plugins_cycle_end(clpd_plugins *p, int ok);
/* Calls shutdown of every initialized plugin in reverse order, once; later
 * calls do nothing. Call it while the environment still exists. */
void clpd_plugins_shutdown(clpd_plugins *p);
/* Shuts down if still needed, unloads every library and frees p. Call it
 * after the environment is destroyed (the UDFs point into the libraries).
 * NULL is allowed. */
void clpd_plugins_close(clpd_plugins *p);

#endif
