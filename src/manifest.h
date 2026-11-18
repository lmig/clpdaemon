/* Rule package manifest: <dir>/package.conf (docs/packages.md). */
#ifndef CLPD_MANIFEST_H
#define CLPD_MANIFEST_H

#include <stdio.h>

#include "kvfile.h"

#define CLPD_MANIFEST_FILE "package.conf"
#define CLPD_PACKAGE_NAME_MAX 64 /* definition and version, each */

typedef struct {
  char *definition;
  char *version;
  char *name;       /* definition@version, the package module name */
  clpd_list files;  /* paths joined to the package directory, in order */
  clpd_list exempt_templates;
  clpd_list exempt_rules;
  clpd_list ingest; /* deftemplate names, unique within the list */
} clpd_manifest;

/* Loads and validates <dir>/package.conf. Every error is written to err,
 * one line each. Returns 0 when valid; otherwise nonzero, and m must still
 * be freed. */
int clpd_manifest_load(const char *dir, clpd_manifest *m, FILE *err);
void clpd_manifest_free(clpd_manifest *m);

#endif
