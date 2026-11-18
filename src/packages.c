#define _POSIX_C_SOURCE 200809L

#include "packages.h"

#include "lint.h"

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

static void capture_write(Environment *env, const char *name, const char *str,
                          void *ctx) {
  capture *c = ctx;
  size_t n = strlen(str);
  (void)env;
  (void)name;
  if (c->len + n + 1 > c->cap) {
    size_t cap = c->cap == 0 ? 256 : c->cap;
    char *grown;
    while (c->len + n + 1 > cap) cap *= 2;
    grown = realloc(c->buf, cap);
    if (grown == NULL) {
      perror("clpdaemon");
      exit(1);
    }
    c->buf = grown;
    c->cap = cap;
  }
  memcpy(c->buf + c->len, str, n + 1);
  c->len += n;
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

/* Loads one file into the current (package) module. Returns 0 on success. */
static int load_file(Environment *env, clpd_package *pkg, const char *file,
                     FILE *err) {
  capture cap = {NULL, 0, 0};
  size_t modules = module_count(env);
  LoadError rc;
  struct stat st;
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
  if (!AddRouter(env, CAPTURE_ROUTER, 50, capture_query, capture_write, NULL,
                 NULL, NULL, &cap)) {
    fprintf(err, "clpdaemon: %s: cannot capture CLIPS messages\n", file);
    return 1;
  }
  rc = Load(env, file);
  DeleteRouter(env, CAPTURE_ROUTER);
  forward(&cap, pkg->manifest.name, err);
  free(cap.buf);
  if (rc != LE_NO_ERROR) {
    fprintf(err, "clpdaemon: %s: load failed\n", file);
    return 1;
  }
  /* Building a defmodule makes it the current module; a file that defines
   * one and switches back would still grow the module count. */
  if (GetCurrentModule(env) != pkg->module || module_count(env) != modules) {
    fprintf(err, "clpdaemon: %s: defmodule is not allowed in package files\n",
            file);
    return 1;
  }
  return 0;
}

/* Loads one package already appended to out; returns its error count. */
static int load_package(Environment *env, clpd_packages *out, FILE *err) {
  clpd_package *pkg = &out->items[out->count - 1];
  char build[CLPD_PACKAGE_NAME_MAX * 2 + 16];
  clpd_lint_exempt exempt;
  size_t i;

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
  return clpd_lint_package(env, pkg->module, pkg->manifest.name, &exempt, err);
}

int clpd_packages_load(Environment *env, const clpd_list *dirs,
                       clpd_packages *out, FILE *err) {
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
      perror("clpdaemon");
      exit(1);
    }
    out->items = grown;
    out->items[out->count].dir = clpd_strdup(dirs->items[i]);
    out->items[out->count].manifest = m;
    out->items[out->count].module = NULL;
    out->count++;
    errors += load_package(env, out, err);
    SetCurrentModule(env, main_module);
  }
  return errors != 0;
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
    clpd_manifest_free(&p->items[i].manifest);
  }
  free(p->items);
  p->items = NULL;
  p->count = 0;
}
