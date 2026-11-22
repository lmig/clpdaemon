/* Unit tests for the rule package loader (docs/packages.md, T4b). */
#define _POSIX_C_SOURCE 200809L

#include "packages.h"

#include "clock.h"

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

/* Called on every new environment before the packages load (NULL: none). */
static void (*load_setup)(Environment *env);

/* have_db < 0: the plain clpd_packages_load (no database). */
static void load_db(run *r, const char *const *dirs, size_t n, int have_db) {
  clpd_list list;
  FILE *err = tmpfile();
  size_t len;
  if (err == NULL) exit(2);
  list.items = (char **)dirs;
  list.count = n;
  r->env = CreateEnvironment();
  if (load_setup != NULL) load_setup(r->env);
  r->rc = have_db < 0 ? clpd_packages_load(r->env, &list, &r->pkgs, err)
                      : clpd_packages_load_ex(r->env, &list, have_db,
                                              &r->pkgs, err);
  rewind(err);
  len = fread(r->err, 1, sizeof r->err - 1, err);
  r->err[len] = '\0';
  fclose(err);
}

static void load(run *r, const char *const *dirs, size_t n) {
  load_db(r, dirs, n, -1);
}

static void done(run *r) {
  clpd_packages_free(&r->pkgs);
  DestroyEnvironment(r->env);
}

static int err_matches(const run *r, const char *want) {
  if (strcmp(r->err, want) != 0) {
    fprintf(stderr, "  got:  [%s]\n  want: [%s]\n", r->err, want);
    return 0;
  }
  return 1;
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

/* T9e (L87): every construct a package file defines must be in the
 * package's module. A construct in another package's module or in MAIN is
 * a load error naming the file, the construct type, its qualified name and
 * the package module, one line per construct in definition order; checked
 * per construct, so a file that switches back to its own module, a
 * redefinition of another package's construct (also one with the same
 * text), a defglobal evaluated into another module, a construct built
 * while a defglobal is evaluated (the build leaves MAIN current, so the
 * global lands there too), a deftemplate whose default switches the
 * module and a message-handler on another module's class are all
 * caught. */
static void test_construct_in_other_module(void) {
  const char *dirs[2];
  char want[8192], file[300];
  run r;
  const char *a = package("own-a", "definition = shop\nversion = 1\n"
                                   "files = s.clp\n");
  const char *b = package("own-b", "definition = b\nversion = 1\n"
                                   "files = f.clp, never.clp\n");
  write_file(a, "s.clp",
             "(deftemplate ev (slot wf-id))\n"
             "(defglobal ?*count* = 0)\n"
             "(defclass C (is-a USER))\n"
             "(deffunction f () 1)\n"
             "(deffunction g () 2)\n"
             "(defrule start (ev (wf-id ?w)) => )\n");
  write_file(b, "f.clp",
             "(deftemplate own (slot wf-id))\n"
             "(defrule shop@1::planted (ev (wf-id ?w)) => )\n"
             "(defrule b@1::back (own (wf-id ?w)) => )\n"
             "(defrule shop@1::start (ev (wf-id ?w)) => )\n"
             "(deftemplate after (slot wf-id))\n"
             "(deffunction shop@1::f () 1)\n"
             "(deftemplate MAIN::m (slot wf-id))\n"
             "(deffacts MAIN::facts)\n"
             "(defglobal MAIN ?*g* = 1)\n"
             "(defglobal shop@1 ?*count* = 0)\n"
             "(deffunction MAIN::fn () 1)\n"
             "(defgeneric MAIN::gen)\n"
             "(defmethod MAIN::gen2 ((?a INTEGER)) 1)\n"
             "(defclass MAIN::K (is-a USER))\n"
             "(definstances MAIN::inst)\n"
             "(defmessage-handler shop@1::C hi () 1)\n"
             "(defglobal b@1 ?*a* = (set-current-module MAIN)\n"
             "  ?*b* = (set-current-module b@1))\n"
             "(defmessage-handler USER hello () 1)\n"
             "(defglobal ?*z* = (build \"(deffunction MAIN::built () 1)\"))\n"
             "(deftemplate b@1::home (slot wf-id))\n"
             "(deftemplate sw (slot wf-id)\n"
             "  (slot s (default (set-current-module MAIN))))\n"
             "(deftemplate b@1::last (slot wf-id))\n");
  write_file(b, "never.clp", "(deftemplate never (slot wf-id))\n");
  dirs[0] = a;
  dirs[1] = b;
  load(&r, dirs, 2);
  EXPECT(r.rc != 0);
  snprintf(file, sizeof file, "clpdaemon: %s/f.clp: ", b);
  snprintf(want, sizeof want,
           "%sdefrule 'shop@1::planted' is outside the package module b@1\n"
           "%sdefrule 'shop@1::start' is outside the package module b@1\n"
           "%sdeftemplate 'shop@1::after' is outside the package module b@1\n"
           "%sdeffunction 'shop@1::f' is outside the package module b@1\n"
           "%sdeftemplate 'MAIN::m' is outside the package module b@1\n"
           "%sdeffacts 'MAIN::facts' is outside the package module b@1\n"
           "%sdefglobal 'MAIN::?*g*' is outside the package module b@1\n"
           "%sdefglobal 'shop@1::?*count*' is outside the package module "
           "b@1\n"
           "%sdeffunction 'MAIN::fn' is outside the package module b@1\n"
           "%sdefgeneric 'MAIN::gen' is outside the package module b@1\n"
           "%sdefmethod 'MAIN::gen2' is outside the package module b@1\n"
           "%sdefclass 'MAIN::K' is outside the package module b@1\n"
           "%sdefinstances 'MAIN::inst' is outside the package module b@1\n"
           "%sdefmessage-handler 'shop@1::C hi primary' is outside the "
           "package module b@1\n"
           "%sdefglobal 'MAIN::?*a*' is outside the package module b@1\n"
           "%sdefmessage-handler 'MAIN::USER hello primary' is outside the "
           "package module b@1\n"
           "%sdeffunction 'MAIN::built' is outside the package module b@1\n"
           "%sdefglobal 'MAIN::?*z*' is outside the package module b@1\n"
           "%sdeftemplate 'MAIN::sw' is outside the package module b@1\n",
           file, file, file, file, file, file, file, file, file, file, file,
           file, file, file, file, file, file, file, file);
  EXPECT(err_matches(&r, want));
  /* Loading the package stopped at the file. */
  EXPECT(FindDeftemplate(r.env, "b@1::never") == NULL);
  EXPECT(FindDeftemplate(r.env, "b@1::last") != NULL);
  EXPECT(strcmp(DefmoduleName(GetCurrentModule(r.env)), "MAIN") == 0);
  done(&r);
}

/* T9e review follow-ups: a defglobal whose value switches the current
 * module lands in that module, so the guard reports it; and an initial
 * value that turns global watching off cannot hide a defglobal of
 * another module: the file is rejected (fail closed) and the global is
 * still reported. */
static void test_module_guard_review(void) {
  const char *dirs[2];
  char want[2048];
  run r;
  const char *a = package("gshop", "definition = shop\nversion = 1\n"
                                   "files = s.clp\n");
  const char *b = package("gend", "definition = b\nversion = 1\n"
                                  "files = f.clp\n");
  const char *c = package("gunw", "definition = c\nversion = 1\n"
                                  "files = f.clp\n");
  write_file(a, "s.clp", "(deftemplate ev (slot wf-id))\n");
  write_file(b, "f.clp",
             "(deftemplate own (slot wf-id))\n"
             "(defglobal b@1 ?*a* = (set-current-module MAIN))\n");
  write_file(c, "f.clp",
             "(deftemplate own (slot wf-id))\n"
             "(defglobal shop@1 ?*u* = (unwatch globals))\n"
             "(deftemplate c@1::last (slot wf-id))\n");
  dirs[0] = a;
  dirs[1] = b;
  load(&r, dirs, 2);
  EXPECT(r.rc != 0);
  snprintf(want, sizeof want,
           "clpdaemon: %s/f.clp: defglobal 'MAIN::?*a*' is outside the "
           "package module b@1\n", b);
  EXPECT(err_matches(&r, want));
  done(&r);
  dirs[1] = c;
  load(&r, dirs, 2);
  EXPECT(r.rc != 0);
  snprintf(want, sizeof want,
           "clpdaemon: %s/f.clp: cannot check the modules of its constructs"
           " (a defglobal value changed the watch state of globals)\n"
           "clpdaemon: %s/f.clp: defglobal 'shop@1::?*u*' is outside the "
           "package module c@1\n", c, c);
  EXPECT(err_matches(&r, want));
  done(&r);
}

/* T9f (L88): a defglobal value that changes the watch state of globals
 * (watch or unwatch of globals or all, also of one global or with a
 * computed item) rejects the file with one line, and the guard neither
 * reports untouched globals of other modules nor misses the one whose
 * flag the value cleared; watching another item is allowed. */
static void test_watch_state_in_values(void) {
  static const struct {
    const char *name, *text, *defined; /* defined: NULL, or the line */
  } cases[] = {
      {"wwatch", "(defglobal ?*w* = (watch globals))\n", NULL},
      {"wone",
       "(defglobal shop@1 ?*u* = 1 ?*v* = (progn (unwatch globals u)\n"
       "  (set-current-module wone@1)))\n",
       "defglobal 'shop@1::?*u*' is outside the package module wone@1"},
      {"wall", "(defglobal ?*w* = (unwatch all))\n", NULL},
      {"wcomp", "(defglobal ?*w* = (watch (sym-cat glob als)))\n", NULL},
  };
  const char *dirs[2];
  char want[2048], manifest[128];
  size_t i, n;
  run r;
  dirs[0] = package("wshop", "definition = shop\nversion = 1\n"
                              "files = s.clp\n");
  write_file(dirs[0], "s.clp", "(defglobal ?*s* = 1 ?*t* = 2)\n");
  for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    snprintf(manifest, sizeof manifest,
             "definition = %s\nversion = 1\nfiles = f.clp\n", cases[i].name);
    dirs[1] = package(cases[i].name, manifest);
    write_file(dirs[1], "f.clp", cases[i].text);
    load(&r, dirs, 2);
    EXPECT(r.rc != 0);
    n = (size_t)snprintf(want, sizeof want,
                         "clpdaemon: %s/f.clp: cannot check the modules of "
                         "its constructs (a defglobal value changed the "
                         "watch state of globals)\n", dirs[1]);
    if (cases[i].defined != NULL)
      snprintf(want + n, sizeof want - n, "clpdaemon: %s/f.clp: %s\n",
               dirs[1], cases[i].defined);
    EXPECT(err_matches(&r, want));
    done(&r);
  }
  dirs[1] = package("wfacts", "definition = wfacts\nversion = 1\n"
                              "files = f.clp\n");
  write_file(dirs[1], "f.clp", "(defglobal ?*w* = (watch facts))\n");
  load(&r, dirs, 2);
  EXPECT(r.rc == 0);
  EXPECT(err_matches(&r, ""));
  done(&r);
}

