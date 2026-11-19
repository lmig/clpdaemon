/* Tests for the ORM DDL functions db-create-table, db-check-schema and
 * db-drop-table (T9b, T9d) and the sequence functions db-create-sequence
 * and db-nextval (T9c, docs/orm.md#ddl-functions).
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

#include <stdint.h>
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
static char db_spec[512]; /* the spec db was opened with */
static const char *password; /* PostgreSQL: the role's password, never logged */

clpd_db_status __real_clpd_db_exec(clpd_db *db, const char *sql,
                                   const clpd_db_value *params, size_t n,
                                   clpd_db_result **result);

/* The next statement containing fail_sql runs a query on a missing table
 * instead, so it fails with the backend's own message (once). */
static const char *fail_sql;
#define MISSING_TABLE "clpd_missing_table"
/* The next statement containing replace_sql runs replace_with instead,
 * with the same parameters (once). */
static const char *replace_sql, *replace_with;

clpd_db_status __wrap_clpd_db_exec(clpd_db *db, const char *sql,
                                   const clpd_db_value *params, size_t n,
                                   clpd_db_result **result) {
  if (fail_sql != NULL && strstr(sql, fail_sql) != NULL) {
    fail_sql = NULL;
    return __real_clpd_db_exec(db, "SELECT 1 FROM " MISSING_TABLE, NULL, 0,
                               result);
  }
  if (replace_sql != NULL && strstr(sql, replace_sql) != NULL) {
    replace_sql = NULL;
    return __real_clpd_db_exec(db, replace_with, params, n, result);
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
    "customer.slot.seen = seen_at timestamptz string nil\n"
    "seq.order_no = shop_order_no_seq\n"
    "seq.order_no.owned = yes\n"
    "seq.legacy = crm_legacy_seq\n"
    "seq.legacy.owned = no\n";
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
 * items joined with '|'), 'I' (INTEGER, in integer), '?' otherwise; log is what the call wrote to the
 * daemon log stream; ran is 0 when the calling rule never fired. */
typedef struct {
  char kind;
  long long integer;
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
  } else if (v->header->type == INTEGER_TYPE) {
    c->kind = 'I';
    c->integer = v->integerValue->contents;
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

/* A call from a rule of `module` that returned the INTEGER want and logged
 * nothing. */
static int int_in(const char *module, const char *expr, long long want) {
  call_result c;
  call_in(module, expr, &c);
  if (!c.ran || c.kind != 'I' || c.integer != want || c.log[0] != '\0' ||
      c.rc != EE_NO_ERROR) {
    fprintf(stderr, "  %s (%s): ran %d kind %c value %lld (want %lld) rc %d "
            "log [%s]\n", expr, module, c.ran, c.kind, c.integer, want,
            (int)c.rc, c.log);
    return 0;
  }
  return 1;
}

/* A call that failed with a database error: `prefix` followed by the
 * database's last message (the injected query on MISSING_TABLE). */
static int fails_db(const char *module, const char *expr, const char *prefix) {
  char line[1024];
  const char *msg;
  call_result c;
  call_in(module, expr, &c);
  /* The function may run more statements after the failure (db-nextval
   * looks the sequence up), so the expected message comes from the same
   * query run again. */
  __real_clpd_db_exec(db, "SELECT 1 FROM " MISSING_TABLE, NULL, 0, NULL);
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

/* The state of sequence `name`: -1 when it does not exist (SQLite: no
 * clpd_sequence table or no row), else the last value db-nextval returned
 * (0 before the first). */
static long long seq_state(const char *name) {
  static const char *const pg_exists =
      "SELECT 1 FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON "
      "n.oid = c.relnamespace WHERE n.nspname = current_schema() AND "
      "c.relname = $1 AND c.relkind = 'S'";
  clpd_db_value p = clpd_db_text(name);
  clpd_db_result *r = NULL;
  char text[256];
  long long v = -1;
  if (pg) {
    if (clpd_db_exec(db, pg_exists, &p, 1, &r) != CLPD_DB_OK ||
        clpd_db_result_rows(r) == 0) {
      clpd_db_result_free(r);
      return -1;
    }
    clpd_db_result_free(r);
    r = NULL;
    snprintf(text, sizeof text, "SELECT CASE WHEN is_called THEN last_value "
             "ELSE 0 END FROM \"%s\"", name);
    if (clpd_db_exec(db, text, NULL, 0, &r) == CLPD_DB_OK)
      v = (long long)clpd_db_result_value(r, 0, 0)->integer;
  } else if (clpd_db_exec(db, "SELECT value FROM clpd_sequence WHERE name = "
                              "$1", &p, 1, &r) == CLPD_DB_OK &&
             clpd_db_result_rows(r) == 1) {
    v = (long long)clpd_db_result_value(r, 0, 0)->integer;
  }
  clpd_db_result_free(r);
  return v;
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
             "ticket.slot.title = title text string nil\n"
             "seq.ticket_no = solo_ticket_no_seq\n"
             "seq.ticket_no.owned = yes\n");
  write_file(dirs[3], "shop.clp", shop_clp);
  write_file(dirs[3], "orm.conf", shop3_orm);
  if (clpd_db_open(spec, &db, msg, sizeof msg) != CLPD_DB_OK) {
    fprintf(stderr, "orm_ddl_test: cannot open the database: %s\n", msg);
    exit(2);
  }
  snprintf(db_spec, sizeof db_spec, "%s", spec);
  pg = strcmp(clpd_db_backend(db), "postgresql") == 0;
  if (pg) {
    sql("DROP SEQUENCE IF EXISTS shop_order_no_seq");
    sql("DROP SEQUENCE IF EXISTS crm_legacy_seq");
    sql("DROP SEQUENCE IF EXISTS solo_ticket_no_seq");
  } else {
    sql("DROP TABLE IF EXISTS clpd_sequence");
  }
  sql("DROP VIEW IF EXISTS solo_ticket");
  sql("DROP TABLE IF EXISTS shop_order");
  sql("DROP TABLE IF EXISTS crm_customer");
  sql("DROP TABLE IF EXISTS solo_ticket");

  env = CreateEnvironment();
  /* Without registration (no database configured) the names are not
   * functions. */
  EXPECT(FindFunction(env, "db-create-table") == NULL &&
         FindFunction(env, "db-check-schema") == NULL &&
         FindFunction(env, "db-drop-table") == NULL &&
         FindFunction(env, "db-create-sequence") == NULL &&
         FindFunction(env, "db-nextval") == NULL);
  ddl.db = db;
  ddl.packages = &pkgs;
  ddl.allow_drop = 0;
  EXPECT(clpd_orm_ddl_register(env, &ddl) == 0);
  EXPECT(FindFunction(env, "db-create-table") != NULL &&
         FindFunction(env, "db-check-schema") != NULL &&
         FindFunction(env, "db-drop-table") != NULL &&
         FindFunction(env, "db-create-sequence") != NULL &&
         FindFunction(env, "db-nextval") != NULL);
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

#define NO_RULE(udf)                                                       \
  "clpdaemon: " udf ": only a rule of a rule package may call it (no rule " \
  "is executing)"

/* db-create-sequence and db-nextval (T9c): only a rule of a rule package,
 * only the aliases its definition maps (any version); owned sequences
 * only are created; values 1, 2, 3 ... shared by the versions that map
 * the sequence; a refused call leaves the sequence as it is. */
static void test_sequences(void) {
  static const char *const order_missing =
      "clpdaemon: db-nextval: sequence alias 'order_no': sequence "
      "'shop_order_no_seq' does not exist";
  /* Outside a rule, or from a module that is no package. */
  EXPECT(fails("(db-create-sequence shop@1::order_no)",
               NO_RULE("db-create-sequence")));
  EXPECT(fails("(db-nextval shop@1::order_no)", NO_RULE("db-nextval")));
  SetCurrentModule(env, FindDefmodule(env, "shop@1"));
  EXPECT(fails("(db-nextval order_no)", NO_RULE("db-nextval")));
  SetCurrentModule(env, FindDefmodule(env, "MAIN"));
  EXPECT(fails_in("notpkg", "(db-create-sequence shop@1::order_no)",
                  "clpdaemon: db-create-sequence: only a rule of a rule "
                  "package may call it (rule 'ddl-call' is in module "
                  "'notpkg')"));
  EXPECT(fails_in("notpkg", "(db-nextval shop@1::order_no)",
                  "clpdaemon: db-nextval: only a rule of a rule package may "
                  "call it (rule 'ddl-call' is in module 'notpkg')"));
  EXPECT(seq_state("shop_order_no_seq") == -1);
  /* Not created yet (SQLite: not even the counter table). */
  EXPECT(fails_in("shop@1", "(db-nextval order_no)", order_missing));
  /* Alias resolution. */
  EXPECT(fails_in("shop@3", "(db-nextval order_no)",
                  "clpdaemon: db-nextval: sequence alias 'order_no' is "
                  "mapped by shop@1, shop@2: qualify it as "
                  "<package>::order_no"));
  EXPECT(fails_in("shop@3", "(db-create-sequence order_no)",
                  "clpdaemon: db-create-sequence: sequence alias 'order_no' "
                  "is mapped by shop@1, shop@2: qualify it as "
                  "<package>::order_no"));
  EXPECT(fails_in("solo@1", "(db-nextval order_no)",
                  "clpdaemon: db-nextval: sequence alias 'order_no' is not "
                  "mapped by definition solo of the calling rule (mapped by "
                  "shop@1, shop@2)"));
  EXPECT(fails_in("solo@1", "(db-create-sequence shop@2::order_no)",
                  "clpdaemon: db-create-sequence: sequence alias "
                  "'shop@2::order_no' is not mapped by definition solo of "
                  "the calling rule (mapped by shop@2)"));
  EXPECT(fails_in("shop@1", "(db-nextval solo@1::ticket_no)",
                  "clpdaemon: db-nextval: sequence alias 'solo@1::ticket_no' "
                  "is not mapped by definition shop of the calling rule "
                  "(mapped by solo@1)"));
  EXPECT(fails_in("shop@1", "(db-nextval nothing)",
                  "clpdaemon: db-nextval: unknown sequence alias 'nothing' "
                  "(no loaded package maps it)"));
  EXPECT(fails_in("shop@1", "(db-nextval shop@3::order_no)",
                  "clpdaemon: db-nextval: unknown sequence alias "
                  "'shop@3::order_no' (no loaded package maps it)"));
  /* A template name is no sequence alias, and an alias no template. */
  EXPECT(fails_in("shop@1", "(db-create-sequence order)",
                  "clpdaemon: db-create-sequence: unknown sequence alias "
                  "'order' (no loaded package maps it)"));
  EXPECT(fails_in("shop@1", "(db-create-table order_no)",
                  "clpdaemon: db-create-table: unknown template 'order_no' "
                  "(no loaded package maps it)"));
  EXPECT(fails("(db-check-schema shop@1::order_no)",
               "clpdaemon: db-check-schema: unknown template "
               "'shop@1::order_no' (no loaded package maps it)"));
  /* A sequence that is not owned is never created. */
  EXPECT(fails_in("shop@1", "(db-create-sequence legacy)",
                  "clpdaemon: db-create-sequence: sequence alias 'legacy': "
                  "sequence 'crm_legacy_seq' is not owned (owned = no), so "
                  "it is never created"));
  EXPECT(seq_state("shop_order_no_seq") == -1 &&
         seq_state("crm_legacy_seq") == -1);
  /* Created once; IF NOT EXISTS from any version. */
  EXPECT(ok_true_in("shop@1", "(db-create-sequence order_no)"));
  EXPECT(seq_state("shop_order_no_seq") == 0);
  EXPECT(ok_true_in("shop@2", "(db-create-sequence order_no)"));
  EXPECT(ok_true_in("shop@3", "(db-create-sequence shop@1::order_no)"));
  EXPECT(seq_state("shop_order_no_seq") == 0);
  /* One sequence for the versions that map it. */
  EXPECT(int_in("shop@1", "(db-nextval order_no)", 1));
  EXPECT(int_in("shop@2", "(db-nextval order_no)", 2));
  EXPECT(int_in("shop@3", "(db-nextval shop@2::order_no)", 3));
  EXPECT(int_in("shop@2", "(db-nextval \"shop@1::order_no\")", 4));
  /* Creating it again keeps the value. */
  EXPECT(ok_true_in("shop@1", "(db-create-sequence order_no)"));
  EXPECT(seq_state("shop_order_no_seq") == 4);
  /* Refused calls do not advance it. */
  EXPECT(fails("(db-nextval shop@1::order_no)", NO_RULE("db-nextval")));
  EXPECT(fails_in("solo@1", "(db-nextval shop@1::order_no)",
                  "clpdaemon: db-nextval: sequence alias 'shop@1::order_no' "
                  "is not mapped by definition solo of the calling rule "
                  "(mapped by shop@1)"));
  EXPECT(fails_in("shop@3", "(db-nextval order_no)",
                  "clpdaemon: db-nextval: sequence alias 'order_no' is "
                  "mapped by shop@1, shop@2: qualify it as "
                  "<package>::order_no"));
  EXPECT(seq_state("shop_order_no_seq") == 4);
  EXPECT(int_in("shop@1", "(db-nextval order_no)", 5));
  /* Another definition's sequence counts on its own. */
  EXPECT(ok_true_in("solo@1", "(db-create-sequence ticket_no)"));
  EXPECT(int_in("solo@1", "(db-nextval ticket_no)", 1));
  EXPECT(seq_state("shop_order_no_seq") == 5);
  /* A sequence that is not owned: missing until another system creates
   * it, then used as it is. */
  EXPECT(fails_in("shop@1", "(db-nextval legacy)",
                  "clpdaemon: db-nextval: sequence alias 'legacy': sequence "
                  "'crm_legacy_seq' does not exist"));
  sql(pg ? "CREATE SEQUENCE crm_legacy_seq START 40"
         : "INSERT INTO clpd_sequence VALUES ('crm_legacy_seq', 39)");
  EXPECT(int_in("shop@2", "(db-nextval legacy)", 40));
  /* Overflow: an error, the value stays at the maximum. */
  sql(pg ? "ALTER SEQUENCE crm_legacy_seq RESTART WITH 9223372036854775807"
         : "UPDATE clpd_sequence SET value = 9223372036854775806 WHERE name = "
           "'crm_legacy_seq'");
  EXPECT(int_in("shop@1", "(db-nextval legacy)", INT64_MAX));
  EXPECT(fails_in("shop@1", "(db-nextval legacy)",
                  pg ? "clpdaemon: db-nextval: sequence alias 'legacy': "
                       "sequence 'crm_legacy_seq': nextval: reached maximum "
                       "value of sequence \"?\" (9223372036854775807) "
                       "(SQLSTATE 2200H)"
                     : "clpdaemon: db-nextval: sequence alias 'legacy': "
                       "sequence 'crm_legacy_seq' has reached its maximum "
                       "value"));
  EXPECT(seq_state("crm_legacy_seq") == INT64_MAX);
  /* SQLite: a counter row deleted by someone else is a missing sequence. */
  if (!pg) {
    sql("DELETE FROM clpd_sequence WHERE name = 'crm_legacy_seq'");
    EXPECT(fails_in("shop@1", "(db-nextval legacy)",
                    "clpdaemon: db-nextval: sequence alias 'legacy': "
                    "sequence 'crm_legacy_seq' does not exist"));
    EXPECT(seq_state("crm_legacy_seq") == -1);
  }
  EXPECT(!clpd_db_in_transaction(db));
}

/* Two connections (two daemons on one database) never get the same
 * value. */
static void test_two_connections(void) {
  clpd_db *other = NULL;
  char msg[512];
  long long first;
  int i;
  if (clpd_db_open(db_spec, &other, msg, sizeof msg) != CLPD_DB_OK) {
    fprintf(stderr, "orm_ddl_test: second connection: %s\n", msg);
    failures++;
    return;
  }
  first = seq_state("shop_order_no_seq") + 1;
  for (i = 0; i < 6; i++) {
    ddl.db = i % 2 == 0 ? db : other;
    EXPECT(int_in(i % 3 == 0 ? "shop@1" : "shop@2", "(db-nextval order_no)",
                  first + i));
  }
  ddl.db = db;
  EXPECT(seq_state("shop_order_no_seq") == first + 5);
  clpd_db_close(other);
}

/* The database-error paths of db-create-sequence and db-nextval: FALSE,
 * one line with the database message, the value unchanged. */
static void test_sequence_db_errors(void) {
  static const char *const create_prefix =
      "clpdaemon: db-create-sequence: sequence alias 'ticket_no': sequence "
      "'solo_ticket_no_seq': ";
  static const char *const next_prefix =
      "clpdaemon: db-nextval: sequence alias 'ticket_no': sequence "
      "'solo_ticket_no_seq': ";
  long long before = seq_state("solo_ticket_no_seq");
  fail_sql = pg ? "CREATE SEQUENCE" : "CREATE TABLE IF NOT EXISTS";
  EXPECT(fails_db("solo@1", "(db-create-sequence ticket_no)", create_prefix));
  if (!pg) {
    fail_sql = "INSERT";
    EXPECT(fails_db("solo@1", "(db-create-sequence ticket_no)",
                    create_prefix));
  } else {
    fail_sql = "current_schema()";
    EXPECT(fails_db("solo@1", "(db-create-sequence ticket_no)",
                    create_prefix));
    fail_sql = "current_schema()";
    EXPECT(fails_db("solo@1", "(db-nextval ticket_no)", next_prefix));
  }
  fail_sql = pg ? "nextval" : "UPDATE";
  EXPECT(fails_db("solo@1", "(db-nextval ticket_no)", next_prefix));
  EXPECT(seq_state("solo_ticket_no_seq") == before);
  EXPECT(int_in("solo@1", "(db-nextval ticket_no)", before + 1));
  EXPECT(!clpd_db_in_transaction(db));
}

/* The text of one query's rows (columns joined with ' ', rows with '\n')
 * appended to out; "error: <message>" when it fails. */
static void put_rows(const char *query, const clpd_db_value *p, size_t n,
                     char *out, size_t cap) {
  clpd_db_result *r = NULL;
  size_t i, j, len;
  if (__real_clpd_db_exec(db, query, p, n, &r) != CLPD_DB_OK) {
    len = strlen(out);
    snprintf(out + len, cap - len, "error: %s\n", clpd_db_error(db));
    return;
  }
  for (i = 0; i < clpd_db_result_rows(r); i++) {
    for (j = 0; j < clpd_db_result_columns(r); j++) {
      const clpd_db_value *v = clpd_db_result_value(r, i, j);
      len = strlen(out);
      if (v->type == CLPD_DB_INTEGER)
        snprintf(out + len, cap - len, "%s%lld", j ? " " : "",
                 (long long)v->integer);
      else if (v->type == CLPD_DB_FLOAT)
        snprintf(out + len, cap - len, "%s%g", j ? " " : "", v->real);
      else if (v->type == CLPD_DB_TEXT)
        snprintf(out + len, cap - len, "%s%s", j ? " " : "",
                 (const char *)v->data);
      else
        snprintf(out + len, cap - len, "%s?", j ? " " : "");
    }
    len = strlen(out);
    snprintf(out + len, cap - len, "\n");
  }
  clpd_db_result_free(r);
}

/* What the database holds in the current schema, like a dump: every
 * relation with its definition (SQLite) or kind (PostgreSQL), the rows of
 * every table and the state of every sequence. */
static void fingerprint(char *out, size_t cap) {
  static const char *const lite_objects =
      "SELECT type, name, coalesce(sql, '') FROM sqlite_master ORDER BY name";
  static const char *const lite_tables =
      "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name";
  static const char *const pg_objects =
      "SELECT c.relname, c.relkind FROM pg_catalog.pg_class c JOIN "
      "pg_catalog.pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname "
      "= current_schema() ORDER BY 1";
  static const char *const pg_tables =
      "SELECT c.relname, c.relkind FROM pg_catalog.pg_class c JOIN "
      "pg_catalog.pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname "
      "= current_schema() AND c.relkind IN ('r', 'S') ORDER BY 1";
  clpd_db_result *r = NULL;
  size_t i;
  out[0] = '\0';
  put_rows(pg ? pg_objects : lite_objects, NULL, 0, out, cap);
  if (__real_clpd_db_exec(db, pg ? pg_tables : lite_tables, NULL, 0, &r) !=
      CLPD_DB_OK) {
    snprintf(out, cap, "error: %s", clpd_db_error(db));
    return;
  }
  for (i = 0; i < clpd_db_result_rows(r); i++) {
    const char *name = clpd_db_result_value(r, i, 0)->data;
    char query[256];
    if (pg && *(const char *)clpd_db_result_value(r, i, 1)->data == 'S')
      snprintf(query, sizeof query, "SELECT last_value, is_called FROM "
               "\"%s\"", name);
    else
      snprintf(query, sizeof query, "SELECT * FROM \"%s\" ORDER BY 1", name);
    put_rows(query, NULL, 0, out, cap);
  }
  clpd_db_result_free(r);
}

/* T9e (L87): the follow-ups of db-nextval. PostgreSQL: a relation that is
 * no sequence is a missing sequence (relkind 'S'). SQLite: a counter that
 * is no INTEGER is never advanced and has its own message; the maximum
 * value is an error and changes nothing. Both: the lookup after no value
 * can fail, and a value that is no INTEGER is reported. */
static void test_sequence_followups(void) {
  static const char *const prefix =
      "clpdaemon: db-nextval: sequence alias 'ticket_no': sequence "
      "'solo_ticket_no_seq'";
  char line[512], before[8192], after[8192];
  long long value = seq_state("solo_ticket_no_seq");
  EXPECT(value > 0);
  if (pg) {
    sql("DROP SEQUENCE solo_ticket_no_seq");
    sql("CREATE TABLE solo_ticket_no_seq (n BIGINT)");
    snprintf(line, sizeof line, "%s does not exist", prefix);
    EXPECT(fails_in("solo@1", "(db-nextval ticket_no)", line));
    EXPECT(rows("solo_ticket_no_seq") == 0);
    sql("DROP TABLE solo_ticket_no_seq");
    sql("CREATE SEQUENCE solo_ticket_no_seq");
    snprintf(line, sizeof line, "SELECT setval('solo_ticket_no_seq', %lld)",
             value);
    sql(line);
  } else {
    static const char *const counters[] = {"5.5", "'abc'", "x'00'"};
    static const char *const stored[] = {"real 5.5", "text abc", "blob ?"};
    size_t i;
    for (i = 0; i < 3; i++) {
      char update[256], got[256] = "", want[64];
      snprintf(update, sizeof update, "UPDATE clpd_sequence SET value = %s "
               "WHERE name = 'solo_ticket_no_seq'", counters[i]);
      sql(update);
      snprintf(line, sizeof line, "%s: the counter value is not an INTEGER",
               prefix);
      EXPECT(fails_in("solo@1", "(db-nextval ticket_no)", line));
      put_rows("SELECT typeof(value), CASE typeof(value) WHEN 'blob' THEN "
               "'?' ELSE value END FROM clpd_sequence WHERE name = "
               "'solo_ticket_no_seq'", NULL, 0, got, sizeof got);
      snprintf(want, sizeof want, "%s\n", stored[i]);
      EXPECT(same(counters[i], got, want));
    }
    /* The maximum value: an error, nothing changes. */
    sql("UPDATE clpd_sequence SET value = 9223372036854775807 WHERE name = "
        "'solo_ticket_no_seq'");
    fingerprint(before, sizeof before);
    snprintf(line, sizeof line, "%s has reached its maximum value", prefix);
    EXPECT(fails_in("solo@1", "(db-nextval ticket_no)", line));
    fingerprint(after, sizeof after);
    EXPECT(same("the database after the maximum value", after, before));
    EXPECT(seq_state("solo_ticket_no_seq") == INT64_MAX);
    snprintf(line, sizeof line, "UPDATE clpd_sequence SET value = %lld "
             "WHERE name = 'solo_ticket_no_seq'", value);
    sql(line);
  }
  /* No value, then the lookup fails: its message. */
  replace_sql = pg ? "nextval" : "UPDATE";
  replace_with = pg ? "SELECT $1::text WHERE 1 = 0" : "SELECT $1 WHERE 1 = 0";
  fail_sql = pg ? "pg_class" : "sqlite_master";
  snprintf(line, sizeof line, "%s: ", prefix);
  EXPECT(fails_db("solo@1", "(db-nextval ticket_no)", line));
  EXPECT(replace_sql == NULL);
  /* A value that is no INTEGER. */
  replace_sql = pg ? "nextval" : "UPDATE";
  replace_with = pg ? "SELECT $1::text" : "SELECT $1";
  snprintf(line, sizeof line, "%s: the database returned no INTEGER", prefix);
  EXPECT(fails_in("solo@1", "(db-nextval ticket_no)", line));
  EXPECT(replace_sql == NULL);
  EXPECT(seq_state("solo_ticket_no_seq") == value);
  EXPECT(int_in("solo@1", "(db-nextval ticket_no)", value + 1));
  /* SQLite: db-create-sequence creates the counter table, then the row; a
   * failed INSERT leaves the table, without the row (docs/orm.md). */
  if (!pg) {
    sql("ALTER TABLE clpd_sequence RENAME TO clpd_sequence_saved");
    fail_sql = "INSERT";
    EXPECT(fails_db("solo@1", "(db-create-sequence ticket_no)",
                    "clpdaemon: db-create-sequence: sequence alias "
                    "'ticket_no': sequence 'solo_ticket_no_seq': "));
    EXPECT(rows("clpd_sequence") == 0);
    snprintf(line, sizeof line, "%s does not exist", prefix);
    EXPECT(fails_in("solo@1", "(db-nextval ticket_no)", line));
    sql("DROP TABLE clpd_sequence");
    sql("ALTER TABLE clpd_sequence_saved RENAME TO clpd_sequence");
    EXPECT(seq_state("solo_ticket_no_seq") == value + 1);
  }
  EXPECT(!clpd_db_in_transaction(db));
}

/* T9e (L87, the verifier's sweep): odd qualified names from a rule of
 * another definition are refused by every function that changes the
 * database, which stays as it was. */
static void test_cross_definition_sweep(void) {
  /* mapped_by: NULL when no loaded package maps the name as a template,
   * else the packages the refusal names. */
  static const struct {
    const char *arg, *shown, *mapped_by;
  } names[] = {
      {"\"shop@1::order \"", "shop@1::order ", NULL},
      {"\"::order\"", "::order", NULL},
      {"\"shop@1::\"", "shop@1::", NULL},
      {"shop@1::no", "shop@1::no", NULL},
      {"shop@1::order", "shop@1::order", "shop@1"},
      {"order", "order", "shop@1, shop@2"},
  };
  /* table: the function takes a template, else a sequence alias. */
  static const struct {
    const char *name;
    int table;
  } udfs[] = {
      {"db-create-table", 1},
      {"db-drop-table", 1},
      {"db-create-sequence", 0},
      {"db-nextval", 0},
  };
  char before[8192], after[8192];
  size_t i, j;
  ddl.allow_drop = 1;
  EXPECT(ok_true_in("shop@1", "(db-create-table order)"));
  sql("INSERT INTO shop_order (wf_id, line_no, status, price) VALUES "
      "('w7', 7, 'kept', '7')");
  fingerprint(before, sizeof before);
  for (i = 0; i < sizeof names / sizeof names[0]; i++) {
    for (j = 0; j < sizeof udfs / sizeof udfs[0]; j++) {
      char expr[128], line[512];
      snprintf(expr, sizeof expr, "(%s %s)", udfs[j].name, names[i].arg);
      if (names[i].mapped_by != NULL && udfs[j].table)
        snprintf(line, sizeof line, "clpdaemon: %s: template '%s' is not "
                 "mapped by definition solo of the calling rule (mapped by "
                 "%s)", udfs[j].name, names[i].shown, names[i].mapped_by);
      else
        snprintf(line, sizeof line, "clpdaemon: %s: unknown %s '%s' (no "
                 "loaded package maps it)", udfs[j].name,
                 udfs[j].table ? "template" : "sequence alias",
                 names[i].shown);
      EXPECT(fails_in("solo@1", expr, line));
    }
  }
  fingerprint(after, sizeof after);
  EXPECT(same("the database after the sweep", after, before));
  sql("DROP TABLE shop_order");
  ddl.allow_drop = 0;
}

/* PostgreSQL (the verifier's cases): with search_path = s1, s2 the
 * functions act on s1 only, and s2's same-named table and sequence keep
 * their rows and value; a schema name with a double quote is quoted. */
static void test_schema_names(void) {
  static const char *const weird_rows =
      "SELECT count(*) FROM \"we\"\"ird\".\"solo_ticket\"";
  char s2_before[2048] = "", s2_after[2048] = "", got[256] = "";
  static const char *const s2_state =
      "SELECT (SELECT count(*) FROM s2.solo_ticket), (SELECT last_value FROM "
      "s2.solo_ticket_no_seq), (SELECT is_called FROM "
      "s2.solo_ticket_no_seq)";
  ddl.allow_drop = 1;
  sql("DROP SCHEMA IF EXISTS s1 CASCADE");
  sql("DROP SCHEMA IF EXISTS s2 CASCADE");
  sql("DROP SCHEMA IF EXISTS \"we\"\"ird\" CASCADE");
  sql("CREATE SCHEMA s1");
  sql("CREATE SCHEMA s2");
  sql("CREATE TABLE s2.solo_ticket (wf_id TEXT NOT NULL, title TEXT)");
  sql("INSERT INTO s2.solo_ticket VALUES ('w1', 'kept')");
  sql("CREATE SEQUENCE s2.solo_ticket_no_seq START 50");
  sql("SELECT nextval('s2.solo_ticket_no_seq')");
  put_rows(s2_state, NULL, 0, s2_before, sizeof s2_before);
  sql("SET search_path = s1, s2");
  EXPECT(ok_true_in("solo@1", "(db-drop-table ticket)"));
  EXPECT(ok_true_in("solo@1", "(db-create-table ticket)"));
  EXPECT(ok_true_in("solo@1", "(db-create-sequence ticket_no)"));
  EXPECT(int_in("solo@1", "(db-nextval ticket_no)", 1));
  EXPECT(ok_true_in("solo@1", "(db-drop-table ticket)"));
  EXPECT(ok_true_in("solo@1", "(db-create-table ticket)"));
  EXPECT(rows_in("s1", "solo_ticket") == 0);
  sql("RESET search_path");
  put_rows(s2_state, NULL, 0, s2_after, sizeof s2_after);
  EXPECT(same("s2 after the calls", s2_after, s2_before));
  EXPECT(same("s2 state", s2_before, "1 50 1\n"));
  sql("CREATE SCHEMA \"we\"\"ird\"");
  sql("SET search_path = \"we\"\"ird\"");
  EXPECT(ok_true_in("solo@1", "(db-create-table ticket)"));
  EXPECT(differences("(db-check-schema ticket)", ""));
  EXPECT(ok_true_in("solo@1", "(db-create-sequence ticket_no)"));
  EXPECT(int_in("solo@1", "(db-nextval ticket_no)", 1));
  EXPECT(int_in("solo@1", "(db-nextval ticket_no)", 2));
  put_rows(weird_rows, NULL, 0, got, sizeof got);
  EXPECT(same("we\"ird rows", got, "0\n"));
  EXPECT(ok_true_in("solo@1", "(db-drop-table ticket)"));
  got[0] = '\0';
  put_rows("SELECT count(*) FROM pg_catalog.pg_class c JOIN "
           "pg_catalog.pg_namespace n ON n.oid = c.relnamespace WHERE "
           "n.nspname = 'we\"ird' AND c.relname = 'solo_ticket'",
           NULL, 0, got, sizeof got);
  EXPECT(same("we\"ird table dropped", got, "0\n"));
  sql("RESET search_path");
  EXPECT(rows("solo_ticket") == -1);
  sql("DROP SCHEMA s1 CASCADE");
  sql("DROP SCHEMA s2 CASCADE");
  sql("DROP SCHEMA \"we\"\"ird\" CASCADE");
  ddl.allow_drop = 0;
}

/* PostgreSQL: the sequence is created in and read from current_schema(),
 * never a same-named sequence of a later search_path schema; without a
 * current schema both functions are errors. */
static void test_sequence_search_path(void) {
  static const char *const no_schema =
      "sequence alias 'ticket_no': sequence 'solo_ticket_no_seq': no current "
      "schema (the search_path names no existing schema)";
  char line[512];
  sql("DROP SCHEMA IF EXISTS seq_first CASCADE");
  sql("DROP SCHEMA IF EXISTS seq_second CASCADE");
  sql("CREATE SCHEMA seq_first");
  sql("CREATE SCHEMA seq_second");
  sql("CREATE SEQUENCE seq_second.solo_ticket_no_seq START 100");
  sql("SET search_path = seq_first, seq_second");
  EXPECT(fails_in("solo@1", "(db-nextval ticket_no)",
                  "clpdaemon: db-nextval: sequence alias 'ticket_no': "
                  "sequence 'solo_ticket_no_seq' does not exist"));
  EXPECT(ok_true_in("solo@1", "(db-create-sequence ticket_no)"));
  EXPECT(int_in("solo@1", "(db-nextval ticket_no)", 1));
  sql("SET search_path = seq_missing");
  snprintf(line, sizeof line, "clpdaemon: db-create-sequence: %s", no_schema);
  EXPECT(fails_in("solo@1", "(db-create-sequence ticket_no)", line));
  snprintf(line, sizeof line, "clpdaemon: db-nextval: %s", no_schema);
  EXPECT(fails_in("solo@1", "(db-nextval ticket_no)", line));
  sql("RESET search_path");
  EXPECT(rows_in("seq_second", "solo_ticket_no_seq") == 1);
  {
    clpd_db_result *r = NULL;
    EXPECT(clpd_db_exec(db, "SELECT is_called FROM "
                            "seq_second.solo_ticket_no_seq", NULL, 0, &r) ==
               CLPD_DB_OK &&
           clpd_db_result_value(r, 0, 0)->integer == 0);
    clpd_db_result_free(r);
  }
  sql("DROP SCHEMA seq_first CASCADE");
  sql("DROP SCHEMA seq_second CASCADE");
}

/* The log of Reset with the deffacts `construct` (one function call in a
 * slot value): no rule is executing while deffacts are evaluated. */
static void reset_log(const char *construct, char *log, size_t cap) {
  FILE *f = tmpfile();
  Deffacts *d;
  size_t n;
  if (f == NULL || Build(env, construct) != BE_NO_ERROR) exit(2);
  clpd_log_set(f);
  Reset(env);
  clpd_log_set(NULL);
  SetEvaluationError(env, false);
  SetHaltExecution(env, false);
  d = FindDeffacts(env, "shop@1::probe-facts");
  if (d == NULL || !Undeffacts(d, env)) exit(2);
  rewind(f);
  n = fread(log, 1, cap - 1, f);
  log[n] = '\0';
  fclose(f);
}

/* T9d review (L85): the functions that change the database refuse a
 * deffacts evaluated at Reset and an Eval after a rule has run, with the
 * exact line, and leave the table and the sequence as they are. */
static void test_refused_outside_rules(void) {
  static const char *const calls[][2] = {
      {"(db-drop-table order)", NO_RULE("db-drop-table")},
      {"(db-create-table solo@1::ticket)", NO_RULE("db-create-table")},
      {"(db-create-sequence legacy)", NO_RULE("db-create-sequence")},
      {"(db-nextval order_no)", NO_RULE("db-nextval")},
      {"(db-create-sequence solo@1::ticket_no)",
       NO_RULE("db-create-sequence")}};
  long long order_no, ticket_no;
  size_t i;
  ddl.allow_drop = 1;
  EXPECT(ok_true_in("shop@1", "(db-create-table order)"));
  sql("INSERT INTO shop_order (wf_id, line_no, status, price) VALUES "
      "('w9', 9, 'kept', '1')");
  if (pg) sql("DROP SEQUENCE solo_ticket_no_seq");
  else sql("DELETE FROM clpd_sequence WHERE name = 'solo_ticket_no_seq'");
  order_no = seq_state("shop_order_no_seq");
  EXPECT(order_no > 0 && rows("shop_order") == 1 &&
         rows("solo_ticket") == -1);
  if (Build(env, "(deftemplate shop@1::probe (slot v))") != BE_NO_ERROR)
    exit(2);
  for (i = 0; i < sizeof calls / sizeof calls[0]; i++) {
    char construct[512], log[2048], want[512];
    snprintf(construct, sizeof construct,
             "(deffacts shop@1::probe-facts (probe (v %s)))", calls[i][0]);
    reset_log(construct, log, sizeof log);
    snprintf(want, sizeof want, "%s\n", calls[i][1]);
    EXPECT(same(construct, log, want));
  }
  /* A rule of the package has run; none is executing now. */
  {
    call_result c;
    call_in("shop@1", "(db-check-schema order)", &c);
    EXPECT(c.ran && c.kind == 'M' && c.log[0] == '\0');
  }
  for (i = 0; i < sizeof calls / sizeof calls[0]; i++)
    EXPECT(fails(calls[i][0], calls[i][1]));
  EXPECT(rows("shop_order") == 1 && rows("solo_ticket") == -1);
  EXPECT(seq_state("shop_order_no_seq") == order_no);
  EXPECT(seq_state("solo_ticket_no_seq") == -1 &&
         seq_state("crm_legacy_seq") == (pg ? INT64_MAX : -1));
  ticket_no = 0;
  EXPECT(int_in("shop@1", "(db-nextval order_no)", order_no + 1));
  EXPECT(ok_true_in("solo@1", "(db-create-sequence ticket_no)"));
  EXPECT(int_in("solo@1", "(db-nextval ticket_no)", ticket_no + 1));
  ddl.allow_drop = 0;
}

/* SQLite: the linked library supports UPDATE ... RETURNING (3.35.0). */
static void test_sqlite_version(void) {
  clpd_db_result *r = NULL;
  int major = 0, minor = 0;
  EXPECT(clpd_db_exec(db, "SELECT sqlite_version()", NULL, 0, &r) ==
         CLPD_DB_OK);
  if (r == NULL) return;
  EXPECT(sscanf(clpd_db_result_value(r, 0, 0)->data, "%d.%d", &major,
                &minor) == 2 &&
         (major > 3 || (major == 3 && minor >= 35)));
  printf("sqlite %s\n", (const char *)clpd_db_result_value(r, 0, 0)->data);
  clpd_db_result_free(r);
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
  test_sequences();
  test_two_connections();
  test_sequence_db_errors();
  test_sequence_followups();
  test_cross_definition_sweep();
  if (pg) test_schema_names();
  if (pg) test_sequence_search_path();
  test_refused_outside_rules();
  if (!pg) test_sqlite_version();
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
