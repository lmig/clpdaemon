/* Unit tests for the wf-id lint of workflow rule packages (FR-7, L38). */
#include "lint.h"

#include <string.h>

#include "../common/expect.h"

#define PKG "pkg@v1"
#define PFX "clpdaemon: " PKG ": lint: "

static const clpd_lint_exempt no_exempt = {NULL, 0, NULL, 0};

/* Templates shared by most cases; all carry wf-id. */
static const char *const base_templates[] = {
    "(deftemplate order (slot wf-id) (slot id))",
    "(deftemplate item (slot wf-id) (slot qty))",
    NULL,
};

/* Builds a MAIN template/rule without wf-id (must never be reported), then
 * the package module with the base templates and `constructs`, makes MAIN
 * current, lints the package and checks the full error text, the return
 * value and that MAIN is still the current module. */
/* When set, constructs are built with conserve-mem on (no pretty-print). */
static int conserve_memory;

static void check(const char *name, const char *const *constructs,
                  const clpd_lint_exempt *exempt, int want_count,
                  const char *want_err) {
  Environment *env = CreateEnvironment();
  Defmodule *main_mod, *pkg;
  FILE *err;
  char got[4096];
  size_t n;
  int count;
  size_t i;

  EXPECT(env != NULL);
  if (env == NULL) return;
  EXPECT(Build(env, "(deftemplate main-tmpl (slot x))") == BE_NO_ERROR);
  EXPECT(Build(env, "(defrule main-rule (main-tmpl (x 1)) =>)") ==
         BE_NO_ERROR);
  EXPECT(Build(env, "(defmodule " PKG ")") == BE_NO_ERROR);
  for (i = 0; base_templates[i] != NULL; i++)
    EXPECT(Build(env, base_templates[i]) == BE_NO_ERROR);
  if (conserve_memory) SetConserveMemory(env, true);
  for (i = 0; constructs[i] != NULL; i++) {
    BuildError be = Build(env, constructs[i]);
    if (be != BE_NO_ERROR)
      fprintf(stderr, "%s: Build failed (%d): %s\n", name, (int)be,
              constructs[i]);
    EXPECT(be == BE_NO_ERROR);
  }
  main_mod = FindDefmodule(env, "MAIN");
  pkg = FindDefmodule(env, PKG);
  EXPECT(main_mod != NULL && pkg != NULL);
  SetCurrentModule(env, main_mod);

  err = tmpfile();
  EXPECT(err != NULL);
  if (err == NULL) {
    DestroyEnvironment(env);
    return;
  }
  count = clpd_lint_package(env, pkg, PKG, exempt, err);
  rewind(err);
  n = fread(got, 1, sizeof got - 1, err);
  got[n] = '\0';
  fclose(err);

  if (count != want_count || strcmp(got, want_err) != 0)
    fprintf(stderr, "%s: got %d errors:\n%s--- expected %d:\n%s---\n", name,
            count, got, want_count, want_err);
  EXPECT(count == want_count);
  EXPECT(strcmp(got, want_err) == 0);
  EXPECT(GetCurrentModule(env) == main_mod);
  DestroyEnvironment(env);
}

static void test_clean_package(void) {
  static const char *const c[] = {
      "(defrule r (order (wf-id ?w) (id ?i)) (item (wf-id ?w)) => "
      "(assert (item (wf-id x))))",
      NULL};
  check("clean", c, &no_exempt, 0, "");
}

static void test_template_without_wf_id(void) {
  static const char *const c[] = {
      "(deftemplate audit (slot msg))",
      "(deftemplate note (slot text))",
      NULL};
  check("template", c, &no_exempt, 2,
        PFX "template 'audit' has no wf-id slot\n"
        PFX "template 'note' has no wf-id slot\n");
}

static void test_exempt_template_and_its_patterns(void) {
  static const char *const c[] = {
      "(deftemplate catalog (slot code))",
      "(defrule r (order (wf-id ?w)) (catalog (code ?c)) "
      "(catalog (code 1)) (item (wf-id ?w)) =>)",
      NULL};
  static const char *const t[] = {"catalog"};
  clpd_lint_exempt ex = {t, 1, NULL, 0};
  check("exempt template", c, &ex, 0, "");
}