/* T9f (L88): redefining a message-handler of another module's class is
 * reported also when its body is empty. */
static void test_empty_handler_redefined(void) {
  const char *dirs[2];
  char want[1024];
  run r;
  dirs[0] = package("hshop", "definition = shop\nversion = 1\n"
                              "files = s.clp\n");
  write_file(dirs[0], "s.clp", "(defclass A (is-a USER)\n"
                               "  (message-handler d))\n"
                               "(defmessage-handler A e ())\n");
  dirs[1] = package("hempty", "definition = h\nversion = 1\n"
                              "files = f.clp\n");
  write_file(dirs[1], "f.clp", "(defmessage-handler shop@1::A e ())\n"
                               "(defmessage-handler shop@1::A d ())\n");
  load(&r, dirs, 2);
  EXPECT(r.rc != 0);
  snprintf(want, sizeof want, "clpdaemon: %s/f.clp: defmessage-handler "
           "'shop@1::A e primary' is outside the package module h@1\n"
           "clpdaemon: %s/f.clp: defmessage-handler 'shop@1::A d primary' "
           "is outside the package module h@1\n", dirs[1], dirs[1]);
  EXPECT(err_matches(&r, want));
  done(&r);
}

/* A plugin-like function that makes CLIPS recompute the scope of the
 * globals (GetNextDefglobalInScope). */
static void scan_globals(Environment *env, UDFContext *ctx, UDFValue *ret) {
  (void)ctx;
  (void)ret;
  GetNextDefglobalInScope(env, NULL);
}

