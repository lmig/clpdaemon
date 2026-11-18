/* Unit tests for the package manifest (docs/packages.md, T4a). */
#define _POSIX_C_SOURCE 200809L

#include "manifest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../common/expect.h"

static char root[] = "/tmp/clpd_manifest_test.XXXXXX";
static char dir[sizeof root + 64];
static char conf[sizeof dir + 32];

/* Creates <root>/<name>/package.conf with `text` and loads the manifest;
 * the error stream is captured into errbuf. */
static int load(const char *name, const char *text, clpd_manifest *m,
                char *errbuf, size_t errcap) {
  FILE *f, *err;
  size_t n;
  int rc;

  snprintf(dir, sizeof dir, "%s/%s", root, name);
  snprintf(conf, sizeof conf, "%s/package.conf", dir);
  mkdir(dir, 0700);
  if (text != NULL) {
    f = fopen(conf, "w");
    if (f == NULL || fputs(text, f) < 0 || fclose(f) != 0) {
      perror("manifest_test");
      exit(2);
    }
  }
  err = tmpfile();
  if (err == NULL) exit(2);
  rc = clpd_manifest_load(dir, m, err);
  rewind(err);
  n = fread(errbuf, 1, errcap - 1, err);
  errbuf[n] = '\0';
  fclose(err);
  remove(conf);
  rmdir(dir);
  return rc;
}

/* Expected text with %s replaced by the manifest path. */
static int err_is(const char *errbuf, const char *fmt) {
  char want[2048];
  snprintf(want, sizeof want, fmt, conf, conf, conf, conf, conf, conf);
  if (strcmp(errbuf, want) != 0) {
    fprintf(stderr, "  got:  [%s]\n  want: [%s]\n", errbuf, want);
    return 0;
  }
  return 1;
}

static void test_valid(void) {
  clpd_manifest m;
  char eb[1024], want[sizeof dir + 64];
  EXPECT(load("ok", "# fibre\ndefinition = fibre-installation\nversion = v3\n"
                    "files = templates.clp, rules/main.clp\n"
                    "lint_exempt = template:catalog, rule:load-catalog, "
                    "template:region\n",
              &m, eb, sizeof eb) == 0);
  EXPECT(strcmp(eb, "") == 0);
  EXPECT(strcmp(m.name, "fibre-installation@v3") == 0);
  EXPECT(strcmp(m.definition, "fibre-installation") == 0);
  EXPECT(strcmp(m.version, "v3") == 0);
  EXPECT(m.files.count == 2);
  snprintf(want, sizeof want, "%s/templates.clp", dir);
  EXPECT(m.files.count == 2 && strcmp(m.files.items[0], want) == 0);
  snprintf(want, sizeof want, "%s/rules/main.clp", dir);
  EXPECT(m.files.count == 2 && strcmp(m.files.items[1], want) == 0);
  EXPECT(m.exempt_templates.count == 2 &&
         strcmp(m.exempt_templates.items[0], "catalog") == 0 &&
         strcmp(m.exempt_templates.items[1], "region") == 0);
  EXPECT(m.exempt_rules.count == 1 &&
         strcmp(m.exempt_rules.items[0], "load-catalog") == 0);
  clpd_manifest_free(&m);
}

static void test_no_exemptions(void) {
  clpd_manifest m;
  char eb[1024];
  EXPECT(load("min", "definition = a\nversion = 1\nfiles = a.clp\nlint_exempt =\n",
              &m, eb, sizeof eb) == 0);
  EXPECT(strcmp(eb, "") == 0);
  EXPECT(strcmp(m.name, "a@1") == 0);
  EXPECT(m.exempt_templates.count == 0 && m.exempt_rules.count == 0);
  clpd_manifest_free(&m);
}

static void test_missing_keys(void) {
  clpd_manifest m;
  char eb[1024];
  EXPECT(load("empty", "# nothing\n", &m, eb, sizeof eb) == 1);
  EXPECT(err_is(eb, "clpdaemon: %s: missing required key 'definition'\n"
                    "clpdaemon: %s: missing required key 'version'\n"
                    "clpdaemon: %s: missing required key 'files'\n"));
  clpd_manifest_free(&m);
}

static void test_invalid_values(void) {
  clpd_manifest m;
  char eb[2048];
  EXPECT(load("bad", "definition = fibre@v3\nversion =\nfiles =\n"
                     "lint_exempt = catalog, rule:\nfoo = 1\n"
                     "definition = x\n",
              &m, eb, sizeof eb) == 1);
  EXPECT(err_is(eb,
                "clpdaemon: %s:1: definition: expected 1-64 characters from "
                "[A-Za-z0-9._-], got 'fibre@v3'\n"
                "clpdaemon: %s:2: version: expected 1-64 characters from "
                "[A-Za-z0-9._-], got ''\n"
                "clpdaemon: %s:3: files: expected at least one file\n"
                "clpdaemon: %s:4: lint_exempt: expected template:<name> or "
                "rule:<name>, got 'catalog'\n"
                "clpdaemon: %s:5: unknown key 'foo'\n"
                "clpdaemon: %s:6: duplicate key 'definition' (first set on "
                "line 1)\n"));
  clpd_manifest_free(&m);
}

static void test_file_paths(void) {
  clpd_manifest m;
  char eb[1024];
  EXPECT(load("paths", "definition = a\nversion = 1\nfiles = a.clp, /etc/b.clp\n",
              &m, eb, sizeof eb) == 1);
  EXPECT(err_is(eb, "clpdaemon: %s:3: files: '/etc/b.clp' must be relative to "
                    "the package directory\n"));
  clpd_manifest_free(&m);
  EXPECT(load("paths2", "definition = a\nversion = 1\nfiles = a.clp,,b.clp\n",
              &m, eb, sizeof eb) == 1);
  EXPECT(err_is(eb, "clpdaemon: %s:3: files: empty list item\n"));
  clpd_manifest_free(&m);
}

static void test_empty_exemption_name(void) {
  clpd_manifest m;
  char eb[1024];
  EXPECT(load("ex", "definition = a\nversion = 1\nfiles = a.clp\n"
                    "lint_exempt = template:\n",
              &m, eb, sizeof eb) == 1);
  EXPECT(err_is(eb, "clpdaemon: %s:4: lint_exempt: expected template:<name> "
                    "or rule:<name>, got 'template:'\n"));
  clpd_manifest_free(&m);
}

static void test_missing_manifest(void) {
  clpd_manifest m;
  char eb[1024], want[sizeof conf + 64];
  EXPECT(load("none", NULL, &m, eb, sizeof eb) == 1);
  snprintf(want, sizeof want,
           "clpdaemon: %s: cannot open: No such file or directory\n", conf);
  EXPECT(strcmp(eb, want) == 0);
  clpd_manifest_free(&m);
}

int main(void) {
  if (mkdtemp(root) == NULL) {
    perror("manifest_test");
    return 2;
  }
  test_valid();
  test_no_exemptions();
  test_missing_keys();
  test_invalid_values();
  test_file_paths();
  test_empty_exemption_name();
  test_missing_manifest();
  rmdir(root);
  if (failures != 0) {
    fprintf(stderr, "%d manifest checks failed\n", failures);
    return 1;
  }
  puts("manifest checks passed");
  return 0;
}
