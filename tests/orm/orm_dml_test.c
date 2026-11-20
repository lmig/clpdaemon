/* Tests for the ORM DML functions db-insert, db-update, db-delete, db-load
 * and db-count (T10d, FR-24, docs/orm.md#dml-functions).
 * usage: orm_dml_test                         (a temporary SQLite file)
 *        orm_dml_test <conninfo> <password>   (PostgreSQL, run by
 *                                              tests/db/pg_cluster.sh)
 * The functions are called by a rule of a package (the rule dml-call of
 * each test package evaluates the expression of a dml-call fact through
 * the test function test-eval), through Eval at the top level (no rule
 * executing), or by a rule of a module that is no package. Some calls run
 * inside a cycle, as the daemon's run_cycle does it
 * (clpd_orm_sync_begin_cycle, Run, clpd_orm_sync_end_cycle). The rows are
 * read back with SELECT; every refused call must leave every table as it
 * was. clpd_db_exec is wrapped (-Wl,--wrap) to record the statements the
 * functions send and to make one of them fail with a real database
 * error. */
#define _POSIX_C_SOURCE 200809L

#include "db.h"
#include "log.h"
#include "orm_ddl.h"
#include "orm_sync.h"
#include "orm_tx.h"
#include "packages.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "expect.h"

static char root[] = "/tmp/clpd_orm_dml_test.XXXXXX";
static Environment *env;
static clpd_packages pkgs;
static clpd_orm_ddl ddl;
static clpd_orm_syncer *syncer;
static clpd_orm_tx tx;
static clpd_db *db;
static int pg;
static char db_spec[512];
static const char *password;

clpd_db_status __real_clpd_db_exec(clpd_db *db, const char *sql,
                                   const clpd_db_value *params, size_t n,
                                   clpd_db_result **result);
clpd_db_status __wrap_clpd_db_exec(clpd_db *db, const char *sql,
                                   const clpd_db_value *params, size_t n,
                                   clpd_db_result **result);

/* Seam: while recording, every statement but SELECT current_schema() is
 * appended to stmts, one per line; the next statement containing fail_sql
 * runs a query on a missing table instead (once). */
static int recording;
static char stmts[8192];
static const char *fail_sql;
#define MISSING_TABLE "clpd_missing_table"

clpd_db_status __wrap_clpd_db_exec(clpd_db *d, const char *sql,
                                   const clpd_db_value *params, size_t n,
                                   clpd_db_result **result) {
  if (recording && strcmp(sql, "SELECT current_schema()") != 0) {
    size_t len = strlen(stmts);
    snprintf(stmts + len, sizeof stmts - len, "%s\n", sql);
  }
  if (fail_sql != NULL && strstr(sql, fail_sql) != NULL) {
    fail_sql = NULL;
    return __real_clpd_db_exec(d, "SELECT 1 FROM " MISSING_TABLE, NULL, 0,
                               result);
  }
  return __real_clpd_db_exec(d, sql, params, n, result);
}

static void write_file(const char *dir, const char *name, const char *text) {
  char path[512];
  FILE *f;
  snprintf(path, sizeof path, "%s/%s", dir, name);
  f = fopen(path, "w");
  if (f == NULL || fputs(text, f) < 0 || fclose(f) != 0) {
    perror("orm_dml_test");
    exit(2);
  }
}

static const char *package(const char *name, const char *manifest) {
  static char dirs[8][256];
  static int next;
  char *dir = dirs[next++ % 8];
  snprintf(dir, 256, "%s/%s", root, name);
  mkdir(dir, 0700);
  write_file(dir, "package.conf", manifest);
  return dir;
}

/* Every test package has this rule: a dml-call fact (of the instance
 * "call", which no table holds) makes a rule of the package call the
 * expression. */
#define DML_CALL_RULE                                                      \
  "(deftemplate dml-call (slot wf-id) (slot expr (type STRING)))\n"       \
  "(defrule dml-call ?f <- (dml-call (wf-id ?w) (expr ?e))\n"             \
  "  => (retract ?f) (test-eval ?e))\n"

static const char *shop_clp =
    DML_CALL_RULE
    "(deftemplate order (slot wf-id) (slot line-no (type INTEGER))\n"
    "  (slot status (type SYMBOL)))\n"
    "(deftemplate item (slot wf-id) (slot n (type INTEGER))\n"
    "  (slot qty (type INTEGER) (default 0)) (slot label (type SYMBOL))\n"
    "  (slot price) (slot paid (type INTEGER) (default 0))\n"
    "  (slot data (type STRING) (default \"\")) (slot at) (slot doc))\n";
static const char *shop_orm =
    "order.table = shop_order\n"
    "order.owned = yes\n"
    "order.key = line-no\n"
    "order.slot.wf-id = wf_id text string error\n"
    "order.slot.line-no = line_no bigint integer error\n"
    "order.slot.status = status text symbol default\n"
    "item.table = shop_item\n"
    "item.owned = yes\n"
    "item.sync = manual\n"
    "item.key = n\n"
    "item.slot.wf-id = wf_id text string error\n"
    "item.slot.n = n bigint integer error\n"
    "item.slot.qty = qty bigint integer default\n"
    "item.slot.label = label text symbol nil\n"
    "item.slot.price = price double float nil\n"
    "item.slot.paid = paid boolean integer default\n"
    "item.slot.data = data bytea string default\n"
    "item.slot.at = placed_at timestamptz string nil\n"
    "item.slot.doc = doc jsonb string nil\n";
static const char *solo_clp =
    DML_CALL_RULE "(deftemplate ticket (slot wf-id) (slot title))\n";
static const char *solo_orm =
    "ticket.table = solo_ticket\n"
    "ticket.owned = yes\n"
    "ticket.sync = manual\n"
    "ticket.slot.wf-id = wf_id text string error\n"
    "ticket.slot.title = title text string nil\n";

static int same(const char *what, const char *got, const char *want) {
  if (strcmp(got, want) != 0) {
    fprintf(stderr, "  %s\n    got:  [%s]\n    want: [%s]\n", what, got, want);
    return 0;
  }
  return 1;
}

static void sql(const char *text) {
  if (clpd_db_exec(db, text, NULL, 0, NULL) != CLPD_DB_OK) {
    fprintf(stderr, "orm_dml_test: %s: %s\n", text, clpd_db_error(db));
    exit(2);
  }
}

/* --- reading back ------------------------------------------------------ */

/* Appends v as text: NULL, an integer, %g, 'text', x'hex'. */
static void put_value(char *out, size_t cap, const clpd_db_value *v) {
  size_t len = strlen(out), i;
  switch (v->type) {
  case CLPD_DB_NULL:
    snprintf(out + len, cap - len, "NULL");
    break;
  case CLPD_DB_INTEGER:
    snprintf(out + len, cap - len, "%lld", (long long)v->integer);
    break;
  case CLPD_DB_FLOAT:
    snprintf(out + len, cap - len, "%g", v->real);
    break;
  case CLPD_DB_TEXT:
    snprintf(out + len, cap - len, "'%s'", (const char *)v->data);
    break;
  case CLPD_DB_BLOB:
    snprintf(out + len, cap - len, "x'");
    for (i = 0; i < v->size; i++) {
      len = strlen(out);
      snprintf(out + len, cap - len, "%02x",
               ((const unsigned char *)v->data)[i]);
    }
    len = strlen(out);
    snprintf(out + len, cap - len, "'");
    break;
  }
}

