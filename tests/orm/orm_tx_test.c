/* Tests for the cycle transaction (T10a, src/orm_tx.h,
 * docs/database.md#cycle-transaction) and the workflow_instance table.
 * usage: orm_tx_test                         (a temporary SQLite file)
 *        orm_tx_test <conninfo> <password>   (PostgreSQL, run by
 *                                             tests/db/pg_cluster.sh)
 * A cycle here is what the daemon's run_cycle does around Run: begin,
 * assert a go fact (standing in for the ingest drain), Run, end. The rules
 * of the test package call the ORM DDL and sequence functions, so a cycle
 * can create or drop a table and then fail on a real database error
 * (db-nextval of a sequence that does not exist). Failures the database
 * cannot be made to produce on demand (begin, commit, reconnect) are
 * injected through link seams (-Wl,--wrap, this test binary only). */
#define _POSIX_C_SOURCE 200809L

#include "db_backend.h"
#include "log.h"
#include "loop.h"
#include "orm_ddl.h"
#include "orm_tx.h"
#include "packages.h"

#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "expect.h"

static char root[] = "/tmp/clpd_orm_tx_test.XXXXXX";
static Environment *env;
static clpd_packages pkgs;
static clpd_orm_ddl ddl;
static clpd_db *db;
static int pg;
static char db_spec[512];
static const char *password;

/* Seams. begin_fail > 0: that many clpd_db_begin_atomic calls fail with
 * begin_status and "injected begin failure". commit_fail: the next commit
 * rolls back and fails with "injected commit failure". reconnect_fail:
 * that many reconnects fail with "cannot connect: injected". Every
 * reconnect is counted and its spec kept. */
static int begin_fail, begin_calls, commit_fail, reconnect_fail,
    reconnect_calls;
static clpd_db_status begin_status = CLPD_DB_ERROR;
static char reconnect_spec[512];

clpd_db_status __real_clpd_db_begin_atomic(clpd_db *d);
clpd_db_status __wrap_clpd_db_begin_atomic(clpd_db *d);
clpd_db_status __wrap_clpd_db_begin_atomic(clpd_db *d) {
  begin_calls++;
  if (begin_fail > 0) {
    begin_fail--;
    clpd_db_set_error(d, "injected begin failure");
    return begin_status;
  }
  return __real_clpd_db_begin_atomic(d);
}

clpd_db_status __real_clpd_db_commit(clpd_db *d);
clpd_db_status __wrap_clpd_db_commit(clpd_db *d);
clpd_db_status __wrap_clpd_db_commit(clpd_db *d) {
  if (commit_fail) {
    commit_fail = 0;
    clpd_db_rollback(d);
    clpd_db_set_error(d, "injected commit failure");
    return CLPD_DB_ERROR;
  }
  return __real_clpd_db_commit(d);
}

clpd_db_status __real_clpd_db_reconnect(clpd_db *d, const char *spec,
                                        char *msg, size_t n);
clpd_db_status __wrap_clpd_db_reconnect(clpd_db *d, const char *spec,
                                        char *msg, size_t n);
clpd_db_status __wrap_clpd_db_reconnect(clpd_db *d, const char *spec,
                                        char *msg, size_t n) {
  reconnect_calls++;
  snprintf(reconnect_spec, sizeof reconnect_spec, "%s", spec);
  if (reconnect_fail > 0) {
    reconnect_fail--;
    snprintf(msg, n, "cannot connect: injected");
    return CLPD_DB_ERROR;
  }
  return __real_clpd_db_reconnect(d, spec, msg, n);
}

static void write_file(const char *dir, const char *name, const char *text) {
  char path[512];
  FILE *f;
  snprintf(path, sizeof path, "%s/%s", dir, name);
  f = fopen(path, "w");
  if (f == NULL || fputs(text, f) < 0 || fclose(f) != 0) {
    perror("orm_tx_test");
    exit(2);
  }
}

