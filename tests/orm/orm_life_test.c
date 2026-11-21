/* Tests for the workflow lifecycle functions wf-complete and wf-fail
 * (T11a, FR-8, src/orm_life.h, docs/orm.md#lifecycle-functions) and the
 * startup shape check of workflow_instance (clpd_orm_tx_check_instance_table,
 * docs/database.md#workflow_instance).
 * usage: orm_life_test                         (a temporary SQLite file)
 *        orm_life_test <conninfo> <password>   (PostgreSQL, run by
 *                                               tests/db/pg_cluster.sh)
 * A real CLIPS environment loads two packages (shop@1 and crm@2) with the
 * ORM functions and the synchronization attached as in the daemon; a cycle
 * is what run_cycle does (clpd_orm_sync_begin_cycle, the changes, Run,
 * clpd_orm_sync_end_cycle). Everything is observed through the public
 * interface: the daemon log, the rows read back with SELECT, the facts in
 * working memory, and the statements sent (clpd_db_exec is wrapped,
 * -Wl,--wrap, this test binary only, to record them and to make the one
 * containing a given text fail). */
#define _POSIX_C_SOURCE 200809L

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

static char root[] = "/tmp/clpd_orm_life_test.XXXXXX";
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
 * appended to stmts, one per line; the first one containing fail_on (when
 * set) is replaced by a statement that fails on the database. */
static int recording;
static const char *fail_on;
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
  }
  if (fail_on != NULL && strstr(sql, fail_on) != NULL) {
    fail_on = NULL;
    return __real_clpd_db_exec(d, "SELECT * FROM clpd_injected_failure", NULL,
                               0, result);
  }
  return __real_clpd_db_exec(d, sql, params, n, result);
}

static void write_file(const char *dir, const char *name, const char *text) {
  char path[512];
  FILE *f;
  snprintf(path, sizeof path, "%s/%s", dir, name);
  f = fopen(path, "w");
  if (f == NULL || fputs(text, f) < 0 || fclose(f) != 0) {
    perror("orm_life_test");
    exit(2);
  }
}

static const char *shop_clp =
    "(deftemplate go (slot wf-id) (slot act (type SYMBOL)) (slot why))\n"
    "(deftemplate order (slot wf-id) (slot line-no (type INTEGER))\n"
    "  (slot status (type SYMBOL)) (slot amount))\n"
    "(deftemplate memo (slot wf-id) (slot text))\n"
    "(defrule complete (go (wf-id ?w) (act complete)) => (wf-complete ?w))\n"
    "(defrule fail (go (wf-id ?w) (act fail) (why ?r)) => (wf-fail ?w ?r))\n"
    "(defrule pay (go (wf-id ?w) (act pay))\n"
    "  ?o <- (order (wf-id ?w) (line-no 1))\n"
    "  => (modify ?o (status paid)) (wf-complete ?w))\n"
    "(defrule bad (go (wf-id ?w) (act bad))\n"
    "  ?o <- (order (wf-id ?w) (line-no 1))\n"
    "  => (modify ?o (amount 3)) (wf-complete ?w))\n"
    "(defrule undo (go (wf-id ?w) (act undo)) =>\n"
    "  (wf-complete ?w) (db-nextval missing_no))\n"
    "(defrule twice (go (wf-id ?w) (act twice)) =>\n"
    "  (wf-complete ?w) (wf-fail ?w \"again\"))\n"
    "(defrule late (go (wf-id ?w) (act late)) =>\n"
    "  (assert (order (wf-id ?w) (line-no 7) (status late))))\n";
static const char *shop_orm =
    "order.table = life_order\n"
    "order.owned = yes\n"
    "order.key = line-no\n"
    "order.slot.wf-id = wf_id text symbol error\n"
    "order.slot.line-no = line_no bigint integer error\n"
    "order.slot.status = status text symbol default\n"
    "order.slot.amount = amount double float nil\n"
    "seq.missing_no = life_missing_seq\n"
    "seq.missing_no.owned = yes\n";
static const char *crm_clp = "(deftemplate party (slot wf-id) (slot name))\n";
static const char *crm_orm =
    "party.table = life_party\n"
    "party.owned = yes\n"
    "party.slot.wf-id = wf_id text symbol error\n"
    "party.slot.name = name text symbol nil\n";

static int same(const char *what, const char *got, const char *want) {
  if (strcmp(got, want) != 0) {
    fprintf(stderr, "  %s\n    got:  [%s]\n    want: [%s]\n", what, got, want);
    return 0;
  }
  return 1;
}

static void sql(const char *text) {
  if (clpd_db_exec(db, text, NULL, 0, NULL) != CLPD_DB_OK) {
    fprintf(stderr, "orm_life_test: %s: %s\n", text, clpd_db_error(db));
    exit(2);
  }
}