/* The rows of a query, "v v v; v v v" (values as put_value). */
static void query(const char *text, char *out, size_t cap) {
  clpd_db_result *r = NULL;
  size_t i, j, len;
  int was = recording;
  recording = 0;
  out[0] = '\0';
  if (__real_clpd_db_exec(db, text, NULL, 0, &r) != CLPD_DB_OK) {
    snprintf(out, cap, "error: %s", clpd_db_error(db));
    recording = was;
    return;
  }
  for (i = 0; i < clpd_db_result_rows(r); i++) {
    for (j = 0; j < clpd_db_result_columns(r); j++) {
      len = strlen(out);
      snprintf(out + len, cap - len, "%s",
               j > 0 ? " " : i > 0 ? "; " : "");
      put_value(out, cap, clpd_db_result_value(r, i, j));
    }
  }
  clpd_db_result_free(r);
  recording = was;
}

#define ITEM_ROWS                                                          \
  "SELECT wf_id, n, qty, label, price, paid, data, placed_at, doc FROM "  \
  "shop_item ORDER BY 1, 2"

static void items(char *out, size_t cap) { query(ITEM_ROWS, out, cap); }

static const char *const tables[] = {"shop_item", "shop_order",
                                     "solo_ticket", "workflow_instance"};

/* Every table of the test with its rows: what a refused call must leave
 * unchanged. */
static void fingerprint(char *out, size_t cap) {
  char one[4096], text[128];
  size_t i, len;
  out[0] = '\0';
  for (i = 0; i < sizeof tables / sizeof tables[0]; i++) {
    snprintf(text, sizeof text, "SELECT * FROM \"%s\" ORDER BY 1, 2",
             tables[i]);
    query(text, one, sizeof one);
    len = strlen(out);
    snprintf(out + len, cap - len, "%s: %s\n", tables[i], one);
  }
}

/* Appends one slot value: an integer, %g, a symbol as it is, "a string". */
static void put_clips(char *out, size_t cap, const CLIPSValue *v) {
  size_t len = strlen(out);
  switch (v->header->type) {
  case INTEGER_TYPE:
    snprintf(out + len, cap - len, "%lld", v->integerValue->contents);
    break;
  case FLOAT_TYPE:
    snprintf(out + len, cap - len, "%g", v->floatValue->contents);
    break;
  case SYMBOL_TYPE:
    snprintf(out + len, cap - len, "%s", v->lexemeValue->contents);
    break;
  case STRING_TYPE:
    snprintf(out + len, cap - len, "\"%s\"", v->lexemeValue->contents);
    break;
  default:
    snprintf(out + len, cap - len, "<type %d>", v->header->type);
    break;
  }
}

/* The facts whose wf-id is the symbol or string wf, in fact-index order:
 * "<module>::<template>(<slot> <value> ...); ...". */
static void facts_of(const char *wf, char *out, size_t cap) {
  Fact *f;
  out[0] = '\0';
  for (f = GetNextFact(env, NULL); f != NULL; f = GetNextFact(env, f)) {
    Deftemplate *d = FactDeftemplate(f);
    struct templateSlot *ts;
    CLIPSValue v;
    size_t len;
    if (!DeftemplateSlotExistP(d, "wf-id") ||
        GetFactSlot(f, "wf-id", &v) != GSE_NO_ERROR ||
        (v.header->type != SYMBOL_TYPE && v.header->type != STRING_TYPE) ||
        strcmp(v.lexemeValue->contents, wf) != 0)
      continue;
    len = strlen(out);
    snprintf(out + len, cap - len, "%s%s::%s(", len > 0 ? "; " : "",
             DeftemplateModule(d), DeftemplateName(d));
    for (ts = d->slotList; ts != NULL; ts = ts->next) {
      len = strlen(out);
      snprintf(out + len, cap - len, "%s%s ", ts == d->slotList ? "" : " ",
               ts->slotName->contents);
      if (GetFactSlot(f, ts->slotName->contents, &v) == GSE_NO_ERROR)
        put_clips(out, cap, &v);
    }
    len = strlen(out);
    snprintf(out + len, cap - len, ")");
  }
}

/* Retracts the facts of instance wf (between tests). */
static void forget(const char *wf) {
  Fact *f, *next;
  for (f = GetNextFact(env, NULL); f != NULL; f = next) {
    CLIPSValue v;
    next = GetNextFact(env, f);
    if (DeftemplateSlotExistP(FactDeftemplate(f), "wf-id") &&
        GetFactSlot(f, "wf-id", &v) == GSE_NO_ERROR &&
        (v.header->type == SYMBOL_TYPE || v.header->type == STRING_TYPE) &&
        strcmp(v.lexemeValue->contents, wf) == 0) {
      clpd_orm_sync_suspend(syncer);
      Retract(f);
      clpd_orm_sync_resume(syncer);
    }
  }
}

/* The expected statements: every '@' becomes the table qualifier
 * ("public". on PostgreSQL, nothing on SQLite). */
static const char *expect_sql(const char *fmt) {
  static char out[8192];
  size_t len = 0;
  for (; *fmt != '\0' && len + 64 < sizeof out; fmt++) {
    if (*fmt == '@') {
      if (pg)
        len += (size_t)snprintf(out + len, sizeof out - len, "\"public\".");
    } else {
      out[len++] = *fmt;
    }
  }
  out[len] = '\0';
  return out;
}

/* --- calls ------------------------------------------------------------- */

/* The result of one call: kind 'T' (TRUE), 'F' (FALSE), 'I' (INTEGER),
 * '?' otherwise; log is what was written to the daemon log stream (with
 * a cycle: by the cycle too); ran is 0 when the calling rule never
 * fired. */
typedef struct {
  char kind;
  long long integer;
  char log[4096];
  EvalError rc;
  int ran;
  int rolled_back; /* a cycle: 1 when it was rolled back */
} call_result;

static void result_of(const CLIPSValue *v, call_result *c) {
  c->kind = '?';
  if (v->header->type == SYMBOL_TYPE && v->lexemeValue == TrueSymbol(env)) {
    c->kind = 'T';
  } else if (v->header->type == SYMBOL_TYPE &&
             v->lexemeValue == FalseSymbol(env)) {
    c->kind = 'F';
  } else if (v->header->type == INTEGER_TYPE) {
    c->kind = 'I';
    c->integer = v->integerValue->contents;
  }
}

static call_result *pending; /* where test-eval stores its result */

/* (test-eval <string>): evaluates the string inside the calling rule. */
static void test_eval(Environment *e, UDFContext *ctx, UDFValue *ret) {
  UDFValue arg;
  CLIPSValue v;
  if (!UDFFirstArgument(ctx, STRING_BIT, &arg) || pending == NULL) return;
  pending->rc = Eval(e, arg.lexemeValue->contents, &v);
  result_of(&v, pending);
  pending->ran = 1;
  ret->lexemeValue = TrueSymbol(e);
}