static const char *shop_clp =
    "(deftemplate go (slot wf-id) (slot act (type SYMBOL)) (slot n))\n"
    "(deftemplate order (slot wf-id) (slot note))\n"
    "(defrule create (go (wf-id ?w) (act create)) => (db-create-table order))\n"
    "(defrule create-fail (go (wf-id ?w) (act create-fail)) =>\n"
    "  (db-create-table order) (db-nextval missing_no))\n"
    "(defrule drop-fail (go (wf-id ?w) (act drop-fail)) =>\n"
    "  (db-drop-table order) (db-nextval missing_no))\n"
    "(defrule halt (go (wf-id ?w) (act halt)) =>\n"
    "  (db-create-table order) (halt))\n"
    "(defrule eval-error (go (wf-id ?w) (act eval-error)) =>\n"
    "  (db-create-table order) (+ ?w 1))\n"
    "(defrule refused (go (wf-id ?w) (act refused)) =>\n"
    "  (db-create-table order) (db-drop-table order))\n"
    "(defrule nothing (go (wf-id ?w) (act nothing)) => (bind ?x 1))\n"
    "(defrule count (go (wf-id ?w) (act count)) =>\n"
    "  (bind ?n (db-nextval counter_no)))\n";
static const char *shop_orm =
    "order.table = shop_order\n"
    "order.owned = yes\n"
    "order.slot.wf-id = wf_id text string error\n"
    "order.slot.note = note text string nil\n"
    "seq.missing_no = shop_missing_seq\n"
    "seq.missing_no.owned = yes\n"
    "seq.counter_no = shop_counter_seq\n"
    "seq.counter_no.owned = yes\n";

static int same(const char *what, const char *got, const char *want) {
  if (strcmp(got, want) != 0) {
    fprintf(stderr, "  %s\n    got:  [%s]\n    want: [%s]\n", what, got, want);
    return 0;
  }
  return 1;
}