static void scan_setup(Environment *env) {
  AddUDF(env, "scan-globals", "v", 0, 0, NULL, scan_globals, "scan_globals",
         NULL);
}

/* T9f (L88): a defglobal value that makes CLIPS recompute the scope of
 * the globals defeats the defglobal check: the file is rejected (fail
 * closed) without reporting the untouched globals. */
static void test_scope_recomputed(void) {
  const char *dirs[2];
  char want[1024];
  run r;
  dirs[0] = package("sshop", "definition = shop\nversion = 1\n"
                              "files = s.clp\n");
  write_file(dirs[0], "s.clp", "(defglobal ?*s* = 1)\n");
  dirs[1] = package("scan", "definition = scan\nversion = 1\n"
                            "files = f.clp\n");
  write_file(dirs[1], "f.clp",
             "(defglobal ?*x* = (progn (set-current-module MAIN)\n"
             "  (set-current-module scan@1) (scan-globals)))\n");
  load_setup = scan_setup;
  load(&r, dirs, 2);
  load_setup = NULL;
  EXPECT(r.rc != 0);
  snprintf(want, sizeof want, "clpdaemon: %s/f.clp: cannot check the "
           "modules of its constructs\n", dirs[1]);
  EXPECT(err_matches(&r, want));
  done(&r);
}

/* What a test router collected from STDOUT. */
static char out_text[4096];
static size_t out_len;

static bool out_query(Environment *env, const char *name, void *ctx) {
  (void)env;
  (void)ctx;
  return strcmp(name, STDOUT) == 0;
}

static void out_write(Environment *env, const char *name, const char *str,
                      void *ctx) {
  size_t n = strlen(str);
  (void)env;
  (void)name;
  (void)ctx;
  if (n > sizeof out_text - 1 - out_len) n = sizeof out_text - 1 - out_len;
  memcpy(out_text + out_len, str, n);
  out_len += n;
  out_text[out_len] = '\0';
}

static bool watch_on;

static void watch_setup(Environment *env) {
  out_len = 0;
  out_text[0] = '\0';
  AddRouter(env, "test-stdout", 60, out_query, out_write, NULL, NULL, NULL,
            NULL);
  if (watch_on) Watch(env, GLOBALS);
}

/* T9f (L88): with global watching off or on, the guarded load leaves the
 * watch flags as plain CLIPS sets them for the same text and writes to
 * STDOUT exactly what plain CLIPS writes (nothing with watching off: the
 * guard prints no ":==" lines of its own). */
static void test_watch_flags_and_stdout(void) {
  static const char *const names[] = {"a", "b", "c"};
  static const char *const text =
      "(defglobal ?*a* = 1 ?*b* = (bind ?*a* 2))\n"
      "(defglobal ?*c* = 3)\n"
      "(defglobal ?*a* = 4)\n";
  const char *dirs[1];
  char file[512], plain_out[4096];
  int on, plain_flag[3];
  size_t i;
  run r;
  dirs[0] = package("wflags", "definition = wflags\nversion = 1\n"
                              "files = f.clp\n");
  write_file(dirs[0], "f.clp", text);
  snprintf(file, sizeof file, "%s/f.clp", dirs[0]);
  for (on = 0; on < 2; on++) {
    Environment *plain = CreateEnvironment();
    watch_on = on;
    watch_setup(plain);
    EXPECT(Load(plain, file) == LE_NO_ERROR);
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
      plain_flag[i] = DefglobalGetWatch(FindDefglobal(plain, names[i]));
    memcpy(plain_out, out_text, out_len + 1);
    DestroyEnvironment(plain);
    EXPECT(on ? strstr(plain_out, ":== ?*a*") != NULL
              : strcmp(plain_out, "") == 0);
    load_setup = watch_setup;
    load(&r, dirs, 1);
    load_setup = NULL;
    EXPECT(r.rc == 0);
    EXPECT(err_matches(&r, ""));
    if (strcmp(out_text, plain_out) != 0)
      fprintf(stderr, "  stdout: [%s]\n  plain:  [%s]\n", out_text,
              plain_out);
    EXPECT(strcmp(out_text, plain_out) == 0);
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
      char qualified[32];
      Defglobal *g;
      snprintf(qualified, sizeof qualified, "wflags@1::%s", names[i]);
      g = FindDefglobal(r.env, qualified);
      EXPECT(g != NULL && DefglobalGetWatch(g) == (bool)plain_flag[i]);
      EXPECT(plain_flag[i] == on);
    }
    done(&r);
  }
  watch_on = false;
}

/* T9f (L88): a file that leaves another module current although it
 * defined nothing there (a defglobal naming only a module) is an
 * error. */
static void test_file_ends_in_other_module(void) {
  const char *dirs[1];
  char want[1024];
  run r;
  dirs[0] = package("gends", "definition = e\nversion = 1\n"
                             "files = f.clp, never.clp\n");
  write_file(dirs[0], "f.clp", "(deftemplate own (slot wf-id))\n"
                               "(defglobal MAIN)\n");
  write_file(dirs[0], "never.clp", "(deftemplate never (slot wf-id))\n");
  load(&r, dirs, 1);
  EXPECT(r.rc != 0);
  snprintf(want, sizeof want, "clpdaemon: %s/f.clp: the file ends in module "
           "MAIN, not the package module e@1\n", dirs[0]);
  EXPECT(err_matches(&r, want));
  EXPECT(FindDeftemplate(r.env, "e@1::never") == NULL);
  done(&r);
}

/* Constructs of every type in the package's own module load, also with
 * the module named explicitly, and a later package loads after them. */