static void clear_flags(void) {
  SetEvaluationError(env, false);
  SetHaltExecution(env, false);
  SetHaltRules(env, false);
}

/* Calls expr from the rule dml-call of module `module`, or through Eval
 * at the top level (no rule executing) when module is NULL; with
 * in_cycle, inside a cycle (begin with the reload of evicted instances,
 * the rule, then the flush and the commit). The statements are recorded
 * into stmts. */
static void call_any(const char *module, const char *expr, int in_cycle,
                     call_result *c) {
  FILE *f = tmpfile();
  size_t n;
  if (f == NULL) exit(2);
  memset(c, 0, sizeof *c);
  c->kind = '?';
  clpd_log_set(f);
  stmts[0] = '\0';
  recording = 1;
  if (in_cycle && clpd_orm_sync_begin_cycle(syncer, &tx, f) != 0) {
    fprintf(stderr, "orm_dml_test: the cycle did not begin\n");
    exit(2);
  }
  if (module == NULL) {
    CLIPSValue v;
    c->rc = Eval(env, expr, &v);
    result_of(&v, c);
    c->ran = 1;
  } else {
    Defmodule *m = FindDefmodule(env, module);
    FactBuilder *fb;
    if (m == NULL) exit(2);
    SetCurrentModule(env, m);
    fb = CreateFactBuilder(env, "dml-call");
    if (fb == NULL || FBPutSlotSymbol(fb, "wf-id", "call") != PSE_NO_ERROR ||
        FBPutSlotString(fb, "expr", expr) != PSE_NO_ERROR ||
        FBAssert(fb) == NULL)
      exit(2);
    FBDispose(fb);
    clear_flags();
    pending = c;
    ClearFocusStack(env);
    Focus(m);
    Run(env, -1);
    pending = NULL;
    ClearFocusStack(env);
    clear_flags();
    SetCurrentModule(env, FindDefmodule(env, "MAIN"));
  }
  if (in_cycle) c->rolled_back = clpd_orm_sync_end_cycle(syncer, &tx, f) != 0;
  recording = 0;
  clpd_log_set(NULL);
  fflush(f);
  rewind(f);
  n = fread(c->log, 1, sizeof c->log - 1, f);
  c->log[n] = '\0';
  fclose(f);
  if (password != NULL && strstr(c->log, password) != NULL) {
    fprintf(stderr, "  %s: the log holds the password\n", expr);
    failures++;
  }
  if (strstr(c->log, db_spec) != NULL) {
    fprintf(stderr, "  %s: the log holds the database spec\n", expr);
    failures++;
  }
}

static void call_in(const char *module, const char *expr, call_result *c) {
  call_any(module, expr, 0, c);
}

static const char *where(const char *module) {
  return module != NULL ? module : "top level";
}

/* A call that returned TRUE and logged nothing. */
static int ok_true_in(const char *module, const char *expr) {
  call_result c;
  call_in(module, expr, &c);
  if (!c.ran || c.kind != 'T' || c.log[0] != '\0' || c.rc != EE_NO_ERROR) {
    fprintf(stderr, "  %s (%s): ran %d kind %c rc %d log [%s]\n", expr,
            where(module), c.ran, c.kind, (int)c.rc, c.log);
    return 0;
  }
  return 1;
}

/* A call that returned the INTEGER want and logged nothing. */
static int int_in(const char *module, const char *expr, long long want) {
  call_result c;
  call_in(module, expr, &c);
  if (!c.ran || c.kind != 'I' || c.integer != want || c.log[0] != '\0' ||
      c.rc != EE_NO_ERROR) {
    fprintf(stderr, "  %s (%s): ran %d kind %c value %lld (want %lld) rc %d "
            "log [%s]\n", expr, where(module), c.ran, c.kind, c.integer,
            want, (int)c.rc, c.log);
    return 0;
  }
  return 1;
}

/* A call that failed: FALSE, an evaluation error, exactly `line` logged,
 * and every table as it was. */
static int fails_in(const char *module, const char *expr, const char *line) {
  char before[16384], after[16384], want[1024];
  call_result c;
  fingerprint(before, sizeof before);
  call_in(module, expr, &c);
  fingerprint(after, sizeof after);
  snprintf(want, sizeof want, "%s\n", line);
  if (!c.ran || c.kind != 'F' || c.rc != EE_PROCESSING_ERROR) {
    fprintf(stderr, "  %s (%s): ran %d kind %c rc %d log [%s]\n", expr,
            where(module), c.ran, c.kind, (int)c.rc, c.log);
    return 0;
  }
  return same(expr, c.log, want) && same("tables after a refused call",
                                         after, before);
}

/* The message of the query on the missing table (the injected failure). */
static void missing_message(char *out, size_t cap) {
  __real_clpd_db_exec(db, "SELECT 1 FROM " MISSING_TABLE, NULL, 0, NULL);
  snprintf(out, cap, "%s", clpd_db_error(db));
}

/* A call whose statement containing `stmt` fails on the database:
 * `prefix` followed by the database message, tables unchanged. */
static int fails_db(const char *module, const char *expr, const char *stmt,
                    const char *prefix) {
  char msg[512], line[1024];
  int ok;
  missing_message(msg, sizeof msg);
  snprintf(line, sizeof line, "%s%s", prefix, msg);
  fail_sql = stmt;
  ok = fails_in(module, expr, line);
  if (fail_sql != NULL) {
    fprintf(stderr, "  %s: the statement [%s] was never run\n", expr, stmt);
    fail_sql = NULL;
    return 0;
  }
  return ok && strstr(msg, MISSING_TABLE) != NULL;
}

/* --- setup ------------------------------------------------------------- */

static void create_tables(void) {
  char msg[512];
  sql("DROP TABLE IF EXISTS shop_order");
  sql("DROP TABLE IF EXISTS shop_item");
  sql("DROP TABLE IF EXISTS solo_ticket");
  sql("DROP TABLE IF EXISTS workflow_instance");
  sql(pg ? "CREATE TABLE shop_order (wf_id TEXT NOT NULL, line_no BIGINT NOT "
           "NULL, status TEXT, PRIMARY KEY (wf_id, line_no))"
         : "CREATE TABLE shop_order (wf_id TEXT NOT NULL, line_no INTEGER NOT "
           "NULL, status TEXT, PRIMARY KEY (wf_id, line_no))");
  sql(pg ? "CREATE TABLE shop_item (wf_id TEXT NOT NULL, n BIGINT NOT NULL, "
           "qty BIGINT, label TEXT, price DOUBLE PRECISION, paid BOOLEAN, "
           "data BYTEA, placed_at TIMESTAMPTZ, doc JSONB, PRIMARY KEY (wf_id, "
           "n))"
         : "CREATE TABLE shop_item (wf_id TEXT NOT NULL, n INTEGER NOT NULL, "
           "qty INTEGER, label TEXT, price REAL, paid INTEGER, data BLOB, "
           "placed_at TEXT, doc TEXT, PRIMARY KEY (wf_id, n))");
  sql("CREATE TABLE solo_ticket (wf_id TEXT NOT NULL PRIMARY KEY, title "
      "TEXT)");
  if (clpd_orm_tx_create_instance_table(db, msg, sizeof msg) != 0) {
    fprintf(stderr, "orm_dml_test: workflow_instance: %s\n", msg);
    exit(2);
  }
}

