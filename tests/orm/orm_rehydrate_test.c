/* Tests for the on-demand rehydration (T11d, FR-9, FR-2, src/orm_sync.h
 * clpd_orm_sync_admit, src/ingest.h clpd_ingest_drain_admit,
 * docs/orm.md#on-demand-rehydration).
 * usage: orm_rehydrate_test                        (a temporary SQLite file)
 *        orm_rehydrate_test <conninfo> <password>  (PostgreSQL, run by
 *                                                   tests/db/pg_cluster.sh)
 * A real CLIPS environment loads one package (rh@1) with the ORM functions
 * and the synchronization attached as in the daemon. A cycle is what
 * run_cycle does (src/main.c): the idle sweep, clpd_orm_sync_begin_cycle,
 * the ingest drain with the on-demand admission (a stopped drain rolls the
 * cycle back without Run), Run, clpd_orm_sync_end_cycle. A restart is a
 * new environment and syncer on the same database. Everything is observed
 * through the public interface: the daemon log, the facts in working
 * memory (in fact-index order, so the facts of a reload come before the
 * ingested fact), the rows read back with SELECT, the ingest queue length,
 * and the statements sent (clpd_db_exec is wrapped, -Wl,--wrap, this test
 * binary only, to record them and to make one of them fail). */
#define _POSIX_C_SOURCE 200809L

#include "db_backend.h"
#include "ingest.h"
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

#define IDLE 10000LL /* the idle period of the tests, ms */

static char root[] = "/tmp/clpd_orm_rehydrate_test.XXXXXX";
static char pkg_dir[sizeof root + 16];
static Environment *env;
static clpd_packages pkgs;
static clpd_orm_ddl ddl;
static clpd_orm_syncer *syncer;
static clpd_orm_tx tx;
static clpd_db *db;
static int pg;
static char db_spec[512];
static long long now = 1000; /* the syncer's monotonic clock, ms */

static long long fake_clock(void *ctx) {
  (void)ctx;
  return now;
}

/* Seam: while recording, every statement but SELECT current_schema() is
 * appended to stmts, one per line; the fail_at-th of them (1-based)
 * returns fail_status with the message "injected" without running (inside
 * the atomic cycle transaction it is that transaction's first failure, as
 * a real failure would be). */
static int recording, fail_at, seen;
static clpd_db_status fail_status;
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
    if (++seen == fail_at) {
      if (d->in_tx && d->atomic && d->failed == CLPD_DB_OK) {
        /* A real failure first, so the backend's own transaction state
         * (PostgreSQL: aborted) matches. */
        __real_clpd_db_exec(d, "SELECT * FROM clpd_injected_failure", NULL, 0,
                            NULL);
        d->failed = fail_status;
        snprintf(d->failed_error, sizeof d->failed_error, "injected");
      }
      if (result != NULL) *result = NULL;
      clpd_db_set_error(d, "injected");
      return fail_status;
    }
  }
  return __real_clpd_db_exec(d, sql, params, n, result);
}

static void write_file(const char *dir, const char *name, const char *text) {
  char path[512];
  FILE *f;
  snprintf(path, sizeof path, "%s/%s", dir, name);
  f = fopen(path, "w");
  if (f == NULL || fputs(text, f) < 0 || fclose(f) != 0) {
    perror("orm_rehydrate_test");
    exit(2);
  }
}

/* order: mapped, sync = auto; event and seen: not mapped. saw records
 * every order fact a look event of its instance finds; done ends the
 * instance; poison fails its statement (isolation by the poison retry,
 * instance_isolate_after = 1). */
static const char *rh_clp =
    "(deftemplate order (slot wf-id) (slot line-no (type INTEGER))\n"
    "  (slot status (type SYMBOL)))\n"
    "(deftemplate event (slot wf-id) (slot kind (type SYMBOL)))\n"
    "(deftemplate seen (slot wf-id) (slot line-no) (slot status))\n"
    "(defrule saw (event (wf-id ?w) (kind look))\n"
    "  (order (wf-id ?w) (line-no ?n) (status ?s)) =>\n"
    "  (assert (seen (wf-id ?w) (line-no ?n) (status ?s))))\n"
    "(defrule done (event (wf-id ?w) (kind done)) => (wf-complete ?w))\n"
    "(defrule poison (event (wf-id ?w) (kind fail)) =>\n"
    "  (db-nextval missing_no))\n";