static void test_constructs_in_own_module(void) {
  const char *dirs[2];
  run r;
  const char *a = package("mine", "definition = mine\nversion = 1\n"
                                  "files = a.clp, b.clp\n"
                                  "lint_exempt = template:flag\n");
  const char *b = package("next", "definition = next\nversion = 1\n"
                                  "files = t.clp\n");
  write_file(a, "a.clp",
             "(deftemplate ev (slot wf-id))\n"
             "(deftemplate mine@1::other (slot wf-id))\n"
             "(defrule r (ev (wf-id ?w)) (flag) => )\n"
             "(defrule mine@1::q (other (wf-id ?w)) => )\n"
             "(deffacts seed (ev (wf-id w1)))\n"
             "(deffacts mine@1::more (other (wf-id w2)))\n"
             "(defglobal ?*x* = 1)\n"
             "(defglobal mine@1 ?*y* = 2 ?*x* = 3)\n"
             "(deffunction f () 1)\n"
             "(deffunction mine@1::f () 2)\n"
             "(defgeneric gen)\n"
             "(defmethod gen ((?a INTEGER)) 1)\n"
             "(defmethod mine@1::gen ((?a STRING)) 2)\n"
             "(defclass K (is-a USER) (slot s))\n"
             "(defmessage-handler K hi () 1)\n"
             "(defmessage-handler mine@1::K hi () 2)\n"
             "(definstances inst (k1 of K))\n");
  write_file(a, "b.clp", "(defrule mine@1::later (ev (wf-id ?w)) => )\n");
  write_file(b, "t.clp", tmpl);
  dirs[0] = a;
  dirs[1] = b;
  load(&r, dirs, 2);
  EXPECT(r.rc == 0 && err_matches(&r, ""));
  EXPECT(FindDefrule(r.env, "mine@1::later") != NULL &&
         FindDeftemplate(r.env, "next@1::ev") != NULL);
  /* The check leaves the watch flags of the globals as CLIPS set them. */
  EXPECT(!GetWatchState(r.env, GLOBALS));
  EXPECT(FindDefglobal(r.env, "mine@1::x") != NULL &&
         !DefglobalGetWatch(FindDefglobal(r.env, "mine@1::x")) &&
         !DefglobalGetWatch(FindDefglobal(r.env, "mine@1::y")));
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

/* T7b: the ingest list resolves to the package's own deftemplates; an
 * unknown name (also an implied template) and a name another package
 * already lists are load errors, each reported. */
static void test_ingest(void) {
  const char *dirs[3];
  char want[1024];
  run r;
  const char *a, *b, *c;
  a = package("ia", "definition = ia\nversion = 1\nfiles = t.clp\n"
                    "ingest = ev, order\n");
  write_file(a, "t.clp", "(deftemplate ev (slot wf-id))\n"
                         "(deftemplate order (slot wf-id))\n");
  dirs[0] = a;
  load(&r, dirs, 1);
  EXPECT(r.rc == 0 && strcmp(r.err, "") == 0);
  EXPECT(r.pkgs.count == 1 && r.pkgs.items[0].ingest != NULL &&
         r.pkgs.items[0].ingest[0] == FindDeftemplate(r.env, "ia@1::ev") &&
         r.pkgs.items[0].ingest[1] == FindDeftemplate(r.env, "ia@1::order"));
  EXPECT(strcmp(DefmoduleName(GetCurrentModule(r.env)), "MAIN") == 0);
  done(&r);

  b = package("ib", "definition = ib\nversion = 1\nfiles = t.clp\n"
                    "lint_exempt = rule:r, template:flag\n"
                    "ingest = order, nope, flag, ev\n");
  write_file(b, "t.clp", "(deftemplate order (slot wf-id))\n"
                         "(defrule r (flag) (order (wf-id ?w)) => )\n");
  c = package("ic", "definition = ic\nversion = 1\nfiles = t.clp\n"
                    "ingest = order\n");
  write_file(c, "t.clp", "(deftemplate order (slot wf-id))\n");
  dirs[0] = a;
  dirs[1] = b;
  dirs[2] = c;
  load(&r, dirs, 3);
  EXPECT(r.rc != 0);
  snprintf(want, sizeof want,
           "clpdaemon: ib@1: ingest names template 'order', already listed "
           "by ia@1\n"
           "clpdaemon: ib@1: ingest names unknown template 'nope'\n"
           "clpdaemon: ib@1: ingest names unknown template 'flag'\n"
           "clpdaemon: ib@1: ingest names template 'ev', already listed by "
           "ia@1\n"
           "clpdaemon: ic@1: ingest names template 'order', already listed "
           "by ia@1\n");
  if (strcmp(r.err, want) != 0)
    fprintf(stderr, "  got:  [%s]\n  want: [%s]\n", r.err, want);
  EXPECT(strcmp(r.err, want) == 0);
  EXPECT(r.pkgs.count == 3 && r.pkgs.items[1].ingest == NULL &&
         r.pkgs.items[2].ingest == NULL);
  done(&r);
}

static const char *orm_tmpl =
    "(deftemplate order (slot wf-id) (slot line-no (type INTEGER))\n"
    "  (slot status (type SYMBOL)) (slot note))\n";
static const char *orm_map =
    "order.table = shop_order\n"
    "order.owned = yes\n"
    "order.key = line-no\n"
    "order.slot.wf-id = wf_id text string error\n"
    "order.slot.line-no = line_no bigint integer error\n"
    "order.slot.status = status text symbol default\n"
    "order.slot.note = note text string nil\n"
    "seq.order_no = shop_order_no\n"
    "seq.order_no.owned = yes\n";

/* T9a: a package without `orm` loads as before, with or without a
 * database, and has no mapping. */
static void test_no_orm_unchanged(void) {
  const char *dirs[1];
  run r;
  int db;
  const char *a = package("noorm", "definition = noorm\nversion = 1\n"
                                   "files = t.clp\ningest = ev\n");
  write_file(a, "t.clp", tmpl);
  dirs[0] = a;
  for (db = -1; db <= 1; db++) {
    load_db(&r, dirs, 1, db);
    EXPECT(r.rc == 0 && strcmp(r.err, "") == 0);
    EXPECT(r.pkgs.count == 1 && r.pkgs.items[0].manifest.orm == NULL &&
           r.pkgs.items[0].orm == NULL && r.pkgs.items[0].orm_templates == NULL);
    EXPECT(r.pkgs.count == 1 && r.pkgs.items[0].ingest != NULL);
    done(&r);
  }
}

/* A valid mapping binds to the package's own deftemplates. */
static void test_orm_valid(void) {
  const char *dirs[1];
  char want[512];
  run r;
  const clpd_orm *m;
  const char *a = package("orm", "definition = shop\nversion = 1\n"
                                 "files = t.clp\norm = map/orm.conf\n");
  char sub[300];
  snprintf(sub, sizeof sub, "%s/map", a);
  mkdir(sub, 0700);
  write_file(a, "t.clp", orm_tmpl);
  write_file(sub, "orm.conf", orm_map);
  dirs[0] = a;
  load_db(&r, dirs, 1, 1);
  EXPECT(r.rc == 0 && err_matches(&r, ""));
  EXPECT(r.pkgs.count == 1 && r.pkgs.items[0].orm != NULL);
  if (r.pkgs.count == 1 && r.pkgs.items[0].orm != NULL) {
    m = r.pkgs.items[0].orm;
    snprintf(want, sizeof want, "%s/map/orm.conf", a);
    EXPECT(strcmp(r.pkgs.items[0].manifest.orm, want) == 0);
    EXPECT(strcmp(m->path, want) == 0);
    EXPECT(m->template_count == 1 && m->sequence_count == 1);
    EXPECT(r.pkgs.items[0].orm_templates != NULL &&
           r.pkgs.items[0].orm_templates[0] ==
               FindDeftemplate(r.env, "shop@1::order"));
    EXPECT(m->templates[0].key_columns != NULL &&
           m->templates[0].key_columns[0] == 0 &&
           m->templates[0].key_columns[1] == 1);
    EXPECT(strcmp(m->sequences[0].name, "shop_order_no") == 0);
  }
  EXPECT(strcmp(DefmoduleName(GetCurrentModule(r.env)), "MAIN") == 0);
  done(&r);
}

/* Without a database a mapping is an error, also through the plain
 * loader; the mapping is still checked. */
static void test_orm_needs_database(void) {
  const char *dirs[1];
  run r;
  int db;
  const char *a = package("ormnodb", "definition = nodb\nversion = 1\n"
                                     "files = t.clp\norm = orm.conf\n");
  write_file(a, "t.clp", orm_tmpl);
  write_file(a, "orm.conf", orm_map);
  dirs[0] = a;
  for (db = -1; db <= 0; db++) {
    load_db(&r, dirs, 1, db);
    EXPECT(r.rc != 0);
    EXPECT(err_matches(&r, "clpdaemon: nodb@1: orm: a mapping needs a "
                           "database (the configuration sets no database)\n"));
    done(&r);
  }
}

/* Mapping file errors keep the kvfile format, with the mapping path. */
static void test_orm_file_errors(void) {
  const char *dirs[2];
  char want[1024];
  run r;
  const char *a = package("ormsyn", "definition = syn\nversion = 1\n"
                                    "files = t.clp\norm = orm.conf\n");
  const char *b = package("ormgone", "definition = gone\nversion = 1\n"
                                     "files = t.clp\norm = none.conf\n");
  write_file(a, "t.clp", orm_tmpl);
  write_file(a, "orm.conf", "order.table = shop_order\norder.owned = maybe\n"
                            "order.slot.wf-id = wf_id text string\n");
  write_file(b, "t.clp", orm_tmpl);
  dirs[0] = a;
  dirs[1] = b;
  load_db(&r, dirs, 2, 0);
  EXPECT(r.rc != 0);
  snprintf(want, sizeof want,
           "clpdaemon: %s/orm.conf:2: order.owned: expected yes or no, got "
           "'maybe'\n"
           "clpdaemon: %s/orm.conf:3: order.slot.wf-id: expected '<column> "
           "<sql-type> <clips-type> <null>', got 'wf_id text string'\n"
           "clpdaemon: syn@1: orm: a mapping needs a database (the "
           "configuration sets no database)\n"
           "clpdaemon: %s/none.conf: cannot open: No such file or directory\n"
           "clpdaemon: gone@1: orm: a mapping needs a database (the "
           "configuration sets no database)\n",
           a, a, b);
  EXPECT(err_matches(&r, want));
  EXPECT(r.pkgs.count == 2 && r.pkgs.items[0].orm == NULL &&
         r.pkgs.items[1].orm == NULL);
  done(&r);
}

/* Checks against the package's deftemplates, in this order per template:
 * no wf-id, the slot lines (unknown slot, clips type its type constraint
 * does not allow), unknown key slots, then the template's slots
 * (multislot, unmapped). */
static void test_orm_bind_errors(void) {
  const char *dirs[1];
  run r;
  const char *a = package("ormbind", "definition = bind\nversion = 1\n"
                                     "files = t.clp\norm = orm.conf\n"
                                     "lint_exempt = template:ref, "
                                     "template:implied-fact\n");
  write_file(a, "t.clp",
             "(deftemplate order (slot wf-id) (slot n (type INTEGER))\n"
             "  (slot f (type FLOAT)) (slot s (type STRING))\n"
             "  (slot y (type SYMBOL)) (slot lex (type LEXEME))\n"
             "  (slot num (type NUMBER)) (slot any) (multislot tags)\n"
             "  (slot gone))\n"
             "(deftemplate ref (slot code))\n"
             "(deftemplate ok (slot wf-id) (slot lex (type LEXEME))\n"
             "  (slot num (type NUMBER)) (slot any) (slot n (type INTEGER))\n"
             "  (slot y (type SYMBOL STRING)))\n"
             "(defrule r (order (wf-id ?w)) => (assert (implied-fact)))\n");
  write_file(a, "orm.conf",
             "order.table = o\norder.owned = yes\norder.key = n, nope, tags\n"
             "order.slot.wf-id = wf_id text string error\n"
             "order.slot.n = n text string error\n"
             "order.slot.f = f bigint integer default\n"
             "order.slot.s = s text symbol default\n"
             "order.slot.y = y text string nil\n"
             "order.slot.lex = lex bigint integer nil\n"
             "order.slot.num = num text symbol default\n"
             "order.slot.any = any text symbol nil\n"
             "order.slot.tags = tags text string error\n"
             "order.slot.extra = extra text string nil\n"
             "ref.table = ref\nref.owned = no\n"
             "ref.slot.wf-id = wf_id text string error\n"
             "ref.slot.code = code text string nil\n"
             "nope.table = nope\nnope.owned = no\n"
             "implied-fact.table = imp\nimplied-fact.owned = no\n"
             "ok.table = ok\nok.owned = no\nok.key = n\n"
             "ok.slot.wf-id = wf_id text symbol error\n"
             "ok.slot.lex = lex text symbol nil\n"
             "ok.slot.num = num double float default\n"
             "ok.slot.any = any jsonb string nil\n"
             "ok.slot.n = n bigint integer error\n"
             "ok.slot.y = y text string default\n");
  dirs[0] = a;
  load_db(&r, dirs, 1, 1);
  EXPECT(r.rc != 0);
  EXPECT(err_matches(&r,
      "clpdaemon: bind@1: orm: template 'order': slot 'n' is mapped as "
      "string, which its type constraint does not allow\n"
      "clpdaemon: bind@1: orm: template 'order': slot 'f' is mapped as "
      "integer, which its type constraint does not allow\n"
      "clpdaemon: bind@1: orm: template 'order': slot 's' is mapped as "
      "symbol, which its type constraint does not allow\n"
      "clpdaemon: bind@1: orm: template 'order': slot 'y' is mapped as "
      "string, which its type constraint does not allow\n"
      "clpdaemon: bind@1: orm: template 'order': slot 'lex' is mapped as "
      "integer, which its type constraint does not allow\n"
      "clpdaemon: bind@1: orm: template 'order': slot 'num' is mapped as "
      "symbol, which its type constraint does not allow\n"
      "clpdaemon: bind@1: orm: template 'order' has no slot 'extra'\n"
      "clpdaemon: bind@1: orm: template 'order' has no slot 'nope'\n"
      "clpdaemon: bind@1: orm: template 'order': slot 'tags' is a "
      "multislot, which cannot be mapped\n"
      "clpdaemon: bind@1: orm: template 'order': slot 'gone' is not "
      "mapped\n"
      "clpdaemon: bind@1: orm: template 'ref' has no wf-id slot\n"
      "clpdaemon: bind@1: orm: unknown template 'nope'\n"
      "clpdaemon: bind@1: orm: unknown template 'implied-fact'\n"));
  EXPECT(r.pkgs.count == 1 && r.pkgs.items[0].orm_templates == NULL);
  done(&r);
}

/* A template of another package (or of MAIN) is not the package's own. */
static void test_orm_other_module(void) {
  const char *dirs[2];
  run r;
  const char *a = package("ormown1", "definition = own1\nversion = 1\n"
                                     "files = t.clp\n");
  const char *b = package("ormown2", "definition = own2\nversion = 1\n"
                                     "files = t.clp\norm = orm.conf\n");
  write_file(a, "t.clp", orm_tmpl);
  write_file(b, "t.clp", "(deftemplate other (slot wf-id))\n");
  write_file(b, "orm.conf", orm_map);
  dirs[0] = a;
  dirs[1] = b;
  load_db(&r, dirs, 2, 1);
  EXPECT(r.rc != 0);
  EXPECT(err_matches(&r, "clpdaemon: own2@1: orm: unknown template "
                         "'order'\n"));
  done(&r);
}

/* Tables and sequences belong to one definition: its versions may share
 * them, another definition may not (also after a failed check). */
static void test_orm_ownership(void) {
  const char *dirs[4];
  run r;
  const char *v1 = package("shop1", "definition = shop\nversion = 1\n"
                                    "files = t.clp\norm = orm.conf\n");
  const char *v2 = package("shop2", "definition = shop\nversion = 2\n"
                                    "files = t.clp\norm = orm.conf\n");
  const char *bad = package("crm1", "definition = crm\nversion = 1\n"
                                    "files = t.clp\norm = orm.conf\n");
  const char *late = package("crm2", "definition = billing\nversion = 1\n"
                                     "files = t.clp\norm = orm.conf\n");
  write_file(v1, "t.clp", orm_tmpl);
  write_file(v1, "orm.conf", orm_map);
  write_file(v2, "t.clp", orm_tmpl);
  write_file(v2, "orm.conf", orm_map);
  write_file(bad, "t.clp", "(deftemplate order (slot wf-id))\n"
                           "(deftemplate item (slot wf-id))\n");
  write_file(bad, "orm.conf",
             "order.table = crm_order\norder.owned = no\n"
             "order.slot.wf-id = wf_id text string error\n"
             "item.table = shop_order\nitem.owned = no\n"
             "item.slot.wf-id = wf_id text string error\n"
             "seq.n = shop_order_no\nseq.n.owned = no\n"
             "seq.m = crm_seq\nseq.m.owned = yes\n");
  write_file(late, "t.clp", "(deftemplate order (slot wf-id) (multislot x))\n");
  write_file(late, "orm.conf",
             "order.table = crm_order\norder.owned = no\n"
             "order.slot.wf-id = wf_id text string error\n"
             "seq.m = crm_seq\nseq.m.owned = yes\n");
  dirs[0] = v1;
  dirs[1] = v2;
  load_db(&r, dirs, 2, 1);
  EXPECT(r.rc == 0 && err_matches(&r, ""));
  done(&r);
  dirs[2] = bad;
  dirs[3] = late;
  load_db(&r, dirs, 4, 1);
  EXPECT(r.rc != 0);
  EXPECT(err_matches(&r,
      "clpdaemon: crm@1: orm: table 'shop_order' is already mapped by "
      "shop@1\n"
      "clpdaemon: crm@1: orm: sequence 'shop_order_no' is already mapped by "
      "shop@1\n"
      "clpdaemon: billing@1: orm: template 'order': slot 'x' is a "
      "multislot, which cannot be mapped\n"
      "clpdaemon: billing@1: orm: table 'crm_order' is already mapped by "
      "crm@1\n"
      "clpdaemon: billing@1: orm: sequence 'crm_seq' is already mapped by "
      "crm@1\n"));
  done(&r);
}

/* T9b (L83): the sql type must hold the clips type, null policy nil needs
 * a slot that allows SYMBOL, a sequence cannot have the name of a mapped
 * table (or a table that of a mapped sequence), per slot line in file
 * order and then per name. */
static void test_orm_mapping_checks(void) {
  const char *dirs[2];
  run r;
  const char *v1 = package("chkshop", "definition = shop\nversion = 1\n"
                                      "files = t.clp\norm = orm.conf\n");
  const char *c = package("chk", "definition = chk\nversion = 1\n"
                                 "files = t.clp\norm = orm.conf\n");
  write_file(v1, "t.clp", orm_tmpl);
  write_file(v1, "orm.conf", orm_map);
  write_file(c, "t.clp",
             "(deftemplate order (slot wf-id) (slot n (type INTEGER))\n"
             "  (slot f (type FLOAT)) (slot s (type STRING)) (slot b)\n"
             "  (slot x (type INTEGER FLOAT)) (slot y (type SYMBOL))\n"
             "  (slot j))\n"
             "(deftemplate item (slot wf-id))\n");
  write_file(c, "orm.conf",
             "order.table = chk_order\norder.owned = yes\n"
             "order.slot.wf-id = wf_id text string error\n"
             "order.slot.n = n bigint string nil\n"
             "order.slot.f = f double float nil\n"
             "order.slot.s = s bytea symbol default\n"
             "order.slot.b = b boolean integer nil\n"
             "order.slot.x = x numeric integer default\n"
             "order.slot.y = y timestamptz symbol nil\n"
             "order.slot.j = j jsonb float default\n"
             "item.table = shop_order_no\nitem.owned = no\n"
             "item.slot.wf-id = wf_id text string error\n"
             "seq.own = chk_order\nseq.own.owned = yes\n"
             "seq.other = shop_order\nseq.other.owned = no\n");
  dirs[0] = v1;
  dirs[1] = c;
  load_db(&r, dirs, 2, 1);
  EXPECT(r.rc != 0);
  EXPECT(err_matches(&r,
      "clpdaemon: chk@1: orm: template 'order': slot 'n': sql type bigint "
      "cannot hold clips type string (expected integer)\n"
      "clpdaemon: chk@1: orm: template 'order': slot 'n' is mapped as "
      "string, which its type constraint does not allow\n"
      "clpdaemon: chk@1: orm: template 'order': slot 'n' has null policy "
      "nil, but its type constraint does not allow SYMBOL\n"
      "clpdaemon: chk@1: orm: template 'order': slot 'f' has null policy "
      "nil, but its type constraint does not allow SYMBOL\n"
      "clpdaemon: chk@1: orm: template 'order': slot 's': sql type bytea "
      "cannot hold clips type symbol (expected string)\n"
      "clpdaemon: chk@1: orm: template 'order': slot 's' is mapped as "
      "symbol, which its type constraint does not allow\n"
      "clpdaemon: chk@1: orm: template 'order': slot 'x': sql type numeric "
      "cannot hold clips type integer (expected float, string or symbol)\n"
      "clpdaemon: chk@1: orm: template 'order': slot 'j': sql type jsonb "
      "cannot hold clips type float (expected string or symbol)\n"
      "clpdaemon: chk@1: orm: sequence 'chk_order' has the name of a table "
      "mapped by chk@1\n"
      "clpdaemon: chk@1: orm: sequence 'shop_order' has the name of a table "
      "mapped by shop@1\n"
      "clpdaemon: chk@1: orm: table 'shop_order_no' has the name of a "
      "sequence mapped by shop@1\n"));
  EXPECT(r.pkgs.count == 2 && r.pkgs.items[1].orm_templates == NULL);
  done(&r);
}

/* Versions of one definition that share a table map it identically: the
 * same owned flag, primary key columns, and columns with their sql types
 * and NOT NULL (null policy error or not). Slot names, clips types and
 * nil versus default may differ. Compared with the first earlier version
 * mapping the table. */
static void test_orm_versions_share_table(void) {
  static const char *const maps[] = {
      /* 2: owned */
      "order.table = shop_order\norder.owned = no\norder.key = line-no\n",
      /* 3: key */
      "order.table = shop_order\norder.owned = yes\n",
      /* 4: a column only in version 1 */
      "order.table = shop_order\norder.owned = yes\norder.key = line-no\n",
      /* 5: a column with another sql type */
      "order.table = shop_order\norder.owned = yes\norder.key = line-no\n",
      /* 6: NOT NULL */
      "order.table = shop_order\norder.owned = yes\norder.key = line-no\n",
      /* 7: identical table, nil instead of default, a string slot as
       * symbol */
      "order.table = shop_order\norder.owned = yes\norder.key = line-no\n"};
  static const char *const note[] = {
      "order.slot.note = note text string nil\n",
      "order.slot.note = note text string nil\n",
      "order.slot.note = note2 text string nil\n",
      "order.slot.note = note numeric string nil\n",
      "order.slot.note = note text string error\n",
      "order.slot.note = note text symbol nil\n"};
  const char *dirs[7];
  char text[1024], dname[32], manifest[128];
  run r;
  size_t i;
  dirs[0] = package("ver1", "definition = shop\nversion = 1\n"
                            "files = t.clp\norm = orm.conf\n");
  write_file(dirs[0], "t.clp", orm_tmpl);
  write_file(dirs[0], "orm.conf", orm_map);
  for (i = 0; i < 6; i++) {
    snprintf(dname, sizeof dname, "ver%zu", i + 2);
    snprintf(manifest, sizeof manifest, "definition = shop\nversion = %zu\n"
             "files = t.clp\norm = orm.conf\n", i + 2);
    dirs[i + 1] = package(dname, manifest);
    write_file(dirs[i + 1], "t.clp", orm_tmpl);
    snprintf(text, sizeof text,
             "%sorder.slot.wf-id = wf_id text string error\n"
             "order.slot.line-no = line_no bigint integer error\n"
             "order.slot.status = status text symbol nil\n%s",
             maps[i], note[i]);
    write_file(dirs[i + 1], "orm.conf", text);
  }
  load_db(&r, dirs, 7, 1);
  EXPECT(r.rc != 0);
  EXPECT(err_matches(&r,
      "clpdaemon: shop@2: orm: table 'shop_order' is mapped differently by "
      "shop@1 (owned)\n"
      "clpdaemon: shop@3: orm: table 'shop_order' is mapped differently by "
      "shop@1 (key)\n"
      "clpdaemon: shop@4: orm: table 'shop_order' is mapped differently by "
      "shop@1 (column 'note')\n"
      "clpdaemon: shop@5: orm: table 'shop_order' is mapped differently by "
      "shop@1 (column 'note')\n"
      "clpdaemon: shop@6: orm: table 'shop_order' is mapped differently by "
      "shop@1 (column 'note')\n"));
  EXPECT(r.pkgs.count == 7 && r.pkgs.items[6].orm_templates != NULL);
  done(&r);
}

static void clock_setup(Environment *env) {
  EXPECT(clpd_clock_define(env) == 0);
}

/* T11b: every generated package module imports MAIN::clpd-clock, so its
 * rules match the clock fact without a wf-id (lint-exempt); the loader
 * defines the template when the environment has none yet, and keeps the
 * one the core defined. */
static void test_clock_imported(void) {
  const char *dirs[2];
  int setup;
  run r;
  dirs[0] = package("clk-a", "definition = clka\nversion = 1\nfiles = a.clp\n");
  write_file(dirs[0], "a.clp",
             "(deftemplate due (slot wf-id) (slot at))\n"
             "(defrule ring ?d <- (due (wf-id ?w) (at ?at))\n"
             "  (clpd-clock (now-ms ?n&:(>= ?n ?at))) => (retract ?d))\n");
  dirs[1] = package("clk-b", "definition = clkb\nversion = 1\nfiles = b.clp\n");
  write_file(dirs[1], "b.clp", "(defrule tick (clpd-clock (now ?s)) =>)\n");
  for (setup = 0; setup < 2; setup++) {
    Deftemplate *core;
    load_setup = setup ? clock_setup : NULL;
    load(&r, dirs, 2);
    load_setup = NULL;
    EXPECT(r.rc == 0 && err_matches(&r, ""));
    core = FindDeftemplate(r.env, "MAIN::clpd-clock");
    EXPECT(core != NULL && clpd_clock_is_template(core));
    /* Visible from each package module as its own name. */
    SetCurrentModule(r.env, FindDefmodule(r.env, "clka@1"));
    EXPECT(FindDeftemplate(r.env, "clpd-clock") == core);
    SetCurrentModule(r.env, FindDefmodule(r.env, "clkb@1"));
    EXPECT(FindDeftemplate(r.env, "clpd-clock") == core);
    SetCurrentModule(r.env, FindDefmodule(r.env, "MAIN"));
    Reset(r.env);
    SetCurrentModule(r.env, FindDefmodule(r.env, "clka@1"));
    EXPECT(AssertString(r.env, "(due (wf-id w1) (at 5))") != NULL);
    SetCurrentModule(r.env, FindDefmodule(r.env, "MAIN"));
    EXPECT(AssertString(r.env, "(clpd-clock (now-ms 4) (now \"x\"))") != NULL);
    clpd_packages_focus(r.env, &r.pkgs);
    EXPECT(Run(r.env, -1) == 1); /* tick only: 4 < 5 */
    EXPECT(AssertString(r.env, "(clpd-clock (now-ms 5) (now \"y\"))") != NULL);
    clpd_packages_focus(r.env, &r.pkgs);
    EXPECT(Run(r.env, -1) == 2); /* ring and tick */
    done(&r);
  }
}

/* T11b: a package cannot define its own clpd-clock (the name is reserved
 * for the core's template), with or without a module name; the load fails
 * after CLIPS's own diagnostics and names the file. */
static void test_clock_reserved(void) {
  static const char *const texts[] = {
      "(deftemplate clpd-clock (slot now-ms))\n",
      "(deftemplate own@1::clpd-clock\n  \"comment\" (slot x))\n"};
  const char *dirs[1];
  char want[2048];
  size_t i;
  run r;
  dirs[0] = package("own", "definition = own\nversion = 1\nfiles = c.clp, never.clp\n");
  for (i = 0; i < 2; i++) {
    write_file(dirs[0], "c.clp", texts[i]);
    load(&r, dirs, 1);
    snprintf(want, sizeof want,
             "clpdaemon: own@1: clips: [CSTRCPSR3] %s/c.clp, Line %d: Cannot "
             "define deftemplate 'clpd-clock' because of an import/export "
             "conflict.\n"
             "clpdaemon: own@1: clips: ERROR:\n"
             "clpdaemon: own@1: clips: (deftemplate own@1::clpd-clock\n"
             "clpdaemon: %s/c.clp: load failed\n"
             "clpdaemon: %s/c.clp: deftemplate 'clpd-clock' is reserved by the "
             "core (MAIN::clpd-clock)\n",
             dirs[0], 1, dirs[0], dirs[0]);
    EXPECT(r.rc != 0);
    EXPECT(err_matches(&r, want));
    EXPECT(FindDeftemplate(r.env, "own@1::clpd-clock") == NULL ||
           clpd_clock_is_template(FindDeftemplate(r.env, "own@1::clpd-clock")));
    done(&r);
  }
  /* MAIN::clpd-clock itself: outside the package module, and reserved. */
  write_file(dirs[0], "c.clp", "(deftemplate MAIN::clpd-clock (slot x))\n");
  load(&r, dirs, 1);
  snprintf(want, sizeof want,
           "clpdaemon: %s/c.clp: deftemplate 'clpd-clock' is reserved by the "
           "core (MAIN::clpd-clock)\n"
           "clpdaemon: %s/c.clp: deftemplate 'MAIN::clpd-clock' is outside "
           "the package module own@1\n",
           dirs[0], dirs[0]);
  EXPECT(r.rc != 0);
  EXPECT(err_matches(&r, want));
  done(&r);
  /* A template whose name only starts with it is not the clock. */
  write_file(dirs[0], "c.clp", "(deftemplate clpd-clocks (slot wf-id))\n");
  write_file(dirs[0], "never.clp", "");
  load(&r, dirs, 1);
  EXPECT(r.rc == 0 && err_matches(&r, ""));
  done(&r);
}

int main(void) {
  if (mkdtemp(root) == NULL) {
    perror("packages_test");
    return 2;
  }
  test_clock_imported();
  test_clock_reserved();
  test_two_packages_focus();
  test_no_packages();
  test_missing_file_and_next_package();
  test_clips_error_is_forwarded();
  test_directory_in_files();
  test_defmodule_rejected();
  test_construct_in_other_module();
  test_module_guard_review();
  test_watch_state_in_values();
  test_empty_handler_redefined();
  test_scope_recomputed();
  test_watch_flags_and_stdout();
  test_file_ends_in_other_module();
  test_constructs_in_own_module();
  test_duplicate_package();
  test_manifest_error();
  test_ingest();
  test_no_orm_unchanged();
  test_orm_valid();
  test_orm_needs_database();
  test_orm_file_errors();
  test_orm_bind_errors();
  test_orm_other_module();
  test_orm_ownership();
  test_orm_mapping_checks();
  test_orm_versions_share_table();
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
