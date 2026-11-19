/* Tests for the automatic synchronization (T10b, src/orm_sync.h,
 * docs/orm.md#automatic-synchronization).
 * usage: orm_sync_test                         (a temporary SQLite file)
 *        orm_sync_test <conninfo> <password>   (PostgreSQL, run by
 *                                               tests/db/pg_cluster.sh)
 * A real CLIPS environment loads two packages (shop@1 and crm@2, two
 * aggregates); a cycle is what the daemon's run_cycle does: begin, the
 * changes (top-level Eval standing in for the ingest drain or a plugin),
 * Run, then clpd_orm_sync_end_cycle (the flush and the commit). The rows
 * are read back with SELECT after each cycle. clpd_db_exec is wrapped
 * (-Wl,--wrap, this test binary only) to record the statements the sync
 * sends and to make one of them fail on demand. */
#define _POSIX_C_SOURCE 200809L

#include "db_backend.h"
#include "log.h"
#include "orm_ddl.h"
#include "orm_sync.h"
#include "orm_tx.h"
#include "packages.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "expect.h"

static char root[] = "/tmp/clpd_orm_sync_test.XXXXXX";
static Environment *env;
static clpd_packages pkgs;
static clpd_orm_ddl ddl;
static clpd_orm_syncer *syncer;
static clpd_orm_tx tx;
static clpd_db *db;
static int pg;
static char db_spec[512];
static const char *password;

enum { SHOP, CRM }; /* package indexes, in load order */

/* Seam: while recording, every statement but SELECT current_schema() is
 * appended to stmts, one per line; the fail_at-th of them (1-based) is
 * replaced by a statement that fails on the database. */
static int recording, fail_at, seen;
static char stmts[16384];

clpd_db_status __real_clpd_db_exec(clpd_db *d, const char *sql,
                                   const clpd_db_value *params, size_t n,
                                   clpd_db_result **result);
clpd_db_status __wrap_clpd_db_exec(clpd_db *d, const char *sql,
                                   const clpd_db_value *params, size_t n,
                                   clpd_db_result **result);
clpd_db_status __wrap_clpd_db_exec(clpd_db *d, const char *sql,
                                   const clpd_db_value *params, size_t n,
                                   clpd_db_result **result) {
  if (recording && strcmp(sql, "SELECT current_schema()") != 0) {
    size_t len = strlen(stmts);
    snprintf(stmts + len, sizeof stmts - len, "%s\n", sql);
    if (++seen == fail_at)
      return __real_clpd_db_exec(d, "SELECT * FROM clpd_injected_failure",
                                 NULL, 0, result);
  }
  return __real_clpd_db_exec(d, sql, params, n, result);
}

static void write_file(const char *dir, const char *name, const char *text) {
  char path[512];
  FILE *f;
  snprintf(path, sizeof path, "%s/%s", dir, name);
  f = fopen(path, "w");
  if (f == NULL || fputs(text, f) < 0 || fclose(f) != 0) {
    perror("orm_sync_test");
    exit(2);
  }
}

static const char *shop_clp =
    "(deftemplate go (slot wf-id) (slot act (type SYMBOL)))\n"
    "(deftemplate order (slot wf-id) (slot line-no (type INTEGER))\n"
    "  (slot status (type SYMBOL)) (slot amount) (slot note)\n"
    "  (slot paid (type INTEGER)) (slot data (type STRING)))\n"
    "(deftemplate note (slot wf-id) (slot text))\n"
    "(deftemplate small (slot wf-id) (slot n))\n"
    "(deftemplate tmp (slot wf-id) (slot v))\n"
    "(deffacts seed (order (wf-id w0) (line-no 1) (status new)))\n"
    "(defrule fail (go (wf-id ?w) (act fail)) =>\n"
    "  (assert (order (wf-id ?w) (line-no 99))) (db-nextval missing_no))\n";
static const char *shop_orm =
    "order.table = shop_order\n"
    "order.owned = yes\n"
    "order.key = line-no\n"
    "order.slot.wf-id = wf_id text string error\n"
    "order.slot.line-no = line_no bigint integer error\n"
    "order.slot.status = status text symbol default\n"
    "order.slot.amount = amount double float nil\n"
    "order.slot.note = note text string nil\n"
    "order.slot.paid = paid boolean integer default\n"
    "order.slot.data = data bytea string default\n"
    "note.table = shop_note\n"
    "note.owned = yes\n"
    "note.sync = manual\n"
    "note.slot.wf-id = wf_id text string error\n"
    "note.slot.text = body text string nil\n"
    "small.table = shop_small\n"
    "small.owned = no\n"
    "small.slot.wf-id = wf_id text string error\n"
    "small.slot.n = n bigint integer nil\n"
    "seq.missing_no = shop_missing_seq\n"
    "seq.missing_no.owned = yes\n";
static const char *crm_clp = "(deftemplate party (slot wf-id) (slot name))\n";
static const char *crm_orm =
    "party.table = crm_party\n"
    "party.owned = yes\n"
    "party.slot.wf-id = wf_id text string error\n"
    "party.slot.name = name text string nil\n";

static int same(const char *what, const char *got, const char *want) {
  if (strcmp(got, want) != 0) {
    fprintf(stderr, "  %s\n    got:  [%s]\n    want: [%s]\n", what, got, want);
    return 0;
  }
  return 1;
}

static void sql(const char *text) {
  if (clpd_db_exec(db, text, NULL, 0, NULL) != CLPD_DB_OK) {
    fprintf(stderr, "orm_sync_test: %s: %s\n", text, clpd_db_error(db));
    exit(2);
  }
}

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

