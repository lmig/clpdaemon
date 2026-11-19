#define _POSIX_C_SOURCE 200809L

#include "packages.h"

#include "lint.h"
#include "log.h"

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
      clpd_log_die_errno();
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

/* The non-implied deftemplate `name` of the current module, or NULL. */
static Deftemplate *own_template(Environment *env, const char *name) {
  Deftemplate *t;
  for (t = GetNextDeftemplate(env, NULL); t != NULL;
       t = GetNextDeftemplate(env, t))
    if (!t->implied && strcmp(DeftemplateName(t), name) == 0) return t;
  return NULL;
}

/* Resolves the ingest list of the last package of out to its own
 * deftemplates (T7b, docs/ingest.md): a name an earlier package lists, or
 * that is not a deftemplate of this package, is an error. Sets pkg->ingest
 * only when every name resolved. Returns the error count. */
static int check_ingest(Environment *env, clpd_packages *out, FILE *err) {
  clpd_package *pkg = &out->items[out->count - 1];
  const clpd_list *names = &pkg->manifest.ingest;
  Deftemplate **found = calloc(names->count + 1, sizeof *found);
  size_t i, j, k;
  int errors = 0;

  if (found == NULL) clpd_log_die_errno();
  SetCurrentModule(env, pkg->module);
  for (i = 0; i < names->count; i++) {
    const char *other = NULL;
    for (j = 0; j + 1 < out->count && other == NULL; j++)
      for (k = 0; k < out->items[j].manifest.ingest.count; k++)
        if (strcmp(out->items[j].manifest.ingest.items[k], names->items[i]) ==
            0) {
          other = out->items[j].manifest.name;
          break;
        }
    if (other != NULL) {
      fprintf(err, "clpdaemon: %s: ingest names template '%s', already "
                   "listed by %s\n", pkg->manifest.name, names->items[i], other);
      errors++;
    } else if ((found[i] = own_template(env, names->items[i])) == NULL) {
      fprintf(err, "clpdaemon: %s: ingest names unknown template '%s'\n",
              pkg->manifest.name, names->items[i]);
      errors++;
    }
  }
  if (errors == 0)
    pkg->ingest = found;
  else
    free(found);
  return errors;
}

#define ORM_PREFIX "clpdaemon: %s: orm: "

static TemplateSlot *slot_of(Deftemplate *t, const char *name) {
  TemplateSlot *s;
  for (s = t->slotList; s != NULL; s = s->next)
    if (strcmp(s->slotName->contents, name) == 0) return s;
  return NULL;
}

/* 1 when the slot's type constraint allows the CLIPS type (an
 * unconstrained slot allows every type). */
static int type_allowed(Deftemplate *t, const char *slot,
                        clpd_orm_clips_type type) {
  static const char *const symbols[CLPD_ORM_CLIPS_TYPE_COUNT] = {
      "INTEGER", "FLOAT", "STRING", "SYMBOL"};
  CLIPSValue v;
  size_t i;
  if (!DeftemplateSlotTypes(t, slot, &v)) return 0;
  for (i = 0; i < v.multifieldValue->length; i++) {
    CLIPSValue *e = &v.multifieldValue->contents[i];
    if (e->header->type == SYMBOL_TYPE &&
        strcmp(e->lexemeValue->contents, symbols[type]) == 0)
      return 1;
  }
  return 0;
}

/* Checks one mapped template against its deftemplate, in this order: no
 * wf-id, the slot lines in file order (unknown slot, a clips type the
 * slot does not allow), key slots that are no slot of the template, then
 * the template's slots (multislot, unmapped). Returns the error count. */
static int bind_template(Deftemplate *d, clpd_orm_template *t,
                         const char *pkg, FILE *err) {
  TemplateSlot *s;
  size_t i;
  int errors = 0, has_wf_id = slot_of(d, CLPD_ORM_WF_ID) != NULL;

  if (!has_wf_id) {
    fprintf(err, ORM_PREFIX "template '%s' has no wf-id slot\n", pkg, t->name);
    errors++;
  }
  for (i = 0; i < t->column_count; i++) {
    const clpd_orm_column *c = &t->columns[i];
    if ((s = slot_of(d, c->slot)) == NULL) {
      if (has_wf_id || strcmp(c->slot, CLPD_ORM_WF_ID) != 0) {
        fprintf(err, ORM_PREFIX "template '%s' has no slot '%s'\n", pkg,
                t->name, c->slot);
        errors++;
      }
    } else if (!s->multislot && !type_allowed(d, c->slot, c->clips_type)) {
      fprintf(err, ORM_PREFIX "template '%s': slot '%s' is mapped as %s, "
                   "which its type constraint does not allow\n", pkg, t->name,
              c->slot, clpd_orm_clips_type_name(c->clips_type));
      errors++;
    }
  }
  for (i = 0; i < t->key.count; i++) {
    if (clpd_orm_column_of(t, t->key.items[i]) == NULL &&
        slot_of(d, t->key.items[i]) == NULL) {
      fprintf(err, ORM_PREFIX "template '%s' has no slot '%s'\n", pkg,
              t->name, t->key.items[i]);
      errors++;
    }
  }
  for (s = d->slotList; s != NULL; s = s->next) {
    if (s->multislot) {
      fprintf(err, ORM_PREFIX "template '%s': slot '%s' is a multislot, "
                   "which cannot be mapped\n", pkg, t->name,
              s->slotName->contents);
      errors++;
    } else if (clpd_orm_column_of(t, s->slotName->contents) == NULL) {
      fprintf(err, ORM_PREFIX "template '%s': slot '%s' is not mapped\n", pkg,
              t->name, s->slotName->contents);
      errors++;
    }
  }
  /* Cannot fail here: wf-id and every key slot are mapped slots. */
  if (errors == 0 && clpd_orm_resolve_key(t) != 0) errors++;
  return errors;
}

