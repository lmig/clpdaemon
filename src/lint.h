/* wf-id lint for workflow rule packages (FR-7, L38). */
#ifndef CLPD_LINT_H
#define CLPD_LINT_H

#include <stddef.h>
#include <stdio.h>

#include "clips.h"

/* Names exempted by the package manifest (lint_exempt). */
typedef struct {
  const char *const *templates;
  size_t template_count;
  const char *const *rules;
  size_t rule_count;
} clpd_lint_exempt;

/* Lints the deftemplates and defrules of package module `module`. Each
 * error is one line "clpdaemon: <pkg>: lint: <message>" on `err`, where
 * <pkg> is `pkg_name` (definition@version). Leaves the current module of
 * `env` unchanged. Returns the number of errors (0 = clean). */
int clpd_lint_package(Environment *env, Defmodule *module, const char *pkg_name,
                      const clpd_lint_exempt *exempt, FILE *err);

#endif
