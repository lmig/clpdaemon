/* Tests for the ORM DDL functions db-create-table, db-check-schema and
 * db-drop-table (T9b, docs/orm.md#ddl-functions).
 * usage: orm_ddl_test                         (a temporary SQLite file)
 *        orm_ddl_test <conninfo> <password>   (PostgreSQL, run by
 *                                              tests/db/pg_cluster.sh)
 * The functions are called through Eval, as a rule would call them; the
 * generated schema is read back through the backend's catalog. */
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
  static char dirs[4][256];
  static int next;
  char *dir = dirs[next++ % 4];
  snprintf(dir, 256, "%s/%s", root, name);
  mkdir(dir, 0700);
  write_file(dir, "package.conf", manifest);
  return dir;
}

static const char *shop_clp =
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

/* The result of one call: kind 'T' (TRUE), 'F' (FALSE), 'M' (multifield,
 * items joined with '|'), '?' otherwise; log is what the call wrote to the
 * daemon log stream. */
typedef struct {
  char kind;
  size_t count;
  char items[4096];
  char log[4096];
  EvalError rc;
} call_result;

static void call(const char *expr, call_result *c) {
  CLIPSValue v;
  FILE *f = tmpfile();
  size_t n, i;
  if (f == NULL) exit(2);
  clpd_log_set(f);
  c->rc = Eval(env, expr, &v);
  clpd_log_set(NULL);
  c->kind = '?';
  c->count = 0;
  c->items[0] = '\0';
  if (v.header->type == SYMBOL_TYPE && v.lexemeValue == TrueSymbol(env)) {
    c->kind = 'T';
  } else if (v.header->type == SYMBOL_TYPE &&
             v.lexemeValue == FalseSymbol(env)) {
    c->kind = 'F';
  } else if (v.header->type == MULTIFIELD_TYPE) {
    c->kind = 'M';
    c->count = v.multifieldValue->length;
    for (i = 0; i < c->count; i++) {
      CLIPSValue *e = &v.multifieldValue->contents[i];
      size_t len = strlen(c->items);
      snprintf(c->items + len, sizeof c->items - len, "%s%s%s",
               i == 0 ? "" : "|",
               e->header->type == STRING_TYPE ? "" : "(not a string)",
               e->lexemeValue->contents);
    }
  }
  rewind(f);
  n = fread(c->log, 1, sizeof c->log - 1, f);
  c->log[n] = '\0';
  fclose(f);
}

static int same(const char *what, const char *got, const char *want) {
  if (strcmp(got, want) != 0) {
    fprintf(stderr, "  %s\n    got:  [%s]\n    want: [%s]\n", what, got, want);
    return 0;
  }
  return 1;
}

/* A call that returned TRUE and logged nothing. */
static int ok_true(const char *expr) {
  call_result c;
  call(expr, &c);
  if (c.kind != 'T' || c.log[0] != '\0' || c.rc != EE_NO_ERROR) {
    fprintf(stderr, "  %s: kind %c rc %d log [%s]\n", expr, c.kind, (int)c.rc,
            c.log);
    return 0;
  }
  return 1;
}

/* A call that failed: FALSE, an evaluation error, exactly `line` logged. */
static int fails(const char *expr, const char *line) {
  call_result c;
  char want[1024];
  call(expr, &c);
  snprintf(want, sizeof want, "%s\n", line);
  return c.kind == 'F' && c.rc == EE_PROCESSING_ERROR &&
         same(expr, c.log, want);
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

static void setup(const char *spec) {
  const char *dirs[3];
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
  write_file(dirs[0], "shop.clp", shop_clp);
  write_file(dirs[0], "orm.conf", shop_orm);
  write_file(dirs[1], "shop.clp", shop_clp);
  write_file(dirs[1], "orm.conf", shop_orm);
  write_file(dirs[2], "solo.clp",
             "(deftemplate ticket (slot wf-id) (slot title))\n"
             "(deftemplate plain (slot wf-id))\n");
  write_file(dirs[2], "orm.conf",
             "ticket.table = solo_ticket\nticket.owned = yes\n"
             "ticket.slot.wf-id = wf_id text string error\n"
             "ticket.slot.title = title text string nil\n");
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
    fprintf(stderr, "orm_ddl_test: packages did not load:\n%s", eb);
    exit(2);
  }
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
  EXPECT(ok_true("(db-create-table shop@1::order)"));
  EXPECT(schema_is("shop_order", want));
  EXPECT(differences("(db-check-schema shop@1::order)", ""));
  /* An existing owned table is left as it is, rows included. */
  sql("INSERT INTO shop_order (wf_id, line_no, status, price) VALUES "
      "('w1', 1, 'new', '9.50')");
  EXPECT(ok_true("(db-create-table shop@2::order)"));
  EXPECT(rows("shop_order") == 1);
  EXPECT(schema_is("shop_order", want));
  /* Only the local key is wf_id here. */
  EXPECT(ok_true("(db-create-table ticket)"));
  EXPECT(schema_is("solo_ticket", pg ? "wf_id text NOT NULL, title text | pk "
                                       "wf_id"
                                     : "wf_id TEXT NOT NULL, title TEXT | pk "
                                       "wf_id"));
  EXPECT(differences("(db-check-schema \"ticket\")", ""));
  EXPECT(!clpd_db_in_transaction(db));
}