static const char *rh_orm =
    "order.table = rh_order\n"
    "order.owned = yes\n"
    "order.key = line-no\n"
    "order.slot.wf-id = wf_id text string error\n"
    "order.slot.line-no = line_no bigint integer error\n"
    "order.slot.status = status text symbol default\n"
    "seq.missing_no = rh_missing_seq\n"
    "seq.missing_no.owned = yes\n";

static int same(const char *what, const char *got, const char *want) {
  if (strcmp(got, want) != 0) {
    fprintf(stderr, "  %s\n    got:  [%s]\n    want: [%s]\n", what, got, want);
    return 0;
  }
  return 1;
}

static void sql(const char *text) {
  if (clpd_db_exec(db, text, NULL, 0, NULL) != CLPD_DB_OK) {
    fprintf(stderr, "orm_rehydrate_test: %s: %s\n", text, clpd_db_error(db));
    exit(2);
  }
}

/* A workflow_instance row written by another writer (or a past run). */
static void instance_row(const char *wf, const char *definition,
                         const char *version, const char *status) {
  clpd_db_value p[4];
  p[0] = clpd_db_text(wf);
  p[1] = clpd_db_text(definition);
  p[2] = clpd_db_text(version);
  p[3] = clpd_db_text(status);
  if (clpd_db_exec(db,
                   "INSERT INTO workflow_instance (wf_id, definition, "
                   "version, status, created_at, updated_at) VALUES ($1, $2, "
                   "$3, $4, '2026-01-01T00:00:00.000Z', "
                   "'2026-01-01T00:00:00.000Z')",
                   p, 4, NULL) != CLPD_DB_OK) {
    fprintf(stderr, "orm_rehydrate_test: instance row: %s\n",
            clpd_db_error(db));
    exit(2);
  }
}

/* A stored rh_order row. */
static void order_row(const char *wf, long long line, const char *status) {
  clpd_db_value p[3];
  p[0] = clpd_db_text(wf);
  p[1] = clpd_db_int(line);
  p[2] = clpd_db_text(status);
  if (clpd_db_exec(db,
                   "INSERT INTO rh_order (wf_id, line_no, status) VALUES "
                   "($1, $2, $3)",
                   p, 3, NULL) != CLPD_DB_OK) {
    fprintf(stderr, "orm_rehydrate_test: order row: %s\n", clpd_db_error(db));
    exit(2);
  }
}

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
      snprintf(out + len, cap - len, "%s", j > 0 ? " " : i > 0 ? "; " : "");
      put_value(out, cap, clpd_db_result_value(r, i, j));
    }
  }
  clpd_db_result_free(r);
}

/* Every table of the test with every row (timestamps included). */
static void fingerprint(char *out, size_t cap) {
  static const char *const q[] = {
      "SELECT * FROM rh_order ORDER BY 1, 2",
      "SELECT wf_id, definition, version, status, reason, created_at, "
      "updated_at FROM workflow_instance ORDER BY 1"};
  char one[8192];
  size_t i, len;
  out[0] = '\0';
  for (i = 0; i < sizeof q / sizeof q[0]; i++) {
    query(q[i], one, sizeof one);
    len = strlen(out);
    snprintf(out + len, cap - len, "%s\n", one);
  }
}

/* The facts whose wf-id is the string wf, in fact-index order:
 * "order <line-no> <status>", "event <kind>", "seen <line-no> <status>",
 * separated by "; ". */