static const char *const dml_names[] = {"db-insert", "db-update", "db-delete",
                                        "db-load", "db-count"};

static void setup(const char *spec) {
  const char *dirs[3];
  char msg[512], eb[4096];
  clpd_list list;
  FILE *err;
  size_t i, n;
  int rc;

  dirs[0] = package("shop1", "definition = shop\nversion = 1\n"
                             "files = shop.clp\norm = orm.conf\n");
  dirs[1] = package("shop2", "definition = shop\nversion = 2\n"
                             "files = shop.clp\norm = orm.conf\n");
  dirs[2] = package("solo", "definition = solo\nversion = 1\n"
                            "files = solo.clp\norm = orm.conf\n");
  write_file(dirs[0], "shop.clp", shop_clp);
  write_file(dirs[0], "orm.conf", shop_orm);
  write_file(dirs[1], "shop.clp", shop_clp);
  write_file(dirs[1], "orm.conf", shop_orm);
  write_file(dirs[2], "solo.clp", solo_clp);
  write_file(dirs[2], "orm.conf", solo_orm);
  if (clpd_db_open(spec, &db, msg, sizeof msg) != CLPD_DB_OK) {
    fprintf(stderr, "orm_dml_test: cannot open the database: %s\n", msg);
    exit(2);
  }
  snprintf(db_spec, sizeof db_spec, "%s", spec);
  pg = strcmp(clpd_db_backend(db), "postgresql") == 0;
  if (pg) sql("SET TimeZone = 'UTC'");
  create_tables();

  env = CreateEnvironment();
  /* Without registration (no database configured) the names are not
   * functions. */
  for (i = 0; i < sizeof dml_names / sizeof dml_names[0]; i++)
    EXPECT(FindFunction(env, dml_names[i]) == NULL);
  ddl.db = db;
  ddl.packages = &pkgs;
  EXPECT(clpd_orm_ddl_register(env, &ddl) == 0);
  for (i = 0; i < sizeof dml_names / sizeof dml_names[0]; i++)
    EXPECT(FindFunction(env, dml_names[i]) != NULL);
  if (AddUDF(env, "test-eval", "b", 1, 1, "s", test_eval, "test_eval",
             NULL) != AUE_NO_ERROR)
    exit(2);
  list.items = (char **)dirs;
  list.count = 3;
  err = tmpfile();
  if (err == NULL) exit(2);
  rc = clpd_packages_load_ex(env, &list, 1, &pkgs, err);
  rewind(err);
  n = fread(eb, 1, sizeof eb - 1, err);
  eb[n] = '\0';
  fclose(err);
  if (rc != 0) {
    fprintf(stderr, "orm_dml_test: packages did not load:\n%s", eb);
    exit(2);
  }
  /* A module that is no package, with the same calling rule. */
  if (Build(env, "(defmodule notpkg)") != BE_NO_ERROR ||
      Build(env, "(deftemplate notpkg::dml-call (slot wf-id) (slot expr "
                 "(type STRING)))") != BE_NO_ERROR ||
      Build(env, "(defrule notpkg::dml-call ?f <- (dml-call (wf-id ?w) "
                 "(expr ?e)) => (retract ?f) (test-eval ?e))") != BE_NO_ERROR)
    exit(2);
  SetCurrentModule(env, FindDefmodule(env, "MAIN"));
  if (clpd_orm_sync_attach(env, db, &pkgs, &syncer) != 0) {
    fprintf(stderr, "orm_dml_test: cannot attach the sync\n");
    exit(2);
  }
  ddl.sync = syncer; /* as main.c does after the attach */
  Reset(env);
  clpd_orm_tx_init(&tx, db, db_spec, 100, 30000);
}

/* --- tests ------------------------------------------------------------- */

#define INSERT_ITEM_PREFIX "clpdaemon: db-insert: template 'item': table 'shop_item'"

/* db-insert: TRUE, the row with every value typed per the mapping, nil in
 * a null nil column and an omitted slot as NULL; one INSERT naming only
 * the given columns, in mapping order. */
static void test_insert(void) {
  char got[4096];
  EXPECT(ok_true_in("shop@1",
                    "(db-insert item wf-id w1 n 1 qty 5 label red price 2.5 "
                    "paid 1 data \"ab\" at \"2026-01-02 03:04:05+00\" doc "
                    "\"[1]\")"));
  EXPECT(same("insert statement", stmts,
              expect_sql("INSERT INTO @\"shop_item\" (\"wf_id\", \"n\", "
                         "\"qty\", \"label\", \"price\", \"paid\", \"data\", "
                         "\"placed_at\", \"doc\") VALUES ($1, $2, $3, $4, $5, "
                         "$6, $7, $8, $9)\n")));
  /* Arguments in another order: the columns keep the mapping order. */
  EXPECT(ok_true_in("shop@1", "(db-insert item label nil n 2 wf-id \"w1\")"));
  EXPECT(same("insert statement, mapping order", stmts,
              expect_sql("INSERT INTO @\"shop_item\" (\"wf_id\", \"n\", "
                         "\"label\") VALUES ($1, $2, $3)\n")));
  /* Another version of the own definition, qualified; paid 0 is false. */
  EXPECT(ok_true_in("shop@1", "(db-insert shop@2::item wf-id w2 n 1 paid 0 "
                              "price 1.0)"));
  /* The other definition's own template, unqualified. */
  EXPECT(ok_true_in("solo@1", "(db-insert ticket wf-id t1 title \"hello\")"));
  items(got, sizeof got);
  EXPECT(same("rows after the inserts", got,
              "'w1' 1 5 'red' 2.5 1 x'6162' '2026-01-02 03:04:05+00' '[1]'; "
              "'w1' 2 NULL NULL NULL NULL NULL NULL NULL; "
              "'w2' 1 NULL NULL 1 0 NULL NULL NULL"));
  query("SELECT * FROM solo_ticket", got, sizeof got);
  EXPECT(same("ticket row", got, "'t1' 'hello'"));
  /* No workflow_instance row: only the automatic synchronization writes
   * them. */
  query("SELECT count(*) FROM workflow_instance", got, sizeof got);
  EXPECT(same("no instance rows", got, "0"));
}