/* Appends v as text: NULL, an integer, %g, 'text'. */
static void put_value(char *out, size_t cap, const clpd_db_value *v) {
  size_t len = strlen(out);
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
  default:
    snprintf(out + len, cap - len, "'%.*s'", (int)v->size,
             (const char *)v->data);
    break;
  }
}

/* The rows of a query, "v v v; v v v". */
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

/* Every table with its rows, timestamps included as text. */
static void fingerprint(char *out, size_t cap) {
  char one[8192];
  size_t len;
  out[0] = '\0';
  query("SELECT * FROM life_order ORDER BY 1, 2", one, sizeof one);
  len = strlen(out);
  snprintf(out + len, cap - len, "life_order: %s\n", one);
  query("SELECT * FROM life_party ORDER BY 1", one, sizeof one);
  len = strlen(out);
  snprintf(out + len, cap - len, "life_party: %s\n", one);
  query(pg ? "SELECT wf_id, definition, version, status, reason, "
             "created_at::text, updated_at::text FROM workflow_instance "
             "ORDER BY 1"
           : "SELECT * FROM workflow_instance ORDER BY 1",
        one, sizeof one);
  len = strlen(out);
  snprintf(out + len, cap - len, "workflow_instance: %s\n", one);
}

/* The created_at and updated_at of an instance, as text. */
static void times_of(const char *wf, char *out, size_t cap) {
  char q[256];
  snprintf(q, sizeof q,
           pg ? "SELECT created_at::text, updated_at::text FROM "
                "workflow_instance WHERE wf_id = '%s'"
              : "SELECT created_at, updated_at FROM workflow_instance WHERE "
                "wf_id = '%s'",
           wf);
  query(q, out, cap);
}

/* "1" when the instance's updated_at is later than its created_at. */
static void moved(const char *wf, char *out, size_t cap) {
  char q[256];
  snprintf(q, sizeof q,
           "SELECT count(*) FROM workflow_instance WHERE wf_id = '%s' AND "
           "updated_at > created_at",
           wf);
  query(q, out, cap);
}

/* Appends one slot value. */
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

