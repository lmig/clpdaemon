/* Unit tests for the rule package loader (docs/packages.md, T4b). */
#define _POSIX_C_SOURCE 200809L

#include "packages.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../common/expect.h"

static char root[] = "/tmp/clpd_packages_test.XXXXXX";

static void write_file(const char *dir, const char *name, const char *text) {
  char path[512];
  FILE *f;
  snprintf(path, sizeof path, "%s/%s", dir, name);
  f = fopen(path, "w");
  if (f == NULL || fputs(text, f) < 0 || fclose(f) != 0) {
    perror("packages_test");
    exit(2);
  }
}

/* Creates <root>/<name>/ with a manifest and returns its path (static). */
static const char *package(const char *name, const char *manifest) {
  static char dirs[8][256];
  static int next;
  char *dir = dirs[next++ % 8];
  snprintf(dir, 256, "%s/%s", root, name);
  mkdir(dir, 0700);
  write_file(dir, "package.conf", manifest);
  return dir;
}

typedef struct {
  clpd_packages pkgs;
  Environment *env;
  char err[4096];
  int rc;
} run;

static void load(run *r, const char *const *dirs, size_t n) {
  clpd_list list;
  FILE *err = tmpfile();
  size_t len;
  if (err == NULL) exit(2);
  list.items = (char **)dirs;
  list.count = n;
  r->env = CreateEnvironment();
  r->rc = clpd_packages_load(r->env, &list, &r->pkgs, err);
  rewind(err);
  len = fread(r->err, 1, sizeof r->err - 1, err);
  r->err[len] = '\0';
  fclose(err);
}

static void done(run *r) {
  clpd_packages_free(&r->pkgs);
  DestroyEnvironment(r->env);
}

static const char *tmpl = "(deftemplate ev (slot wf-id))\n";

/* Two packages load into their own modules; focus lets the rules of both
 * fire, the first package on top of the focus stack. */
static void test_two_packages_focus(void) {
  const char *dirs[2];
  run r;
  const char *a, *b;
  a = package("a", "definition = alpha\nversion = v1\nfiles = t.clp, r.clp\n");
  write_file(a, "t.clp", tmpl);
  write_file(a, "r.clp", "(deftemplate seen (slot wf-id))\n"
                         "(defrule ra (ev (wf-id ?w)) => (assert (seen (wf-id ?w))))\n");
  b = package("b", "definition = beta\nversion = v2\nfiles = all.clp\n");
  write_file(b, "all.clp", "(deftemplate ev (slot wf-id))\n"
                           "(defrule rb (ev (wf-id ?w)) => (assert (ev (wf-id done))))\n");
  dirs[0] = a;
  dirs[1] = b;
  load(&r, dirs, 2);
  EXPECT(r.rc == 0);
  EXPECT(strcmp(r.err, "") == 0);
  EXPECT(r.pkgs.count == 2);
  EXPECT(FindDefmodule(r.env, "alpha@v1") != NULL);
  EXPECT(FindDefmodule(r.env, "beta@v2") != NULL);
  EXPECT(r.pkgs.count == 2 && r.pkgs.items[0].module == FindDefmodule(r.env, "alpha@v1"));
  EXPECT(strcmp(DefmoduleName(GetCurrentModule(r.env)), "MAIN") == 0);
  Reset(r.env);
  SetCurrentModule(r.env, FindDefmodule(r.env, "alpha@v1"));
  EXPECT(AssertString(r.env, "(ev (wf-id w1))") != NULL);
  SetCurrentModule(r.env, FindDefmodule(r.env, "MAIN"));
  SetCurrentModule(r.env, FindDefmodule(r.env, "beta@v2"));
  EXPECT(AssertString(r.env, "(ev (wf-id w2))") != NULL);
  SetCurrentModule(r.env, FindDefmodule(r.env, "MAIN"));
  EXPECT(Run(r.env, -1) == 0); /* MAIN has no rules: nothing fires */
  clpd_packages_focus(r.env, &r.pkgs);
  EXPECT(GetFocus(r.env) == FindDefmodule(r.env, "alpha@v1"));
  /* One Run fires ra (alpha), then rb twice (beta: w2, then done). */
  EXPECT(Run(r.env, -1) == 3);
  clpd_packages_focus(r.env, &r.pkgs); /* repeated focus does not pile up */
  EXPECT(Run(r.env, -1) == 0);
  EXPECT(GetFocus(r.env) == NULL || GetFocus(r.env) == FindDefmodule(r.env, "MAIN"));
  done(&r);
}

static void test_no_packages(void) {
  run r;
  load(&r, NULL, 0);
  EXPECT(r.rc == 0 && r.pkgs.count == 0 && strcmp(r.err, "") == 0);
  clpd_packages_focus(r.env, &r.pkgs);
  EXPECT(Run(r.env, -1) == 0);
  done(&r);
}

