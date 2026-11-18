/* clpdaemon plugin contract, version 1 (FR-17, FR-18, FR-19;
 * docs/plugins.md).
 *
 * A plugin is a shared library listed in the `plugins` configuration key.
 * It exports exactly one symbol, clpd_plugin_get, and registers its CLIPS
 * functions with AddUDF from init, following the CLIPS UserDefinedFunction
 * contract. Plugins do not link CLIPS: the daemon exports every CLIPS symbol
 * (-rdynamic, but none of its own clpd_* internals), and a plugin only
 * compiles against the CLIPS headers.
 *
 * Every hook runs on the engine thread, the only thread that may call CLIPS
 * (NFR-1). Only host->post_fact may be called from another thread. */
#ifndef CLPD_PLUGIN_H
#define CLPD_PLUGIN_H

#include "clips.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Contract version. The daemon loads a plugin only when its abi_version is
 * exactly this value. */
#define CLPD_PLUGIN_ABI 1

#if defined(__GNUC__)
#define CLPD_PLUGIN_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define CLPD_PLUGIN_PRINTF(f, a)
#endif

struct clpd_plugin;

/* Services the daemon offers to a plugin. The daemon owns this structure;
 * the pointer given to init stays valid until shutdown returns. */
typedef struct clpd_host {
  /* sizeof(clpd_host) of the daemon. Later contract revisions only append
   * fields; a plugin reads a field only when struct_size covers it. */
  size_t struct_size;
  /* Writes "clpdaemon: <plugin name>: <message>" and a newline to the
   * daemon log stream (stderr, never the trace). plugin is the plugin's own
   * descriptor; fmt and the arguments follow printf. Engine thread only. */
  void (*log)(const struct clpd_plugin *plugin, const char *fmt, ...)
      CLPD_PLUGIN_PRINTF(2, 3);
  /* Queues one fact for the engine thread, written as fact JSON
   * (docs/ingest.md), such as
   * {"template":"order","slots":{"wf-id":"w1","qty":3}}. Callable from any
   * thread, init included. Returns 0 when queued; otherwise -1 with errno
   * EINVAL (NULL or not JSON within the limits), EAGAIN (queue full) or
   * ENOMEM. The template and slots are checked when the next engine cycle
   * asserts the fact; a rejected fact is logged on the daemon log stream as
   * "clpdaemon: <plugin name>: post_fact rejected: <message>". */
  int (*post_fact)(const char *fact_text);
  /* The node_id of the daemon configuration. */
  const char *node_id;
} clpd_host;

/* Plugin descriptor returned by clpd_plugin_get. The daemon reads it after
 * the call and keeps the pointer, so it must stay valid (usually a static
 * const object) until the library is unloaded. Every hook except init may be
 * NULL. state is the value init stored through its last argument. */
typedef struct clpd_plugin {
  /* Must be CLPD_PLUGIN_ABI. */
  int abi_version;
  /* sizeof(clpd_plugin) as the plugin was compiled; the daemon rejects a
   * smaller value. */
  size_t struct_size;
  /* 1-64 characters from [A-Za-z0-9._-], unique among the loaded plugins;
   * it prefixes the plugin's log lines. */
  const char *name;
  /* Called once, in configured order, after the CLIPS environment is
   * created and before the rule packages are loaded: register the UDFs here
   * so package files can call them. Store the plugin state in *state
   * (NULL on entry). Returns 0 on success; any other value aborts startup,
   * and shutdown is then not called for this plugin. */
  int (*init)(const clpd_host *host, Environment *env, void **state);
  /* Called once, in configured order, after every rule package loaded and
   * before Reset. Not called under --check. */
  void (*after_load)(void *state);
  /* Called before the Run of every engine cycle, in configured order. */
  void (*cycle_begin)(void *state);
  /* Called after the Run of every engine cycle, in configured order. ok is
   * 0 when that Run stopped on a CLIPS evaluation error, 1 otherwise. */
  void (*cycle_end)(void *state, int ok);
  /* Reserved for the exchange acknowledgements (T13/T14): the daemon never
   * calls it in contract version 1. Set it to NULL. */
  void (*on_ack)(void *state, const void *ack);
  /* Called once, in reverse configured order, for every plugin whose init
   * succeeded, while the CLIPS environment still exists; also when a later
   * plugin or a rule package fails to load, and under --check. Free the
   * state here. The library is unloaded only after the environment is
   * destroyed. */
  void (*shutdown)(void *state);
} clpd_plugin;

/* The one symbol a plugin exports. Returns the plugin's descriptor. */
typedef const clpd_plugin *clpd_plugin_get_fn(void);
const clpd_plugin *clpd_plugin_get(void);

#ifdef __cplusplus
}
#endif

#endif
