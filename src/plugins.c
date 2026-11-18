#define _POSIX_C_SOURCE 200809L

#include "plugins.h"

#include "clpd_plugin.h"
#include "config.h"
#include "log.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define CLPD_PLUGIN_NAME_MAX 64

typedef struct {
  const char *path;           /* the configured path (owned by the config) */
  void *handle;               /* dlopen handle, closed by clpd_plugins_close */
  const clpd_plugin *desc;    /* NULL until the descriptor checks pass */
  void *state;                /* what init stored */
  int initialized;            /* init returned 0 */
} entry;

struct clpd_plugins {
  clpd_host host;
  entry *items;
  size_t count;
  int active; /* 0 once shut down (or after a failed load) */
};

static void host_log(const clpd_plugin *plugin, const char *fmt, ...) {
  FILE *log = clpd_log();
  va_list ap;
  flockfile(log);
  fprintf(log, "clpdaemon: %s: ",
          plugin != NULL && plugin->name != NULL ? plugin->name : "?");
  va_start(ap, fmt);
  vfprintf(log, fmt, ap);
  va_end(ap);
  fputc('\n', log);
  fflush(log);
  funlockfile(log);
}

/* The ingest pipeline arrives in T7 (docs/plugins.md). */
static int host_post_fact(const char *fact_text) {
  (void)fact_text;
  errno = ENOSYS;
  return -1;
}

/* Opens, checks and initializes the last entry of p. On failure writes one
 * message to err and returns nonzero. */
static int load_one(Environment *env, clpd_plugins *p, FILE *err) {
  entry *e = &p->items[p->count - 1];
  const clpd_plugin *d;
  clpd_plugin_get_fn *get;
  void *sym;
  size_t i;

  /* A path without '/' would make dlopen search the library paths; plugins
   * load only from configured paths (NFR-2). */
  if (strchr(e->path, '/') == NULL) {
    fprintf(err, "clpdaemon: %s: plugin path must contain a '/'\n", e->path);
    return 1;
  }
  for (i = 0; i + 1 < p->count; i++) {
    if (strcmp(p->items[i].path, e->path) == 0) {
      fprintf(err, "clpdaemon: %s: plugin path listed more than once\n",
              e->path);
      return 1;
    }
  }
  dlerror();
  e->handle = dlopen(e->path, RTLD_NOW | RTLD_LOCAL);
  if (e->handle == NULL) {
    const char *why = dlerror();
    fprintf(err, "clpdaemon: %s: cannot load: %s\n", e->path,
            why != NULL ? why : "unknown error");
    return 1;
  }
  dlerror();
  sym = dlsym(e->handle, "clpd_plugin_get");
  if (sym == NULL) {
    fprintf(err, "clpdaemon: %s: missing clpd_plugin_get\n", e->path);
    return 1;
  }
  /* POSIX dlsym returns object and function addresses alike; the copy
   * avoids the ISO C object-to-function pointer conversion. */
  memcpy(&get, &sym, sizeof get);
  d = get();
  if (d == NULL) {
    fprintf(err, "clpdaemon: %s: clpd_plugin_get returned NULL\n", e->path);
    return 1;
  }
  if (d->abi_version != CLPD_PLUGIN_ABI) {
    fprintf(err, "clpdaemon: %s: ABI version %d, expected %d\n", e->path,
            d->abi_version, CLPD_PLUGIN_ABI);
    return 1;
  }
  if (d->struct_size < sizeof(clpd_plugin)) {
    fprintf(err, "clpdaemon: %s: descriptor size %zu, expected at least %zu\n",
            e->path, d->struct_size, sizeof(clpd_plugin));
    return 1;
  }
  if (d->name == NULL || !clpd_valid_name(d->name, CLPD_PLUGIN_NAME_MAX)) {
    fprintf(err,
            "clpdaemon: %s: plugin name: expected 1-%d characters from "
            "[A-Za-z0-9._-], got '%s'\n",
            e->path, CLPD_PLUGIN_NAME_MAX, d->name != NULL ? d->name : "");
    return 1;
  }
  for (i = 0; i + 1 < p->count; i++) {
    if (p->items[i].desc != NULL &&
        strcmp(p->items[i].desc->name, d->name) == 0) {
      fprintf(err, "clpdaemon: %s: plugin %s is already loaded from %s\n",
              e->path, d->name, p->items[i].path);
      return 1;
    }
  }
  if (d->init == NULL) {
    fprintf(err, "clpdaemon: %s: descriptor has no init\n", e->path);
    return 1;
  }
  e->desc = d;
  if (d->init(&p->host, env, &e->state) != 0) {
    fprintf(err, "clpdaemon: %s: init failed\n", e->path);
    return 1;
  }
  e->initialized = 1;
  return 0;
}

int clpd_plugins_load(Environment *env, const clpd_list *paths,
                      const char *node_id, clpd_plugins **out, FILE *err) {
  clpd_plugins *p = calloc(1, sizeof *p);
  size_t i;

  if (p == NULL) clpd_log_die_errno();
  *out = p;
  p->host.struct_size = sizeof p->host;
  p->host.log = host_log;
  p->host.post_fact = host_post_fact;
  p->host.node_id = node_id;
  p->active = 1;
  for (i = 0; paths != NULL && i < paths->count; i++) {
    entry *grown = realloc(p->items, (p->count + 1) * sizeof *grown);
    if (grown == NULL) clpd_log_die_errno();
    p->items = grown;
    memset(&p->items[p->count], 0, sizeof *grown);
    p->items[p->count].path = paths->items[i];
    p->count++;
    if (load_one(env, p, err) != 0) {
      clpd_plugins_shutdown(p);
      return 1;
    }
  }
  return 0;
}

void clpd_plugins_after_load(clpd_plugins *p) {
  size_t i;
  if (p == NULL || !p->active) return;
  for (i = 0; i < p->count; i++)
    if (p->items[i].desc->after_load != NULL)
      p->items[i].desc->after_load(p->items[i].state);
}

void clpd_plugins_cycle_begin(clpd_plugins *p) {
  size_t i;
  if (p == NULL || !p->active) return;
  for (i = 0; i < p->count; i++)
    if (p->items[i].desc->cycle_begin != NULL)
      p->items[i].desc->cycle_begin(p->items[i].state);
}

void clpd_plugins_cycle_end(clpd_plugins *p, int ok) {
  size_t i;
  if (p == NULL || !p->active) return;
  for (i = 0; i < p->count; i++)
    if (p->items[i].desc->cycle_end != NULL)
      p->items[i].desc->cycle_end(p->items[i].state, ok);
}

void clpd_plugins_shutdown(clpd_plugins *p) {
  size_t i;
  if (p == NULL || !p->active) return;
  p->active = 0;
  for (i = p->count; i > 0; i--) {
    entry *e = &p->items[i - 1];
    if (e->initialized && e->desc->shutdown != NULL) e->desc->shutdown(e->state);
    e->initialized = 0;
  }
}

void clpd_plugins_close(clpd_plugins *p) {
  size_t i;
  if (p == NULL) return;
  clpd_plugins_shutdown(p);
  for (i = p->count; i > 0; i--)
    if (p->items[i - 1].handle != NULL) dlclose(p->items[i - 1].handle);
  free(p->items);
  free(p);
}