static void facts_of(const char *wf, char *out, size_t cap) {
  static const char *const slots[] = {"line-no", "status", "kind"};
  Fact *f;
  out[0] = '\0';
  for (f = GetNextFact(env, NULL); f != NULL; f = GetNextFact(env, f)) {
    Deftemplate *d = FactDeftemplate(f);
    CLIPSValue v;
    size_t len, i;
    if (!DeftemplateSlotExistP(d, "wf-id") ||
        GetFactSlot(f, "wf-id", &v) != GSE_NO_ERROR ||
        v.header->type != STRING_TYPE ||
        strcmp(v.lexemeValue->contents, wf) != 0)
      continue;
    len = strlen(out);
    snprintf(out + len, cap - len, "%s%s", len > 0 ? "; " : "",
             DeftemplateName(d));
    for (i = 0; i < sizeof slots / sizeof slots[0]; i++) {
      if (!DeftemplateSlotExistP(d, slots[i]) ||
          GetFactSlot(f, slots[i], &v) != GSE_NO_ERROR)
        continue;
      len = strlen(out);
      if (v.header->type == INTEGER_TYPE)
        snprintf(out + len, cap - len, " %lld", v.integerValue->contents);
      else if (v.header->type == SYMBOL_TYPE ||
               v.header->type == STRING_TYPE)
        snprintf(out + len, cap - len, " %s", v.lexemeValue->contents);
    }
  }
}

static size_t fact_count(void) {
  Fact *f;
  size_t n = 0;
  for (f = GetNextFact(env, NULL); f != NULL; f = GetNextFact(env, f)) n++;
  return n;
}

static void post(const char *json) {
  if (clpd_ingest_post(NULL, json, strlen(json)) != 0) {
    fprintf(stderr, "orm_rehydrate_test: cannot post %s\n", json);
    exit(2);
  }
}

/* The statements the reload of instance wf sends, in order. */
static void reload_stmts(char *out, size_t cap, int with_order) {
  const char *schema = pg ? "\"public\"." : "";
  size_t len = strlen(out);
  len += (size_t)snprintf(out + len, cap - len,
                          "SELECT \"definition\", \"version\", \"status\" "
                          "FROM %s\"workflow_instance\" WHERE \"wf_id\" = "
                          "$1\n",
                          schema);
  if (with_order)
    snprintf(out + len, cap - len,
             "SELECT \"wf_id\", \"line_no\", \"status\" FROM %s\"rh_order\" "
             "WHERE \"wf_id\" = $1 ORDER BY \"wf_id\", \"line_no\"\n",
             schema);
}

/* The number of times needle occurs in hay. */
static int occurrences(const char *hay, const char *needle) {
  int n = 0;
  for (hay = strstr(hay, needle); hay != NULL;
       hay = strstr(hay + strlen(needle), needle))
    n++;
  return n;
}

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
  seen = 0;
  recording = 1;
}

static void close_log(char *log, size_t cap) {
  size_t n;
  recording = 0;
  fail_at = 0;
  clpd_log_set(NULL);
  fflush(cycle_log);
  rewind(cycle_log);
  n = fread(log, 1, cap - 1, cycle_log);
  log[n] = '\0';
  fclose(cycle_log);
}

static void on_reject(void *ctx, const void *origin, const char *message) {
  (void)ctx;
  (void)origin;
  fprintf(cycle_log, "rejected: %s\n", message);
}

static int admit(void *ctx, const CLIPSValue *wf_id) {
  (void)ctx;
  return clpd_orm_sync_admit(syncer, wf_id, cycle_log);
}

/* One cycle like run_cycle with the facts posted so far: 0 committed, 1
 * rolled back, 2 the drain stopped (rolled back without Run); the log in
 * log. */
static int cycle(char *log, size_t cap) {
  int rc;
  open_log();
  clpd_orm_sync_evict_idle(syncer, cycle_log);
  if (clpd_orm_sync_begin_cycle(syncer, &tx, cycle_log) != 0) {
    close_log(log, cap);
    fprintf(stderr, "orm_rehydrate_test: the cycle did not begin: %s\n", log);
    exit(2);
  }
  if (clpd_ingest_drain_admit(env, &pkgs, on_reject, NULL, admit, NULL,
                              NULL) != 0) {
    clpd_orm_sync_end_cycle(syncer, &tx, cycle_log);
    close_log(log, cap);
    return 2;
  }
  clpd_packages_focus(env, &pkgs);
  Run(env, -1);
  clear_flags();
  rc = clpd_orm_sync_end_cycle(syncer, &tx, cycle_log) == 0 ? 0 : 1;
  close_log(log, cap);
  return rc;
}

/* --- setup ------------------------------------------------------------ */

/* The daemon's startup on the database: environment, package, hooks,
 * Reset (a restart is stop_env then start_env). */
