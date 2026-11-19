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

/* have_db < 0: the plain clpd_packages_load (no database). */
static void load_db(run *r, const char *const *dirs, size_t n, int have_db) {
  clpd_list list;
  FILE *err = tmpfile();
  size_t len;
  if (err == NULL) exit(2);
  list.items = (char **)dirs;
  list.count = n;
  r->env = CreateEnvironment();
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

static int err_matches(const run *r, const char *want) {
  if (strcmp(r->err, want) != 0) {
    fprintf(stderr, "  got:  [%s]\n  want: [%s]\n", r->err, want);
    return 0;
  }
  return 1;
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