static void test_insert_errors(void) {
  char line[1024];
  EXPECT(fails_in("shop@1", "(db-insert order wf-id w1 line-no 7)",
                  "clpdaemon: db-insert: template 'order': table 'shop_order' "
                  "has sync = auto: only the automatic synchronization "
                  "writes it"));
  EXPECT(fails_in("shop@1", "(db-insert item wf-id w1 n)",
                  INSERT_ITEM_PREFIX ": the slot values must come in <slot> "
                  "<value> pairs"));
  EXPECT(fails_in("shop@1", "(db-insert item)",
                  INSERT_ITEM_PREFIX ": key slot 'wf-id' needs a value"));
  EXPECT(fails_in("shop@1", "(db-insert item 3 w1)",
                  INSERT_ITEM_PREFIX ": expected a slot name, got INTEGER"));
  EXPECT(fails_in("shop@1", "(db-insert item wf-id w1 n 3 colour red)",
                  INSERT_ITEM_PREFIX ": slot 'colour' is not mapped"));
  EXPECT(fails_in("shop@1", "(db-insert item wf-id w1 n 3 n 4)",
                  INSERT_ITEM_PREFIX ": slot 'n' is given twice"));
  EXPECT(fails_in("shop@1", "(db-insert item wf-id w1 n \"3\")",
                  INSERT_ITEM_PREFIX ": slot 'n' (column 'n'): expected "
                  "integer, got STRING"));
  EXPECT(fails_in("shop@1", "(db-insert item wf-id w1 n 3 price 2)",
                  INSERT_ITEM_PREFIX ": slot 'price' (column 'price'): "
                  "expected float, got INTEGER"));
  EXPECT(fails_in("shop@1", "(db-insert item wf-id w1 n 3 paid 2)",
                  INSERT_ITEM_PREFIX ": slot 'paid' (column 'paid'): a "
                  "boolean column takes 0 or 1"));
  EXPECT(fails_in("shop@1", "(db-insert item wf-id w1 n 3 qty (create$ 1 2))",
                  INSERT_ITEM_PREFIX ": slot 'qty' (column 'qty'): expected "
                  "integer, got MULTIFIELD"));
  /* nil is NULL only in a null nil column: in an integer column it is a
   * symbol. */
  EXPECT(fails_in("shop@1", "(db-insert item wf-id w1 n 3 qty nil)",
                  INSERT_ITEM_PREFIX ": slot 'qty' (column 'qty'): expected "
                  "integer, got SYMBOL"));
  EXPECT(fails_in("shop@1", "(db-insert item n 3)",
                  INSERT_ITEM_PREFIX ": key slot 'wf-id' needs a value"));
  EXPECT(fails_in("shop@1", "(db-insert item wf-id w1 qty 3)",
                  INSERT_ITEM_PREFIX ": key slot 'n' needs a value"));
  /* A slot name with control characters is shown with '?'. */
  EXPECT(fails_in("shop@1", "(db-insert item wf-id w1 n 3 \"a\tb\" 1)",
                  INSERT_ITEM_PREFIX ": slot 'a?b' is not mapped"));
  /* The primary key exists: the database's unique violation. */
  snprintf(line, sizeof line, INSERT_ITEM_PREFIX ": %s",
           pg ? "duplicate key value violates unique constraint "
                "\"shop_item_pkey\" (SQLSTATE 23505)"
              : "UNIQUE constraint failed: shop_item.wf_id, shop_item.n");
  EXPECT(fails_in("shop@1", "(db-insert item wf-id w1 n 1)", line));
  EXPECT(fails_db("shop@1", "(db-insert item wf-id w1 n 9)", "INSERT INTO",
                  INSERT_ITEM_PREFIX ": "));
  /* PostgreSQL types the text from its column: a timestamptz that is no
   * timestamp is the database's error (the value redacted). */
  if (pg)
    EXPECT(fails_in("shop@1", "(db-insert item wf-id w1 n 9 at \"soon\")",
                    INSERT_ITEM_PREFIX ": invalid input syntax for type "
                    "timestamp with time zone: \"?\" (SQLSTATE 22007)"));
}

/* The caller scope of the DDL functions (T9d): only a rule of a rule
 * package, templates of its own definition. */
static void test_scope(void) {
  size_t i;
  static const char *const calls[] = {
      "(db-insert item wf-id w1 n 9)",
      "(db-update item (create$ wf-id w1) qty 1)",
      "(db-delete item (create$ wf-id w1))",
      "(db-load item (create$ wf-id w1))",
      "(db-count item (create$ wf-id w1))"};
  char line[512], before[64];
  query("SELECT count(*) FROM shop_item", before, sizeof before);
  for (i = 0; i < sizeof calls / sizeof calls[0]; i++) {
    snprintf(line, sizeof line, "clpdaemon: %s: only a rule of a rule "
             "package may call it (no rule is executing)", dml_names[i]);
    EXPECT(fails_in(NULL, calls[i], line));
    snprintf(line, sizeof line, "clpdaemon: %s: only a rule of a rule "
             "package may call it (rule 'dml-call' is in module 'notpkg')",
             dml_names[i]);
    EXPECT(fails_in("notpkg", calls[i], line));
    snprintf(line, sizeof line, "clpdaemon: %s: template 'item' is not "
             "mapped by definition solo of the calling rule (mapped by "
             "shop@1, shop@2)", dml_names[i]);
    EXPECT(fails_in("solo@1", calls[i], line));
  }
  EXPECT(fails_in("shop@1", "(db-insert solo@1::ticket wf-id t2)",
                  "clpdaemon: db-insert: template 'solo@1::ticket' is not "
                  "mapped by definition shop of the calling rule (mapped by "
                  "solo@1)"));
  EXPECT(fails_in("shop@1", "(db-count nosuch (create$))",
                  "clpdaemon: db-count: unknown template 'nosuch' (no loaded "
                  "package maps it)"));
  /* No fact was asserted by a refused db-load. */
  {
    char facts[1024];
    facts_of("w1", facts, sizeof facts);
    EXPECT(same("no facts after refused loads", facts, ""));
  }
}

#define UPDATE_ITEM_PREFIX "clpdaemon: db-update: template 'item': table 'shop_item'"

/* db-update: the number of rows changed; SET in mapping order, the
 * filters in argument order, RETURNING the instance of each row. */
static void test_update(void) {
  char got[4096];
  EXPECT(int_in("shop@1", "(db-update item (create$ wf-id w1 n 1) label nil "
                          "qty 6)", 1));
  EXPECT(same("update statement", stmts,
              expect_sql("UPDATE @\"shop_item\" SET \"qty\" = $1, \"label\" "
                         "= $2 WHERE \"wf_id\" = $3 AND \"n\" = $4 RETURNING "
                         "\"wf_id\"\n")));
  EXPECT(int_in("shop@1", "(db-update item (create$ wf-id w1) paid 0)", 2));
  EXPECT(int_in("shop@1", "(db-update item (create$ wf-id w9) qty 1)", 0));
  /* A local key filter alone reaches every instance with that key. */
  EXPECT(int_in("shop@1", "(db-update item (create$ n 1) price 3.5)", 2));
  EXPECT(int_in("shop@2", "(db-update item (create$ n 2 wf-id w1) at "
                          "\"2026-02-03 04:05:06+00\" doc \"{}\")", 1));
  items(got, sizeof got);
  EXPECT(same("rows after the updates", got,
              "'w1' 1 6 NULL 3.5 0 x'6162' '2026-01-02 03:04:05+00' '[1]'; "
              "'w1' 2 NULL NULL NULL 0 NULL '2026-02-03 04:05:06+00' '{}'; "
              "'w2' 1 NULL NULL 3.5 0 NULL NULL NULL"));
}