/* The facts whose wf-id prints as wf, in fact-index order. */
static void facts_of(const char *wf, char *out, size_t cap) {
  Fact *f;
  out[0] = '\0';
  for (f = GetNextFact(env, NULL); f != NULL; f = GetNextFact(env, f)) {
    Deftemplate *d = FactDeftemplate(f);
    struct templateSlot *ts;
    CLIPSValue v;
    char text[256] = "";
    size_t len;
    if (!DeftemplateSlotExistP(d, "wf-id") ||
        GetFactSlot(f, "wf-id", &v) != GSE_NO_ERROR)
      continue;
    put_clips(text, sizeof text, &v);
    if (strcmp(text, wf) != 0) continue;
    len = strlen(out);
    snprintf(out + len, cap - len, "%s%s(", len > 0 ? "; " : "",
             DeftemplateName(d));
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

/* Expected statements: '@' is the table qualifier ("public". on
 * PostgreSQL), '~' the database clock. */
static const char *expect_sql(const char *fmt) {
  static char out[16384];
  const char *now = pg ? "now()" : "strftime('%Y-%m-%dT%H:%M:%fZ', 'now')";
  size_t len = 0;
  for (; *fmt != '\0' && len + 64 < sizeof out; fmt++) {
    if (*fmt == '@') {
      if (pg)
        len += (size_t)snprintf(out + len, sizeof out - len, "\"public\".");
    } else if (*fmt == '~') {
      len += (size_t)snprintf(out + len, sizeof out - len, "%s", now);
    } else {
      out[len++] = *fmt;
    }
  }
  out[len] = '\0';
  return out;
}

#define SEL_INSTANCE                                                       \
  "SELECT \"definition\", \"version\", \"status\" FROM "                  \
  "@\"workflow_instance\" WHERE \"wf_id\" = $1\n"
#define UPD_STATUS                                                         \
  "UPDATE @\"life_order\" SET \"status\" = $1 WHERE \"wf_id\" = $2 AND "  \
  "\"line_no\" = $3\n"
#define INSTANCE                                                           \
  "INSERT INTO @\"workflow_instance\" (\"wf_id\", \"definition\", "       \
  "\"version\", \"status\", \"created_at\", \"updated_at\") VALUES ($1, "  \
  "$2, $3, 'active', ~, ~) ON CONFLICT (\"wf_id\") DO UPDATE SET "         \
  "\"updated_at\" = excluded.\"updated_at\"\n"
#define FINAL                                                              \
  "INSERT INTO @\"workflow_instance\" (\"wf_id\", \"definition\", "       \
  "\"version\", \"status\", \"reason\", \"created_at\", \"updated_at\") "  \
  "VALUES ($1, $2, $3, $4, $5, ~, ~) ON CONFLICT (\"wf_id\") DO UPDATE "   \
  "SET \"status\" = excluded.\"status\", \"reason\" = "                    \
  "excluded.\"reason\", \"updated_at\" = excluded.\"updated_at\"\n"

/* --- cycles ---------------------------------------------------------- */

static FILE *cycle_log;

static void clear_flags(void) {
  SetEvaluationError(env, false);
  SetHaltExecution(env, false);
  SetHaltRules(env, false);
}

static void open_log(void) {
  cycle_log = tmpfile();
  if (cycle_log == NULL) exit(2);
  clpd_log_set(cycle_log);
  stmts[0] = '\0';
  recording = 1;
}

static void close_log(char *log, size_t cap) {
  size_t n;
  recording = 0;
  fail_on = NULL;
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
}

static void begin_cycle(void) {
  open_log();
  if (clpd_orm_sync_begin_cycle(syncer, &tx, cycle_log) != 0) {
    fprintf(stderr, "orm_life_test: the cycle transaction did not begin\n");
    exit(2);
  }
}

/* Evaluates expr in the module of package pkg; returns 1 when CLIPS
 * reported an error. */
static int eval_in(size_t pkg, const char *expr) {
  CLIPSValue v;
  int err;
  SetCurrentModule(env, pkgs.items[pkg].module);
  err = Eval(env, expr, &v) != EE_NO_ERROR;
  SetCurrentModule(env, FindDefmodule(env, "MAIN"));
  clear_flags();
  return err;
}

static void act(size_t pkg, const char *expr) {
  if (eval_in(pkg, expr)) {
    fprintf(stderr, "  cannot evaluate %s\n", expr);
    failures++;
  }
}

/* Run, flush and commit; 0 committed, 1 rolled back. */
static int end_cycle(char *log, size_t cap) {
  int rc;
  clpd_packages_focus(env, &pkgs);
  Run(env, -1);
  clear_flags();
  rc = clpd_orm_sync_end_cycle(syncer, &tx, cycle_log) == 0 ? 0 : 1;
  close_log(log, cap);
  return rc;
}

/* A cycle of pkg with one expression (NULL: none). */
static int cycle_in(size_t pkg, const char *expr, char *log, size_t cap) {
  begin_cycle();
  if (expr != NULL) act(pkg, expr);
  return end_cycle(log, cap);
}

static int cycle(const char *expr, char *log, size_t cap) {
  return cycle_in(SHOP, expr, log, cap);
}

/* SQLite timestamps have milliseconds: a later cycle gets a later one. */
static void tick(void) {
  struct timespec ts = {0, 20 * 1000 * 1000};
  nanosleep(&ts, NULL);
}

/* The message of the injected failure. */
static const char *injected(void) {
  return pg ? "relation \"clpd_injected_failure\" does not exist (SQLSTATE "
              "42P01)"
            : "no such table: clpd_injected_failure";
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
  sql("DROP TABLE IF EXISTS life_order");
  sql("DROP TABLE IF EXISTS life_party");
  sql("DROP TABLE IF EXISTS workflow_instance");
  sql(pg ? "CREATE TABLE life_order (wf_id TEXT NOT NULL, line_no BIGINT NOT "
           "NULL, status TEXT, amount DOUBLE PRECISION, PRIMARY KEY (wf_id, "
           "line_no))"
         : "CREATE TABLE life_order (wf_id TEXT NOT NULL, line_no INTEGER NOT "
           "NULL, status TEXT, amount REAL, PRIMARY KEY (wf_id, line_no))");
  sql("CREATE TABLE life_party (wf_id TEXT NOT NULL PRIMARY KEY, name TEXT)");
  if (clpd_orm_tx_create_instance_table(db, msg, sizeof msg) != 0) {
    fprintf(stderr, "orm_life_test: workflow_instance: %s\n", msg);
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
    fprintf(stderr, "orm_life_test: cannot open the database: %s\n", msg);
    exit(2);
  }
  snprintf(db_spec, sizeof db_spec, "%s", spec);
  pg = strcmp(clpd_db_backend(db), "postgresql") == 0;
  create_tables();
  if (pg) sql("DROP SEQUENCE IF EXISTS life_missing_seq");
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
    fprintf(stderr, "orm_life_test: packages did not load:\n%s", eb);
    exit(2);
  }
  fclose(err);
  if (clpd_orm_sync_attach(env, db, &pkgs, &syncer) != 0) {
    fprintf(stderr, "orm_life_test: cannot attach the sync\n");
    exit(2);
  }
  ddl.sync = syncer;
  Reset(env);
  clpd_orm_tx_init(&tx, db, db_spec, 100, 30000);
}

/* --- tests ------------------------------------------------------------ */

/* Both functions are defined with the other ORM functions. */
static void test_registered(void) {
  EXPECT(FindFunction(env, "wf-complete") != NULL);
  EXPECT(FindFunction(env, "wf-fail") != NULL);
}

/* Only the actions of a rule of a rule package may call them: the top
 * level and a rule of MAIN are refused with the exact line, and nothing
 * changes (rows, facts). */
static void test_scope(void) {
  char log[4096], before[8192], after[8192], got[4096];
  EXPECT(cycle("(assert (order (wf-id s1) (line-no 1) (status new)))", log,
               sizeof log) == 0);
  EXPECT(same("scope seed", log, ""));
  fingerprint(before, sizeof before);
  begin_cycle();
  EXPECT(eval_in(SHOP, "(wf-complete s1)") == 1);
  EXPECT(eval_in(SHOP, "(wf-fail s1 \"x\")") == 1);
  EXPECT(end_cycle(log, sizeof log) == 0);
  EXPECT(same("scope top level", log,
              "clpdaemon: wf-complete: only a rule of a rule package may call "
              "it (no rule is executing)\n"
              "clpdaemon: wf-fail: only a rule of a rule package may call it "
              "(no rule is executing)\n"));
  fingerprint(after, sizeof after);
  EXPECT(same("scope rows", after, before));
  /* A rule of MAIN. */
  if (Build(env, "(deftemplate MAIN::outside (slot wf-id))") !=
          BE_NO_ERROR ||
      Build(env, "(defrule MAIN::outsider (outside (wf-id ?w)) => "
                 "(wf-complete ?w))") != BE_NO_ERROR) {
    fprintf(stderr, "orm_life_test: cannot build the MAIN rule\n");
    exit(2);
  }
  begin_cycle();
  {
    CLIPSValue v;
    Eval(env, "(assert (outside (wf-id s1)))", &v);
  }
  Focus(FindDefmodule(env, "MAIN"));
  EXPECT(end_cycle(log, sizeof log) == 0);
  EXPECT(same("scope MAIN rule", log,
              "clpdaemon: wf-complete: only a rule of a rule package may call "
              "it (rule 'outsider' is in module 'MAIN')\n"));
  fingerprint(after, sizeof after);
  EXPECT(same("scope rows after the MAIN rule", after, before));
  facts_of("s1", got, sizeof got);
  EXPECT(same("scope facts kept", got,
              "order(wf-id s1 line-no 1 status new amount nil); "
              "outside(wf-id s1)"));
}

/* wf-complete: the pending change of the rule (the modify) is written
 * first, then the instance row gets status completed, reason NULL and a
 * new updated_at; every fact of the instance (mapped or not) leaves
 * working memory without a DELETE, and its mapped row stays. */
static void test_complete(void) {
  char log[4096], got[8192], t1[256], t2[256];
  begin_cycle();
  act(SHOP, "(assert (order (wf-id w1) (line-no 1) (status new)))");
  act(SHOP, "(assert (memo (wf-id w1) (text \"m\")))");
  EXPECT(end_cycle(log, sizeof log) == 0);
  EXPECT(same("w1 seed", log, ""));
  times_of("w1", t1, sizeof t1);
  tick();
  EXPECT(cycle("(assert (go (wf-id w1) (act pay)))", log, sizeof log) == 0);
  EXPECT(same("w1 complete log", log, ""));
  EXPECT(same("w1 statements", stmts,
              expect_sql(SEL_INSTANCE UPD_STATUS INSTANCE FINAL)));
  query("SELECT wf_id, line_no, status, amount FROM life_order WHERE wf_id = "
        "'w1'",
        got, sizeof got);
  EXPECT(same("w1 mapped row kept", got, "'w1' 1 'paid' NULL"));
  query("SELECT wf_id, definition, version, status, reason FROM "
        "workflow_instance WHERE wf_id = 'w1'",
        got, sizeof got);
  EXPECT(same("w1 instance row", got, "'w1' 'shop' '1' 'completed' NULL"));
  times_of("w1", t2, sizeof t2);
  EXPECT(strncmp(t1, t2, strcspn(t1, " ")) == 0); /* created_at kept */
  EXPECT(strcmp(t1, t2) != 0);                    /* updated_at changed */
  moved("w1", got, sizeof got);
  EXPECT(same("w1 updated_at later", got, "1"));
  facts_of("w1", got, sizeof got);
  EXPECT(same("w1 facts gone", got, ""));
  /* The next cycle writes nothing for it. */
  EXPECT(cycle(NULL, log, sizeof log) == 0);
  EXPECT(same("w1 nothing later", stmts, ""));
}

/* wf-fail: status failed with the reason (a string or a symbol). */
static void test_fail(void) {
  char log[4096], got[8192];
  EXPECT(cycle("(assert (order (wf-id w2) (line-no 1) (status new)))", log,
               sizeof log) == 0);
  tick();
  EXPECT(cycle("(assert (go (wf-id w2) (act fail) (why \"no stock\")))", log,
               sizeof log) == 0);
  EXPECT(same("w2 log", log, ""));
  EXPECT(same("w2 statements", stmts, expect_sql(SEL_INSTANCE FINAL)));
  query("SELECT wf_id, definition, version, status, reason FROM "
        "workflow_instance WHERE wf_id = 'w2'",
        got, sizeof got);
  EXPECT(same("w2 instance row", got, "'w2' 'shop' '1' 'failed' 'no stock'"));
  moved("w2", got, sizeof got);
  EXPECT(same("w2 updated_at later", got, "1"));
  query("SELECT wf_id, line_no, status FROM life_order WHERE wf_id = 'w2'",
        got, sizeof got);
  EXPECT(same("w2 mapped row kept", got, "'w2' 1 'new'"));
  facts_of("w2", got, sizeof got);
  EXPECT(same("w2 facts gone", got, ""));
  EXPECT(cycle("(assert (go (wf-id w2b) (act fail) (why timeout)))", log,
               sizeof log) == 0);
  EXPECT(same("w2b log", log, ""));
  query("SELECT status, reason FROM workflow_instance WHERE wf_id = 'w2b'",
        got, sizeof got);
  EXPECT(same("w2b symbol reason", got, "'failed' 'timeout'"));
}

/* An instance without a row gets one with the final status (the
 * definition and version of the calling rule's package); a numeric wf-id
 * is keyed by its text. */
static void test_no_row(void) {
  char log[4096], got[8192];
  EXPECT(cycle("(assert (go (wf-id w3) (act complete)))", log, sizeof log) ==
         0);
  EXPECT(same("w3 log", log, ""));
  EXPECT(same("w3 statements", stmts,
              expect_sql(SEL_INSTANCE SEL_INSTANCE FINAL)));
  query("SELECT wf_id, definition, version, status, reason, "
        "(created_at = updated_at) FROM workflow_instance WHERE wf_id = 'w3'",
        got, sizeof got);
  EXPECT(same("w3 new row", got, "'w3' 'shop' '1' 'completed' NULL 1"));
  EXPECT(cycle("(assert (go (wf-id 42) (act fail) (why \"n\")))", log,
               sizeof log) == 0);
  EXPECT(same("42 log", log, ""));
  query("SELECT wf_id, status, reason FROM workflow_instance WHERE wf_id = "
        "'42'",
        got, sizeof got);
  EXPECT(same("42 row", got, "'42' 'failed' 'n'"));
  facts_of("42", got, sizeof got);
  EXPECT(same("42 facts gone", got, ""));
}

/* A finished instance cannot be finished again (also within one rule);
 * the refused call changes nothing. */
static void test_already(void) {
  char log[4096], before[8192], after[8192], got[4096];
  fingerprint(before, sizeof before);
  EXPECT(cycle("(assert (go (wf-id w1) (act complete)))", log, sizeof log) ==
         0);
  EXPECT(same("w1 again", log,
              "clpdaemon: wf-complete: instance 'w1' is already completed\n"));
  EXPECT(cycle("(assert (go (wf-id w2) (act fail) (why later)))", log,
               sizeof log) == 0);
  EXPECT(same("w2 again", log,
              "clpdaemon: wf-fail: instance 'w2' is already failed\n"));
  fingerprint(after, sizeof after);
  EXPECT(same("already: rows unchanged", after, before));
  facts_of("w1", got, sizeof got);
  EXPECT(same("already: the go fact stays", got,
              "go(wf-id w1 act complete why nil)"));
  EXPECT(cycle("(assert (go (wf-id w4) (act twice)))", log, sizeof log) == 0);
  EXPECT(same("w4 twice", log,
              "clpdaemon: wf-fail: instance 'w4' is already completed\n"));
  query("SELECT status, reason FROM workflow_instance WHERE wf_id = 'w4'",
        got, sizeof got);
  EXPECT(same("w4 row", got, "'completed' NULL"));
}

/* The instance's row must belong to the calling rule's definition. */
static void test_definition(void) {
  char log[4096], before[8192], after[8192], got[4096];
  EXPECT(cycle_in(CRM, "(assert (party (wf-id c1) (name x)))", log,
                  sizeof log) == 0);
  fingerprint(before, sizeof before);
  EXPECT(cycle("(assert (go (wf-id c1) (act complete)))", log, sizeof log) ==
         0);
  EXPECT(same("c1 log", log,
              "clpdaemon: wf-complete: instance 'c1' belongs to definition "
              "crm, not to definition shop of the calling rule\n"));
  EXPECT(same("c1 statements", stmts, expect_sql(SEL_INSTANCE)));
  fingerprint(after, sizeof after);
  EXPECT(same("c1 rows unchanged", after, before));
  facts_of("c1", got, sizeof got);
  EXPECT(same("c1 facts kept", got,
              "party(wf-id c1 name x); go(wf-id c1 act complete why nil)"));
}

/* Facts asserted for a finished instance later are never written. */
static void test_later(void) {
  char log[4096], before[8192], after[8192], got[4096];
  fingerprint(before, sizeof before);
  EXPECT(cycle("(assert (go (wf-id w1) (act late)))", log, sizeof log) == 0);
  EXPECT(same("late log", log, ""));
  EXPECT(same("late statements", stmts, ""));
  act(SHOP, "(assert (order (wf-id w2) (line-no 9) (status late)))");
  EXPECT(cycle(NULL, log, sizeof log) == 0);
  EXPECT(same("late statements (between cycles)", stmts, ""));
  fingerprint(after, sizeof after);
  EXPECT(same("late rows unchanged", after, before));
  facts_of("w1", got, sizeof got);
  EXPECT(strstr(got, "order(wf-id w1 line-no 7 status late") != NULL);
}

/* A rolled-back cycle undoes the final state: the row is active again and
 * the retry reloads the instance's facts; a later call succeeds. */
static void test_rollback(void) {
  char log[4096], before[8192], after[8192], got[4096], want[1024];
  EXPECT(cycle("(assert (order (wf-id w5) (line-no 1) (status new)))", log,
               sizeof log) == 0);
  fingerprint(before, sizeof before);
  EXPECT(cycle("(assert (go (wf-id w5) (act undo)))", log, sizeof log) == 1);
  snprintf(want, sizeof want,
           "clpdaemon: db-nextval: sequence alias 'missing_no': sequence "
           "'life_missing_seq': %s\n"
           "clpdaemon: cycle transaction failed: %s; retrying in 100 ms\n",
           pg ? "relation \"public.life_missing_seq\" does not exist "
                "(SQLSTATE 42P01)"
              : "no such table: clpd_sequence",
           pg ? "relation \"public.life_missing_seq\" does not exist "
                "(SQLSTATE 42P01)"
              : "no such table: clpd_sequence");
  EXPECT(same("w5 rollback log", log, want));
  fingerprint(after, sizeof after);
  EXPECT(same("w5 rows unchanged", after, before));
  /* The retry reloads w5 from its stored row. */
  EXPECT(cycle(NULL, log, sizeof log) == 0);
  facts_of("w5", got, sizeof got);
  EXPECT(same("w5 reloaded", got,
              "order(wf-id w5 line-no 1 status new amount nil)"));
  tick();
  EXPECT(cycle("(assert (go (wf-id w5) (act complete)))", log, sizeof log) ==
         0);
  EXPECT(same("w5 complete after the rollback", log, ""));
  query("SELECT status FROM workflow_instance WHERE wf_id = 'w5'", got,
        sizeof got);
  EXPECT(same("w5 completed", got, "'completed'"));
}

/* A pending change that cannot be written (a value of another type): the
 * synchronization line, the function's line, then the cycle is rolled
 * back and the instance isolated; a later call on it is refused. */
static void test_pending_abort(void) {
  static const char *why =
      "slot 'amount' (column 'amount'): expected float, got INTEGER";
  char log[4096], before[8192], after[8192], want[2048], got[4096];
  EXPECT(cycle("(assert (order (wf-id w6) (line-no 1) (status new)))", log,
               sizeof log) == 0);
  fingerprint(before, sizeof before);
  EXPECT(cycle("(assert (go (wf-id w6) (act bad)))", log, sizeof log) == 1);
  snprintf(want, sizeof want,
           "clpdaemon: sync: shop@1::order: table 'life_order': %s\n"
           "clpdaemon: wf-complete: instance 'w6': its pending changes could "
           "not be written\n"
           "clpdaemon: cycle transaction failed: %s; retrying in 100 ms\n"
           "clpdaemon: sync: instance 'w6' is no longer synchronized: %s\n"
           "clpdaemon: rehydrate: instance 'w6' is not reloaded: it is no "
           "longer synchronized\n",
           why, why, why);
  EXPECT(same("w6 log", log, want));
  fingerprint(after, sizeof after);
  EXPECT(same("w6 rows unchanged", after, before));
  EXPECT(cycle("(assert (go (wf-id w6) (act complete)))", log, sizeof log) ==
         0);
  EXPECT(same("w6 isolated", log,
              "clpdaemon: wf-complete: instance 'w6' is no longer "
              "synchronized\n"));
  fingerprint(after, sizeof after);
  EXPECT(same("w6 rows still unchanged", after, before));
  facts_of("w6", got, sizeof got);
  EXPECT(same("w6 go fact stays", got, "go(wf-id w6 act complete why nil)"));
}

/* A statement of the function that the database refuses: its line, then
 * the cycle is rolled back (nothing written). */
static void test_db_errors(void) {
  char log[4096], before[8192], after[8192], want[2048];
  fingerprint(before, sizeof before);
  begin_cycle();
  fail_on = "\"reason\"";
  act(SHOP, "(assert (go (wf-id w7) (act complete)))");
  EXPECT(end_cycle(log, sizeof log) == 1);
  snprintf(want, sizeof want,
           "clpdaemon: wf-complete: instance 'w7': %s\n"
           "clpdaemon: cycle transaction failed: %s; retrying in 100 ms\n",
           injected(), injected());
  EXPECT(same("w7 final write refused", log, want));
  fingerprint(after, sizeof after);
  EXPECT(same("w7 rows unchanged", after, before));
  EXPECT(cycle(NULL, log, sizeof log) == 0); /* the retry commits */
  begin_cycle();
  fail_on = "SELECT \"definition\"";
  act(SHOP, "(assert (go (wf-id w8) (act complete)))");
  EXPECT(end_cycle(log, sizeof log) == 1);
  snprintf(want, sizeof want,
           "clpdaemon: wf-complete: instance 'w8': %s\n"
           "clpdaemon: cycle transaction failed: %s; retrying in 100 ms\n",
           injected(), injected());
  EXPECT(same("w8 lookup refused", log, want));
  fingerprint(after, sizeof after);
  EXPECT(same("w8 rows unchanged", after, before));
  EXPECT(cycle(NULL, log, sizeof log) == 0);
}

/* --- the startup shape check ------------------------------------------ */

/* Replaces workflow_instance with a table of the given columns ('T' is
 * TIMESTAMPTZ on PostgreSQL, TEXT on SQLite). */
static void make_instance(const char *cols) {
  char text[2048];
  size_t len = 0;
  const char *p;
  sql("DROP TABLE IF EXISTS workflow_instance");
  len = (size_t)snprintf(text, sizeof text, "CREATE TABLE workflow_instance (");
  for (p = cols; *p != '\0' && len + 16 < sizeof text; p++) {
    if (*p == '~')
      len += (size_t)snprintf(text + len, sizeof text - len, "%s",
                              pg ? "TIMESTAMPTZ" : "TEXT");
    else
      text[len++] = *p;
  }
  snprintf(text + len, sizeof text - len, ")");
  sql(text);
}

#define COLS_HEAD                                                          \
  "wf_id TEXT NOT NULL, definition TEXT NOT NULL, version TEXT NOT NULL, " \
  "status TEXT NOT NULL, "
#define COLS_TAIL                                                          \
  "parent_wf_id TEXT, owner_node TEXT, lease_until ~, created_at ~ NOT "    \
  "NULL, updated_at ~ NOT NULL"

static void shape(const char *what, const char *cols, int want_rc,
                  const char *want_msg) {
  char msg[512];
  make_instance(cols);
  EXPECT(clpd_orm_tx_check_instance_table(db, msg, sizeof msg) == want_rc);
  EXPECT(same(what, msg, want_msg));
}

static void test_shape(void) {
  char msg[512];
  EXPECT(clpd_orm_tx_check_instance_table(db, msg, sizeof msg) == 0);
  EXPECT(same("the daemon's table", msg, ""));
  shape("exact", COLS_HEAD "reason TEXT, " COLS_TAIL ", PRIMARY KEY (wf_id)",
        0, "");
  shape("other column order",
        "reason TEXT, " COLS_HEAD COLS_TAIL ", PRIMARY KEY (wf_id)", 0, "");
  shape("T10 table without reason", COLS_HEAD COLS_TAIL ", PRIMARY KEY (wf_id)",
        1, "missing column 'reason'");
  shape("extra column",
        COLS_HEAD "reason TEXT, " COLS_TAIL ", note TEXT, PRIMARY KEY (wf_id)",
        1, "unexpected column 'note'");
  shape("nullable status",
        "wf_id TEXT NOT NULL, definition TEXT NOT NULL, version TEXT NOT "
        "NULL, status TEXT, reason TEXT, " COLS_TAIL ", PRIMARY KEY (wf_id)",
        1, "column 'status' is nullable, expected NOT NULL");
  shape("NOT NULL reason",
        COLS_HEAD "reason TEXT NOT NULL, " COLS_TAIL ", PRIMARY KEY (wf_id)",
        1, "column 'reason' is NOT NULL, expected nullable");
  shape("lease_until of another type",
        COLS_HEAD "reason TEXT, parent_wf_id TEXT, owner_node TEXT, "
                  "lease_until BIGINT, created_at ~ NOT NULL, updated_at ~ "
                  "NOT NULL, PRIMARY KEY (wf_id)",
        1,
        pg ? "column 'lease_until' has type 'int8', expected TIMESTAMPTZ"
           : "column 'lease_until' has type 'BIGINT', expected TEXT");
  shape("no primary key", COLS_HEAD "reason TEXT, " COLS_TAIL, 1,
        "there is no primary key, expected (wf_id)");
  shape("another primary key",
        COLS_HEAD "reason TEXT, " COLS_TAIL ", PRIMARY KEY (wf_id, version)",
        1, "the primary key is (wf_id, version), expected (wf_id)");
  shape("missing before extra",
        COLS_HEAD COLS_TAIL ", note TEXT, PRIMARY KEY (wf_id)", 1,
        "missing column 'reason'");
  if (!pg)
    shape("SQLite types in any case",
          "wf_id text NOT NULL, definition Text NOT NULL, version TEXT NOT "
          "NULL, status TEXT NOT NULL, reason text, parent_wf_id TEXT, "
          "owner_node TEXT, lease_until text, created_at TEXT NOT NULL, "
          "updated_at TEXT NOT NULL, PRIMARY KEY (wf_id)",
          0, "");
  /* A catalog query the database refuses: -1 with its message. */
  make_instance(COLS_HEAD "reason TEXT, " COLS_TAIL ", PRIMARY KEY (wf_id)");
  fail_on = pg ? "information_schema.columns" : "pragma_table_info";
  EXPECT(clpd_orm_tx_check_instance_table(db, msg, sizeof msg) == -1);
  EXPECT(same("catalog refused", msg, injected()));
  fail_on = pg ? "PRIMARY KEY" : "pragma_table_info";
  if (pg) {
    EXPECT(clpd_orm_tx_check_instance_table(db, msg, sizeof msg) == -1);
    EXPECT(same("primary key query refused", msg, injected()));
  }
  fail_on = NULL;
  /* The table the daemon creates matches. */
  sql("DROP TABLE workflow_instance");
  EXPECT(clpd_orm_tx_create_instance_table(db, msg, sizeof msg) == 0);
  EXPECT(clpd_orm_tx_check_instance_table(db, msg, sizeof msg) == 0);
  EXPECT(same("created table", msg, ""));
  if (pg) {
    /* current_schema() only: a wrongly shaped table of another schema of
     * the search_path is not the one checked. */
    sql("DROP SCHEMA IF EXISTS clpd_l2 CASCADE");
    sql("CREATE SCHEMA clpd_l2");
    sql("CREATE TABLE clpd_l2.workflow_instance (x BIGINT)");
    sql("SET search_path = public, clpd_l2");
    EXPECT(clpd_orm_tx_check_instance_table(db, msg, sizeof msg) == 0);
    EXPECT(same("current schema only", msg, ""));
    sql("SET search_path = \"$user\", public");
    sql("DROP SCHEMA clpd_l2 CASCADE");
  }
}

int main(int argc, char **argv) {
  char spec[sizeof root + 32];
  if (argc != 1 && argc != 3) {
    fprintf(stderr, "usage: orm_life_test [<conninfo> <password>]\n");
    return 2;
  }
  if (mkdtemp(root) == NULL) {
    perror("orm_life_test");
    return 2;
  }
  snprintf(spec, sizeof spec, "sqlite:%s/life.db", root);
  if (argc == 3) password = argv[2];
  setup(argc == 3 ? argv[1] : spec);
  test_registered();
  test_scope();
  test_complete();
  test_fail();
  test_no_row();
  test_already();
  test_definition();
  test_later();
  test_rollback();
  test_pending_abort();
  test_db_errors();
  test_shape();
  clpd_orm_sync_detach(syncer);
  ddl.sync = NULL;
  clpd_packages_free(&pkgs);
  DestroyEnvironment(env);
  sql("DROP TABLE life_order");
  sql("DROP TABLE life_party");
  sql("DROP TABLE workflow_instance");
  clpd_db_close(db);
  if (failures != 0) {
    fprintf(stderr, "%d orm life checks failed (fixtures in %s)\n", failures,
            root);
    return 1;
  }
  {
    char cmd[sizeof root + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0) fprintf(stderr, "cannot remove %s\n", root);
  }
  puts("orm life checks passed");
  return 0;
}