static void sql(const char *text) {
  if (clpd_db_exec(db, text, NULL, 0, NULL) != CLPD_DB_OK) {
    fprintf(stderr, "orm_tx_test: %s: %s\n", text, clpd_db_error(db));
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

/* The tables of the test schema with their rows, as one string: the
 * database state a rolled-back cycle must leave unchanged. */
static void fingerprint(char *out, size_t cap) {
  static const char *const pg_tables =
      "SELECT table_name FROM information_schema.tables WHERE table_schema "
      "= current_schema() ORDER BY table_name";
  static const char *const lite_tables =
      "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name";
  clpd_db_result *r = NULL, *rr;
  size_t i, j, len;
  char text[256];
  out[0] = '\0';
  if (clpd_db_exec(db, pg ? pg_tables : lite_tables, NULL, 0, &r) !=
      CLPD_DB_OK) {
    snprintf(out, cap, "error: %s", clpd_db_error(db));
    return;
  }
  for (i = 0; i < clpd_db_result_rows(r); i++) {
    const char *t = clpd_db_result_value(r, i, 0)->data;
    len = strlen(out);
    snprintf(out + len, cap - len, "%s:", t);
    snprintf(text, sizeof text, "SELECT * FROM \"%s\" ORDER BY 1", t);
    rr = NULL;
    if (clpd_db_exec(db, text, NULL, 0, &rr) != CLPD_DB_OK) continue;
    for (j = 0; j < clpd_db_result_rows(rr) * clpd_db_result_columns(rr);
         j++) {
      const clpd_db_value *v = clpd_db_result_value(
          rr, j / clpd_db_result_columns(rr), j % clpd_db_result_columns(rr));
      len = strlen(out);
      if (v->type == CLPD_DB_TEXT)
        snprintf(out + len, cap - len, " '%s'", (const char *)v->data);
      else if (v->type == CLPD_DB_INTEGER)
        snprintf(out + len, cap - len, " %lld", (long long)v->integer);
      else
        snprintf(out + len, cap - len, " ?");
    }
    clpd_db_result_free(rr);
    len = strlen(out);
    snprintf(out + len, cap - len, ";");
  }
  clpd_db_result_free(r);
}

/* "name type[ NOT NULL], ... | pk <columns>" of `table`. */
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

static clpd_orm_tx tx;

/* Clears what an ended Run leaves set, like the daemon's next Run does. */
static void clear_flags(void) {
  SetEvaluationError(env, false);
  SetHaltExecution(env, false);
  SetHaltRules(env, false);
}

/* One cycle: begin, a go fact with `act` (NULL: none), Run, end. Returns
 * -1 when begin failed (no Run), 0 committed, 1 rolled back; the daemon
 * log (the UDF lines and the tx line) goes to log. */
static int cycle(const char *act, char *log, size_t cap) {
  static long long serial; /* each go fact differs: never a duplicate */
  FILE *f = tmpfile();
  size_t n;
  int rc;
  if (f == NULL) exit(2);
  clpd_log_set(f);
  if (clpd_orm_tx_begin(&tx, f) != 0) {
    rc = -1;
  } else {
    if (act != NULL) {
      FactBuilder *fb;
      SetCurrentModule(env, pkgs.items[0].module);
      fb = CreateFactBuilder(env, "go");
      if (fb == NULL || FBPutSlotSymbol(fb, "wf-id", "w1") != PSE_NO_ERROR ||
          FBPutSlotSymbol(fb, "act", act) != PSE_NO_ERROR ||
          FBPutSlotInteger(fb, "n", ++serial) != PSE_NO_ERROR ||
          FBAssert(fb) == NULL)
        exit(2);
      FBDispose(fb);
      SetCurrentModule(env, FindDefmodule(env, "MAIN"));
    }
    clpd_packages_focus(env, &pkgs);
    Run(env, -1);
    clear_flags();
    rc = clpd_orm_tx_end(&tx, f) == 0 ? 0 : 1;
  }
  clpd_log_set(NULL);
  fflush(f);
  rewind(f);
  n = fread(log, 1, cap - 1, f);
  log[n] = '\0';
  fclose(f);
  if (password != NULL && strstr(log, password) != NULL) {
    fprintf(stderr, "  the log holds the password\n");
    failures++;
  }
  if (strstr(log, db_spec) != NULL) {
    fprintf(stderr, "  the log holds the database spec\n");
    failures++;
  }
  return rc;
}

/* The message of a db-nextval on the missing sequence, inside the cycle
 * transaction (the lookup after the failure cannot run there). */
static const char *missing_seq_msg(void) {
  return pg ? "relation \"public.shop_missing_seq\" does not exist (SQLSTATE "
              "42P01)"
            : "no such table: clpd_sequence";
}

/* The two log lines of a cycle rolled back by db-nextval missing_no. */
static void nextval_failure(char *out, size_t cap, int retry_ms) {
  snprintf(out, cap,
           "clpdaemon: db-nextval: sequence alias 'missing_no': sequence "
           "'shop_missing_seq': %s\n"
           "clpdaemon: cycle transaction failed: %s; retrying in %d ms\n",
           missing_seq_msg(), missing_seq_msg(), retry_ms);
}

static void reset_tables(void) {
  sql("DROP TABLE IF EXISTS shop_order");
}

static void setup(const char *spec) {
  char dir[sizeof root + 16], msg[512], eb[4096];
  const char *dirs[1];
  clpd_list list;
  FILE *err;
  size_t n;

  snprintf(dir, sizeof dir, "%s/shop", root);
  mkdir(dir, 0700);
  write_file(dir, "package.conf",
             "definition = shop\nversion = 1\nfiles = shop.clp\n"
             "orm = orm.conf\n");
  write_file(dir, "shop.clp", shop_clp);
  write_file(dir, "orm.conf", shop_orm);
  if (clpd_db_open(spec, &db, msg, sizeof msg) != CLPD_DB_OK) {
    fprintf(stderr, "orm_tx_test: cannot open the database: %s\n", msg);
    exit(2);
  }
  snprintf(db_spec, sizeof db_spec, "%s", spec);
  pg = strcmp(clpd_db_backend(db), "postgresql") == 0;
  reset_tables();
  sql("DROP TABLE IF EXISTS workflow_instance");
  if (pg) sql("DROP SEQUENCE IF EXISTS shop_missing_seq");
  if (pg) sql("DROP SEQUENCE IF EXISTS shop_counter_seq");
  else sql("DROP TABLE IF EXISTS clpd_sequence");
  env = CreateEnvironment();
  ddl.db = db;
  ddl.packages = &pkgs;
  ddl.allow_drop = 1;
  if (clpd_orm_ddl_register(env, &ddl) != 0) exit(2);
  dirs[0] = dir;
  list.items = (char **)dirs;
  list.count = 1;
  err = tmpfile();
  if (err == NULL) exit(2);
  if (clpd_packages_load_ex(env, &list, 1, &pkgs, err) != 0) {
    rewind(err);
    n = fread(eb, 1, sizeof eb - 1, err);
    eb[n] = '\0';
    fprintf(stderr, "orm_tx_test: packages did not load:\n%s", eb);
    exit(2);
  }
  fclose(err);
  Reset(env);
  clpd_orm_tx_init(&tx, db, db_spec, 100, 30000);
}

static void test_next_delay(void) {
  EXPECT(clpd_orm_tx_next_delay(0, 100, 30000) == 100);
  EXPECT(clpd_orm_tx_next_delay(100, 100, 30000) == 200);
  EXPECT(clpd_orm_tx_next_delay(16000, 100, 30000) == 30000);
  EXPECT(clpd_orm_tx_next_delay(30000, 100, 30000) == 30000);
  EXPECT(clpd_orm_tx_next_delay(0, 7, 7) == 7);
  EXPECT(clpd_orm_tx_next_delay(7, 7, 7) == 7);
  /* No overflow near INT_MAX. */
  EXPECT(clpd_orm_tx_next_delay(2000000000, 1, 2147483647) == 2147483647);
  EXPECT(clpd_orm_tx_next_delay(2147483647, 1, 2147483647) == 2147483647);
}

/* workflow_instance: created when missing (with the reason column of
 * T11a), kept (rows included) when it exists; on PostgreSQL in
 * current_schema(). */
static void test_instance_table(void) {
  char got[2048], msg[512];
  const char *want =
      pg ? "wf_id text NOT NULL, definition text NOT NULL, version text NOT "
           "NULL, status text NOT NULL, reason text, parent_wf_id text, "
           "owner_node text, "
           "lease_until timestamptz, created_at timestamptz NOT NULL, "
           "updated_at timestamptz NOT NULL | pk wf_id"
         : "wf_id TEXT NOT NULL, definition TEXT NOT NULL, version TEXT NOT "
           "NULL, status TEXT NOT NULL, reason TEXT, parent_wf_id TEXT, "
           "owner_node TEXT, "
           "lease_until TEXT, created_at TEXT NOT NULL, updated_at TEXT NOT "
           "NULL | pk wf_id";
  EXPECT(rows("workflow_instance") == -1);
  EXPECT(clpd_orm_tx_create_instance_table(db, msg, sizeof msg) == 0);
  EXPECT(same("msg", msg, ""));
  describe("workflow_instance", got, sizeof got);
  EXPECT(same("workflow_instance", got, want));
  sql("INSERT INTO workflow_instance (wf_id, definition, version, status, "
      "created_at, updated_at) VALUES ('w1', 'shop', '1', 'active', "
      "'2026-01-01 00:00:00+00', '2026-01-01 00:00:00+00')");
  EXPECT(clpd_orm_tx_create_instance_table(db, msg, sizeof msg) == 0);
  EXPECT(rows("workflow_instance") == 1);
  /* wf_id is the primary key. */
  EXPECT(clpd_db_exec(db,
                      "INSERT INTO workflow_instance (wf_id, definition, "
                      "version, status, created_at, updated_at) VALUES "
                      "('w1', 'shop', '1', 'active', '2026-01-02 00:00:00+00', "
                      "'2026-01-02 00:00:00+00')",
                      NULL, 0, NULL) == CLPD_DB_UNIQUE);
  sql("DELETE FROM workflow_instance");
  if (pg) {
    /* Created in current_schema(), not in a later search_path schema. */
    sql("DROP SCHEMA IF EXISTS clpd_s1 CASCADE");
    sql("DROP SCHEMA IF EXISTS clpd_s2 CASCADE");
    sql("CREATE SCHEMA clpd_s1");
    sql("CREATE SCHEMA clpd_s2");
    sql("CREATE TABLE clpd_s2.workflow_instance (x BIGINT)");
    sql("SET search_path = clpd_s1, clpd_s2");
    EXPECT(clpd_orm_tx_create_instance_table(db, msg, sizeof msg) == 0);
    describe("workflow_instance", got, sizeof got);
    EXPECT(same("clpd_s1.workflow_instance", got, want));
    sql("SET search_path = clpd_none");
    EXPECT(clpd_orm_tx_create_instance_table(db, msg, sizeof msg) == -1);
    EXPECT(same("no schema", msg,
                "no current schema (the search_path names no existing "
                "schema)"));
    sql("SET search_path = \"$user\", public");
    sql("DROP SCHEMA clpd_s1 CASCADE");
    sql("DROP SCHEMA clpd_s2 CASCADE");
  }
  /* A database error is returned as its one-line message. */
  begin_fail = 0;
  sql("DROP TABLE workflow_instance");
  sql("CREATE VIEW workflow_instance AS SELECT 1 AS x");
  {
    int rc = clpd_orm_tx_create_instance_table(db, msg, sizeof msg);
    /* PostgreSQL CREATE TABLE IF NOT EXISTS skips a view of that name
     * with a notice; SQLite too. Either way the call does not fail. */
    EXPECT(rc == 0);
  }
  sql("DROP VIEW workflow_instance");
  /* The CREATE itself fails (L91 R3-002): -1 with the database's one-line
   * message, nothing created. */
  sql(pg ? "SET default_transaction_read_only = on" : "PRAGMA query_only = ON");
  EXPECT(clpd_orm_tx_create_instance_table(db, msg, sizeof msg) == -1);
  EXPECT(same("create error", msg,
              pg ? "cannot execute CREATE TABLE in a read-only transaction "
                   "(SQLSTATE 25006)"
                 : "attempt to write a readonly database"));
  sql(pg ? "SET default_transaction_read_only = off" : "PRAGMA query_only = OFF");
  EXPECT(rows("workflow_instance") == -1);
  EXPECT(clpd_orm_tx_create_instance_table(db, msg, sizeof msg) == 0);
}

/* A committed cycle keeps its DDL, logs nothing and keeps the backoff at
 * 0; a cycle without a database does nothing either. */
static void test_commit(void) {
  char log[4096];
  clpd_orm_tx none;
  reset_tables();
  EXPECT(cycle("create", log, sizeof log) == 0);
  EXPECT(same("create", log, ""));
  EXPECT(rows("shop_order") == 0);
  EXPECT(tx.retry_ms == 0 && tx.reconnect == 0);
  EXPECT(clpd_db_in_transaction(db) == 0);
  clpd_orm_tx_init(&none, NULL, NULL, 100, 30000);
  EXPECT(clpd_orm_tx_begin(&none, stderr) == 0);
  EXPECT(clpd_orm_tx_end(&none, stderr) == 0);
  EXPECT(none.retry_ms == 0);
}

/* The first failing statement rolls the whole cycle back on both backends:
 * a table created earlier in the cycle is gone, a table dropped earlier in
 * the cycle is still there with its rows; the database is unchanged. */
static void test_rollback_undoes_ddl(void) {
  char log[4096], want[2048], before[8192], after[8192];
  reset_tables();
  tx.retry_ms = 0;
  fingerprint(before, sizeof before);
  EXPECT(cycle("create-fail", log, sizeof log) == 1);
  nextval_failure(want, sizeof want, 100);
  EXPECT(same("create-fail", log, want));
  EXPECT(rows("shop_order") == -1);
  fingerprint(after, sizeof after);
  EXPECT(same("database after create-fail", after, before));
  EXPECT(clpd_db_in_transaction(db) == 0);
  EXPECT(tx.retry_ms == 100);

  /* A committed cycle resets the backoff. */
  EXPECT(cycle("create", log, sizeof log) == 0);
  EXPECT(tx.retry_ms == 0);
  sql("INSERT INTO shop_order (wf_id, note) VALUES ('w1', 'kept')");
  fingerprint(before, sizeof before);
  EXPECT(cycle("drop-fail", log, sizeof log) == 1);
  nextval_failure(want, sizeof want, 100);
  EXPECT(same("drop-fail", log, want));
  EXPECT(rows("shop_order") == 1);
  fingerprint(after, sizeof after);
  EXPECT(same("database after drop-fail", after, before));
}

/* Consecutive failures double the wait up to the maximum; a commit resets
 * it to the initial value. */
static void test_backoff_sequence(void) {
  static const int waits[] = {100, 200, 400, 800, 1000, 1000};
  char log[4096], want[2048];
  size_t i;
  reset_tables();
  clpd_orm_tx_init(&tx, db, db_spec, 100, 1000);
  for (i = 0; i < sizeof waits / sizeof waits[0]; i++) {
    EXPECT(cycle("create-fail", log, sizeof log) == 1);
    nextval_failure(want, sizeof want, waits[i]);
    EXPECT(same("backoff", log, want));
    EXPECT(tx.retry_ms == waits[i]);
  }
  EXPECT(cycle("nothing", log, sizeof log) == 0);
  EXPECT(same("reset", log, ""));
  EXPECT(tx.retry_ms == 0);
  EXPECT(cycle("create-fail", log, sizeof log) == 1);
  nextval_failure(want, sizeof want, 100);
  EXPECT(same("after reset", log, want));
  EXPECT(rows("shop_order") == -1);
  clpd_orm_tx_init(&tx, db, db_spec, 100, 30000);
}

/* What is not a database failure commits: a rule's (halt), an evaluation
 * error that is no database status, a DDL function that refuses (no
 * statement ran). */
static void test_not_database_failures(void) {
  static const char *const acts[] = {"halt", "eval-error", "refused"};
  char log[4096];
  size_t i;
  for (i = 0; i < sizeof acts / sizeof acts[0]; i++) {
    reset_tables();
    ddl.allow_drop = 0;
    EXPECT(cycle(acts[i], log, sizeof log) == 0);
    EXPECT(rows("shop_order") == 0);
    EXPECT(tx.retry_ms == 0);
    EXPECT(strstr(log, "cycle transaction failed") == NULL);
    ddl.allow_drop = 1;
  }
  EXPECT(same("refused", log,
              "clpdaemon: db-drop-table: dropping tables is disabled "
              "(orm_allow_drop = no)\n"));
}

/* A begin that fails skips the cycle; a lost connection is reopened with
 * the same spec before the next begin, again after a failed reconnect. */
static void test_begin_and_reconnect(void) {
  char log[4096];
  int calls;
  reset_tables();
  begin_fail = 1;
  begin_status = CLPD_DB_ERROR;
  calls = begin_calls;
  EXPECT(cycle("create", log, sizeof log) == -1);
  EXPECT(same("begin error", log,
              "clpdaemon: cycle transaction failed: injected begin failure; "
              "retrying in 100 ms\n"));
  EXPECT(begin_calls == calls + 1 && tx.reconnect == 0);
  EXPECT(rows("shop_order") == -1); /* the cycle did not run */
  EXPECT(reconnect_calls == 0);

  begin_fail = 1;
  begin_status = CLPD_DB_LOST;
  EXPECT(cycle("create", log, sizeof log) == -1);
  EXPECT(same("begin lost", log,
              "clpdaemon: cycle transaction failed: injected begin failure; "
              "retrying in 200 ms\n"));
  EXPECT(tx.reconnect == 1 && reconnect_calls == 0);

  reconnect_fail = 1;
  EXPECT(cycle("create", log, sizeof log) == -1);
  EXPECT(same("reconnect fails", log,
              "clpdaemon: cycle transaction failed: cannot connect: "
              "injected; retrying in 400 ms\n"));
  EXPECT(reconnect_calls == 1 && tx.reconnect == 1);
  EXPECT(same("reconnect spec", reconnect_spec, db_spec));

  calls = begin_calls;
  EXPECT(cycle("create", log, sizeof log) == 0);
  EXPECT(same("reconnected", log, ""));
  EXPECT(reconnect_calls == 2 && tx.reconnect == 0 && tx.retry_ms == 0);
  EXPECT(begin_calls == calls + 1);
  EXPECT(rows("shop_order") == 0);
  /* No reconnect while the connection is fine. */
  EXPECT(cycle("nothing", log, sizeof log) == 0);
  EXPECT(reconnect_calls == 2);
}

/* A commit that fails rolls the cycle back. */
static void test_commit_failure(void) {
  char log[4096], before[8192], after[8192];
  reset_tables();
  fingerprint(before, sizeof before);
  commit_fail = 1;
  EXPECT(cycle("create", log, sizeof log) == 1);
  EXPECT(same("commit failure", log,
              "clpdaemon: cycle transaction failed: injected commit failure; "
              "retrying in 100 ms\n"));
  EXPECT(rows("shop_order") == -1);
  fingerprint(after, sizeof after);
  EXPECT(same("database after a failed commit", after, before));
  EXPECT(cycle("nothing", log, sizeof log) == 0);
  EXPECT(tx.retry_ms == 0);
}

/* The last value of the counter sequence (SQLite: its counter row). */
static long long counter(void) {
  clpd_db_result *r = NULL;
  long long v = -1;
  if (clpd_db_exec(db, pg ? "SELECT last_value FROM shop_counter_seq"
                          : "SELECT value FROM clpd_sequence WHERE name = "
                            "'shop_counter_seq'",
                   NULL, 0, &r) == CLPD_DB_OK &&
      clpd_db_result_rows(r) == 1)
    v = (long long)clpd_db_result_value(r, 0, 0)->integer;
  clpd_db_result_free(r);
  return v;
}

/* db-nextval in a rolled-back cycle: a PostgreSQL sequence is not
 * transactional, so the value stays used; the SQLite counter row is an
 * ordinary row, rolled back with the cycle (docs/orm.md). */
static void test_nextval_rolled_back(void) {
  char log[4096];
  if (pg) {
    sql("CREATE SEQUENCE shop_counter_seq");
    sql("SELECT nextval('shop_counter_seq')");
  } else {
    sql("CREATE TABLE IF NOT EXISTS clpd_sequence (\"name\" TEXT PRIMARY KEY, "
        "\"value\" INTEGER NOT NULL)");
    sql("INSERT INTO clpd_sequence VALUES ('shop_counter_seq', 1)");
  }
  EXPECT(counter() == 1);
  commit_fail = 1;
  EXPECT(cycle("count", log, sizeof log) == 1);
  EXPECT(same("count", log,
              "clpdaemon: cycle transaction failed: injected commit failure; "
              "retrying in 100 ms\n"));
  EXPECT(counter() == (pg ? 2 : 1));
  EXPECT(cycle("count", log, sizeof log) == 0);
  EXPECT(counter() == (pg ? 3 : 2));
  if (pg) sql("DROP SEQUENCE shop_counter_seq");
  else sql("DROP TABLE clpd_sequence");
}

/* Through the event loop, as the daemon schedules it: the retry waits are
 * the backoff (doubling, capped, reset after a commit). */
typedef struct {
  clpd_loop *loop;
  int attempts;
  long long at[16];
} timing;

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void timed_cycle(void *ctx) {
  timing *t = ctx;
  char log[4096];
  int rc;
  if (t->attempts < 16) t->at[t->attempts] = now_ms();
  t->attempts++;
  /* Attempts 1-4 fail at begin, 5 commits, 6 fails, 7 commits. */
  if (t->attempts <= 4 || t->attempts == 6) begin_fail = 1;
  begin_status = CLPD_DB_ERROR;
  rc = cycle("nothing", log, sizeof log);
  if (rc != 0) clpd_loop_defer(t->loop, tx.retry_ms);
  else clpd_loop_notify(t->loop); /* the next cycle at once */
  if (t->attempts == 7 || t->attempts >= 16) raise(SIGTERM);
}

static void test_loop_timing(void) {
  static const long long waits[] = {40, 80, 160, 160, 0, 40};
  timing t;
  size_t i;
  memset(&t, 0, sizeof t);
  t.loop = clpd_loop_open(60000, 0, stderr);
  EXPECT(t.loop != NULL);
  if (t.loop == NULL) return;
  clpd_orm_tx_init(&tx, db, db_spec, 40, 160);
  EXPECT(clpd_loop_run(t.loop, timed_cycle, &t) == SIGTERM);
  EXPECT(t.attempts == 7);
  for (i = 0; i < sizeof waits / sizeof waits[0] && i + 1 < (size_t)t.attempts;
       i++) {
    long long d = t.at[i + 1] - t.at[i];
    /* 1 ms of millisecond truncation below; generous above. */
    if (!(d >= waits[i] - 1 && d < waits[i] + 150)) {
      fprintf(stderr, "  wait %zu: %lld ms, want %lld\n", i, d, waits[i]);
      failures++;
    }
  }
  clpd_loop_close(t.loop);
  clpd_orm_tx_init(&tx, db, db_spec, 100, 30000);
}

/* SIGTERM during a long backoff stops the loop at once. */
static void stop_cb(void *ctx, int fd, short revents) {
  uint64_t n;
  (void)ctx;
  (void)revents;
  if (read(fd, &n, sizeof n) == (ssize_t)sizeof n) raise(SIGTERM);
}

static void failing_cycle(void *ctx) {
  timing *t = ctx;
  char log[4096];
  t->attempts++;
  begin_fail = 1;
  if (cycle("nothing", log, sizeof log) != 0)
    clpd_loop_defer(t->loop, tx.retry_ms);
  if (t->attempts >= 16) raise(SIGTERM);
}

static void test_sigterm_during_backoff(void) {
  struct itimerspec its = {{0, 0}, {0, 100 * 1000000L}};
  timing t;
  long long t0;
  int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  memset(&t, 0, sizeof t);
  t.loop = clpd_loop_open(60000, 0, stderr);
  EXPECT(t.loop != NULL && fd >= 0);
  if (t.loop == NULL || fd < 0) return;
  clpd_orm_tx_init(&tx, db, db_spec, 10000, 10000);
  EXPECT(clpd_loop_add_fd(t.loop, fd, POLLIN, stop_cb, NULL) == 0);
  EXPECT(timerfd_settime(fd, 0, &its, NULL) == 0);
  t0 = now_ms();
  EXPECT(clpd_loop_run(t.loop, failing_cycle, &t) == SIGTERM);
  EXPECT(t.attempts == 1);
  EXPECT(now_ms() - t0 < 2000);
  EXPECT(tx.retry_ms == 10000);
  begin_fail = 0;
  clpd_loop_close(t.loop);
  close(fd);
  clpd_orm_tx_init(&tx, db, db_spec, 100, 30000);
}

int main(int argc, char **argv) {
  char spec[sizeof root + 32];
  if (argc != 1 && argc != 3) {
    fprintf(stderr, "usage: orm_tx_test [<conninfo> <password>]\n");
    return 2;
  }
  if (mkdtemp(root) == NULL) {
    perror("orm_tx_test");
    return 2;
  }
  snprintf(spec, sizeof spec, "sqlite:%s/tx.db", root);
  if (argc == 3) password = argv[2];
  /* The loop's signal handling, as in the daemon. */
  if (clpd_loop_block_signals() != 0) return 2;
  setup(argc == 3 ? argv[1] : spec);
  test_next_delay();
  test_instance_table();
  test_commit();
  test_rollback_undoes_ddl();
  test_backoff_sequence();
  test_not_database_failures();
  test_begin_and_reconnect();
  test_commit_failure();
  test_nextval_rolled_back();
  test_loop_timing();
  test_sigterm_during_backoff();
  reset_tables();
  sql("DROP TABLE IF EXISTS workflow_instance");
  clpd_packages_free(&pkgs);
  DestroyEnvironment(env);
  clpd_db_close(db);
  if (failures != 0) {
    fprintf(stderr, "%d orm tx checks failed (fixtures in %s)\n", failures,
            root);
    return 1;
  }
  {
    char cmd[sizeof root + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0) fprintf(stderr, "cannot remove %s\n", root);
  }
  puts("orm tx checks passed");
  return 0;
}