/* The first earlier package of another definition whose mapping names
 * the table (or, with seq, the sequence) `name`, or NULL. */
static const char *mapped_by(const clpd_packages *out, const char *name,
                             int seq) {
  const clpd_package *pkg = &out->items[out->count - 1];
  size_t i, j;
  for (i = 0; i + 1 < out->count; i++) {
    const clpd_package *o = &out->items[i];
    /* check_orm stores o->orm only after clpd_orm_parse succeeded, and a
     * valid mapping has a table for every template and a name for every
     * sequence, so neither strcmp below sees NULL. */
    if (o->orm == NULL ||
        strcmp(o->manifest.definition, pkg->manifest.definition) == 0)
      continue;
    if (seq) {
      for (j = 0; j < o->orm->sequence_count; j++)
        if (strcmp(o->orm->sequences[j].name, name) == 0)
          return o->manifest.name;
    } else {
      for (j = 0; j < o->orm->template_count; j++)
        if (strcmp(o->orm->templates[j].table, name) == 0)
          return o->manifest.name;
    }
  }
  return NULL;
}

/* Reads the mapping file of the last package of out and checks it (T9a,
 * docs/orm.md): the file, a configured database, the package's own
 * deftemplates, then tables and sequences an earlier package of another
 * definition maps. Sets pkg->orm when the file is valid (so later packages
 * see its tables) and pkg->orm_templates when every check passed. Returns
 * the error count. */
static int check_orm(Environment *env, clpd_packages *out, int have_database,
                     FILE *err) {
  clpd_package *pkg = &out->items[out->count - 1];
  const char *name = pkg->manifest.name, *other;
  clpd_orm *orm;
  Deftemplate **found;
  size_t i;
  int errors = 0;

  if (pkg->manifest.orm == NULL) return 0;
  orm = malloc(sizeof *orm);
  if (orm == NULL) clpd_log_die_errno();
  if (clpd_orm_parse(pkg->manifest.orm, orm, err) != 0) {
    clpd_orm_free(orm);
    free(orm);
    orm = NULL;
    errors++;
  }
  if (!have_database) {
    fprintf(err, ORM_PREFIX "a mapping needs a database (the configuration "
                 "sets no database)\n", name);
    errors++;
  }
  if (orm == NULL) return errors;
  pkg->orm = orm;
  found = calloc(orm->template_count + 1, sizeof *found);
  if (found == NULL) clpd_log_die_errno();
  SetCurrentModule(env, pkg->module);
  for (i = 0; i < orm->template_count; i++) {
    clpd_orm_template *t = &orm->templates[i];
    if ((found[i] = own_template(env, t->name)) == NULL) {
      fprintf(err, ORM_PREFIX "unknown template '%s'\n", name, t->name);
      errors++;
    } else {
      errors += bind_template(found[i], t, name, err);
    }
  }
  for (i = 0; i < orm->template_count; i++) {
    if ((other = mapped_by(out, orm->templates[i].table, 0)) != NULL) {
      fprintf(err, ORM_PREFIX "table '%s' is already mapped by %s\n", name,
              orm->templates[i].table, other);
      errors++;
    }
  }
  for (i = 0; i < orm->sequence_count; i++) {
    if ((other = mapped_by(out, orm->sequences[i].name, 1)) != NULL) {
      fprintf(err, ORM_PREFIX "sequence '%s' is already mapped by %s\n", name,
              orm->sequences[i].name, other);
      errors++;
    }
  }
  if (errors == 0)
    pkg->orm_templates = found;
  else
    free(found);
  return errors;
}

/* Loads one package already appended to out; returns its error count. */
static int load_package(Environment *env, clpd_packages *out,
                        int have_database, FILE *err) {
  clpd_package *pkg = &out->items[out->count - 1];
  char build[CLPD_PACKAGE_NAME_MAX * 2 + 16];
  clpd_lint_exempt exempt;
  size_t i;
  int errors;

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
  errors = clpd_lint_package(env, pkg->module, pkg->manifest.name, &exempt, err);
  errors += check_ingest(env, out, err);
  return errors + check_orm(env, out, have_database, err);
}

int clpd_packages_load_ex(Environment *env, const clpd_list *dirs,
                          int have_database, clpd_packages *out, FILE *err) {
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
      clpd_log_die_errno();
    }
    out->items = grown;
    out->items[out->count].dir = clpd_strdup(dirs->items[i]);
    out->items[out->count].manifest = m;
    out->items[out->count].module = NULL;
    out->items[out->count].ingest = NULL;
    out->items[out->count].orm = NULL;
    out->items[out->count].orm_templates = NULL;
    out->count++;
    errors += load_package(env, out, have_database, err);
    SetCurrentModule(env, main_module);
  }
  return errors != 0;
}

int clpd_packages_load(Environment *env, const clpd_list *dirs,
                       clpd_packages *out, FILE *err) {
  return clpd_packages_load_ex(env, dirs, 0, out, err);
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
    free(p->items[i].ingest);
    free(p->items[i].orm_templates);
    if (p->items[i].orm != NULL) clpd_orm_free(p->items[i].orm);
    free(p->items[i].orm);
    clpd_manifest_free(&p->items[i].manifest);
  }
  free(p->items);
  p->items = NULL;
  p->count = 0;
}
