/* Rule package loader (FR-2, FR-6, FR-7; docs/packages.md). */
#ifndef CLPD_PACKAGES_H
#define CLPD_PACKAGES_H

#include <stdio.h>

#include "clips.h"
#include "kvfile.h"
#include "manifest.h"

typedef struct {
  char *dir;
  clpd_manifest manifest;
  Defmodule *module; /* definition@version */
  /* The deftemplates named by manifest.ingest, in the same order; NULL
   * until the package loaded and its ingest list was checked. */
  Deftemplate **ingest;
} clpd_package;

typedef struct {
  clpd_package *items;
  size_t count;
} clpd_packages;

/* Loads each package directory in order: manifest, generated defmodule,
 * files, wf-id lint, ingest list. Errors are written to err, one line each; loading
 * continues with the next package so every error is reported. Leaves MAIN
 * as the current module. Returns 0 when every package loaded cleanly. */
int clpd_packages_load(Environment *env, const clpd_list *dirs,
                       clpd_packages *out, FILE *err);
/* Replaces the focus stack with MAIN under every package module, the first
 * package on top, so one Run can fire the rules of all packages. */
void clpd_packages_focus(Environment *env, const clpd_packages *p);
void clpd_packages_free(clpd_packages *p);

#endif