static void test_update_errors(void) {
  EXPECT(fails_in("shop@1", "(db-update order (create$ wf-id w1) status x)",
                  "clpdaemon: db-update: template 'order': table 'shop_order' "
                  "has sync = auto: only the automatic synchronization "
                  "writes it"));
  EXPECT(fails_in("shop@1", "(db-update item (create$) qty 1)",
                  UPDATE_ITEM_PREFIX ": the filters must name wf-id or a key "
                  "slot"));
  /* The documented order (T10e, R3-check-order-update): both lists are
   * read first, the slot values after the filters, then the filter rule,
   * then no slot to set. */
  EXPECT(fails_in("shop@1", "(db-update item (create$) qty 1 qty 2)",
                  UPDATE_ITEM_PREFIX ": slot 'qty' is given twice"));
  EXPECT(fails_in("shop@1", "(db-update item (create$) colour 1)",
                  UPDATE_ITEM_PREFIX ": slot 'colour' is not mapped"));
  EXPECT(fails_in("shop@1", "(db-update item (create$) n 5)",
                  UPDATE_ITEM_PREFIX ": slot 'n' is a key slot, which "
                  "db-update never changes"));
  EXPECT(fails_in("shop@1", "(db-update item (create$))",
                  UPDATE_ITEM_PREFIX ": the filters must name wf-id or a key "
                  "slot"));
  EXPECT(fails_in("shop@1", "(db-update item (create$ colour w1) qty 1 qty "
                            "2)",
                  UPDATE_ITEM_PREFIX ": filter slot 'colour' is not mapped"));
  EXPECT(fails_in("shop@1", "(db-update item (create$ qty 6) qty 1)",
                  UPDATE_ITEM_PREFIX ": filter slot 'qty' is not a key slot"));
  EXPECT(fails_in("shop@1", "(db-update item (create$ wf-id w1 n) qty 1)",
                  UPDATE_ITEM_PREFIX ": the filters must come in <slot> "
                  "<value> pairs"));
  EXPECT(fails_in("shop@1", "(db-update item (create$ 1 2) qty 1)",
                  UPDATE_ITEM_PREFIX ": filter: expected a slot name, got "
                  "INTEGER"));
  EXPECT(fails_in("shop@1", "(db-update item (create$ wf-id w1 wf-id w2) qty "
                            "1)",
                  UPDATE_ITEM_PREFIX ": filter slot 'wf-id' is given twice"));
  EXPECT(fails_in("shop@1", "(db-update item (create$ colour w1) qty 1)",
                  UPDATE_ITEM_PREFIX ": filter slot 'colour' is not mapped"));
  EXPECT(fails_in("shop@1", "(db-update item (create$ n \"1\") qty 1)",
                  UPDATE_ITEM_PREFIX ": filter slot 'n' (column 'n'): "
                  "expected integer, got STRING"));
  EXPECT(fails_in("shop@1", "(db-update item (create$ wf-id w1))",
                  UPDATE_ITEM_PREFIX ": no slot to set"));
  EXPECT(fails_in("shop@1", "(db-update item (create$ wf-id w1) qty)",
                  UPDATE_ITEM_PREFIX ": the slot values must come in <slot> "
                  "<value> pairs"));
  EXPECT(fails_in("shop@1", "(db-update item (create$ wf-id w1) n 5)",
                  UPDATE_ITEM_PREFIX ": slot 'n' is a key slot, which "
                  "db-update never changes"));
  EXPECT(fails_in("shop@1", "(db-update item (create$ wf-id w1) wf-id w5)",
                  UPDATE_ITEM_PREFIX ": slot 'wf-id' is a key slot, which "
                  "db-update never changes"));
  EXPECT(fails_in("shop@1", "(db-update item (create$ wf-id w1) qty 1 qty 2)",
                  UPDATE_ITEM_PREFIX ": slot 'qty' is given twice"));
  EXPECT(fails_in("shop@1", "(db-update item (create$ wf-id w1) qty 1.5)",
                  UPDATE_ITEM_PREFIX ": slot 'qty' (column 'qty'): expected "
                  "integer, got FLOAT"));
  EXPECT(fails_in("shop@1", "(db-update item (create$ wf-id w1) colour 1)",
                  UPDATE_ITEM_PREFIX ": slot 'colour' is not mapped"));
  EXPECT(fails_db("shop@1", "(db-update item (create$ wf-id w1) qty 1)",
                  "UPDATE ", UPDATE_ITEM_PREFIX ": "));
  if (pg)
    EXPECT(fails_in("shop@1", "(db-update item (create$ wf-id w1) doc "
                              "\"{bad\")",
                    UPDATE_ITEM_PREFIX ": invalid input syntax for type json "
                    "(SQLSTATE 22P02)"));
}

#define COUNT_ITEM_PREFIX "clpdaemon: db-count: template 'item': table 'shop_item'"

/* db-count: an INTEGER; filters typed per the mapping (boolean, bytea,
 * timestamptz and jsonb compared on PostgreSQL through their column's
 * type), nil in a null nil column is IS NULL, no filter counts every
 * row; any mapped template (sync auto too). */
static void test_count(void) {
  EXPECT(int_in("shop@1", "(db-count item (create$))", 3));
  EXPECT(same("count statement, no filter", stmts,
              expect_sql("SELECT count(*) FROM @\"shop_item\"\n")));
  EXPECT(int_in("shop@1", "(db-count item (create$ wf-id w1))", 2));
  EXPECT(int_in("shop@1", "(db-count item (create$ label nil wf-id w1))", 2));
  EXPECT(same("count statement, IS NULL", stmts,
              expect_sql("SELECT count(*) FROM @\"shop_item\" WHERE "
                         "\"label\" IS NULL AND \"wf_id\" = $1\n")));
  EXPECT(int_in("shop@1", "(db-count item (create$ qty 6 price 3.5))", 1));
  EXPECT(int_in("shop@1", "(db-count item (create$ paid 0))", 3));
  EXPECT(int_in("shop@1", "(db-count item (create$ paid 1))", 0));
  EXPECT(int_in("shop@1", "(db-count item (create$ data \"ab\"))", 1));
  EXPECT(int_in("shop@1", "(db-count item (create$ at \"2026-01-02 "
                          "03:04:05+00\"))", 1));
  EXPECT(int_in("shop@1", "(db-count item (create$ doc \"[1]\"))", 1));
  EXPECT(int_in("shop@1", "(db-count item (create$ label red))", 0));
  EXPECT(int_in("shop@2", "(db-count shop@1::item (create$ n 1))", 2));
  EXPECT(int_in("shop@1", "(db-count order (create$ wf-id w1))", 0));
  EXPECT(int_in("solo@1", "(db-count ticket (create$ title \"hello\"))", 1));
  EXPECT(fails_in("shop@1", "(db-count item (create$ wf-id))",
                  COUNT_ITEM_PREFIX ": the filters must come in <slot> "
                  "<value> pairs"));
  EXPECT(fails_in("shop@1", "(db-count item (create$ colour red))",
                  COUNT_ITEM_PREFIX ": filter slot 'colour' is not mapped"));
  EXPECT(fails_in("shop@1", "(db-count item (create$ qty 1.0))",
                  COUNT_ITEM_PREFIX ": filter slot 'qty' (column 'qty'): "
                  "expected integer, got FLOAT"));
  EXPECT(fails_in("shop@1", "(db-count item (create$ paid 3))",
                  COUNT_ITEM_PREFIX ": filter slot 'paid' (column 'paid'): a "
                  "boolean column takes 0 or 1"));
  EXPECT(fails_db("shop@1", "(db-count item (create$ wf-id w1))",
                  "SELECT count(*)", COUNT_ITEM_PREFIX ": "));
}