static void start_env(void) {
  char msg[4096];
  const char *dirs[1];
  clpd_list list;
  FILE *err;
  size_t n;
  env = CreateEnvironment();
  memset(&ddl, 0, sizeof ddl);
  ddl.db = db;
  ddl.packages = &pkgs;
  if (clpd_orm_ddl_register(env, &ddl) != 0) exit(2);
  dirs[0] = pkg_dir;
  list.items = (char **)dirs;
  list.count = 1;
  err = tmpfile();
  if (err == NULL) exit(2);
  if (clpd_packages_load_ex(env, &list, 1, &pkgs, err) != 0) {
    rewind(err);
    n = fread(msg, 1, sizeof msg - 1, err);
    msg[n] = '\0';
    fprintf(stderr, "orm_rehydrate_test: the package did not load:\n%s",
            msg);
    exit(2);
  }
  fclose(err);
  if (clpd_orm_sync_attach(env, db, &pkgs, &syncer) != 0) {
    fprintf(stderr, "orm_rehydrate_test: cannot attach the sync\n");
    exit(2);
  }
  ddl.sync = syncer;
  clpd_orm_sync_set_isolate_after(syncer, 1);
  clpd_orm_sync_set_clock(syncer, fake_clock, NULL);
  clpd_orm_sync_set_idle_after(syncer, IDLE);
  Reset(env);
  clpd_orm_tx_init(&tx, db, db_spec, 100, 30000);
}

static void stop_env(void) {
  clpd_ingest_clear();
  clpd_orm_sync_detach(syncer);
  syncer = NULL;
  ddl.sync = NULL;
  clpd_packages_free(&pkgs);
  DestroyEnvironment(env);
  env = NULL;
}

static void setup(const char *spec) {
  char msg[512];
  snprintf(pkg_dir, sizeof pkg_dir, "%s/rh", root);
  mkdir(pkg_dir, 0700);
  write_file(pkg_dir, "package.conf",
             "definition = rh\nversion = 1\nfiles = p.clp\norm = orm.conf\n"
             "ingest = order, event\n");
  write_file(pkg_dir, "p.clp", rh_clp);
  write_file(pkg_dir, "orm.conf", rh_orm);
  if (clpd_db_open(spec, &db, msg, sizeof msg) != CLPD_DB_OK) {
    fprintf(stderr, "orm_rehydrate_test: cannot open the database: %s\n",
            msg);
    exit(2);
  }
  snprintf(db_spec, sizeof db_spec, "%s", spec);
  pg = strcmp(clpd_db_backend(db), "postgresql") == 0;
  sql("DROP TABLE IF EXISTS rh_order");
  sql("DROP TABLE IF EXISTS workflow_instance");
  if (pg) sql("DROP SEQUENCE IF EXISTS rh_missing_seq");
  else sql("DROP TABLE IF EXISTS clpd_sequence");
  sql(pg ? "CREATE TABLE rh_order (wf_id TEXT NOT NULL, line_no BIGINT NOT "
           "NULL, status TEXT, PRIMARY KEY (wf_id, line_no))"
         : "CREATE TABLE rh_order (wf_id TEXT NOT NULL, line_no INTEGER NOT "
           "NULL, status TEXT, PRIMARY KEY (wf_id, line_no))");
  if (clpd_orm_tx_create_instance_table(db, msg, sizeof msg) != 0) {
    fprintf(stderr, "orm_rehydrate_test: workflow_instance: %s\n", msg);
    exit(2);
  }
  start_env();
}

/* --- tests ------------------------------------------------------------ */

/* No workflow_instance row: a new instance, asserted as before T11d; only
 * the first fact of an instance with no fact in memory reads the row, and
 * nothing is loaded. */
