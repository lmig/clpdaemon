/* Tests for the ORM DDL functions db-create-table, db-check-schema and
 * db-drop-table (T9b, docs/orm.md#ddl-functions).
 * usage: orm_ddl_test                         (a temporary SQLite file)
 *        orm_ddl_test <conninfo> <password>   (PostgreSQL, run by
 *                                              tests/db/pg_cluster.sh)
 * The functions are called by a rule of a package (the rule ddl-call of
 * each test package evaluates the expression of a ddl-call fact through
 * the test function test-eval), or through Eval at the top level, where
 * no rule is executing; the generated schema is read back through the
 * backend's catalog. clpd_db_exec is wrapped (-Wl,--wrap) so a test can
 * make one generated statement fail with a real database error. */
#define _POSIX_C_SOURCE 200809L

#include "db.h"
#include "log.h"
#include "orm_ddl.h"
#include "packages.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "expect.h"

static char root[] = "/tmp/clpd_orm_ddl_test.XXXXXX";
static Environment *env;
static clpd_packages pkgs;
static clpd_orm_ddl ddl;
static clpd_db *db;
static int pg; /* 1 on PostgreSQL */
static const char *password; /* PostgreSQL: the role's password, never logged */

clpd_db_status __real_clpd_db_exec(clpd_db *db, const char *sql,
                                   const clpd_db_value *params, size_t n,
                                   clpd_db_result **result);

/* The next statement containing fail_sql runs a query on a missing table
 * instead, so it fails with the backend's own message (once). */
static const char *fail_sql;
#define MISSING_TABLE "clpd_missing_table"

clpd_db_status __wrap_clpd_db_exec(clpd_db *db, const char *sql,
                                   const clpd_db_value *params, size_t n,
                                   clpd_db_result **result) {
  if (fail_sql != NULL && strstr(sql, fail_sql) != NULL) {
    fail_sql = NULL;
    return __real_clpd_db_exec(db, "SELECT 1 FROM " MISSING_TABLE, NULL, 0,
                               result);
  }
  return __real_clpd_db_exec(db, sql, params, n, result);
}