#define LOAD_ITEM_PREFIX "clpdaemon: db-load: template 'item': table 'shop_item'"

/* db-load: the matching rows asserted as facts of the resolved package's
 * template (the loader of T10c), the count of facts asserted. */
static void test_load(void) {
  char got[4096];
  call_result c;
  EXPECT(int_in("shop@1", "(db-load item (create$ wf-id w1))", 2));
  EXPECT(same("load statement", stmts,
              expect_sql("SELECT \"wf_id\", \"n\", \"qty\", \"label\", "
                         "\"price\", \"paid\", \"data\", \"placed_at\", "
                         "\"doc\" FROM @\"shop_item\" WHERE \"wf_id\" = $1 "
                         "ORDER BY \"wf_id\", \"n\"\n")));
  facts_of("w1", got, sizeof got);
  EXPECT(same("loaded facts", got,
              "shop@1::item(wf-id \"w1\" n 1 qty 6 label nil price 3.5 paid 0 "
              "data \"ab\" at \"2026-01-02 03:04:05+00\" doc \"[1]\"); "
              "shop@1::item(wf-id \"w1\" n 2 qty 0 label nil price nil paid 0 "
              "data \"\" at \"2026-02-03 04:05:06+00\" doc \"{}\")"));
  forget("w1");
  /* Another version's template: its facts; a key filter alone. */
  EXPECT(int_in("shop@1", "(db-load shop@2::item (create$ n 1 label nil))",
                2));
  facts_of("w2", got, sizeof got);
  EXPECT(same("loaded facts of shop@2", got,
              "shop@2::item(wf-id \"w2\" n 1 qty 0 label nil price 3.5 paid 0 "
              "data \"\" at nil doc nil)"));
  forget("w1");
  forget("w2");
  EXPECT(int_in("shop@1", "(db-load item (create$ wf-id w9))", 0));
  /* A row the loader rejects: its line, not counted, not asserted. */
  sql(pg ? "INSERT INTO shop_item (wf_id, n, data) VALUES ('w7', 1, "
           "'\\x610062'::bytea), ('w7', 2, NULL)"
         : "INSERT INTO shop_item (wf_id, n, data) VALUES ('w7', 1, "
           "x'610062'), ('w7', 2, NULL)");
  call_in("shop@1", "(db-load item (create$ wf-id w7))", &c);
  EXPECT(c.ran && c.kind == 'I' && c.integer == 1 && c.rc == EE_NO_ERROR);
  EXPECT(same("rejected row line", c.log,
              "clpdaemon: load: shop@1::item: table 'shop_item': instance "
              "'w7': row rejected: column 'data' (slot 'data'): the value "
              "holds a NUL byte\n"));
  facts_of("w7", got, sizeof got);
  EXPECT(same("only the good row", got,
              "shop@1::item(wf-id \"w7\" n 2 qty 0 label nil price nil paid 0 "
              "data \"\" at nil doc nil)"));
  forget("w7");
  sql("DELETE FROM shop_item WHERE wf_id = 'w7'");
  /* Refusals: no facts, no rows changed. */
  EXPECT(fails_in("shop@1", "(db-load item (create$))",
                  LOAD_ITEM_PREFIX ": the filters must name wf-id or a key "
                  "slot"));
  EXPECT(fails_in("shop@1", "(db-load item (create$ qty 6))",
                  LOAD_ITEM_PREFIX ": the filters must name wf-id or a key "
                  "slot"));
  EXPECT(fails_in("shop@1", "(db-load item (create$ wf-id w1 n x))",
                  LOAD_ITEM_PREFIX ": filter slot 'n' (column 'n'): expected "
                  "integer, got SYMBOL"));
  EXPECT(fails_db("shop@1", "(db-load item (create$ wf-id w1))",
                  "SELECT \"wf_id\"", LOAD_ITEM_PREFIX ": "));
  facts_of("w1", got, sizeof got);
  EXPECT(same("no facts after the refusals", got, ""));
}

#define DELETE_ITEM_PREFIX "clpdaemon: db-delete: template 'item': table 'shop_item'"

static void test_delete(void) {
  char got[4096];
  EXPECT(fails_in("shop@1", "(db-delete order (create$ wf-id w1))",
                  "clpdaemon: db-delete: template 'order': table 'shop_order' "
                  "has sync = auto: only the automatic synchronization "
                  "writes it"));
  EXPECT(fails_in("shop@1", "(db-delete item (create$))",
                  DELETE_ITEM_PREFIX ": the filters must name wf-id or a key "
                  "slot"));
  EXPECT(fails_in("shop@1", "(db-delete item (create$ label nil))",
                  DELETE_ITEM_PREFIX ": the filters must name wf-id or a key "
                  "slot"));
  EXPECT(fails_in("shop@1", "(db-delete item (create$ wf-id w1 price 1))",
                  DELETE_ITEM_PREFIX ": filter slot 'price' (column 'price'): "
                  "expected float, got INTEGER"));
  EXPECT(fails_db("shop@1", "(db-delete item (create$ wf-id w1))",
                  "DELETE FROM", DELETE_ITEM_PREFIX ": "));
  /* A non-key filter with a key filter; nil is IS NULL. */
  EXPECT(int_in("shop@1", "(db-delete item (create$ wf-id w9))", 0));
  EXPECT(int_in("shop@1", "(db-delete item (create$ wf-id w1 label nil "
                          "doc \"{}\"))", 1));
  EXPECT(same("delete statement", stmts,
              expect_sql("DELETE FROM @\"shop_item\" WHERE \"wf_id\" = $1 "
                         "AND \"label\" IS NULL AND \"doc\" = $2 RETURNING "
                         "\"wf_id\"\n")));
  EXPECT(int_in("shop@1", "(db-delete item (create$ n 1))", 2));
  items(got, sizeof got);
  EXPECT(same("rows after the deletes", got, ""));
  EXPECT(int_in("solo@1", "(db-delete ticket (create$ wf-id t1))", 1));
  query("SELECT count(*) FROM solo_ticket", got, sizeof got);
  EXPECT(same("ticket deleted", got, "0"));
}

/* Inside the cycle transaction: the writes commit with the cycle; a
 * database error rolls the whole cycle back (the rows written before it
 * included), and the instances the functions wrote count as touched, so
 * the facts db-load asserted from the rolled-back rows are evicted. A
 * db-load of a sync = auto template is never written back (echo
 * suppression). */