static void test_new_instance(void) {
  char log[4096], got[4096], want[4096];
  post("{\"template\":\"order\",\"slots\":{\"wf-id\":\"n1\",\"line-no\":1,"
       "\"status\":{\"sym\":\"new\"}}}");
  post("{\"template\":\"event\",\"slots\":{\"wf-id\":\"n1\",\"kind\":"
       "{\"sym\":\"look\"}}}");
  EXPECT(cycle(log, sizeof log) == 0);
  EXPECT(same("new instance: log", log, ""));
  facts_of("n1", got, sizeof got);
  EXPECT(same("new instance: facts", got, "order 1 new; event look; seen 1 "
                                          "new"));
  want[0] = '\0';
  reload_stmts(want, sizeof want, 0);
  EXPECT(strncmp(stmts, want, strlen(want)) == 0);
  EXPECT(occurrences(stmts, "\"workflow_instance\" WHERE") == 1);
  EXPECT(strstr(stmts, "FROM \"rh_order\"") == NULL &&
         strstr(stmts, "FROM \"public\".\"rh_order\"") == NULL);
  query("SELECT wf_id, line_no, status FROM rh_order WHERE wf_id = 'n1'",
        got, sizeof got);
  EXPECT(same("new instance: its row is written", got, "'n1' 1 'new'"));
  query("SELECT definition, version, status FROM workflow_instance WHERE "
        "wf_id = 'n1'",
        got, sizeof got);
  EXPECT(same("new instance: workflow_instance", got, "'rh' '1' 'active'"));
}

/* An instance with a fact in working memory is not reloaded: no
 * statement at all for the fact. */
static void test_in_memory(void) {
  char log[4096], got[4096], before[8192], after[8192];
  fingerprint(before, sizeof before);
  sql("UPDATE rh_order SET status = 'changed' WHERE wf_id = 'n1'");
  post("{\"template\":\"event\",\"slots\":{\"wf-id\":\"n1\",\"kind\":"
       "{\"sym\":\"ping\"}}}");
  EXPECT(cycle(log, sizeof log) == 0);
  EXPECT(same("in memory: log", log, ""));
  EXPECT(same("in memory: no statement", stmts, ""));
  facts_of("n1", got, sizeof got);
  EXPECT(same("in memory: the facts as they were, plus the event", got,
              "order 1 new; event look; seen 1 new; event ping"));
  sql("UPDATE rh_order SET status = 'new' WHERE wf_id = 'n1'");
  fingerprint(after, sizeof after);
  EXPECT(same("in memory: nothing written", after, before));
}

/* Evict then inject: the idle eviction removes the instance, another
 * writer changes a stored row, and a fact for it reloads the stored state
 * before the fact is asserted (the reloaded facts have the lower
 * indexes), so its rules see it; nothing is written. */
static void test_evict_then_inject(void) {
  char log[4096], got[4096], want[4096], before[8192], after[8192];
  now = 1000;
  post("{\"template\":\"order\",\"slots\":{\"wf-id\":\"e1\",\"line-no\":1,"
       "\"status\":{\"sym\":\"new\"}}}");
  post("{\"template\":\"order\",\"slots\":{\"wf-id\":\"e1\",\"line-no\":2,"
       "\"status\":{\"sym\":\"new\"}}}");
  EXPECT(cycle(log, sizeof log) == 0);
  now += IDLE;
  EXPECT(cycle(log, sizeof log) == 0);
  EXPECT(same("evict: the line",
              log, "clpdaemon: evict: instance 'e1' after 10 s without "
                   "activity\n"));
  facts_of("e1", got, sizeof got);
  EXPECT(same("evict: gone", got, ""));
  sql("UPDATE rh_order SET status = 'other' WHERE wf_id = 'e1' AND "
      "line_no = 2");
  fingerprint(before, sizeof before);
  post("{\"template\":\"event\",\"slots\":{\"wf-id\":\"e1\",\"kind\":"
       "{\"sym\":\"look\"}}}");
  EXPECT(cycle(log, sizeof log) == 0);
  EXPECT(same("evict then inject: log", log, ""));
  want[0] = '\0';
  reload_stmts(want, sizeof want, 1);
  EXPECT(same("evict then inject: the row, then the reload, nothing "
              "written",
              stmts, want));
  facts_of("e1", got, sizeof got);
  EXPECT(strncmp(got, "order 1 new; order 2 other; event look; ",
                 strlen("order 1 new; order 2 other; event look; ")) == 0);
  EXPECT(strstr(got, "seen 1 new") != NULL &&
         strstr(got, "seen 2 other") != NULL);
  if (strstr(got, "seen 2 other") == NULL)
    fprintf(stderr, "  evict then inject: facts [%s]\n", got);
  fingerprint(after, sizeof after);
  EXPECT(same("evict then inject: nothing written", after, before));
}