static void test_missing_file_and_next_package(void) {
  const char *dirs[2];
  char want[1024];
  run r;
  const char *a = package("m", "definition = m\nversion = 1\nfiles = t.clp, gone.clp\n");
  const char *b = package("ok", "definition = ok\nversion = 1\nfiles = t.clp\n");
  write_file(a, "t.clp", tmpl);
  write_file(b, "t.clp", tmpl);
  dirs[0] = a;
  dirs[1] = b;
  load(&r, dirs, 2);
  snprintf(want, sizeof want,
           "clpdaemon: %s/gone.clp: cannot open: No such file or directory\n", a);
  EXPECT(r.rc != 0);
  EXPECT(strcmp(r.err, want) == 0);
  EXPECT(FindDefmodule(r.env, "ok@1") != NULL); /* later packages still load */
  done(&r);
}

static void test_clips_error_is_forwarded(void) {
  const char *dirs[1];
  char tail[512];
  run r;
  const char *a = package("syn", "definition = syn\nversion = 1\nfiles = bad.clp, never.clp\n");
  write_file(a, "bad.clp", "(deftemplate ev (slot wf-id))\n(defrule r (ev (wf-id ?w)) =>\n");
  dirs[0] = a;
  load(&r, dirs, 1);
  EXPECT(r.rc != 0);
  /* CLIPS diagnostics, prefixed, then the failure line; never.clp is not
   * reported because loading stops at the first failed file. */
  EXPECT(strncmp(r.err, "clpdaemon: syn@1: clips: [", 26) == 0);
  snprintf(tail, sizeof tail, "\nclpdaemon: %s/bad.clp: load failed\n", a);
  EXPECT(strlen(r.err) > strlen(tail) &&
         strcmp(r.err + strlen(r.err) - strlen(tail), tail) == 0);
  EXPECT(strstr(r.err, "\nclpdaemon: syn@1: clips: \n") == NULL); /* no blank lines */
  EXPECT(strstr(r.err, "never.clp") == NULL);
  done(&r);
}

/* A directory named in files is not a loadable file. */
static void test_directory_in_files(void) {
  const char *dirs[1];
  char want[512], sub[300];
  run r;
  const char *a = package("dirf", "definition = dirf\nversion = 1\nfiles = sub\n");
  snprintf(sub, sizeof sub, "%s/sub", a);
  mkdir(sub, 0700);
  dirs[0] = a;
  load(&r, dirs, 1);
  snprintf(want, sizeof want, "clpdaemon: %s/sub: cannot open: Is a directory\n", a);
  EXPECT(r.rc != 0);
  EXPECT(strcmp(r.err, want) == 0);
  done(&r);
}

static void test_defmodule_rejected(void) {
  const char *dirs[1];
  char want[512];
  run r;
  const char *a = package("dm", "definition = dm\nversion = 1\nfiles = m.clp\n");
  write_file(a, "m.clp", "(defmodule other)\n(deftemplate ev (slot wf-id))\n");
  dirs[0] = a;
  load(&r, dirs, 1);
  snprintf(want, sizeof want,
           "clpdaemon: %s/m.clp: defmodule is not allowed in package files\n", a);
  EXPECT(r.rc != 0);
  EXPECT(strcmp(r.err, want) == 0);
  EXPECT(strcmp(DefmoduleName(GetCurrentModule(r.env)), "MAIN") == 0);
  done(&r);
}

static void test_duplicate_package(void) {
  const char *dirs[2];
  char want[1024];
  run r;
  const char *a = package("d1", "definition = dup\nversion = 1\nfiles = t.clp\n");
  const char *b = package("d2", "definition = dup\nversion = 1\nfiles = t.clp\n");
  write_file(a, "t.clp", tmpl);
  write_file(b, "t.clp", tmpl);
  dirs[0] = a;
  dirs[1] = b;
  load(&r, dirs, 2);
  snprintf(want, sizeof want,
           "clpdaemon: %s: package dup@1 is already loaded from %s\n", b, a);
  EXPECT(r.rc != 0);
  EXPECT(strcmp(r.err, want) == 0);
  done(&r);
}

static void test_manifest_error(void) {
  const char *dirs[1];
  char want[1024];
  run r;
  const char *a = package("bad", "definition = x\nversion = 1\n");
  dirs[0] = a;
  load(&r, dirs, 1);
  snprintf(want, sizeof want,
           "clpdaemon: %s/package.conf: missing required key 'files'\n", a);
  EXPECT(r.rc != 0);
  EXPECT(strcmp(r.err, want) == 0);
  EXPECT(r.pkgs.count == 0);
  done(&r);
}

int main(void) {
  if (mkdtemp(root) == NULL) {
    perror("packages_test");
    return 2;
  }
  test_two_packages_focus();
  test_no_packages();
  test_missing_file_and_next_package();
  test_clips_error_is_forwarded();
  test_directory_in_files();
  test_defmodule_rejected();
  test_duplicate_package();
  test_manifest_error();
  if (failures != 0) {
    fprintf(stderr, "%d package checks failed (fixtures in %s)\n", failures, root);
    return 1;
  }
  {
    char cmd[sizeof root + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0) fprintf(stderr, "cannot remove %s\n", root);
  }
  puts("package checks passed");
  return 0;
}