static void test_ordered_fact_pattern(void) {
  static const char *const c[] = {
      "(defrule r (order (wf-id ?w)) (flag ?w) =>)", NULL};
  check("ordered", c, &no_exempt, 2,
        PFX "template 'flag' has no wf-id slot\n"
        PFX "rule 'r': pattern 2 (flag) does not bind wf-id to a variable\n");
}

static void test_non_variable_values(void) {
  static const char *const c[] = {
      "(deftemplate multi (multislot wf-id))",
      "(defrule constants (order (wf-id \"abc\")) (item (wf-id x)) "
      "(order (wf-id 42)) =>)",
      "(defrule multifield (multi (wf-id $?w)) =>)",
      "(defrule wildcard (order (wf-id ?)) (item (wf-id ~x)) =>)",
      "(defrule missing (order (id ?i)) (item (qty 1) (wf-id ?w)) =>)",
      NULL};
  check("non-variable", c, &no_exempt, 7,
        PFX "rule 'constants': pattern 1 (order) does not bind wf-id to a "
            "variable\n"
        PFX "rule 'constants': pattern 2 (item) does not bind wf-id to a "
            "variable\n"
        PFX "rule 'constants': pattern 3 (order) does not bind wf-id to a "
            "variable\n"
        PFX "rule 'multifield': pattern 1 (multi) does not bind wf-id to a "
            "variable\n"
        PFX "rule 'wildcard': pattern 1 (order) does not bind wf-id to a "
            "variable\n"
        PFX "rule 'wildcard': pattern 2 (item) does not bind wf-id to a "
            "variable\n"
        PFX "rule 'missing': pattern 1 (order) does not bind wf-id to a "
            "variable\n");
}

static void test_mismatched_variables(void) {
  static const char *const c[] = {
      "(defrule r (order (wf-id ?w)) (item (wf-id ?v)) "
      "(item (wf-id ?w)) (order (wf-id ?x)) =>)",
      NULL};
  check("mismatch", c, &no_exempt, 2,
        PFX "rule 'r': pattern 2 (item) binds wf-id to ?v, expected ?w from "
            "pattern 1\n"
        PFX "rule 'r': pattern 4 (order) binds wf-id to ?x, expected ?w from "
            "pattern 1\n");
}

/* The first checked pattern sets the expected variable, even when an earlier
 * pattern failed or was exempt. */
static void test_expected_from_first_checked_pattern(void) {
  static const char *const c[] = {
      "(deftemplate catalog (slot code))",
      "(defrule r (catalog (code 1)) (order (wf-id ?w)) (item (wf-id ?v)) =>)",
      NULL};
  static const char *const t[] = {"catalog"};
  clpd_lint_exempt ex = {t, 1, NULL, 0};
  check("first checked", c, &ex, 1,
        PFX "rule 'r': pattern 3 (item) binds wf-id to ?v, expected ?w from "
            "pattern 2\n");
}

static void test_constraints_after_variable(void) {
  static const char *const c[] = {
      "(defrule r (order (wf-id ?w&:(neq ?w x))) (item (wf-id ?w&~y)) "
      "(order (wf-id ?w&~z) (id \"a\\\"(b\")) =>)",
      NULL};
  check("constraints", c, &no_exempt, 0, "");
}

/* An or-constraint also matches the other alternative, which may be another
 * workflow instance, so it does not bind wf-id (FR-7). */
static void test_or_constraint_is_not_a_join(void) {
  static const char *const c[] = {
      "(defrule r (order (wf-id ?w)) (item (wf-id ?w|z)) (item (wf-id ?w&:(neq ?w q)|z)) =>)",
      NULL};
  check("or-constraint", c, &no_exempt, 2,
        PFX "rule 'r': pattern 2 (item) does not bind wf-id to a variable\n"
        PFX "rule 'r': pattern 3 (item) does not bind wf-id to a variable\n");
}

/* Without its pretty-print form a rule cannot be checked: fail closed. */
static void test_rule_without_pp_form(void) {
  static const char *const c[] = {"(defrule r (order (wf-id 1)) =>)", NULL};
  conserve_memory = 1;
  check("no-ppform", c, &no_exempt, 1,
        PFX "rule 'r': cannot be checked without its pretty-print form\n");
  conserve_memory = 0;
}