/* Restart then inject: a new daemon (environment, syncer) on the same
 * database has none of the instances in memory and reloads none at
 * startup; a fact for one reloads it like after an eviction. */
static void test_restart_then_inject(void) {
  char log[4096], got[4096], want[4096], before[8192], after[8192];
  stop_env();
  start_env();
  EXPECT(fact_count() == 0); /* no eager reload */
  fingerprint(before, sizeof before);
  post("{\"template\":\"event\",\"slots\":{\"wf-id\":\"n1\",\"kind\":"
       "{\"sym\":\"look\"}}}");
  EXPECT(cycle(log, sizeof log) == 0);
  EXPECT(same("restart then inject: log", log, ""));
  want[0] = '\0';
  reload_stmts(want, sizeof want, 1);
  EXPECT(same("restart then inject: statements", stmts, want));
  facts_of("n1", got, sizeof got);
  EXPECT(same("restart then inject: facts", got,
              "order 1 new; event look; seen 1 new"));
  EXPECT(fact_count() == 3);
  fingerprint(after, sizeof after);
  EXPECT(same("restart then inject: nothing written", after, before));
}

/* Finished instances: ended by wf-complete in this run, or completed or
 * failed in workflow_instance; their facts are dropped with the exact
 * line, nothing is asserted (or loaded) and nothing is written. */
static void test_finished(void) {
  char log[4096], got[4096], want[4096], before[8192], after[8192],
      big[251], json[512];
  size_t total;
  post("{\"template\":\"event\",\"slots\":{\"wf-id\":\"n1\",\"kind\":"
       "{\"sym\":\"done\"}}}");
  EXPECT(cycle(log, sizeof log) == 0);
  EXPECT(same("wf-complete: log", log, ""));
  facts_of("n1", got, sizeof got);
  EXPECT(same("wf-complete: gone", got, ""));
  order_row("ff", 1, "stored");
  instance_row("ff", "rh", "1", "failed");
  memset(big, 'y', 250);
  big[250] = '\0';
  instance_row(big, "rh", "1", "completed");
  instance_row("f\ax", "rh", "1", "completed");
  fingerprint(before, sizeof before);
  total = fact_count();
  post("{\"template\":\"event\",\"slots\":{\"wf-id\":\"n1\",\"kind\":"
       "{\"sym\":\"look\"}}}");
  post("{\"template\":\"order\",\"slots\":{\"wf-id\":\"ff\",\"line-no\":2}}");
  snprintf(json, sizeof json,
           "{\"template\":\"event\",\"slots\":{\"wf-id\":\"%s\"}}", big);
  post(json);
  post("{\"template\":\"event\",\"slots\":{\"wf-id\":\"f\\u0007x\"}}");
  EXPECT(cycle(log, sizeof log) == 0);
  big[200] = '\0';
  snprintf(want, sizeof want,
           "clpdaemon: ingest: instance 'n1' is completed; fact dropped\n"
           "clpdaemon: ingest: instance 'ff' is failed; fact dropped\n"
           "clpdaemon: ingest: instance '%s' is completed; fact dropped\n"
           "clpdaemon: ingest: instance 'f?x' is completed; fact dropped\n",
           big);
  EXPECT(same("finished: the drop lines", log, want));
  want[0] = '\0';
  reload_stmts(want, sizeof want, 0);
  reload_stmts(want, sizeof want, 0);
  reload_stmts(want, sizeof want, 0);
  reload_stmts(want, sizeof want, 0);
  EXPECT(same("finished: only the rows are read", stmts, want));
  EXPECT(fact_count() == total);
  facts_of("ff", got, sizeof got);
  EXPECT(same("finished: nothing of ff", got, ""));
  fingerprint(after, sizeof after);
  EXPECT(same("finished: nothing written", after, before));
}

/* An active row whose package (definition@version) is not loaded: the
 * fact is dropped with the exact line (the row's names shown sanitized),
 * nothing asserted, nothing written. */