/* The rows of a query, "v v v; v v v" (values typed as put_value). */
static void query(const char *text, char *out, size_t cap) {
  clpd_db_result *r = NULL;
  size_t i, j, len;
  out[0] = '\0';
  if (clpd_db_exec(db, text, NULL, 0, &r) != CLPD_DB_OK) {
    snprintf(out, cap, "error: %s", clpd_db_error(db));
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
}

static void rows_of(const char *table, char *out, size_t cap) {
  char text[256];
  snprintf(text, sizeof text, "SELECT * FROM \"%s\" ORDER BY 1, 2", table);
  query(text, out, cap);
}

/* workflow_instance without the timestamps. */
static void instances(char *out, size_t cap) {
  query("SELECT wf_id, definition, version, status, parent_wf_id, "
        "owner_node, lease_until FROM workflow_instance ORDER BY 1",
        out, cap);
}

static const char *const tables[] = {"shop_order", "shop_note", "shop_small",
                                     "crm_party", "workflow_instance"};

/* Every table of the test with its rows (timestamps included): what a
 * rolled-back cycle must leave unchanged. */
static void fingerprint(char *out, size_t cap) {
  char one[8192];
  size_t i, len;
  out[0] = '\0';
  for (i = 0; i < sizeof tables / sizeof tables[0]; i++) {
    rows_of(tables[i], one, sizeof one);
    len = strlen(out);
    snprintf(out + len, cap - len, "%s: %s\n", tables[i], one);
  }
}

/* The single text value of a one-row query ("" when none). */
static void scalar(const char *text, const clpd_db_value *params, size_t n,
                   char *out, size_t cap) {
  clpd_db_result *r = NULL;
  const clpd_db_value *v;
  out[0] = '\0';
  if (clpd_db_exec(db, text, params, n, &r) == CLPD_DB_OK &&
      clpd_db_result_rows(r) == 1) {
    v = clpd_db_result_value(r, 0, 0);
    put_value(out, cap, v);
  }
  clpd_db_result_free(r);
}

static void updated_at(const char *wf, char *out, size_t cap) {
  clpd_db_value p = clpd_db_text(wf);
  scalar(pg ? "SELECT updated_at::text FROM workflow_instance WHERE wf_id = $1"
            : "SELECT updated_at FROM workflow_instance WHERE wf_id = $1",
         &p, 1, out, cap);
}

/* 1 when the instance's timestamps have the documented form: SQLite TEXT
 * YYYY-MM-DDTHH:MM:SS.mmmZ (UTC), PostgreSQL TIMESTAMPTZ; recent, and
 * created_at <= updated_at (equal when `first`). */
static int times_ok(const char *wf, int first) {
  static const char *const pg_check =
      "SELECT count(*) FROM workflow_instance WHERE wf_id = $1 AND "
      "created_at <= updated_at AND updated_at > now() - interval '1 hour' "
      "AND updated_at < now() + interval '1 minute'";
  static const char *const lite_check =
      "SELECT count(*) FROM workflow_instance WHERE wf_id = $1 AND "
      "created_at <= updated_at AND updated_at GLOB "
      "'[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:"
      "[0-9][0-9].[0-9][0-9][0-9]Z' AND created_at GLOB "
      "'[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:"
      "[0-9][0-9].[0-9][0-9][0-9]Z' AND updated_at > "
      "strftime('%Y-%m-%dT%H:%M:%fZ', 'now', '-1 hour') AND updated_at < "
      "strftime('%Y-%m-%dT%H:%M:%fZ', 'now', '+1 minute')";
  clpd_db_value p = clpd_db_text(wf);
  char got[64];
  scalar(pg ? pg_check : lite_check, &p, 1, got, sizeof got);
  if (strcmp(got, "1") != 0) return 0;
  if (first) {
    scalar("SELECT count(*) FROM workflow_instance WHERE wf_id = $1 AND "
           "created_at = updated_at",
           &p, 1, got, sizeof got);
    return strcmp(got, "1") == 0;
  }
  return 1;
}

/* The expected statements: every '@' becomes the table qualifier
 * ("public". on PostgreSQL, nothing on SQLite) and every '~' the
 * database clock of the instance row. */
static const char *expect_sql(const char *fmt) {
  static char out[16384];
  const char *now = pg ? "now()" : "strftime('%Y-%m-%dT%H:%M:%fZ', 'now')";
  size_t len = 0;
  for (; *fmt != '\0' && len + 64 < sizeof out; fmt++) {
    if (*fmt == '@') {
      if (pg) len += (size_t)snprintf(out + len, sizeof out - len, "\"public\".");
    } else if (*fmt == '~') {
      len += (size_t)snprintf(out + len, sizeof out - len, "%s", now);
    } else {
      out[len++] = *fmt;
    }
  }
  out[len] = '\0';
  return out;
}

#define INS_ORDER                                                          \
  "INSERT INTO @\"shop_order\" (\"wf_id\", \"line_no\", \"status\", "     \
  "\"amount\", \"note\", \"paid\", \"data\") VALUES ($1, $2, $3, $4, $5, " \
  "$6, $7)\n"
#define DEL_ORDER                                                          \
  "DELETE FROM @\"shop_order\" WHERE \"wf_id\" = $1 AND \"line_no\" = "   \
  "$2\n"
#define INS_PARTY                                                          \
  "INSERT INTO @\"crm_party\" (\"wf_id\", \"name\") VALUES ($1, $2)\n"
#define INSTANCE                                                           \
  "INSERT INTO @\"workflow_instance\" (\"wf_id\", \"definition\", "       \
  "\"version\", \"status\", \"created_at\", \"updated_at\") VALUES ($1, "  \
  "$2, $3, 'active', ~, ~) ON CONFLICT (\"wf_id\") DO UPDATE SET "         \
  "\"updated_at\" = excluded.\"updated_at\"\n"

/* --- cycles ---------------------------------------------------------- */

static FILE *cycle_log;

static void clear_flags(void) {
  SetEvaluationError(env, false);
  SetHaltExecution(env, false);
  SetHaltRules(env, false);
}

static void begin_cycle(void) {
  cycle_log = tmpfile();
  if (cycle_log == NULL) exit(2);
  clpd_log_set(cycle_log);
  stmts[0] = '\0';
  seen = 0;
  recording = 1;
  if (clpd_orm_tx_begin(&tx, cycle_log) != 0) {
    fprintf(stderr, "orm_sync_test: the cycle transaction did not begin\n");
    exit(2);
  }
}

/* Evaluates one expression in the module of package pkg (outside a
 * cycle too). */
static void act(size_t pkg, const char *expr) {
  CLIPSValue v;
  SetCurrentModule(env, pkgs.items[pkg].module);
  if (Eval(env, expr, &v) != EE_NO_ERROR) {
    fprintf(stderr, "  cannot evaluate %s\n", expr);
    failures++;
  }
  SetCurrentModule(env, FindDefmodule(env, "MAIN"));
  clear_flags();
}

/* Run, flush and commit. Returns 0 committed, 1 rolled back; the daemon
 * log of the cycle goes to log. */
static int end_cycle(char *log, size_t cap) {
  size_t n;
  int rc;
  clpd_packages_focus(env, &pkgs);
  Run(env, -1);
  clear_flags();
  rc = clpd_orm_sync_end_cycle(syncer, &tx, cycle_log) == 0 ? 0 : 1;
  recording = 0;
  fail_at = 0;
  clpd_log_set(NULL);
  fflush(cycle_log);
  rewind(cycle_log);
  n = fread(log, 1, cap - 1, cycle_log);
  log[n] = '\0';
  fclose(cycle_log);
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

/* A cycle of shop with one expression (NULL: no change). */
static int cycle(const char *expr, char *log, size_t cap) {
  begin_cycle();
  if (expr != NULL) act(SHOP, expr);
  return end_cycle(log, cap);
}

/* The two log lines of a write the database refused. */
static void refused(char *out, size_t cap, const char *what,
                    const char *message) {
  snprintf(out, cap,
           "clpdaemon: sync: %s: %s\n"
           "clpdaemon: cycle transaction failed: %s; retrying in 100 ms\n",
           what, message, message);
}

/* --- setup ------------------------------------------------------------ */

static void package(const char *name, const char *conf, const char *clp,
                    const char *orm, char *dir, size_t cap) {
  snprintf(dir, cap, "%s/%s", root, name);
  mkdir(dir, 0700);
  write_file(dir, "package.conf", conf);
  write_file(dir, "p.clp", clp);
  write_file(dir, "orm.conf", orm);
}

static void create_tables(void) {
  char msg[512];
  sql("DROP TABLE IF EXISTS shop_order");
  sql("DROP TABLE IF EXISTS shop_note");
  sql("DROP TABLE IF EXISTS shop_small");
  sql("DROP TABLE IF EXISTS crm_party");
  sql("DROP TABLE IF EXISTS workflow_instance");
  sql(pg ? "CREATE TABLE shop_order (wf_id TEXT NOT NULL, line_no BIGINT NOT "
           "NULL, status TEXT, amount DOUBLE PRECISION, note TEXT, paid "
           "BOOLEAN, data BYTEA, PRIMARY KEY (wf_id, line_no))"
         : "CREATE TABLE shop_order (wf_id TEXT NOT NULL, line_no INTEGER NOT "
           "NULL, status TEXT, amount REAL, note TEXT, paid INTEGER, data "
           "BLOB, PRIMARY KEY (wf_id, line_no))");
  sql("CREATE TABLE shop_note (wf_id TEXT NOT NULL PRIMARY KEY, body TEXT)");
  /* INTEGER: int4 on PostgreSQL, 64 bits on SQLite. */
  sql("CREATE TABLE shop_small (wf_id TEXT NOT NULL PRIMARY KEY, n INTEGER)");
  sql("CREATE TABLE crm_party (wf_id TEXT NOT NULL PRIMARY KEY, name TEXT)");
  if (clpd_orm_tx_create_instance_table(db, msg, sizeof msg) != 0) {
    fprintf(stderr, "orm_sync_test: workflow_instance: %s\n", msg);
    exit(2);
  }
}

static void setup(const char *spec) {
  char shop[sizeof root + 16], crm[sizeof root + 16], msg[512], eb[4096];
  const char *dirs[2];
  clpd_list list;
  FILE *err;
  size_t n;

  package("shop", "definition = shop\nversion = 1\nfiles = p.clp\n"
                  "orm = orm.conf\n",
          shop_clp, shop_orm, shop, sizeof shop);
  package("crm", "definition = crm\nversion = 2\nfiles = p.clp\n"
                 "orm = orm.conf\n",
          crm_clp, crm_orm, crm, sizeof crm);
  if (clpd_db_open(spec, &db, msg, sizeof msg) != CLPD_DB_OK) {
    fprintf(stderr, "orm_sync_test: cannot open the database: %s\n", msg);
    exit(2);
  }
  snprintf(db_spec, sizeof db_spec, "%s", spec);
  pg = strcmp(clpd_db_backend(db), "postgresql") == 0;
  create_tables();
  if (pg) sql("DROP SEQUENCE IF EXISTS shop_missing_seq");
  else sql("DROP TABLE IF EXISTS clpd_sequence");
  env = CreateEnvironment();
  ddl.db = db;
  ddl.packages = &pkgs;
  if (clpd_orm_ddl_register(env, &ddl) != 0) exit(2);
  dirs[0] = shop;
  dirs[1] = crm;
  list.items = (char **)dirs;
  list.count = 2;
  err = tmpfile();
  if (err == NULL) exit(2);
  if (clpd_packages_load_ex(env, &list, 1, &pkgs, err) != 0) {
    rewind(err);
    n = fread(eb, 1, sizeof eb - 1, err);
    eb[n] = '\0';
    fprintf(stderr, "orm_sync_test: packages did not load:\n%s", eb);
    exit(2);
  }
  fclose(err);
  if (clpd_orm_sync_attach(env, db, &pkgs, &syncer) != 0) {
    fprintf(stderr, "orm_sync_test: cannot attach the sync\n");
    exit(2);
  }
  /* After the attach, as in the daemon: the deffacts facts are changes
   * since the last commit (there was none). */
  Reset(env);
  clpd_orm_tx_init(&tx, db, db_spec, 100, 30000);
}

/* --- tests ------------------------------------------------------------ */

/* The facts Reset asserts before the first cycle are written by it, with
 * the workflow_instance row of their instance. */
static void test_first_cycle(void) {
  char log[4096], got[8192];
  EXPECT(cycle(NULL, log, sizeof log) == 0);
  EXPECT(same("first cycle log", log, ""));
  EXPECT(same("first cycle statements", stmts, expect_sql(INS_ORDER INSTANCE)));
  rows_of("shop_order", got, sizeof got);
  EXPECT(same("seed row", got, "'w0' 1 'new' NULL NULL 0 x''"));
  instances(got, sizeof got);
  EXPECT(same("seed instance", got, "'w0' 'shop' '1' 'active' NULL NULL NULL"));
  EXPECT(times_ok("w0", 1));
  /* Nothing changed: nothing is written. */
  EXPECT(cycle(NULL, log, sizeof log) == 0);
  EXPECT(same("no change", stmts, ""));
}

/* Values per the declared CLIPS type and null policy: nil in a null nil
 * column is NULL, nil elsewhere the text 'nil'; a symbol or a string in a
 * text column is text; a string in a bytea column is its bytes; 0 and 1
 * in a boolean column are false and true. */
static void test_types(void) {
  char log[4096], got[8192];
  EXPECT(cycle("(progn (assert (order (wf-id w1) (line-no 2) (status open) "
               "(amount 1.5) (note \"a b\") (paid 1) (data \"AB\"))) "
               "(assert (order (wf-id w1) (line-no 3) (note sym))))",
               log, sizeof log) == 0);
  EXPECT(same("types log", log, ""));
  EXPECT(same("types statements", stmts,
              expect_sql(INS_ORDER INS_ORDER INSTANCE)));
  rows_of("shop_order", got, sizeof got);
  EXPECT(same("typed rows", got,
              "'w0' 1 'new' NULL NULL 0 x''; "
              "'w1' 2 'open' 1.5 'a b' 1 x'4142'; "
              "'w1' 3 'nil' NULL 'sym' 0 x''"));
  instances(got, sizeof got);
  EXPECT(same("two instances", got,
              "'w0' 'shop' '1' 'active' NULL NULL NULL; "
              "'w1' 'shop' '1' 'active' NULL NULL NULL"));
  EXPECT(times_ok("w1", 1));
}

/* A fact asserted and modified in one cycle: one INSERT with the final
 * values. Asserted and retracted: nothing at all. */
static void test_net_changes(void) {
  char log[4096], got[8192], before[16384], after[16384];
  EXPECT(cycle("(progn (bind ?f (assert (order (wf-id w1) (line-no 4) "
               "(status a)))) (bind ?f (modify ?f (status b))) "
               "(modify ?f (status c) (amount 2.0)))",
               log, sizeof log) == 0);
  EXPECT(same("assert+modify", stmts, expect_sql(INS_ORDER INSTANCE)));
  query("SELECT status, amount FROM shop_order WHERE line_no = 4", got,
        sizeof got);
  EXPECT(same("final values", got, "'c' 2"));
  fingerprint(before, sizeof before);
  EXPECT(cycle("(progn (bind ?f (assert (order (wf-id w1) (line-no 5)))) "
               "(retract ?f))",
               log, sizeof log) == 0);
  EXPECT(same("assert+retract", stmts, ""));
  /* Modified there and back: no change. */
  EXPECT(cycle("(do-for-fact ((?f order)) (= ?f:line-no 4) "
               "(bind ?g (modify ?f (status z))) (modify ?g (status c)))",
               log, sizeof log) == 0);
  EXPECT(same("there and back", stmts, ""));
  fingerprint(after, sizeof after);
  EXPECT(same("nothing written", after, before));
}

/* Several modifies of a stored fact: one UPDATE of the changed columns by
 * the primary key; the instance's updated_at moves, created_at stays. */
static void test_update(void) {
  char log[4096], got[8192], t0[128], t1[128], c0[128], c1[128];
  clpd_db_value p = clpd_db_text("w1");
  updated_at("w1", t0, sizeof t0);
  scalar("SELECT created_at FROM workflow_instance WHERE wf_id = $1", &p, 1,
         c0, sizeof c0);
  { struct timespec ts = {0, 20 * 1000000L}; nanosleep(&ts, NULL); }
  EXPECT(cycle("(do-for-fact ((?f order)) (= ?f:line-no 2) "
               "(bind ?g (modify ?f (note \"x\"))) "
               "(modify ?g (amount 2.5) (paid 0)))",
               log, sizeof log) == 0);
  EXPECT(same("update log", log, ""));
  EXPECT(same("update", stmts,
              expect_sql("UPDATE @\"shop_order\" SET \"amount\" = $1, "
                         "\"note\" = $2, \"paid\" = $3 WHERE \"wf_id\" = $4 "
                         "AND \"line_no\" = $5\n" INSTANCE)));
  query("SELECT * FROM shop_order WHERE line_no = 2", got, sizeof got);
  EXPECT(same("updated row", got, "'w1' 2 'open' 2.5 'x' 0 x'4142'"));
  updated_at("w1", t1, sizeof t1);
  scalar("SELECT created_at FROM workflow_instance WHERE wf_id = $1", &p, 1,
         c1, sizeof c1);
  EXPECT(strcmp(t1, t0) > 0);
  EXPECT(same("created_at kept", c1, c0));
  EXPECT(times_ok("w1", 0));
}

/* A modify of a key slot is a DELETE of the old key and an INSERT; one of
 * wf-id touches both instances. A retract is a DELETE. */
static void test_keys_and_retract(void) {
  char log[4096], got[8192];
  EXPECT(cycle("(do-for-fact ((?f order)) (= ?f:line-no 3) "
               "(modify ?f (line-no 30)))",
               log, sizeof log) == 0);
  EXPECT(same("key change", stmts, expect_sql(DEL_ORDER INS_ORDER INSTANCE)));
  query("SELECT wf_id, line_no, note FROM shop_order WHERE wf_id = 'w1' "
        "ORDER BY 2",
        got, sizeof got);
  EXPECT(same("rekeyed", got, "'w1' 2 'x'; 'w1' 4 NULL; 'w1' 30 'sym'"));
  EXPECT(cycle("(do-for-fact ((?f order)) (= ?f:line-no 30) "
               "(modify ?f (wf-id w2)))",
               log, sizeof log) == 0);
  EXPECT(same("wf-id change", stmts,
              expect_sql(DEL_ORDER INS_ORDER INSTANCE INSTANCE)));
  query("SELECT wf_id, line_no FROM shop_order WHERE line_no = 30", got,
        sizeof got);
  EXPECT(same("moved", got, "'w2' 30"));
  instances(got, sizeof got);
  EXPECT(same("w2 instance", got,
              "'w0' 'shop' '1' 'active' NULL NULL NULL; "
              "'w1' 'shop' '1' 'active' NULL NULL NULL; "
              "'w2' 'shop' '1' 'active' NULL NULL NULL"));
  EXPECT(cycle("(do-for-fact ((?f order)) (= ?f:line-no 4) (retract ?f))",
               log, sizeof log) == 0);
  EXPECT(same("retract", stmts, expect_sql(DEL_ORDER INSTANCE)));
  query("SELECT count(*) FROM shop_order WHERE line_no = 4", got, sizeof got);
  EXPECT(same("deleted", got, "0"));
}

/* Within an aggregate the DELETEs come first, then the UPDATEs, then the
 * INSERTs, then the instance rows; aggregates go in package order (shop
 * before crm), whatever order the changes came in. */
static void test_order(void) {
  char log[4096], got[8192];
  begin_cycle();
  act(CRM, "(assert (party (wf-id c1) (name \"Ann\")))");
  act(SHOP, "(assert (order (wf-id w1) (line-no 8)))");
  act(SHOP, "(do-for-fact ((?f order)) (= ?f:line-no 2) "
            "(modify ?f (note \"y\")))");
  act(SHOP, "(do-for-fact ((?f order)) (= ?f:line-no 30) "
            "(modify ?f (line-no 31)))");
  EXPECT(end_cycle(log, sizeof log) == 0);
  EXPECT(same("order log", log, ""));
  EXPECT(same("order", stmts,
              expect_sql(DEL_ORDER
                         "UPDATE @\"shop_order\" SET \"note\" = $1 WHERE "
                         "\"wf_id\" = $2 AND \"line_no\" = $3\n"
                         INS_ORDER INS_ORDER INSTANCE INSTANCE
                         INS_PARTY INSTANCE)));
  rows_of("crm_party", got, sizeof got);
  EXPECT(same("party", got, "'c1' 'Ann'"));
  query("SELECT wf_id, definition, version FROM workflow_instance WHERE "
        "wf_id = 'c1'",
        got, sizeof got);
  EXPECT(same("crm instance", got, "'c1' 'crm' '2'"));
  query("SELECT wf_id, line_no FROM shop_order WHERE line_no IN (8, 30, 31) "
        "ORDER BY 2",
        got, sizeof got);
  EXPECT(same("order rows", got, "'w1' 8; 'w2' 31"));
}

/* Facts of a manual template, of a template no mapping names and of a
 * package template outside the mapping are never written. */
static void test_not_written(void) {
  char log[4096], got[8192], before[16384], after[16384];
  fingerprint(before, sizeof before);
  EXPECT(cycle("(progn (assert (note (wf-id w1) (text \"t\"))) "
               "(assert (tmp (wf-id w1) (v 1))) "
               "(assert (go (wf-id w1) (act none))))",
               log, sizeof log) == 0);
  EXPECT(same("manual and unmapped", stmts, ""));
  EXPECT(same("manual and unmapped log", log, ""));
  fingerprint(after, sizeof after);
  EXPECT(same("nothing written", after, before));
  rows_of("shop_note", got, sizeof got);
  EXPECT(same("no note row", got, ""));
}

/* Facts asserted while suspended (loading) are not written; a later
 * modify is an UPDATE against what was loaded; a suspended retract drops
 * a pending change; suspend nests. */
static void test_suspend(void) {
  char log[4096], got[8192];
  sql("INSERT INTO shop_order (wf_id, line_no, status) VALUES ('w3', 6, "
      "'loaded')");
  EXPECT(clpd_orm_sync_suspended(syncer) == 0);
  clpd_orm_sync_suspend(syncer);
  clpd_orm_sync_suspend(syncer);
  EXPECT(clpd_orm_sync_suspended(syncer) == 1);
  act(SHOP, "(assert (order (wf-id w3) (line-no 6) (status loaded)))");
  clpd_orm_sync_resume(syncer);
  EXPECT(clpd_orm_sync_suspended(syncer) == 1);
  act(SHOP, "(assert (order (wf-id w3) (line-no 7) (status loaded)))");
  clpd_orm_sync_resume(syncer);
  EXPECT(clpd_orm_sync_suspended(syncer) == 0);
  clpd_orm_sync_resume(syncer); /* never below zero */
  EXPECT(clpd_orm_sync_suspended(syncer) == 0);
  EXPECT(cycle(NULL, log, sizeof log) == 0);
  EXPECT(same("loaded facts", stmts, ""));
  query("SELECT line_no FROM shop_order WHERE wf_id = 'w3'", got, sizeof got);
  EXPECT(same("only the stored row", got, "6"));
  EXPECT(cycle("(do-for-all-facts ((?f order)) (eq ?f:wf-id w3) "
               "(modify ?f (status done)))",
               log, sizeof log) == 0);
  /* Line 7 was never stored: its UPDATE finds no row and inserts it. */
  EXPECT(same("modify of loaded facts", stmts,
              expect_sql("UPDATE @\"shop_order\" SET \"status\" = $1 WHERE "
                         "\"wf_id\" = $2 AND \"line_no\" = $3\n"
                         "UPDATE @\"shop_order\" SET \"status\" = $1 WHERE "
                         "\"wf_id\" = $2 AND \"line_no\" = $3\n" INS_ORDER
                         INSTANCE)));
  query("SELECT line_no, status FROM shop_order WHERE wf_id = 'w3' ORDER BY 1",
        got, sizeof got);
  EXPECT(same("loaded rows updated", got, "6 'done'; 7 'done'"));
  /* A pending change dropped by a suspended retract. */
  act(SHOP, "(assert (order (wf-id w3) (line-no 9)))");
  clpd_orm_sync_suspend(syncer);
  act(SHOP, "(do-for-fact ((?f order)) (= ?f:line-no 9) (retract ?f))");
  act(SHOP, "(do-for-fact ((?f order)) (= ?f:line-no 7) (retract ?f))");
  clpd_orm_sync_resume(syncer);
  EXPECT(cycle(NULL, log, sizeof log) == 0);
  EXPECT(same("suspended retracts", stmts, ""));
  query("SELECT line_no FROM shop_order WHERE wf_id = 'w3' ORDER BY 1", got,
        sizeof got);
  EXPECT(same("rows kept", got, "6; 7"));
  sql("DELETE FROM shop_order WHERE wf_id = 'w3' AND line_no = 7");
}

/* Two facts with one primary key: the second INSERT is a unique violation
 * and the cycle is rolled back, the database unchanged; the changes are
 * kept, so the retry after the cause is gone writes the rest. */
static void test_duplicate_key(void) {
  char log[4096], want[4096], got[8192], before[16384], after[16384];
  const char *unique =
      pg ? "duplicate key value violates unique constraint "
           "\"shop_order_pkey\" (SQLSTATE 23505)"
         : "UNIQUE constraint failed: shop_order.wf_id, shop_order.line_no";
  fingerprint(before, sizeof before);
  EXPECT(cycle("(progn (assert (order (wf-id w4) (line-no 1) (status a))) "
               "(assert (order (wf-id w4) (line-no 1) (status b))))",
               log, sizeof log) == 1);
  refused(want, sizeof want, "shop@1::order: table 'shop_order'", unique);
  EXPECT(same("duplicate log", log, want));
  fingerprint(after, sizeof after);
  EXPECT(same("unchanged after the unique violation", after, before));
  EXPECT(clpd_db_in_transaction(db) == 0);
  EXPECT(cycle("(do-for-fact ((?f order)) (eq ?f:status b) (retract ?f))",
               log, sizeof log) == 0);
  EXPECT(same("retry log", log, ""));
  EXPECT(same("retry", stmts, expect_sql(INS_ORDER INSTANCE)));
  query("SELECT status FROM shop_order WHERE wf_id = 'w4'", got, sizeof got);
  EXPECT(same("first kept", got, "'a'"));
  EXPECT(tx.retry_ms == 0);
}

/* A value the declared type cannot take rolls the cycle back before any
 * of it is committed (statements sent earlier included). */
static void test_value_errors(void) {
  static const struct {
    const char *expr, *reason;
  } cases[] = {
      {"(assert (order (wf-id w5) (line-no 1) (amount \"x\")))",
       "slot 'amount' (column 'amount'): expected float, got STRING"},
      {"(assert (order (wf-id w5) (line-no 1) (amount 3)))",
       "slot 'amount' (column 'amount'): expected float, got INTEGER"},
      {"(assert (order (wf-id w5) (line-no 1) (paid 2)))",
       "slot 'paid' (column 'paid'): a boolean column takes 0 or 1"},
      {"(assert (order (wf-id 5) (line-no 1)))",
       "slot 'wf-id' (column 'wf_id'): expected string, got INTEGER"},
      {"(assert (order (wf-id w5) (line-no 1) (note 1.5)))",
       "slot 'note' (column 'note'): expected string, got FLOAT"},
  };
  char log[4096], want[4096], expr[512], before[16384], after[16384];
  size_t i;
  for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    fingerprint(before, sizeof before);
    /* A valid change first: its INSERT goes out, then is rolled back. */
    snprintf(expr, sizeof expr,
             "(progn (assert (order (wf-id w5) (line-no 2))) %s)",
             cases[i].expr);
    EXPECT(cycle(expr, log, sizeof log) == 1);
    snprintf(want, sizeof want,
             "clpdaemon: sync: shop@1::order: table 'shop_order': %s\n"
             "clpdaemon: cycle transaction failed: %s; retrying in 100 ms\n",
             cases[i].reason, cases[i].reason);
    EXPECT(same(cases[i].expr, log, want));
    EXPECT(strstr(stmts, "INSERT INTO") != NULL);
    fingerprint(after, sizeof after);
    EXPECT(same("unchanged after a value error", after, before));
    EXPECT(clpd_db_in_transaction(db) == 0);
    /* Gone again: the retry writes nothing. */
    act(SHOP, "(do-for-all-facts ((?f order)) (or (eq ?f:wf-id w5) "
              "(eq ?f:wf-id 5)) (retract ?f))");
    EXPECT(cycle(NULL, log, sizeof log) == 0);
    EXPECT(same("retry after a value error", stmts, ""));
  }
}

/* An INTEGER written to a PostgreSQL int4 column that cannot hold it is a
 * database error: rolled back. SQLite stores 64 bits. */
static void test_int4_overflow(void) {
  char log[4096], want[4096], got[8192], before[16384], after[16384];
  fingerprint(before, sizeof before);
  if (pg) {
    EXPECT(cycle("(assert (small (wf-id w6) (n 3000000000)))", log,
                 sizeof log) == 1);
    refused(want, sizeof want, "shop@1::small: table 'shop_small'",
            "integer out of range (SQLSTATE 22003)");
    EXPECT(same("int4 overflow", log, want));
    fingerprint(after, sizeof after);
    EXPECT(same("unchanged after the overflow", after, before));
    act(SHOP, "(do-for-fact ((?f small)) TRUE (modify ?f (n 7)))");
    EXPECT(cycle(NULL, log, sizeof log) == 0);
    rows_of("shop_small", got, sizeof got);
    EXPECT(same("int4 row", got, "'w6' 7"));
  } else {
    EXPECT(cycle("(assert (small (wf-id w6) (n 3000000000)))", log,
                 sizeof log) == 0);
    rows_of("shop_small", got, sizeof got);
    EXPECT(same("sqlite 64 bits", got, "'w6' 3000000000"));
  }
  /* nil in a null nil column of a template without a local key. */
  EXPECT(cycle("(do-for-fact ((?f small)) TRUE (modify ?f (n nil)))", log,
               sizeof log) == 0);
  EXPECT(same("small update", stmts,
              expect_sql("UPDATE @\"shop_small\" SET \"n\" = $1 WHERE "
                         "\"wf_id\" = $2\n" INSTANCE)));
  rows_of("shop_small", got, sizeof got);
  EXPECT(same("small null", got, "'w6' NULL"));
}

/* Any statement of the flush that fails rolls the whole cycle back (the
 * mapped rows written before it, other aggregates included); the retry
 * writes everything. */
static void test_failed_flush(void) {
  char log[4096], want[4096], got[8192], before[16384], after[16384];
  const char *injected =
      pg ? "relation \"clpd_injected_failure\" does not exist (SQLSTATE "
           "42P01)"
         : "no such table: clpd_injected_failure";
  static const char *const what[] = {
      "shop@1::order: table 'shop_order'", "table 'workflow_instance'",
      "crm@2::party: table 'crm_party'", "table 'workflow_instance'"};
  int k;
  for (k = 1; k <= 4; k++) {
    fingerprint(before, sizeof before);
    begin_cycle();
    fail_at = k;
    act(SHOP, "(assert (order (wf-id w7) (line-no 1)))");
    act(CRM, "(assert (party (wf-id c7)))");
    EXPECT(end_cycle(log, sizeof log) == 1);
    refused(want, sizeof want, what[k - 1], injected);
    EXPECT(same("injected failure", log, want));
    fingerprint(after, sizeof after);
    EXPECT(same("unchanged after a failed flush", after, before));
    /* Undo the facts: the next attempt starts from the same state. */
    act(SHOP, "(do-for-fact ((?f order)) (eq ?f:wf-id w7) (retract ?f))");
    act(CRM, "(do-for-fact ((?f party)) (eq ?f:wf-id c7) (retract ?f))");
    EXPECT(cycle(NULL, log, sizeof log) == 0);
    EXPECT(same("nothing pending", stmts, ""));
  }
  begin_cycle();
  fail_at = 3;
  act(SHOP, "(assert (order (wf-id w7) (line-no 1)))");
  act(CRM, "(assert (party (wf-id c7)))");
  EXPECT(end_cycle(log, sizeof log) == 1);
  EXPECT(cycle(NULL, log, sizeof log) == 0);
  EXPECT(same("retry writes all", stmts,
              expect_sql(INS_ORDER INSTANCE INS_PARTY INSTANCE)));
  query("SELECT wf_id FROM workflow_instance WHERE wf_id IN ('w7', 'c7') "
        "ORDER BY 1",
        got, sizeof got);
  EXPECT(same("both instances", got, "'c7'; 'w7'"));
}

/* After a statement of the cycle failed (a rule's db-nextval), the flush
 * sends nothing; the commit reports that failure and the changes wait for
 * the retry. */
static void test_rule_failure(void) {
  char log[4096], want[4096], got[8192];
  const char *missing =
      pg ? "relation \"public.shop_missing_seq\" does not exist (SQLSTATE "
           "42P01)"
         : "no such table: clpd_sequence";
  EXPECT(cycle("(assert (go (wf-id w8) (act fail)))", log, sizeof log) == 1);
  snprintf(want, sizeof want,
           "clpdaemon: db-nextval: sequence alias 'missing_no': sequence "
           "'shop_missing_seq': %s\n"
           "clpdaemon: cycle transaction failed: %s; retrying in 100 ms\n",
           missing, missing);
  EXPECT(same("rule failure log", log, want));
  EXPECT(strstr(stmts, "shop_order") == NULL &&
         strstr(stmts, "workflow_instance") == NULL);
  query("SELECT count(*) FROM shop_order WHERE wf_id = 'w8'", got, sizeof got);
  EXPECT(same("not written", got, "0"));
  EXPECT(cycle(NULL, log, sizeof log) == 0);
  EXPECT(same("written on the retry", stmts, expect_sql(INS_ORDER INSTANCE)));
  query("SELECT line_no FROM shop_order WHERE wf_id = 'w8'", got, sizeof got);
  EXPECT(same("retry row", got, "99"));
}

/* An existing workflow_instance row keeps its definition, version, status
 * and created_at; only updated_at changes. */
static void test_existing_instance(void) {
  char log[4096], got[8192];
  sql("INSERT INTO workflow_instance (wf_id, definition, version, status, "
      "created_at, updated_at) VALUES ('w9', 'other', '9', 'done', "
      "'2020-01-01 00:00:00+00', '2020-01-01 00:00:00+00')");
  EXPECT(cycle("(assert (order (wf-id w9) (line-no 1)))", log, sizeof log) ==
         0);
  query("SELECT definition, version, status, created_at = '2020-01-01 "
        "00:00:00+00', updated_at > created_at FROM workflow_instance WHERE "
        "wf_id = 'w9'",
        got, sizeof got);
  EXPECT(same("existing instance", got, "'other' '9' 'done' 1 1"));
}

/* Without a database the end of a cycle only commits; NULL is accepted. */
static void test_attach(void) {
  clpd_orm_tx none;
  clpd_orm_tx_init(&none, NULL, NULL, 100, 30000);
  EXPECT(clpd_orm_tx_begin(&none, stderr) == 0);
  EXPECT(clpd_orm_sync_end_cycle(NULL, &none, stderr) == 0);
  clpd_orm_sync_detach(NULL);
  clpd_orm_sync_suspend(NULL);
  clpd_orm_sync_resume(NULL);
  clpd_orm_sync_clear(NULL);
  EXPECT(clpd_orm_sync_suspended(NULL) == 0);
}

/* clear forgets the recorded changes; after detach nothing is recorded. */
static void test_clear_and_detach(void) {
  char log[4096], got[8192];
  act(SHOP, "(assert (order (wf-id w10) (line-no 1)))");
  clpd_orm_sync_clear(syncer);
  EXPECT(cycle(NULL, log, sizeof log) == 0);
  EXPECT(same("cleared", stmts, ""));
  clpd_orm_sync_detach(syncer);
  syncer = NULL;
  EXPECT(cycle("(assert (order (wf-id w10) (line-no 2)))", log, sizeof log) ==
         0);
  EXPECT(same("detached", stmts, ""));
  query("SELECT count(*) FROM shop_order WHERE wf_id = 'w10'", got,
        sizeof got);
  EXPECT(same("w10 rows", got, "0"));
}

int main(int argc, char **argv) {
  char spec[sizeof root + 32];
  if (argc != 1 && argc != 3) {
    fprintf(stderr, "usage: orm_sync_test [<conninfo> <password>]\n");
    return 2;
  }
  if (mkdtemp(root) == NULL) {
    perror("orm_sync_test");
    return 2;
  }
  snprintf(spec, sizeof spec, "sqlite:%s/sync.db", root);
  if (argc == 3) password = argv[2];
  setup(argc == 3 ? argv[1] : spec);
  test_first_cycle();
  test_types();
  test_net_changes();
  test_update();
  test_keys_and_retract();
  test_order();
  test_not_written();
  test_suspend();
  test_duplicate_key();
  test_value_errors();
  test_int4_overflow();
  test_failed_flush();
  test_rule_failure();
  test_existing_instance();
  test_attach();
  test_clear_and_detach();
  clpd_packages_free(&pkgs);
  DestroyEnvironment(env);
  {
    const char *const drop[] = {"DROP TABLE shop_order", "DROP TABLE shop_note",
                                "DROP TABLE shop_small", "DROP TABLE crm_party",
                                "DROP TABLE workflow_instance"};
    size_t i;
    for (i = 0; i < sizeof drop / sizeof drop[0]; i++) sql(drop[i]);
  }
  clpd_db_close(db);
  if (failures != 0) {
    fprintf(stderr, "%d orm sync checks failed (fixtures in %s)\n", failures,
            root);
    return 1;
  }
  {
    char cmd[sizeof root + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0) fprintf(stderr, "cannot remove %s\n", root);
  }
  puts("orm sync checks passed");
  return 0;
}