static void test_nesting_and_numbering(void) {
  static const char *const c[] = {
      "(defrule r \"doc => (x)\" "
      "(logical ?f <- (order (wf-id ?w))) "
      "(not (item (wf-id ?v))) "
      "(exists (item (wf-id ?w))) "
      "(or (item (wf-id ?w)) (and (order (wf-id 7)) (item (wf-id ?w)))) "
      "(forall (order (wf-id ?w)) (item (wf-id ?x))) "
      "=> (retract ?f))",
      NULL};
  check("nesting", c, &no_exempt, 3,
        PFX "rule 'r': pattern 2 (item) binds wf-id to ?v, expected ?w from "
            "pattern 1\n"
        PFX "rule 'r': pattern 5 (order) does not bind wf-id to a variable\n"
        PFX "rule 'r': pattern 8 (item) binds wf-id to ?x, expected ?w from "
            "pattern 1\n");
}

static void test_test_and_declare_skipped(void) {
  static const char *const c[] = {
      "(defrule r (declare (salience 10)) (order (wf-id ?w)) "
      "(test (eq ?w (create$ x))) (item (wf-id ?v)) =>)",
      NULL};
  check("test/declare", c, &no_exempt, 1,
        PFX "rule 'r': pattern 2 (item) binds wf-id to ?v, expected ?w from "
            "pattern 1\n");
}

static void test_exempt_rule(void) {
  static const char *const c[] = {
      "(defrule load-catalog (order (wf-id 1)) (item (qty 2)) =>)",
      "(defrule bad (order (wf-id 1)) =>)",
      NULL};
  static const char *const r[] = {"load-catalog"};
  clpd_lint_exempt ex = {NULL, 0, r, 1};
  check("exempt rule", c, &ex, 1,
        PFX "rule 'bad': pattern 1 (order) does not bind wf-id to a "
            "variable\n");
}

static void test_unknown_exemptions(void) {
  static const char *const c[] = {
      "(deftemplate audit (slot msg))",
      "(defrule bad (order (wf-id 1)) =>)",
      NULL};
  static const char *const t[] = {"nope", "audit", "main-tmpl"};
  static const char *const r[] = {"ghost", "main-rule", "bad"};
  clpd_lint_exempt ex = {t, 3, r, 3};
  check("unknown exemptions", c, &ex, 4,
        PFX "lint_exempt names unknown template 'nope'\n"
        PFX "lint_exempt names unknown template 'main-tmpl'\n"
        PFX "lint_exempt names unknown rule 'ghost'\n"
        PFX "lint_exempt names unknown rule 'main-rule'\n");
}

static void test_rule_without_patterns(void) {
  static const char *const c[] = {
      "(defrule start => (assert (order (wf-id 1))))",
      "(defrule only-test (test (> 2 1)) =>)",
      NULL};
  check("no patterns", c, &no_exempt, 0, "");
}

/* The current module before the call (here a third module) is restored and
 * a NULL exemption list is accepted. */
static void test_current_module_restored(void) {
  Environment *env = CreateEnvironment();
  Defmodule *pkg, *other;
  FILE *err = tmpfile();
  EXPECT(env != NULL && err != NULL);
  if (env == NULL || err == NULL) return;
  EXPECT(Build(env, "(defmodule " PKG ")") == BE_NO_ERROR);
  EXPECT(Build(env, "(deftemplate audit (slot msg))") == BE_NO_ERROR);
  EXPECT(Build(env, "(defmodule other)") == BE_NO_ERROR);
  EXPECT(Build(env, "(deftemplate other-tmpl (slot x))") == BE_NO_ERROR);
  pkg = FindDefmodule(env, PKG);
  other = FindDefmodule(env, "other");
  EXPECT(GetCurrentModule(env) == other);
  EXPECT(clpd_lint_package(env, pkg, PKG, NULL, err) == 1);
  EXPECT(GetCurrentModule(env) == other);
  fclose(err);
  DestroyEnvironment(env);
}

int main(void) {
  test_clean_package();
  test_template_without_wf_id();
  test_exempt_template_and_its_patterns();
  test_ordered_fact_pattern();
  test_non_variable_values();
  test_mismatched_variables();
  test_expected_from_first_checked_pattern();
  test_constraints_after_variable();
  test_or_constraint_is_not_a_join();
  test_rule_without_pp_form();
  test_nesting_and_numbering();
  test_test_and_declare_skipped();
  test_exempt_rule();
  test_unknown_exemptions();
  test_rule_without_patterns();
  test_current_module_restored();
  if (failures != 0) {
    fprintf(stderr, "%d lint checks failed\n", failures);
    return 1;
  }
  puts("lint checks passed");
  return 0;
}