static void test_package_not_loaded(void) {
  char log[4096], want[4096], before[8192], after[8192];
  size_t total;
  instance_row("p1", "z\tz", "9", "active");
  instance_row("p2", "rh", "2", "active");
  order_row("p2", 1, "stored");
  fingerprint(before, sizeof before);
  total = fact_count();
  post("{\"template\":\"event\",\"slots\":{\"wf-id\":\"p1\"}}");
  post("{\"template\":\"order\",\"slots\":{\"wf-id\":\"p2\",\"line-no\":2}}");
  EXPECT(cycle(log, sizeof log) == 0);
  EXPECT(same("package not loaded: lines", log,
              "clpdaemon: ingest: instance 'p1' belongs to package z?z@9, "
              "which is not loaded; fact dropped\n"
              "clpdaemon: ingest: instance 'p2' belongs to package rh@2, "
              "which is not loaded; fact dropped\n"));
  want[0] = '\0';
  reload_stmts(want, sizeof want, 0);
  reload_stmts(want, sizeof want, 0);
  EXPECT(same("package not loaded: statements", stmts, want));
  EXPECT(fact_count() == total);
  fingerprint(after, sizeof after);
  EXPECT(same("package not loaded: nothing written", after, before));
}

/* An isolated instance is never reloaded: its later fact is asserted as
 * before T11d (never written) without reading anything. */
static void test_isolated(void) {
  char log[4096], got[4096], want[4096], before[8192], after[8192];
  instance_row("i1", "rh", "1", "active");
  order_row("i1", 1, "stored");
  post("{\"template\":\"event\",\"slots\":{\"wf-id\":\"i1\",\"kind\":"
       "{\"sym\":\"fail\"}}}");
  EXPECT(cycle(log, sizeof log) == 1);
  want[0] = '\0';
  reload_stmts(want, sizeof want, 1);
  EXPECT(strncmp(stmts, want, strlen(want)) == 0);
  EXPECT(strstr(log, "clpdaemon: sync: instance 'i1' is no longer "
                     "synchronized: ") != NULL);
  facts_of("i1", got, sizeof got);
  EXPECT(same("isolated: evicted", got, ""));
  fingerprint(before, sizeof before);
  post("{\"template\":\"event\",\"slots\":{\"wf-id\":\"i1\",\"kind\":"
       "{\"sym\":\"look\"}}}");
  EXPECT(cycle(log, sizeof log) == 0);
  EXPECT(same("isolated: log", log, ""));
  EXPECT(same("isolated: no statement", stmts, ""));
  facts_of("i1", got, sizeof got);
  EXPECT(same("isolated: the fact alone, not reloaded", got, "event look"));
  fingerprint(after, sizeof after);
  EXPECT(same("isolated: nothing written", after, before));
}

/* A reload failure fails the cycle: the drain stops there (that fact and
 * the ones after it stay queued), the cycle is rolled back without Run.
 * Transient contention (T10f) isolates nothing and the retry reloads the
 * instance; another error isolates it, and the retry asserts the fact
 * without a reload. */
