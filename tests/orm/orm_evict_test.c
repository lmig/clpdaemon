/* Tests for the idle eviction (T11c, FR-9, src/orm_sync.h
 * clpd_orm_sync_evict_idle, src/orm_evict.h, docs/orm.md#idle-eviction).
 * usage: orm_evict_test                         (a temporary SQLite file)
 *        orm_evict_test <conninfo> <password>   (PostgreSQL, run by
 *                                                tests/db/pg_cluster.sh)
 * A real CLIPS environment loads one package (ev@1) with the ORM functions
 * and the synchronization attached as in the daemon; the monotonic clock
 * of the syncer is replaced by `now` (milliseconds) so every idle time is
 * exact. A cycle is what run_cycle does: the sweep at its start
 * (clpd_orm_sync_evict_idle), clpd_orm_sync_begin_cycle, the changes, Run,
 * clpd_orm_sync_end_cycle. Everything is observed through the public
 * interface: the daemon log, the facts in working memory, the rows read
 * back with SELECT, and the statements sent (clpd_db_exec is wrapped,
 * -Wl,--wrap, this test binary only, to record them and to report
 * transient contention for one of them). */
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
#include <unistd.h>

#include "expect.h"

#define IDLE 10000LL /* the idle period of the tests, ms */

static char root[] = "/tmp/clpd_orm_evict_test.XXXXXX";
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
 * appended to stmts, one per line; the transient_at-th of them (1-based)
 * returns CLPD_DB_TRANSIENT without running (T10f), which rolls the cycle
 * back and evicts and reloads the instances it touched. */
static int recording, transient_at, seen;
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
    if (++seen == transient_at) {
      if (d->in_tx && d->atomic && d->failed == CLPD_DB_OK) {
        __real_clpd_db_exec(d, "SELECT * FROM clpd_injected_failure", NULL, 0,
                            NULL);
        d->failed = CLPD_DB_TRANSIENT;
        snprintf(d->failed_error, sizeof d->failed_error, "injected");
      }
      if (result != NULL) *result = NULL;
      clpd_db_set_error(d, "injected");
      return CLPD_DB_TRANSIENT;
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
    perror("orm_evict_test");
    exit(2);
  }
}

/* order: mapped, sync = auto; note: mapped, sync = manual; memo: not
 * mapped. */
static const char *ev_clp =
    "(deftemplate order (slot wf-id) (slot line-no (type INTEGER))\n"
    "  (slot status (type SYMBOL)))\n"
    "(deftemplate note (slot wf-id) (slot text))\n"
    "(deftemplate memo (slot wf-id))\n"
    "(defrule ping (order (wf-id ?w) (status ping)) =>)\n"
    "(defrule load (order (wf-id ?w) (status load)) =>\n"
    "  (db-load order (create$ wf-id ld)))\n"
    "(defrule fail (order (wf-id ?w) (status fail)) =>\n"
    "  (db-nextval missing_no))\n";
static const char *ev_orm =
    "order.table = ev_order\n"
    "order.owned = yes\n"
    "order.key = line-no\n"
    "order.slot.wf-id = wf_id text string error\n"
    "order.slot.line-no = line_no bigint integer error\n"
    "order.slot.status = status text symbol default\n"
    "note.table = ev_note\n"
    "note.owned = yes\n"
    "note.sync = manual\n"
    "note.slot.wf-id = wf_id text string error\n"
    "note.slot.text = body text string nil\n"
    "seq.missing_no = ev_missing_seq\n"
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
    fprintf(stderr, "orm_evict_test: %s: %s\n", text, clpd_db_error(db));
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
      snprintf(out + len, cap - len, "%s", j > 0 ? " " : i > 0 ? "; " : "");
      put_value(out, cap, clpd_db_result_value(r, i, j));
    }
  }
  clpd_db_result_free(r);
}

