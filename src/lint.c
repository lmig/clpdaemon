/* Placeholder until T4c lands: reports no errors. */
#include "lint.h"

int clpd_lint_package(Environment *env, Defmodule *module, const char *pkg_name,
                      const clpd_lint_exempt *exempt, FILE *err) {
  (void)env;
  (void)module;
  (void)pkg_name;
  (void)exempt;
  (void)err;
  return 0;
}