static void test_reload_failures(void) {
  char log[4096], got[4096], want[4096], before[8192], after[8192];
  /* (a) The SELECT of the load meets transient contention. */
  instance_row("t1", "rh", "1", "active");
  order_row("t1", 1, "stored");
  post("{\"template\":\"event\",\"slots\":{\"wf-id\":\"t1\",\"kind\":"
       "{\"sym\":\"look\"}}}");
  post("{\"template\":\"event\",\"slots\":{\"wf-id\":\"t1\",\"kind\":"
       "{\"sym\":\"ping\"}}}");
  fail_status = CLPD_DB_TRANSIENT;
  fail_at = 2;
  EXPECT(cycle(log, sizeof log) == 2);
  EXPECT(same("transient load: lines", log,
              "clpdaemon: rehydrate: instance 't1': rh@1::order: table "
              "'rh_order': injected\n"
              "clpdaemon: cycle transaction failed: injected; retrying in "
              "100 ms\n"));
  EXPECT(clpd_ingest_queue_length() == 2);
  facts_of("t1", got, sizeof got);
  EXPECT(same("transient load: nothing of t1", got, ""));
  EXPECT(!clpd_orm_sync_isolated(syncer, "t1"));
  EXPECT(cycle(log, sizeof log) == 0);
  EXPECT(same("transient load: the retry", log, ""));
  EXPECT(clpd_ingest_queue_length() == 0);
  facts_of("t1", got, sizeof got);
  EXPECT(same("transient load: reloaded on the retry", got,
              "order 1 stored; event look; event ping; seen 1 stored"));
  /* (b) The SELECT of the row meets transient contention. */
  instance_row("t2", "rh", "1", "active");
  order_row("t2", 1, "stored");
  post("{\"template\":\"event\",\"slots\":{\"wf-id\":\"t2\",\"kind\":"
       "{\"sym\":\"look\"}}}");
  fail_status = CLPD_DB_TRANSIENT;
  fail_at = 1;
  EXPECT(cycle(log, sizeof log) == 2);
  EXPECT(same("transient row: lines", log,
              "clpdaemon: rehydrate: instance 't2': table "
              "'workflow_instance': injected\n"
              "clpdaemon: cycle transaction failed: injected; retrying in "
              "100 ms\n"));
  EXPECT(clpd_ingest_queue_length() == 1);
  EXPECT(cycle(log, sizeof log) == 0);
  EXPECT(same("transient row: the retry", log, ""));
  facts_of("t2", got, sizeof got);
  EXPECT(same("transient row: reloaded on the retry", got,
              "order 1 stored; event look; seen 1 stored"));
  /* (c) The SELECT of the load fails for good: isolated. */
  instance_row("x1", "rh", "1", "active");
  order_row("x1", 1, "stored");
  post("{\"template\":\"event\",\"slots\":{\"wf-id\":\"x1\",\"kind\":"
       "{\"sym\":\"look\"}}}");
  fail_status = CLPD_DB_ERROR;
  fail_at = 2;
  EXPECT(cycle(log, sizeof log) == 2);
  EXPECT(same("failed load: lines", log,
              "clpdaemon: rehydrate: instance 'x1': rh@1::order: table "
              "'rh_order': injected\n"
              "clpdaemon: sync: instance 'x1' is no longer synchronized: "
              "injected\n"
              "clpdaemon: cycle transaction failed: injected; retrying in "
              "100 ms\n"));
  EXPECT(clpd_ingest_queue_length() == 1);
  EXPECT(clpd_orm_sync_isolated(syncer, "x1"));
  fingerprint(before, sizeof before);
  EXPECT(cycle(log, sizeof log) == 0);
  EXPECT(same("failed load: the retry", log, ""));
  EXPECT(same("failed load: the retry reads nothing", stmts, ""));
  facts_of("x1", got, sizeof got);
  EXPECT(same("failed load: the fact alone", got, "event look"));
  fingerprint(after, sizeof after);
  EXPECT(same("failed load: nothing written", after, before));
  (void)want;
}

/* Without a database there is no syncer: every fact is asserted. */
static void test_no_database(void) {
  CLIPSValue v;
  FILE *f = tmpfile();
  if (f == NULL) exit(2);
  v.lexemeValue = CreateString(env, "nd");
  EXPECT(clpd_orm_sync_admit(NULL, &v, f) == CLPD_INGEST_ASSERT);
  EXPECT(ftell(f) == 0);
  fclose(f);
}

int main(int argc, char **argv) {
  char spec[sizeof root + 32];
  if (argc != 1 && argc != 3) {
    fprintf(stderr, "usage: orm_rehydrate_test [<conninfo> <password>]\n");
    return 2;
  }
  if (mkdtemp(root) == NULL) {
    perror("orm_rehydrate_test");
    return 2;
  }
  snprintf(spec, sizeof spec, "sqlite:%s/rehydrate.db", root);
  setup(argc == 3 ? argv[1] : spec);
  test_new_instance();
  test_in_memory();
  test_evict_then_inject();
  test_restart_then_inject();
  test_finished();
  test_package_not_loaded();
  test_isolated();
  test_reload_failures();
  test_no_database();
  stop_env();
  sql("DROP TABLE rh_order");
  sql("DROP TABLE workflow_instance");
  if (!pg) sql("DROP TABLE IF EXISTS clpd_sequence");
  clpd_db_close(db);
  if (failures != 0) {
    fprintf(stderr, "%d orm rehydrate checks failed (fixtures in %s)\n",
            failures, root);
    return 1;
  }
  {
    char cmd[sizeof root + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0) fprintf(stderr, "cannot remove %s\n", root);
  }
  puts("orm rehydrate checks passed");
  return 0;
}