/* Every table of the test with every row (timestamps included). */
static void fingerprint(char *out, size_t cap) {
  static const char *const q[] = {
      "SELECT * FROM ev_order ORDER BY 1, 2",
      "SELECT * FROM ev_note ORDER BY 1",
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

/* The facts whose wf-id is the symbol or string wf: "<template> <line-no
 * or ->; ..." in fact-index order. */
static void facts_of(const char *wf, char *out, size_t cap) {
  Fact *f;
  out[0] = '\0';
  for (f = GetNextFact(env, NULL); f != NULL; f = GetNextFact(env, f)) {
    Deftemplate *d = FactDeftemplate(f);
    CLIPSValue v;
    size_t len;
    if (!DeftemplateSlotExistP(d, "wf-id") ||
        GetFactSlot(f, "wf-id", &v) != GSE_NO_ERROR ||
        (v.header->type != SYMBOL_TYPE && v.header->type != STRING_TYPE) ||
        strcmp(v.lexemeValue->contents, wf) != 0)
      continue;
    len = strlen(out);
    snprintf(out + len, cap - len, "%s%s", len > 0 ? "; " : "",
             DeftemplateName(d));
    if (DeftemplateSlotExistP(d, "line-no") &&
        GetFactSlot(f, "line-no", &v) == GSE_NO_ERROR &&
        v.header->type == INTEGER_TYPE) {
      len = strlen(out);
      snprintf(out + len, cap - len, " %lld", v.integerValue->contents);
    }
  }
}

static size_t fact_count(void) {
  Fact *f;
  size_t n = 0;
  for (f = GetNextFact(env, NULL); f != NULL; f = GetNextFact(env, f)) n++;
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
  transient_at = 0;
  clpd_log_set(NULL);
  fflush(cycle_log);
  rewind(cycle_log);
  n = fread(log, 1, cap - 1, cycle_log);
  log[n] = '\0';
  fclose(cycle_log);
}

/* Evaluates one expression in the package module (outside a cycle too). */
static void act(const char *expr) {
  CLIPSValue v;
  SetCurrentModule(env, pkgs.items[0].module);
  if (Eval(env, expr, &v) != EE_NO_ERROR) {
    fprintf(stderr, "  cannot evaluate %s\n", expr);
    failures++;
  }
  SetCurrentModule(env, FindDefmodule(env, "MAIN"));
  clear_flags();
}

/* The sweep alone, at the current time: its log in log, the number of
 * instances evicted returned; it must send no statement. */
static int sweep(char *log, size_t cap) {
  int n;
  open_log();
  n = clpd_orm_sync_evict_idle(syncer, cycle_log);
  close_log(log, cap);
  if (stmts[0] != '\0') {
    fprintf(stderr, "  the sweep sent statements: [%s]\n", stmts);
    failures++;
  }
  return n;
}

static void begin_cycle(void) {
  open_log();
  if (clpd_orm_sync_begin_cycle(syncer, &tx, cycle_log) != 0) {
    fprintf(stderr, "orm_evict_test: the cycle transaction did not begin\n");
    exit(2);
  }
}

/* Run, flush and commit: 0 committed, 1 rolled back; the log in log. */
static int end_cycle(char *log, size_t cap) {
  int rc;
  clpd_packages_focus(env, &pkgs);
  Run(env, -1);
  clear_flags();
  rc = clpd_orm_sync_end_cycle(syncer, &tx, cycle_log) == 0 ? 0 : 1;
  close_log(log, cap);
  return rc;
}

/* A cycle with one expression (NULL: no change). */
static int cycle(const char *expr, char *log, size_t cap) {
  begin_cycle();
  if (expr != NULL) act(expr);
  return end_cycle(log, cap);
}

/* The eviction line of instance wf after n seconds, appended to out. */
static void evicted(char *out, size_t cap, const char *wf, int n) {
  size_t len = strlen(out);
  snprintf(out + len, cap - len,
           "clpdaemon: evict: instance '%s' after %d s without activity\n",
           wf, n);
}

/* --- setup ------------------------------------------------------------ */

static void setup(const char *spec) {
  char dir[sizeof root + 16], msg[512], eb[4096];
  const char *dirs[1];
  clpd_list list;
  FILE *err;
  size_t n;

  snprintf(dir, sizeof dir, "%s/ev", root);
  mkdir(dir, 0700);
  write_file(dir, "package.conf",
             "definition = ev\nversion = 1\nfiles = p.clp\norm = orm.conf\n");
  write_file(dir, "p.clp", ev_clp);
  write_file(dir, "orm.conf", ev_orm);
  if (clpd_db_open(spec, &db, msg, sizeof msg) != CLPD_DB_OK) {
    fprintf(stderr, "orm_evict_test: cannot open the database: %s\n", msg);
    exit(2);
  }
  snprintf(db_spec, sizeof db_spec, "%s", spec);
  pg = strcmp(clpd_db_backend(db), "postgresql") == 0;
  sql("DROP TABLE IF EXISTS ev_order");
  sql("DROP TABLE IF EXISTS ev_note");
  sql("DROP TABLE IF EXISTS workflow_instance");
  if (pg) sql("DROP SEQUENCE IF EXISTS ev_missing_seq");
  else sql("DROP TABLE IF EXISTS clpd_sequence");
  sql(pg ? "CREATE TABLE ev_order (wf_id TEXT NOT NULL, line_no BIGINT NOT "
           "NULL, status TEXT, PRIMARY KEY (wf_id, line_no))"
         : "CREATE TABLE ev_order (wf_id TEXT NOT NULL, line_no INTEGER NOT "
           "NULL, status TEXT, PRIMARY KEY (wf_id, line_no))");
  sql("CREATE TABLE ev_note (wf_id TEXT NOT NULL PRIMARY KEY, body TEXT)");
  if (clpd_orm_tx_create_instance_table(db, msg, sizeof msg) != 0) {
    fprintf(stderr, "orm_evict_test: workflow_instance: %s\n", msg);
    exit(2);
  }
  env = CreateEnvironment();
  ddl.db = db;
  ddl.packages = &pkgs;
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
    fprintf(stderr, "orm_evict_test: the package did not load:\n%s", eb);
    exit(2);
  }
  fclose(err);
  if (clpd_orm_sync_attach(env, db, &pkgs, &syncer) != 0) {
    fprintf(stderr, "orm_evict_test: cannot attach the sync\n");
    exit(2);
  }
  ddl.sync = syncer;
  clpd_orm_sync_set_isolate_after(syncer, 1);
  clpd_orm_sync_set_clock(syncer, fake_clock, NULL);
  clpd_orm_sync_set_idle_after(syncer, IDLE);
  Reset(env);
  clpd_orm_tx_init(&tx, db, db_spec, 100, 30000);
}

/* --- tests ------------------------------------------------------------ */

/* An idle instance whose facts are all of a sync = auto template is
 * evicted at the first sweep once it has been idle for the idle period
 * (not one millisecond before): the exact line, its facts gone, its rows
 * and workflow_instance row unchanged, no statement sent, and the next
 * cycle writes nothing either (no DELETE). Several instances: one line
 * each, the oldest activity first; then the instances are forgotten. */
static void test_evicted(void) {
  char log[4096], want[4096], got[4096], before[8192], after[8192];
  size_t total;
  now = 1000;
  EXPECT(cycle("(progn (assert (order (wf-id \"e1\") (line-no 1) (status "
               "new))) (assert (order (wf-id \"e1\") (line-no 2) (status "
               "new))))",
               log, sizeof log) == 0);
  now = 1500;
  EXPECT(cycle("(assert (order (wf-id \"e2\") (line-no 1) (status new)))",
               log, sizeof log) == 0);
  fingerprint(before, sizeof before);
  total = fact_count();
  now = 1000 + IDLE - 1;
  EXPECT(sweep(log, sizeof log) == 0);
  EXPECT(same("not idle yet", log, ""));
  facts_of("e1", got, sizeof got);
  EXPECT(same("e1 kept", got, "order 1; order 2"));
  now = 1500 + IDLE + 2400;
  EXPECT(sweep(log, sizeof log) == 2);
  want[0] = '\0';
  evicted(want, sizeof want, "e1", 12);
  evicted(want, sizeof want, "e2", 12);
  EXPECT(same("eviction lines", log, want));
  facts_of("e1", got, sizeof got);
  EXPECT(same("e1 evicted", got, ""));
  facts_of("e2", got, sizeof got);
  EXPECT(same("e2 evicted", got, ""));
  EXPECT(fact_count() == total - 3);
  fingerprint(after, sizeof after);
  EXPECT(same("rows unchanged by the eviction", after, before));
  EXPECT(cycle(NULL, log, sizeof log) == 0);
  EXPECT(same("nothing written after the eviction", stmts, ""));
  EXPECT(same("the next cycle's log", log, ""));
  fingerprint(after, sizeof after);
  EXPECT(same("rows unchanged after the next cycle", after, before));
  now += 5 * IDLE;
  EXPECT(sweep(log, sizeof log) == 0);
  EXPECT(same("evicted instances are forgotten", log, ""));
}

/* An instance with an unmapped fact, a sync = manual fact, a recorded
 * change not written yet, or isolated, stays in memory; once the reason
 * is gone (the change committed, the unmapped fact retracted) it is
 * evicted one idle period after its last check or activity. */
static void test_not_evicted(void) {
  char log[4096], want[4096], got[4096], before[8192], after[8192];
  now = 100000;
  EXPECT(cycle("(progn (assert (order (wf-id \"u1\") (line-no 1))) "
               "(assert (memo (wf-id \"u1\"))) "
               "(assert (order (wf-id \"m1\") (line-no 1))) "
               "(assert (note (wf-id \"m1\") (text \"t\"))) "
               "(assert (order (wf-id \"p1\") (line-no 1) (status new))))",
               log, sizeof log) == 0);
  /* A change of p1 recorded outside a cycle: written by the next one. */
  act("(do-for-fact ((?f order)) (eq ?f:wf-id \"p1\") "
      "(modify ?f (status changed)))");
  /* i1 is isolated by the poison retry (instance_isolate_after = 1); a
   * fact asserted for it later stays in memory, never written. */
  EXPECT(cycle("(assert (order (wf-id \"i1\") (line-no 1) (status fail)))",
               log, sizeof log) == 1);
  EXPECT(strstr(log, "clpdaemon: sync: instance 'i1' is no longer "
                     "synchronized: ") != NULL);
  act("(assert (order (wf-id \"i1\") (line-no 2) (status late)))");
  fingerprint(before, sizeof before);
  now = 100000 + 3 * IDLE;
  EXPECT(sweep(log, sizeof log) == 0);
  EXPECT(same("nothing evicted", log, ""));
  facts_of("u1", got, sizeof got);
  EXPECT(same("u1 kept (unmapped fact)", got, "order 1; memo"));
  facts_of("m1", got, sizeof got);
  EXPECT(same("m1 kept (manual fact)", got, "order 1; note"));
  facts_of("p1", got, sizeof got);
  EXPECT(same("p1 kept (pending change)", got, "order 1"));
  facts_of("i1", got, sizeof got);
  EXPECT(same("i1 kept (isolated)", got, "order 2"));
  fingerprint(after, sizeof after);
  EXPECT(same("rows unchanged", after, before));
  /* The next cycle writes p1's change; u1's memo goes. */
  EXPECT(cycle("(do-for-fact ((?f memo)) (eq ?f:wf-id \"u1\") (retract ?f))",
               log, sizeof log) == 0);
  EXPECT(strstr(stmts, "UPDATE") != NULL);
  /* p1 is checked again one idle period after the last check; u1 counts
   * from the retract of its memo. */
  now = 100000 + 4 * IDLE - 1;
  EXPECT(sweep(log, sizeof log) == 0);
  EXPECT(same("not yet", log, ""));
  now = 100000 + 4 * IDLE;
  EXPECT(sweep(log, sizeof log) == 2);
  want[0] = '\0';
  evicted(want, sizeof want, "p1", 40);
  evicted(want, sizeof want, "u1", 10);
  EXPECT(same("evicted once the reason is gone", log, want));
  now += 10 * IDLE;
  EXPECT(sweep(log, sizeof log) == 0);
  EXPECT(same("m1 and i1 still not evicted", log, ""));
  facts_of("m1", got, sizeof got);
  EXPECT(same("m1 still there", got, "order 1; note"));
  facts_of("i1", got, sizeof got);
  EXPECT(same("i1 still there", got, "order 2"));
  /* Clean up for the next tests (with the recording suspended). */
  clpd_orm_sync_suspend(syncer);
  act("(do-for-all-facts ((?f order note)) (or (eq ?f:wf-id \"m1\") "
      "(eq ?f:wf-id \"i1\")) (retract ?f))");
  clpd_orm_sync_resume(syncer);
}

/* Each source of activity resets the idle time: an assert, a modify and a
 * retract of a fact of the instance, a rule firing for it, facts loaded
 * for it by db-load and by the rehydration, and a fact asserted with the
 * recording suspended. c0, without activity, is the control. */
static void test_activity(void) {
  char log[4096], want[4096], got[4096];
  long long t0 = 1000000, t1 = t0 + IDLE / 2;
  now = t0;
  EXPECT(cycle("(progn (assert (order (wf-id \"c0\") (line-no 1))) "
               "(assert (order (wf-id \"aa\") (line-no 1))) "
               "(assert (order (wf-id \"am\") (line-no 1))) "
               "(assert (order (wf-id \"ar\") (line-no 1))) "
               "(assert (order (wf-id \"ar\") (line-no 2))) "
               "(assert (order (wf-id \"af\") (line-no 1) (status ping))) "
               "(assert (order (wf-id \"ld\") (line-no 1))) "
               "(assert (order (wf-id \"as\") (line-no 1))) "
               "(assert (order (wf-id \"rh\") (line-no 1))))",
               log, sizeof log) == 0);
  /* A row of ld written by another writer, loaded by db-load later. */
  sql("INSERT INTO ev_order (wf_id, line_no, status) VALUES ('ld', 2, "
      "'other')");
  /* rh's modify meets transient contention: the rollback evicts rh, the
   * next cycle start reloads it. */
  begin_cycle();
  act("(do-for-fact ((?f order)) (eq ?f:wf-id \"rh\") "
      "(modify ?f (status changed)))");
  transient_at = 1;
  EXPECT(end_cycle(log, sizeof log) == 1);
  facts_of("rh", got, sizeof got);
  EXPECT(same("rh evicted by the rollback", got, ""));
  now = t1;
  begin_cycle(); /* reloads rh */
  act("(assert (order (wf-id \"aa\") (line-no 2)))");
  act("(do-for-fact ((?f order)) (eq ?f:wf-id \"am\") "
      "(modify ?f (status modified)))");
  act("(do-for-fact ((?f order)) (and (eq ?f:wf-id \"ar\") "
      "(eq ?f:line-no 2)) (retract ?f))");
  act("(refresh ping)");
  act("(assert (order (wf-id \"lt\") (line-no 1) (status load)))");
  EXPECT(end_cycle(log, sizeof log) == 0);
  facts_of("rh", got, sizeof got);
  EXPECT(same("rh reloaded", got, "order 1"));
  facts_of("ld", got, sizeof got);
  EXPECT(same("ld loaded", got, "order 1; order 2"));
  clpd_orm_sync_suspend(syncer);
  act("(assert (order (wf-id \"as\") (line-no 2)))");
  clpd_orm_sync_resume(syncer);
  now = t0 + IDLE;
  EXPECT(sweep(log, sizeof log) == 1);
  want[0] = '\0';
  evicted(want, sizeof want, "c0", 10);
  EXPECT(same("only the control is idle", log, want));
  now = t1 + IDLE - 1;
  EXPECT(sweep(log, sizeof log) == 0);
  EXPECT(same("one ms before", log, ""));
  now = t1 + IDLE;
  EXPECT(sweep(log, sizeof log) == 8);
  want[0] = '\0';
  evicted(want, sizeof want, "rh", 10);
  evicted(want, sizeof want, "aa", 10);
  evicted(want, sizeof want, "am", 10);
  evicted(want, sizeof want, "ar", 10);
  EXPECT(strncmp(log, want, strlen(want)) == 0);
  {
    static const char *const rest[] = {"af", "lt", "ld", "as"};
    size_t i;
    for (i = 0; i < sizeof rest / sizeof rest[0]; i++) {
      want[0] = '\0';
      evicted(want, sizeof want, rest[i], 10);
      if (strstr(log, want) == NULL) {
        fprintf(stderr, "  no eviction line for %s in [%s]\n", rest[i], log);
        failures++;
      }
    }
  }
  {
    static const char *const all[] = {"c0", "aa", "am", "ar", "af",
                                      "ld", "as", "rh", "lt"};
    size_t i;
    for (i = 0; i < sizeof all / sizeof all[0]; i++) {
      facts_of(all[i], got, sizeof got);
      if (got[0] != '\0') {
        fprintf(stderr, "  %s not evicted: [%s]\n", all[i], got);
        failures++;
      }
    }
  }
  EXPECT(cycle(NULL, log, sizeof log) == 0);
  EXPECT(same("nothing written", stmts, ""));
}

/* The wf-id in the line is shown like the rehydrate: lines: control
 * characters as '?', at most 200 bytes. */
static void test_shown(void) {
  char log[4096], want[4096], big[251], expr[512];
  now = 5000000;
  memset(big, 'x', 250);
  big[250] = '\0';
  snprintf(expr, sizeof expr,
           "(progn (assert (order (wf-id \"s\n1\tz\") (line-no 1))) "
           "(assert (order (wf-id \"%s\") (line-no 1))))",
           big);
  EXPECT(cycle(expr, log, sizeof log) == 0);
  now += IDLE;
  EXPECT(sweep(log, sizeof log) == 2);
  big[200] = '\0';
  want[0] = '\0';
  evicted(want, sizeof want, "s?1?z", 10);
  evicted(want, sizeof want, big, 10);
  EXPECT(same("sanitized wf-ids", log, want));
}

/* No activity is kept for an instance a lifecycle function ended: a fact
 * asserted for it later is never evicted; an instance with no fact left
 * is forgotten without a line. */
static void test_finished_and_empty(void) {
  char log[4096], got[4096];
  now = 6000000;
  EXPECT(cycle("(progn (assert (order (wf-id \"f1\") (line-no 1))) "
               "(assert (order (wf-id \"z1\") (line-no 1))))",
               log, sizeof log) == 0);
  begin_cycle();
  clpd_orm_sync_finish(syncer, "f1");
  EXPECT(end_cycle(log, sizeof log) == 0);
  act("(assert (order (wf-id \"f1\") (line-no 9)))");
  EXPECT(cycle("(do-for-fact ((?f order)) (eq ?f:wf-id \"z1\") (retract ?f))",
               log, sizeof log) == 0);
  EXPECT(strstr(stmts, "DELETE") != NULL);
  now += 3 * IDLE;
  EXPECT(sweep(log, sizeof log) == 0);
  EXPECT(same("no line", log, ""));
  facts_of("f1", got, sizeof got);
  EXPECT(same("the finished instance's late fact stays", got, "order 9"));
  clpd_orm_sync_suspend(syncer);
  act("(do-for-all-facts ((?f order)) (eq ?f:wf-id \"f1\") (retract ?f))");
  clpd_orm_sync_resume(syncer);
}

/* Without a database there is no syncer: the sweep does nothing. */
static void test_no_database(void) {
  FILE *f = tmpfile();
  if (f == NULL) exit(2);
  EXPECT(clpd_orm_sync_evict_idle(NULL, f) == 0);
  EXPECT(ftell(f) == 0);
  fclose(f);
}

int main(int argc, char **argv) {
  char spec[sizeof root + 32];
  if (argc != 1 && argc != 3) {
    fprintf(stderr, "usage: orm_evict_test [<conninfo> <password>]\n");
    return 2;
  }
  if (mkdtemp(root) == NULL) {
    perror("orm_evict_test");
    return 2;
  }
  snprintf(spec, sizeof spec, "sqlite:%s/evict.db", root);
  setup(argc == 3 ? argv[1] : spec);
  test_evicted();
  test_not_evicted();
  test_activity();
  test_shown();
  test_finished_and_empty();
  test_no_database();
  clpd_orm_sync_detach(syncer);
  ddl.sync = NULL;
  clpd_packages_free(&pkgs);
  DestroyEnvironment(env);
  sql("DROP TABLE ev_order");
  sql("DROP TABLE ev_note");
  sql("DROP TABLE workflow_instance");
  clpd_db_close(db);
  if (failures != 0) {
    fprintf(stderr, "%d orm evict checks failed (fixtures in %s)\n", failures,
            root);
    return 1;
  }
  {
    char cmd[sizeof root + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0) fprintf(stderr, "cannot remove %s\n", root);
  }
  puts("orm evict checks passed");
  return 0;
}