static void test_cycle(void) {
  char got[4096], want[1024];
  const char *dup = pg ? "duplicate key value violates unique constraint "
                         "\"shop_item_pkey\" (SQLSTATE 23505)"
                       : "UNIQUE constraint failed: shop_item.wf_id, "
                         "shop_item.n";
  call_result c;
  call_any("shop@1", "(db-insert item wf-id c2 n 1 qty 2)", 1, &c);
  EXPECT(c.ran && c.kind == 'T' && !c.rolled_back && c.log[0] == '\0');
  items(got, sizeof got);
  EXPECT(same("committed row", got,
              "'c2' 1 2 NULL NULL NULL NULL NULL NULL"));
  call_any("shop@1",
           "(progn (db-insert item wf-id c1 n 1) (db-load item (create$ "
           "wf-id c1)) (db-update item (create$ wf-id c2) qty 3) (db-insert "
           "item wf-id c1 n 1))",
           1, &c);
  snprintf(want, sizeof want,
           INSERT_ITEM_PREFIX ": %s\n"
           "clpdaemon: cycle transaction failed: %s; retrying in 100 ms\n",
           dup, dup);
  EXPECT(c.ran && c.kind == 'F' && c.rolled_back);
  EXPECT(same("rolled-back cycle log", c.log, want));
  items(got, sizeof got);
  EXPECT(same("rows after the rollback", got,
              "'c2' 1 2 NULL NULL NULL NULL NULL NULL"));
  facts_of("c1", got, sizeof got);
  EXPECT(same("loaded facts evicted", got, ""));
  /* The retry reloads c1 and c2 (nothing of c1 is stored; c2's row). */
  call_any("shop@1", "(db-count item (create$ wf-id c1))", 1, &c);
  EXPECT(c.ran && c.kind == 'I' && c.integer == 0 && !c.rolled_back &&
         c.log[0] == '\0');
  facts_of("c2", got, sizeof got);
  EXPECT(same("c2 reloaded", got,
              "shop@1::item(wf-id \"c2\" n 1 qty 2 label nil price nil paid 0 "
              "data \"\" at nil doc nil)"));
  forget("c2");
  /* Echo suppression: a loaded sync = auto fact is not written back. */
  sql("INSERT INTO shop_order (wf_id, line_no, status) VALUES ('e1', 1, "
      "'new')");
  call_any("shop@1", "(db-load order (create$ wf-id e1))", 1, &c);
  EXPECT(c.ran && c.kind == 'I' && c.integer == 1 && !c.rolled_back &&
         c.log[0] == '\0');
  EXPECT(same("only the load's statement", stmts,
              expect_sql("SELECT \"wf_id\", \"line_no\", \"status\" FROM "
                         "@\"shop_order\" WHERE \"wf_id\" = $1 ORDER BY "
                         "\"wf_id\", \"line_no\"\n")));
  facts_of("e1", got, sizeof got);
  EXPECT(same("loaded order fact", got,
              "shop@1::order(wf-id \"e1\" line-no 1 status new)"));
  query("SELECT * FROM shop_order", got, sizeof got);
  EXPECT(same("order row unchanged", got, "'e1' 1 'new'"));
  query("SELECT count(*) FROM workflow_instance", got, sizeof got);
  EXPECT(same("no instance row written", got, "0"));
  forget("e1");
  /* A refusal sends no statement and fails no cycle: the cycle commits
   * the rows written before it. */
  call_any("shop@1", "(progn (db-insert item wf-id c3 n 1) (db-insert "
                     "order wf-id c3 line-no 1))", 1, &c);
  EXPECT(c.ran && c.kind == 'F' && !c.rolled_back);
  EXPECT(same("refusal in a cycle", c.log,
              "clpdaemon: db-insert: template 'order': table 'shop_order' "
              "has sync = auto: only the automatic synchronization writes "
              "it\n"));
  query("SELECT wf_id, n FROM shop_item WHERE wf_id = 'c3'", got, sizeof got);
  EXPECT(same("committed before the refusal", got, "'c3' 1"));
  sql("DELETE FROM shop_order");
  sql("DELETE FROM shop_item");
}

/* PostgreSQL: the tables are named in current_schema(); without one every
 * function is an error and nothing is written. */
static void test_no_schema(void) {
  size_t i;
  static const char *const calls[] = {
      "(db-insert item wf-id s1 n 1)",
      "(db-update item (create$ wf-id s1) qty 1)",
      "(db-delete item (create$ wf-id s1))",
      "(db-load item (create$ wf-id s1))",
      "(db-count item (create$ wf-id s1))"};
  char want[512];
  call_result c;
  sql("SET search_path = dml_missing_schema");
  for (i = 0; i < sizeof calls / sizeof calls[0]; i++) {
    snprintf(want, sizeof want, "clpdaemon: %s: template 'item': table "
             "'shop_item': no current schema (the search_path names no "
             "existing schema)\n", dml_names[i]);
    call_in("shop@1", calls[i], &c);
    EXPECT(c.ran && c.kind == 'F' && c.rc == EE_PROCESSING_ERROR);
    EXPECT(same(calls[i], c.log, want));
    EXPECT(same("no statement after the schema query", stmts, ""));
  }
  sql("RESET search_path");
  {
    char got[256];
    query("SELECT count(*) FROM shop_item", got, sizeof got);
    EXPECT(same("nothing written", got, "0"));
  }
}

/* PostgreSQL credentials, as tests/orm/orm_ddl_test.c: the conninfo has no
 * password; the password file of pg_cluster.sh supplies it. */
static void test_credentials(const char *conninfo) {
  char msg[512] = "";
  EXPECT(clpd_db_check_spec(conninfo, msg, sizeof msg) == 1);
  EXPECT(strstr(conninfo, password) == NULL);
  EXPECT(getenv("PGPASSFILE") != NULL);
}

int main(int argc, char **argv) {
  char spec[sizeof root + 32];
  if (argc != 1 && argc != 3) {
    fprintf(stderr, "usage: orm_dml_test [<conninfo> <password>]\n");
    return 2;
  }
  if (mkdtemp(root) == NULL) {
    perror("orm_dml_test");
    return 2;
  }
  snprintf(spec, sizeof spec, "sqlite:%s/dml.db", root);
  if (argc == 3) {
    password = argv[2];
    test_credentials(argv[1]);
  }
  setup(argc == 3 ? argv[1] : spec);
  test_insert();
  test_insert_errors();
  test_scope();
  test_update();
  test_update_errors();
  test_count();
  test_load();
  test_delete();
  test_cycle();
  if (pg) test_no_schema();
  clpd_orm_sync_detach(syncer);
  clpd_packages_free(&pkgs);
  DestroyEnvironment(env);
  if (pg) {
    sql("DROP TABLE IF EXISTS shop_order");
    sql("DROP TABLE IF EXISTS shop_item");
    sql("DROP TABLE IF EXISTS solo_ticket");
    sql("DROP TABLE IF EXISTS workflow_instance");
  }
  clpd_db_close(db);
  if (failures != 0) {
    fprintf(stderr, "%d orm dml checks failed (fixtures in %s)\n", failures,
            root);
    return 1;
  }
  {
    char cmd[sizeof root + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0) fprintf(stderr, "cannot remove %s\n", root);
  }
  puts("orm dml checks passed");
  return 0;
}