/* Template names: <package>::<template>, or unqualified: the current
 * module's package first, then the one package mapping it. */
static void test_resolution(void) {
  EXPECT(fails("(db-create-table order)",
               "clpdaemon: db-create-table: template 'order' is mapped by "
               "shop@1, shop@2: qualify it as <package>::order"));
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
  EXPECT(fails("(db-create-table nothing)",
               "clpdaemon: db-create-table: unknown template 'nothing' (no "
               "loaded package maps it)"));
  EXPECT(fails("(db-check-schema plain)",
               "clpdaemon: db-check-schema: unknown template 'plain' (no "
               "loaded package maps it)"));
  EXPECT(fails("(db-drop-table nope@1::order)",
               "clpdaemon: db-drop-table: unknown template 'nope@1::order' "
               "(no loaded package maps it)"));
  EXPECT(fails("(db-create-table solo@1::order)",
               "clpdaemon: db-create-table: unknown template "
               "'solo@1::order' (no loaded package maps it)"));
}

/* A table that is not owned is never created; db-check-schema reports
 * the missing table, missing columns, incompatible types and NOT NULL
 * columns the mapping may write NULL into, and ignores extra columns. */
static void test_not_owned_and_check(void) {
  EXPECT(fails("(db-create-table shop@1::customer)",
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
  EXPECT(fails("(db-create-table shop@1::customer)",
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
  EXPECT(fails("(db-drop-table shop@1::order)",
               "clpdaemon: db-drop-table: dropping tables is disabled "
               "(orm_allow_drop = no)"));
  EXPECT(fails("(db-drop-table shop@1::customer)",
               "clpdaemon: db-drop-table: dropping tables is disabled "
               "(orm_allow_drop = no)"));
  EXPECT(rows("shop_order") == 1 && rows("crm_customer") == 1);
  ddl.allow_drop = 1;
  EXPECT(fails("(db-drop-table shop@1::customer)",
               "clpdaemon: db-drop-table: template 'shop@1::customer': "
               "table 'crm_customer' is not owned (owned = no), so it is "
               "never dropped"));
  EXPECT(rows("crm_customer") == 1);
  EXPECT(ok_true("(db-drop-table shop@1::order)"));
  EXPECT(rows("shop_order") == -1);
  EXPECT(differences("(db-check-schema shop@1::order)",
                     "table 'shop_order' does not exist"));
  EXPECT(ok_true("(db-drop-table shop@1::order)")); /* already gone */
  EXPECT(!clpd_db_in_transaction(db));
}

/* A database error is reported with the table, one line. */
static void test_db_error(void) {
  call_result c;
  const char *prefix =
      "clpdaemon: db-drop-table: template 'ticket': table 'solo_ticket': ";
  sql("DROP TABLE solo_ticket");
  sql("CREATE VIEW solo_ticket AS SELECT 'w' AS wf_id, 't' AS title");
  call("(db-drop-table ticket)", &c);
  EXPECT(c.kind == 'F' && c.rc == EE_PROCESSING_ERROR);
  EXPECT(strncmp(c.log, prefix, strlen(prefix)) == 0 &&
         strlen(c.log) > strlen(prefix) + 1 &&
         strchr(c.log, '\n') == c.log + strlen(c.log) - 1);
  if (strncmp(c.log, prefix, strlen(prefix)) != 0)
    fprintf(stderr, "  got [%s]\n", c.log);
  sql("DROP VIEW solo_ticket");
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
  setup(argc == 3 ? argv[1] : spec);
  test_create();
  test_resolution();
  test_not_owned_and_check();
  test_drop();
  test_db_error();
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