static void write_file(const char *dir, const char *name, const char *text) {
  char path[512];
  FILE *f;
  snprintf(path, sizeof path, "%s/%s", dir, name);
  f = fopen(path, "w");
  if (f == NULL || fputs(text, f) < 0 || fclose(f) != 0) {
    perror("orm_ddl_test");
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

/* Every test package has this rule: a ddl-call fact makes a rule of the
 * package call the expression. */
#define DDL_CALL_RULE                                                      \
  "(deftemplate ddl-call (slot wf-id) (slot expr (type STRING)))\n"      \
  "(defrule ddl-call ?f <- (ddl-call (wf-id ?w) (expr ?e))\n"            \
  "  => (retract ?f) (test-eval ?e))\n"

static const char *shop_clp =
    DDL_CALL_RULE
    "(deftemplate order (slot wf-id) (slot line-no (type INTEGER))\n"
    "  (slot status (type SYMBOL)) (slot amount (type FLOAT)) (slot note)\n"
    "  (slot photo (type STRING)) (slot paid (type INTEGER)) (slot at)\n"
    "  (slot doc) (slot price))\n"
    "(deftemplate customer (slot wf-id) (slot name) (slot score (type "
    "INTEGER))\n"
    "  (slot note) (slot seen))\n";
static const char *shop_orm =
    "order.table = shop_order\n"
    "order.owned = yes\n"
    "order.key = line-no, status\n"
    "order.slot.wf-id = wf_id text string error\n"
    "order.slot.line-no = line_no bigint integer error\n"
    "order.slot.status = status text symbol error\n"
    "order.slot.amount = amount double float default\n"
    "order.slot.note = note text string nil\n"
    "order.slot.photo = photo bytea string default\n"
    "order.slot.paid = paid boolean integer default\n"
    "order.slot.at = placed_at timestamptz string nil\n"
    "order.slot.doc = doc jsonb string nil\n"
    "order.slot.price = price numeric string error\n"
    "customer.table = crm_customer\n"
    "customer.owned = no\n"
    "customer.slot.wf-id = wf_id text string error\n"
    "customer.slot.name = full_name text string default\n"
    "customer.slot.score = score bigint integer default\n"
    "customer.slot.note = note text string nil\n"
    "customer.slot.seen = seen_at timestamptz string nil\n";
/* shop@3 maps only the customer table, like the other versions. */
static const char *shop3_orm =
    "customer.table = crm_customer\n"
    "customer.owned = no\n"
    "customer.slot.wf-id = wf_id text string error\n"
    "customer.slot.name = full_name text string default\n"
    "customer.slot.score = score bigint integer default\n"
    "customer.slot.note = note text string nil\n"
    "customer.slot.seen = seen_at timestamptz string nil\n";

/* The result of one call: kind 'T' (TRUE), 'F' (FALSE), 'M' (multifield,
 * items joined with '|'), '?' otherwise; log is what the call wrote to the
 * daemon log stream; ran is 0 when the calling rule never fired. */
typedef struct {
  char kind;
  size_t count;
  char items[4096];
  char log[4096];
  EvalError rc;
  int ran;
} call_result;

static void result_of(const CLIPSValue *v, call_result *c) {
  size_t i;
  c->kind = '?';
  c->count = 0;
  c->items[0] = '\0';
  if (v->header->type == SYMBOL_TYPE && v->lexemeValue == TrueSymbol(env)) {
    c->kind = 'T';
  } else if (v->header->type == SYMBOL_TYPE &&
             v->lexemeValue == FalseSymbol(env)) {
    c->kind = 'F';
  } else if (v->header->type == MULTIFIELD_TYPE) {
    c->kind = 'M';
    c->count = v->multifieldValue->length;
    for (i = 0; i < c->count; i++) {
      CLIPSValue *e = &v->multifieldValue->contents[i];
      size_t len = strlen(c->items);
      snprintf(c->items + len, sizeof c->items - len, "%s%s%s",
               i == 0 ? "" : "|",
               e->header->type == STRING_TYPE ? "" : "(not a string)",
               e->lexemeValue->contents);
    }
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

/* Calls expr from the rule ddl-call of module `module`, or through Eval
 * at the top level (no rule executing) when module is NULL. */
static void call_in(const char *module, const char *expr, call_result *c) {
  FILE *f = tmpfile();
  size_t n;
  if (f == NULL) exit(2);
  memset(c, 0, sizeof *c);
  c->kind = '?';
  clpd_log_set(f);
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
    fb = CreateFactBuilder(env, "ddl-call");
    if (fb == NULL || FBPutSlotSymbol(fb, "wf-id", "w") != PSE_NO_ERROR ||
        FBPutSlotString(fb, "expr", expr) != PSE_NO_ERROR ||
        FBAssert(fb) == NULL)
      exit(2);
    FBDispose(fb);
    SetEvaluationError(env, false);
    SetHaltExecution(env, false);
    pending = c;
    ClearFocusStack(env);
    Focus(m);
    Run(env, -1);
    pending = NULL;
    ClearFocusStack(env);
    SetEvaluationError(env, false);
    SetHaltExecution(env, false);
    SetCurrentModule(env, FindDefmodule(env, "MAIN"));
  }
  clpd_log_set(NULL);
  rewind(f);
  n = fread(c->log, 1, sizeof c->log - 1, f);
  c->log[n] = '\0';
  fclose(f);
  /* The role's password never reaches a message. */
  if (password != NULL && strstr(c->log, password) != NULL) {
    fprintf(stderr, "  %s: the log holds the password\n", expr);
    failures++;
  }
}

static void call(const char *expr, call_result *c) { call_in(NULL, expr, c); }

static int same(const char *what, const char *got, const char *want) {
  if (strcmp(got, want) != 0) {
    fprintf(stderr, "  %s\n    got:  [%s]\n    want: [%s]\n", what, got, want);
    return 0;
  }
  return 1;
}

/* A call from a rule of `module` (NULL: the top level) that returned
 * TRUE and logged nothing. */
static int ok_true_in(const char *module, const char *expr) {
  call_result c;
  call_in(module, expr, &c);
  if (!c.ran || c.kind != 'T' || c.log[0] != '\0' || c.rc != EE_NO_ERROR) {
    fprintf(stderr, "  %s (%s): ran %d kind %c rc %d log [%s]\n", expr,
            module != NULL ? module : "top level", c.ran, c.kind, (int)c.rc,
            c.log);
    return 0;
  }
  return 1;
}

/* A call that failed: FALSE, an evaluation error, exactly `line` logged. */
static int fails_in(const char *module, const char *expr, const char *line) {
  call_result c;
  char want[1024];
  call_in(module, expr, &c);
  snprintf(want, sizeof want, "%s\n", line);
  if (!c.ran || c.kind != 'F' || c.rc != EE_PROCESSING_ERROR) {
    fprintf(stderr, "  %s (%s): ran %d kind %c rc %d log [%s]\n", expr,
            module != NULL ? module : "top level", c.ran, c.kind, (int)c.rc,
            c.log);
    return 0;
  }
  return same(expr, c.log, want);
}

static int fails(const char *expr, const char *line) {
  return fails_in(NULL, expr, line);
}

/* A call that failed with a database error: `prefix` followed by the
 * database's last message (the injected query on MISSING_TABLE). */
static int fails_db(const char *module, const char *expr, const char *prefix) {
  char line[1024];
  const char *msg;
  call_result c;
  call_in(module, expr, &c);
  msg = clpd_db_error(db);
  snprintf(line, sizeof line, "%s%s\n", prefix, msg);
  if (fail_sql != NULL) {
    fprintf(stderr, "  %s: the statement [%s] was never run\n", expr,
            fail_sql);
    fail_sql = NULL;
    return 0;
  }
  if (!c.ran || c.kind != 'F' || c.rc != EE_PROCESSING_ERROR ||
      strstr(msg, MISSING_TABLE) == NULL) {
    fprintf(stderr, "  %s: ran %d kind %c rc %d log [%s]\n", expr, c.ran,
            c.kind, (int)c.rc, c.log);
    return 0;
  }
  return same(expr, c.log, line);
}

/* A db-check-schema call returning the difference strings `want` (joined
 * with '|'), logging nothing. */
static int differences(const char *expr, const char *want) {
  call_result c;
  call(expr, &c);
  return c.kind == 'M' && c.log[0] == '\0' && c.rc == EE_NO_ERROR &&
         same(expr, c.items, want);
}

static void sql(const char *text) {
  if (clpd_db_exec(db, text, NULL, 0, NULL) != CLPD_DB_OK) {
    fprintf(stderr, "orm_ddl_test: %s: %s\n", text, clpd_db_error(db));
    exit(2);
  }
}

/* Rows of `table`, or -1 when it cannot be read (it does not exist). */
static long long rows(const char *table) {
  char text[256];
  clpd_db_result *r = NULL;
  long long n = -1;
  snprintf(text, sizeof text, "SELECT count(*) FROM \"%s\"", table);
  if (clpd_db_exec(db, text, NULL, 0, &r) == CLPD_DB_OK)
    n = (long long)clpd_db_result_value(r, 0, 0)->integer;
  clpd_db_result_free(r);
  return n;
}

/* The columns of `table` as the catalog shows them, "name type[ NOT NULL]"
 * joined with ", ", then " | pk " and the primary key columns in order;
 * "" when the table does not exist. */
static void describe(const char *table, char *out, size_t cap) {
  static const char *const lite_cols =
      "SELECT name, type, \"notnull\" FROM pragma_table_info($1) ORDER BY cid";
  static const char *const lite_pk =
      "SELECT name FROM pragma_table_info($1) WHERE pk > 0 ORDER BY pk";
  static const char *const pg_cols =
      "SELECT column_name, udt_name, CASE is_nullable WHEN 'NO' THEN 1 ELSE "
      "0 END FROM information_schema.columns WHERE table_schema = "
      "current_schema() AND table_name = $1 ORDER BY ordinal_position";
  static const char *const pg_pk =
      "SELECT k.column_name FROM information_schema.table_constraints c "
      "JOIN information_schema.key_column_usage k ON k.constraint_schema = "
      "c.constraint_schema AND k.constraint_name = c.constraint_name WHERE "
      "c.table_schema = current_schema() AND c.table_name = $1 AND "
      "c.constraint_type = 'PRIMARY KEY' ORDER BY k.ordinal_position";
  clpd_db_value p = clpd_db_text(table);
  clpd_db_result *r = NULL;
  size_t i, len;
  out[0] = '\0';
  if (clpd_db_exec(db, pg ? pg_cols : lite_cols, &p, 1, &r) != CLPD_DB_OK) {
    snprintf(out, cap, "error: %s", clpd_db_error(db));
    return;
  }
  for (i = 0; i < clpd_db_result_rows(r); i++) {
    len = strlen(out);
    snprintf(out + len, cap - len, "%s%s %s%s", i == 0 ? "" : ", ",
             (const char *)clpd_db_result_value(r, i, 0)->data,
             (const char *)clpd_db_result_value(r, i, 1)->data,
             clpd_db_result_value(r, i, 2)->integer ? " NOT NULL" : "");
  }
  if (clpd_db_result_rows(r) == 0) {
    clpd_db_result_free(r);
    return;
  }
  clpd_db_result_free(r);
  r = NULL;
  if (clpd_db_exec(db, pg ? pg_pk : lite_pk, &p, 1, &r) != CLPD_DB_OK) {
    snprintf(out, cap, "error: %s", clpd_db_error(db));
    return;
  }
  len = strlen(out);
  snprintf(out + len, cap - len, " | pk");
  for (i = 0; i < clpd_db_result_rows(r); i++) {
    len = strlen(out);
    snprintf(out + len, cap - len, "%s%s", i == 0 ? " " : ",",
             (const char *)clpd_db_result_value(r, i, 0)->data);
  }
  clpd_db_result_free(r);
}

static int schema_is(const char *table, const char *want) {
  char got[2048];
  describe(table, got, sizeof got);
  return same(table, got, want);
}

/* Rows of <schema>.<table>, or -1 when it cannot be read. */
static long long rows_in(const char *schema, const char *table) {
  char text[256];
  clpd_db_result *r = NULL;
  long long n = -1;
  snprintf(text, sizeof text, "SELECT count(*) FROM \"%s\".\"%s\"", schema,
           table);
  if (clpd_db_exec(db, text, NULL, 0, &r) == CLPD_DB_OK)
    n = (long long)clpd_db_result_value(r, 0, 0)->integer;
  clpd_db_result_free(r);
  return n;
}

static void setup(const char *spec) {
  const char *dirs[4];
  char msg[512];
  clpd_list list;
  FILE *err;
  char eb[4096];
  size_t n;
  int rc;

  dirs[0] = package("shop1", "definition = shop\nversion = 1\n"
                             "files = shop.clp\norm = orm.conf\n");
  dirs[1] = package("shop2", "definition = shop\nversion = 2\n"
                             "files = shop.clp\norm = orm.conf\n");
  dirs[2] = package("solo", "definition = solo\nversion = 1\n"
                            "files = solo.clp\norm = orm.conf\n");
  dirs[3] = package("shop3", "definition = shop\nversion = 3\n"
                             "files = shop.clp\norm = orm.conf\n");
  write_file(dirs[0], "shop.clp", shop_clp);
  write_file(dirs[0], "orm.conf", shop_orm);
  write_file(dirs[1], "shop.clp", shop_clp);
  write_file(dirs[1], "orm.conf", shop_orm);
  write_file(dirs[2], "solo.clp",
             DDL_CALL_RULE
             "(deftemplate ticket (slot wf-id) (slot title))\n"
             "(deftemplate plain (slot wf-id))\n");
  write_file(dirs[2], "orm.conf",
             "ticket.table = solo_ticket\nticket.owned = yes\n"
             "ticket.slot.wf-id = wf_id text string error\n"
             "ticket.slot.title = title text string nil\n");
  write_file(dirs[3], "shop.clp", shop_clp);
  write_file(dirs[3], "orm.conf", shop3_orm);
  if (clpd_db_open(spec, &db, msg, sizeof msg) != CLPD_DB_OK) {
    fprintf(stderr, "orm_ddl_test: cannot open the database: %s\n", msg);
    exit(2);
  }
  pg = strcmp(clpd_db_backend(db), "postgresql") == 0;
  sql("DROP VIEW IF EXISTS solo_ticket");
  sql("DROP TABLE IF EXISTS shop_order");
  sql("DROP TABLE IF EXISTS crm_customer");
  sql("DROP TABLE IF EXISTS solo_ticket");

  env = CreateEnvironment();
  /* Without registration (no database configured) the names are not
   * functions. */
  EXPECT(FindFunction(env, "db-create-table") == NULL &&
         FindFunction(env, "db-check-schema") == NULL &&
         FindFunction(env, "db-drop-table") == NULL);
  ddl.db = db;
  ddl.packages = &pkgs;
  ddl.allow_drop = 0;
  EXPECT(clpd_orm_ddl_register(env, &ddl) == 0);
  EXPECT(FindFunction(env, "db-create-table") != NULL &&
         FindFunction(env, "db-check-schema") != NULL &&
         FindFunction(env, "db-drop-table") != NULL);
  if (AddUDF(env, "test-eval", "b", 1, 1, "s", test_eval, "test_eval",
             NULL) != AUE_NO_ERROR)
    exit(2);
  list.items = (char **)dirs;
  list.count = 4;
  err = tmpfile();
  if (err == NULL) exit(2);
  rc = clpd_packages_load_ex(env, &list, 1, &pkgs, err);
  rewind(err);
  n = fread(eb, 1, sizeof eb - 1, err);
  eb[n] = '\0';
  fclose(err);
  if (rc != 0) {
    fprintf(stderr, "orm_ddl_test: packages did not load:\n%s", eb);
    exit(2);
  }
  /* A module that is no package, with the same calling rule. */
  if (Build(env, "(defmodule notpkg)") != BE_NO_ERROR ||
      Build(env, "(deftemplate notpkg::ddl-call (slot wf-id) (slot expr "
                 "(type STRING)))") != BE_NO_ERROR ||
      Build(env, "(defrule notpkg::ddl-call ?f <- (ddl-call (wf-id ?w) "
                 "(expr ?e)) => (retract ?f) (test-eval ?e))") != BE_NO_ERROR)
    exit(2);
  SetCurrentModule(env, FindDefmodule(env, "MAIN"));
}

/* PostgreSQL: the test connects with the conninfo pg_cluster.sh passes
 * (no password in it) and the password file it exports as PGPASSFILE, like
 * tests/db/db_pg_test.c; the cluster asks for the password (scram-sha-256),
 * so without the file the same conninfo does not connect. */
static void test_credentials(const char *conninfo) {
  const char *passfile = getenv("PGPASSFILE");
  char msg[512] = "", missing[4096];
  clpd_db *none = NULL;
  EXPECT(clpd_db_check_spec(conninfo, msg, sizeof msg) == 1);
  EXPECT(strstr(conninfo, password) == NULL);
  EXPECT(passfile != NULL);
  if (passfile == NULL) return;
  snprintf(missing, sizeof missing, "%s.missing", passfile);
  setenv("PGPASSFILE", missing, 1);
  EXPECT(clpd_db_open(conninfo, &none, msg, sizeof msg) == CLPD_DB_ERROR);
  EXPECT(none == NULL && strstr(msg, "password") != NULL &&
         strstr(msg, password) == NULL);
  setenv("PGPASSFILE", passfile, 1);
  clpd_db_close(none);
}

/* db-create-table: CREATE TABLE IF NOT EXISTS with the mapped columns in
 * file order, NOT NULL for null policy error, the wf_id + local key
 * primary key, the backend's type names. */
static void test_create(void) {
  const char *want = pg ?
      "wf_id text NOT NULL, line_no int8 NOT NULL, status text NOT NULL, "
      "amount float8, note text, photo bytea, paid bool, placed_at "
      "timestamptz, doc jsonb, price numeric NOT NULL | pk "
      "wf_id,line_no,status" :
      "wf_id TEXT NOT NULL, line_no INTEGER NOT NULL, status TEXT NOT NULL, "
      "amount REAL, note TEXT, photo BLOB, paid INTEGER, placed_at TEXT, "
      "doc TEXT, price TEXT NOT NULL | pk wf_id,line_no,status";
  EXPECT(ok_true_in("shop@1", "(db-create-table shop@1::order)"));
  EXPECT(schema_is("shop_order", want));
  EXPECT(differences("(db-check-schema shop@1::order)", ""));
  /* An existing owned table is left as it is, rows included. */
  sql("INSERT INTO shop_order (wf_id, line_no, status, price) VALUES "
      "('w1', 1, 'new', '9.50')");
  EXPECT(ok_true_in("shop@1", "(db-create-table shop@2::order)"));
  EXPECT(ok_true_in("shop@2", "(db-create-table order)"));
  EXPECT(rows("shop_order") == 1);
  EXPECT(schema_is("shop_order", want));
  /* Only the local key is wf_id here. */
  EXPECT(ok_true_in("solo@1", "(db-create-table ticket)"));
  EXPECT(schema_is("solo_ticket", pg ? "wf_id text NOT NULL, title text | pk "
                                       "wf_id"
                                     : "wf_id TEXT NOT NULL, title TEXT | pk "
                                       "wf_id"));
  EXPECT(differences("(db-check-schema \"ticket\")", ""));
  EXPECT(!clpd_db_in_transaction(db));
}

/* Template names: <package>::<template>, or unqualified: the current
 * module's package first (for db-create-table and db-drop-table, the
 * calling rule's), then the one package mapping it. */
static void test_resolution(void) {
  EXPECT(fails("(db-check-schema order)",
               "clpdaemon: db-check-schema: template 'order' is mapped by "
               "shop@1, shop@2: qualify it as <package>::order"));
  SetCurrentModule(env, FindDefmodule(env, "shop@2"));
  EXPECT(differences("(db-check-schema order)", ""));
  SetCurrentModule(env, FindDefmodule(env, "solo@1"));
  EXPECT(fails("(db-check-schema order)",
               "clpdaemon: db-check-schema: template 'order' is mapped by "
               "shop@1, shop@2: qualify it as <package>::order"));
  SetCurrentModule(env, FindDefmodule(env, "MAIN"));
  EXPECT(fails("(db-check-schema plain)",
               "clpdaemon: db-check-schema: unknown template 'plain' (no "
               "loaded package maps it)"));
  EXPECT(fails("(db-check-schema nope@1::order)",
               "clpdaemon: db-check-schema: unknown template "
               "'nope@1::order' (no loaded package maps it)"));
  EXPECT(fails_in("shop@3", "(db-create-table order)",
                  "clpdaemon: db-create-table: template 'order' is mapped "
                  "by shop@1, shop@2: qualify it as <package>::order"));
  EXPECT(fails_in("solo@1", "(db-create-table nothing)",
                  "clpdaemon: db-create-table: unknown template 'nothing' "
                  "(no loaded package maps it)"));
  EXPECT(fails_in("solo@1", "(db-drop-table nope@1::order)",
                  "clpdaemon: db-drop-table: unknown template "
                  "'nope@1::order' (no loaded package maps it)"));
  EXPECT(fails_in("solo@1", "(db-create-table solo@1::order)",
                  "clpdaemon: db-create-table: unknown template "
                  "'solo@1::order' (no loaded package maps it)"));
}

/* db-create-table and db-drop-table act only on templates of the calling
 * rule's definition (any version); db-check-schema reaches every package
 * (L84). A refused call leaves the table and its rows. */
static void test_scope(void) {
  ddl.allow_drop = 1;
  EXPECT(rows("shop_order") == 1);
  /* No rule is executing: the top level (also a deffacts or a plugin's
   * after_load). */
  EXPECT(fails("(db-create-table shop@1::order)",
               "clpdaemon: db-create-table: only a rule of a rule package "
               "may call it (no rule is executing)"));
  EXPECT(fails("(db-drop-table shop@1::order)",
               "clpdaemon: db-drop-table: only a rule of a rule package may "
               "call it (no rule is executing)"));
  EXPECT(fails("(db-drop-table ticket)",
               "clpdaemon: db-drop-table: only a rule of a rule package may "
               "call it (no rule is executing)"));
  SetCurrentModule(env, FindDefmodule(env, "solo@1"));
  EXPECT(fails("(db-drop-table ticket)",
               "clpdaemon: db-drop-table: only a rule of a rule package may "
               "call it (no rule is executing)"));
  SetCurrentModule(env, FindDefmodule(env, "MAIN"));
  /* A rule of a module that is no package. */
  EXPECT(fails_in("notpkg", "(db-drop-table solo@1::ticket)",
                  "clpdaemon: db-drop-table: only a rule of a rule package "
                  "may call it (rule 'ddl-call' is in module 'notpkg')"));
  /* Another definition's template, qualified or only mapped there. */
  EXPECT(fails_in("solo@1", "(db-drop-table shop@1::order)",
                  "clpdaemon: db-drop-table: template 'shop@1::order' is "
                  "not mapped by definition solo of the calling rule "
                  "(mapped by shop@1)"));
  EXPECT(fails_in("solo@1", "(db-create-table order)",
                  "clpdaemon: db-create-table: template 'order' is not "
                  "mapped by definition solo of the calling rule (mapped by "
                  "shop@1, shop@2)"));
  EXPECT(fails_in("solo@1", "(db-create-table shop@3::customer)",
                  "clpdaemon: db-create-table: template 'shop@3::customer' "
                  "is not mapped by definition solo of the calling rule "
                  "(mapped by shop@3)"));
  EXPECT(fails_in("shop@2", "(db-drop-table solo@1::ticket)",
                  "clpdaemon: db-drop-table: template 'solo@1::ticket' is "
                  "not mapped by definition shop of the calling rule (mapped "
                  "by solo@1)"));
  EXPECT(fails_in("shop@3", "(db-drop-table ticket)",
                  "clpdaemon: db-drop-table: template 'ticket' is not mapped "
                  "by definition shop of the calling rule (mapped by "
                  "solo@1)"));
  EXPECT(rows("shop_order") == 1 && rows("solo_ticket") == 0);
  EXPECT(schema_is("solo_ticket", pg ? "wf_id text NOT NULL, title text | pk "
                                       "wf_id"
                                     : "wf_id TEXT NOT NULL, title TEXT | pk "
                                       "wf_id"));
  /* db-check-schema still inspects any package, from any rule. */
  {
    call_result c;
    call_in("solo@1", "(db-check-schema shop@1::order)", &c);
    EXPECT(c.ran && c.kind == 'M' && c.count == 0 && c.log[0] == '\0');
  }
  /* Any version of the calling rule's definition. */
  EXPECT(ok_true_in("shop@3", "(db-create-table shop@1::order)"));
  EXPECT(rows("shop_order") == 1);
  ddl.allow_drop = 0;
}

/* A table that is not owned is never created; db-check-schema reports
 * the missing table, missing columns, incompatible types and NOT NULL
 * columns the mapping may write NULL into, and ignores extra columns. */
static void test_not_owned_and_check(void) {
  EXPECT(fails_in("shop@1", "(db-create-table shop@1::customer)",
                  "clpdaemon: db-create-table: template 'shop@1::customer': "
                  "table 'crm_customer' is not owned (owned = no), so it is "
                  "never created"));
  EXPECT(schema_is("crm_customer", ""));
  EXPECT(differences("(db-check-schema shop@1::customer)",
                     "table 'crm_customer' does not exist"));
  sql("CREATE TABLE crm_customer (wf_id TEXT, score TEXT NOT NULL, note "
      "TEXT NOT NULL, seen_at TIMESTAMPTZ, extra INTEGER)");
  sql("INSERT INTO crm_customer VALUES ('w1', 's', 'n', NULL, 7)");
  EXPECT(differences("(db-check-schema shop@1::customer)",
                     pg ? "column 'full_name' does not exist|"
                          "column 'score' has type 'text', expected bigint "
                          "(BIGINT)|"
                          "column 'score' is NOT NULL, expected nullable "
                          "(null policy default)|"
                          "column 'note' is NOT NULL, expected nullable "
                          "(null policy nil)"
                        : "column 'full_name' does not exist|"
                          "column 'score' has type 'TEXT', expected bigint "
                          "(INTEGER)|"
                          "column 'score' is NOT NULL, expected nullable "
                          "(null policy default)|"
                          "column 'note' is NOT NULL, expected nullable "
                          "(null policy nil)|"
                          "column 'seen_at' has type 'TIMESTAMPTZ', "
                          "expected timestamptz (TEXT)"));
  /* Still never created or changed. */
  EXPECT(fails_in("shop@2", "(db-create-table shop@1::customer)",
                  "clpdaemon: db-create-table: template 'shop@1::customer': "
                  "table 'crm_customer' is not owned (owned = no), so it is "
                  "never created"));
  EXPECT(rows("crm_customer") == 1);
  /* Compatible types other than the generated ones. */
  sql("DROP TABLE crm_customer");
  sql(pg ? "CREATE TABLE crm_customer (wf_id VARCHAR(40) NOT NULL, "
           "full_name TEXT, score INTEGER, note TEXT, seen_at TIMESTAMPTZ)"
         : "CREATE TABLE crm_customer (wf_id VARCHAR(40) NOT NULL, "
           "full_name CLOB, score BIGINT, note TEXT, seen_at TEXT)");
  EXPECT(differences("(db-check-schema shop@2::customer)", ""));
}

/* db-drop-table: refused unless orm_allow_drop = yes, never for a table
 * that is not owned; both leave the table and its rows. */
static void test_drop(void) {
  sql("INSERT INTO crm_customer (wf_id) VALUES ('w1')");
  ddl.allow_drop = 0;
  EXPECT(fails_in("shop@1", "(db-drop-table shop@1::order)",
                  "clpdaemon: db-drop-table: dropping tables is disabled "
                  "(orm_allow_drop = no)"));
  EXPECT(fails_in("shop@1", "(db-drop-table shop@1::customer)",
                  "clpdaemon: db-drop-table: dropping tables is disabled "
                  "(orm_allow_drop = no)"));
  EXPECT(rows("shop_order") == 1 && rows("crm_customer") == 1);
  ddl.allow_drop = 1;
  EXPECT(fails_in("shop@1", "(db-drop-table shop@1::customer)",
                  "clpdaemon: db-drop-table: template 'shop@1::customer': "
                  "table 'crm_customer' is not owned (owned = no), so it is "
                  "never dropped"));
  EXPECT(rows("crm_customer") == 1);
  EXPECT(ok_true_in("shop@2", "(db-drop-table shop@1::order)"));
  EXPECT(rows("shop_order") == -1);
  EXPECT(differences("(db-check-schema shop@1::order)",
                     "table 'shop_order' does not exist"));
  EXPECT(ok_true_in("shop@1", "(db-drop-table order)")); /* already gone */
  EXPECT(!clpd_db_in_transaction(db));
}

/* A database error is reported with the table, one line. */
static void test_db_error(void) {
  call_result c;
  const char *prefix =
      "clpdaemon: db-drop-table: template 'ticket': table 'solo_ticket': ";
  sql("DROP TABLE solo_ticket");
  sql("CREATE VIEW solo_ticket AS SELECT 'w' AS wf_id, 't' AS title");
  call_in("solo@1", "(db-drop-table ticket)", &c);
  EXPECT(c.ran && c.kind == 'F' && c.rc == EE_PROCESSING_ERROR);
  EXPECT(strncmp(c.log, prefix, strlen(prefix)) == 0 &&
         strlen(c.log) > strlen(prefix) + 1 &&
         strchr(c.log, '\n') == c.log + strlen(c.log) - 1);
  if (strncmp(c.log, prefix, strlen(prefix)) != 0)
    fprintf(stderr, "  got [%s]\n", c.log);
  sql("DROP VIEW solo_ticket");
}

/* The database-error paths of db-check-schema (table lookup, column
 * query) and db-create-table: FALSE, one line with the database message,
 * nothing created. */
static void test_db_error_paths(void) {
  static const char *const check_prefix =
      "clpdaemon: db-check-schema: template 'ticket': table 'solo_ticket': ";
  static const char *const create_prefix =
      "clpdaemon: db-create-table: template 'ticket': table 'solo_ticket': ";
  fail_sql = pg ? "information_schema.tables" : "sqlite_master";
  EXPECT(fails_db(NULL, "(db-check-schema ticket)", check_prefix));
  fail_sql = "CREATE TABLE";
  EXPECT(fails_db("solo@1", "(db-create-table ticket)", create_prefix));
  EXPECT(rows("solo_ticket") == -1);
  if (pg) {
    fail_sql = "current_schema()";
    EXPECT(fails_db("solo@1", "(db-create-table ticket)", create_prefix));
    EXPECT(rows("solo_ticket") == -1);
  }
  EXPECT(ok_true_in("solo@1", "(db-create-table ticket)"));
  fail_sql = pg ? "information_schema.columns" : "pragma_table_info";
  EXPECT(fails_db(NULL, "(db-check-schema ticket)", check_prefix));
  EXPECT(differences("(db-check-schema ticket)", ""));
  EXPECT(!clpd_db_in_transaction(db));
  sql("DROP TABLE solo_ticket");
}

/* SQLite finds a table without ASCII case (as SQLite does, so
 * db-check-schema agrees with CREATE TABLE IF NOT EXISTS); PostgreSQL
 * compares the quoted name exactly. */
static void test_lookup_case(void) {
  sql("CREATE TABLE \"SOLO_TICKET\" (wf_id TEXT NOT NULL, title TEXT)");
  sql("INSERT INTO \"SOLO_TICKET\" VALUES ('w1', 'kept')");
  if (!pg) {
    EXPECT(differences("(db-check-schema ticket)", ""));
    EXPECT(ok_true_in("solo@1", "(db-create-table ticket)"));
    EXPECT(rows("SOLO_TICKET") == 1 && rows("solo_ticket") == 1);
    EXPECT(differences("(db-check-schema ticket)", ""));
  } else {
    EXPECT(differences("(db-check-schema ticket)",
                       "table 'solo_ticket' does not exist"));
    EXPECT(ok_true_in("solo@1", "(db-create-table ticket)"));
    EXPECT(rows("SOLO_TICKET") == 1 && rows("solo_ticket") == 0);
    EXPECT(differences("(db-check-schema ticket)", ""));
    sql("DROP TABLE solo_ticket");
  }
  sql("DROP TABLE \"SOLO_TICKET\"");
}

/* PostgreSQL: CREATE and DROP name the schema db-check-schema inspects
 * (current_schema()), so a same-named table in a later search_path schema
 * is never dropped; without a current schema both are errors. */
static void test_search_path(void) {
  static const char *const no_schema =
      "template 'ticket': table 'solo_ticket': no current schema (the "
      "search_path names no existing schema)";
  char line[512];
  sql("DROP SCHEMA IF EXISTS ddl_first CASCADE");
  sql("DROP SCHEMA IF EXISTS ddl_second CASCADE");
  sql("CREATE SCHEMA ddl_first");
  sql("CREATE SCHEMA ddl_second");
  sql("CREATE TABLE ddl_second.solo_ticket (wf_id TEXT NOT NULL, title "
      "TEXT)");
  sql("INSERT INTO ddl_second.solo_ticket VALUES ('w1', 'kept')");
  sql("SET search_path = ddl_first, ddl_second");
  ddl.allow_drop = 1;
  EXPECT(differences("(db-check-schema ticket)",
                     "table 'solo_ticket' does not exist"));
  EXPECT(ok_true_in("solo@1", "(db-drop-table ticket)"));
  EXPECT(rows_in("ddl_second", "solo_ticket") == 1);
  EXPECT(ok_true_in("solo@1", "(db-create-table ticket)"));
  EXPECT(rows_in("ddl_first", "solo_ticket") == 0);
  EXPECT(differences("(db-check-schema ticket)", ""));
  EXPECT(ok_true_in("solo@1", "(db-drop-table ticket)"));
  EXPECT(rows_in("ddl_first", "solo_ticket") == -1);
  EXPECT(rows_in("ddl_second", "solo_ticket") == 1);
  sql("SET search_path = ddl_missing");
  snprintf(line, sizeof line, "clpdaemon: db-create-table: %s", no_schema);
  EXPECT(fails_in("solo@1", "(db-create-table ticket)", line));
  snprintf(line, sizeof line, "clpdaemon: db-drop-table: %s", no_schema);
  EXPECT(fails_in("solo@1", "(db-drop-table ticket)", line));
  sql("RESET search_path");
  EXPECT(rows_in("ddl_second", "solo_ticket") == 1);
  EXPECT(rows_in("ddl_first", "solo_ticket") == -1);
  ddl.allow_drop = 0;
  sql("DROP SCHEMA ddl_first CASCADE");
  sql("DROP SCHEMA ddl_second CASCADE");
}

int main(int argc, char **argv) {
  char spec[sizeof root + 32];
  if (argc != 1 && argc != 3) {
    fprintf(stderr, "usage: orm_ddl_test [<conninfo> <password>]\n");
    return 2;
  }
  if (mkdtemp(root) == NULL) {
    perror("orm_ddl_test");
    return 2;
  }
  snprintf(spec, sizeof spec, "sqlite:%s/ddl.db", root);
  if (argc == 3) {
    password = argv[2];
    test_credentials(argv[1]);
  }
  setup(argc == 3 ? argv[1] : spec);
  test_create();
  test_resolution();
  test_scope();
  test_not_owned_and_check();
  test_drop();
  test_db_error();
  test_db_error_paths();
  test_lookup_case();
  if (pg) test_search_path();
  clpd_packages_free(&pkgs);
  DestroyEnvironment(env);
  clpd_db_close(db);
  if (failures != 0) {
    fprintf(stderr, "%d orm ddl checks failed (fixtures in %s)\n", failures,
            root);
    return 1;
  }
  {
    char cmd[sizeof root + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0) fprintf(stderr, "cannot remove %s\n", root);
  }
  puts("orm ddl checks passed");
  return 0;
}
