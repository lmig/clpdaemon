/* Tests for the automatic synchronization (T10b, src/orm_sync.h,
 * docs/orm.md#automatic-synchronization), the loader (T10c,
 * src/orm_load.h, docs/orm.md#loading-rows) and the rehydration after a
 * rolled-back cycle (T10c, docs/orm.md#rehydration).
 * usage: orm_sync_test                         (a temporary SQLite file)
 *        orm_sync_test <conninfo> <password>   (PostgreSQL, run by
 *                                               tests/db/pg_cluster.sh)
 * A real CLIPS environment loads two packages (shop@1 and crm@2, two
 * aggregates); a cycle is what the daemon's run_cycle does:
 * clpd_orm_sync_begin_cycle (the rehydration of the instances a rolled-back
 * cycle touched, then the begin), the changes (top-level Eval standing in
 * for a plugin, or the real ingest drain), Run, then
 * clpd_orm_sync_end_cycle (the flush and the commit). The rows are read
 * back with SELECT after each cycle. clpd_db_exec is wrapped (-Wl,--wrap,
 * this test binary only) to record the statements the sync and the loader
 * send and to make one of them fail on demand (or report a lost
 * connection or transient contention, T10f). */
#define _POSIX_C_SOURCE 200809L

#include "clock.h"
#include "db_backend.h"
#include "ingest.h"
#include "log.h"
#include "orm_ddl.h"
#include "orm_load.h"
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
 * replaced by a statement that fails on the database, the lost_at-th
 * returns CLPD_DB_LOST with "injected lost connection" and the
 * transient_at-th CLPD_DB_TRANSIENT with "injected transient failure"
 * (T10f), both without running it. */
static int recording, fail_at, lost_at, transient_at, seen;
static char stmts[16384];

clpd_db_status __real_clpd_db_exec(clpd_db *d, const char *sql,
                                   const clpd_db_value *params, size_t n,
                                   clpd_db_result **result);
clpd_db_status __wrap_clpd_db_exec(clpd_db *d, const char *sql,
                                   const clpd_db_value *params, size_t n,
                                   clpd_db_result **result);

/* An injected failure with status st: inside an atomic transaction that
 * has no failure yet, the transaction is rolled back as for a real one
 * (the failing statement runs) and st with message becomes its first
 * failure, which the commit returns (src/db.h). */
static clpd_db_status inject(clpd_db *d, clpd_db_status st,
                             const char *message, clpd_db_result **result) {
  if (d->in_tx && d->atomic && d->failed == CLPD_DB_OK) {
    __real_clpd_db_exec(d, "SELECT * FROM clpd_injected_failure", NULL, 0,
                        NULL);
    d->failed = st;
    snprintf(d->failed_error, sizeof d->failed_error, "%s", message);
  }
  if (result != NULL) *result = NULL;
  clpd_db_set_error(d, "%s", message);
  return st;
}

clpd_db_status __wrap_clpd_db_exec(clpd_db *d, const char *sql,
                                   const clpd_db_value *params, size_t n,
                                   clpd_db_result **result) {
  if (recording && strcmp(sql, "SELECT current_schema()") != 0) {
    size_t len = strlen(stmts);
    snprintf(stmts + len, sizeof stmts - len, "%s\n", sql);
    if (++seen == fail_at)
      return __real_clpd_db_exec(d, "SELECT * FROM clpd_injected_failure",
                                 NULL, 0, result);
    if (seen == lost_at)
      return inject(d, CLPD_DB_LOST, "injected lost connection", result);
    if (seen == transient_at)
      return inject(d, CLPD_DB_TRANSIENT, "injected transient failure",
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
    "(deftemplate item (slot wf-id) (slot n (type INTEGER))\n"
    "  (slot qty (type INTEGER) (default 7))\n"
    "  (slot label (type SYMBOL) (allowed-symbols nil xyz abc) (default xyz))\n"
    "  (slot req) (slot price (type FLOAT) (default 1.5)))\n"
    "(deffacts seed (order (wf-id w0) (line-no 1) (status new)))\n"
    "(defrule fail (go (wf-id ?w) (act fail)) =>\n"
    "  (assert (order (wf-id ?w) (line-no 99))) (db-nextval missing_no))\n"
    "(defrule poison ?o <- (order (wf-id ?w) (status poison)) =>\n"
    "  (modify ?o (status cured)) (db-nextval missing_no))\n"
    "(defrule anon (go (wf-id ?w) (act anon)) => (db-nextval missing_no))\n";
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
    "item.table = shop_item\n"
    "item.owned = yes\n"
    "item.key = n\n"
    "item.slot.wf-id = wf_id text symbol error\n"
    "item.slot.n = n bigint integer error\n"
    "item.slot.qty = qty bigint integer default\n"
    "item.slot.label = label text symbol nil\n"
    "item.slot.req = req text string error\n"
    "item.slot.price = price double float default\n"
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
                                     "shop_item", "crm_party",
                                     "workflow_instance"};

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

static void open_log(void) {
  cycle_log = tmpfile();
  if (cycle_log == NULL) exit(2);
  clpd_log_set(cycle_log);
  stmts[0] = '\0';
  seen = 0;
  recording = 1;
}

/* The daemon log since open_log into log; the seams are reset. */
static void close_log(char *log, size_t cap) {
  size_t n;
  recording = 0;
  fail_at = 0;
  lost_at = 0;
  transient_at = 0;
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
}

static void begin_cycle(void) {
  open_log();
  if (clpd_orm_sync_begin_cycle(syncer, &tx, cycle_log) != 0) {
    fprintf(stderr, "orm_sync_test: the cycle transaction did not begin\n");
    exit(2);
  }
}

/* The start of a cycle that may fail (a rehydration that lost the
 * connection): returns 0 when the cycle began (end it with end_cycle),
 * otherwise -1 with the log in log. */
static int try_begin(char *log, size_t cap) {
  open_log();
  if (clpd_orm_sync_begin_cycle(syncer, &tx, cycle_log) == 0) return 0;
  close_log(log, cap);
  return -1;
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
  int rc;
  clpd_packages_focus(env, &pkgs);
  Run(env, -1);
  clear_flags();
  rc = clpd_orm_sync_end_cycle(syncer, &tx, cycle_log) == 0 ? 0 : 1;
  close_log(log, cap);
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

static void actf(size_t pkg, const char *fmt, int k) {
  char e[256];
  snprintf(e, sizeof e, fmt, k);
  act(pkg, e);
}

/* Appends the line of an instance no longer synchronized. */
static void isolated(char *out, size_t cap, const char *wf, const char *why) {
  size_t len = strlen(out);
  snprintf(out + len, cap - len, "clpdaemon: sync: instance '%s' is no "
           "longer synchronized: %s\n", wf, why);
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
 * "<template>(<slot> <value> ...); ...". */
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

/* The SELECTs of the loader for the four mapped templates of shop, in
 * mapping order: the rehydration of a shop instance. */
#define SEL_ORDER                                                          \
  "SELECT \"wf_id\", \"line_no\", \"status\", \"amount\", \"note\", "     \
  "\"paid\", \"data\" FROM @\"shop_order\" WHERE \"wf_id\" = $1 ORDER BY "  \
  "\"wf_id\", \"line_no\"\n"
#define SEL_NOTE                                                           \
  "SELECT \"wf_id\", \"body\" FROM @\"shop_note\" WHERE \"wf_id\" = $1 "   \
  "ORDER BY \"wf_id\"\n"
#define SEL_SMALL                                                          \
  "SELECT \"wf_id\", \"n\" FROM @\"shop_small\" WHERE \"wf_id\" = $1 "     \
  "ORDER BY \"wf_id\"\n"
#define SEL_ITEM                                                           \
  "SELECT \"wf_id\", \"n\", \"qty\", \"label\", \"req\", \"price\" FROM "  \
  "@\"shop_item\" WHERE \"wf_id\" = $1 ORDER BY \"wf_id\", \"n\"\n"
#define RELOAD_SHOP SEL_ORDER SEL_NOTE SEL_SMALL SEL_ITEM

/* The line of an instance that a rolled-back cycle evicts without
 * reloading it. */
static void not_reloaded(char *out, size_t cap, const char *wf) {
  size_t len = strlen(out);
  snprintf(out + len, cap - len, "clpdaemon: rehydrate: instance '%s' is not "
           "reloaded: it is no longer synchronized\n", wf);
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
  sql("DROP TABLE IF EXISTS shop_item");
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
  /* req is nullable although its null policy is error: a NULL there is
   * a row the loader rejects. */
  sql(pg ? "CREATE TABLE shop_item (wf_id TEXT NOT NULL, n BIGINT NOT NULL, "
           "qty BIGINT, label TEXT, req TEXT, price DOUBLE PRECISION, "
           "PRIMARY KEY (wf_id, n))"
         : "CREATE TABLE shop_item (wf_id TEXT NOT NULL, n INTEGER NOT NULL, "
           "qty INTEGER, label TEXT, req TEXT, price REAL, "
           "PRIMARY KEY (wf_id, n))");
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
                  "orm = orm.conf\ningest = go\n",
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
 * dropped and the instance isolated. The cycle touched it, so its facts
 * are retracted and, isolated, it is not reloaded (T10c). The changes of
 * an instance the cycle did not touch (asserted before it) are kept: the
 * retry writes only that one. An isolated instance touched again by a
 * rolled-back cycle is evicted again. */
static void test_duplicate_key(void) {
  char log[4096], want[4096], got[8192], before[16384], after[16384];
  const char *unique =
      pg ? "duplicate key value violates unique constraint "
           "\"shop_order_pkey\" (SQLSTATE 23505)"
         : "UNIQUE constraint failed: shop_order.wf_id, shop_order.line_no";
  const char *missing =
      pg ? "relation \"public.shop_missing_seq\" does not exist (SQLSTATE "
           "42P01)"
         : "no such table: clpd_sequence";
  fingerprint(before, sizeof before);
  act(SHOP, "(assert (order (wf-id w4x) (line-no 1)))");
  EXPECT(cycle("(progn (assert (order (wf-id w4) (line-no 1) (status a))) "
               "(assert (order (wf-id w4) (line-no 1) (status b))))",
               log, sizeof log) == 1);
  refused(want, sizeof want, "shop@1::order: table 'shop_order'", unique);
  isolated(want, sizeof want, "w4", unique);
  not_reloaded(want, sizeof want, "w4");
  EXPECT(same("duplicate log", log, want));
  fingerprint(after, sizeof after);
  EXPECT(same("unchanged after the unique violation", after, before));
  EXPECT(clpd_db_in_transaction(db) == 0);
  facts_of("w4", got, sizeof got);
  EXPECT(same("isolated instance evicted", got, ""));
  facts_of("w4x", got, sizeof got);
  EXPECT(same("untouched instance kept", got,
              "order(wf-id w4x line-no 1 status nil amount nil note nil "
              "paid 0 data \"\")"));
  EXPECT(cycle(NULL, log, sizeof log) == 0);
  EXPECT(same("retry log", log, ""));
  EXPECT(same("retry", stmts, expect_sql(INS_ORDER INSTANCE)));
  query("SELECT wf_id FROM shop_order WHERE wf_id IN ('w4', 'w4x')", got,
        sizeof got);
  EXPECT(same("only the other instance", got, "'w4x'"));
  EXPECT(tx.retry_ms == 0);
  /* Isolated earlier, touched again by a rolled-back cycle. */
  EXPECT(cycle("(assert (go (wf-id w4) (act fail)))", log, sizeof log) == 1);
  snprintf(want, sizeof want,
           "clpdaemon: db-nextval: sequence alias 'missing_no': sequence "
           "'shop_missing_seq': %s\n"
           "clpdaemon: cycle transaction failed: %s; retrying in 100 ms\n",
           missing, missing);
  not_reloaded(want, sizeof want, "w4");
  EXPECT(same("isolated instance touched again", log, want));
  facts_of("w4", got, sizeof got);
  EXPECT(same("evicted again", got, ""));
  EXPECT(cycle(NULL, log, sizeof log) == 0);
  EXPECT(same("nothing to reload", stmts, ""));
}

/* A value the declared type cannot take rolls the cycle back before any
 * of it is committed (statements sent earlier included). */
static void test_value_errors(void) {
  static const struct {
    const char *expr, *wf, *reason;
  } cases[] = {
      {"(assert (order (wf-id w51) (line-no 1) (amount \"x\")))", "w51",
       "slot 'amount' (column 'amount'): expected float, got STRING"},
      {"(assert (order (wf-id w52) (line-no 1) (amount 3)))", "w52",
       "slot 'amount' (column 'amount'): expected float, got INTEGER"},
      {"(assert (order (wf-id w53) (line-no 1) (paid 2)))", "w53",
       "slot 'paid' (column 'paid'): a boolean column takes 0 or 1"},
      {"(assert (order (wf-id 5) (line-no 1)))", "5",
       "slot 'wf-id' (column 'wf_id'): expected string, got INTEGER"},
      {"(assert (order (wf-id w55) (line-no 1) (note 1.5)))", "w55",
       "slot 'note' (column 'note'): expected string, got FLOAT"},
  };
  char log[4096], want[4096], expr[512], before[16384], after[16384];
  size_t i;
  for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    fingerprint(before, sizeof before);
    /* A valid change first (before the cycle, so the cycle does not touch
     * its instance): its INSERT goes out, then is rolled back, and the
     * retry writes it again. */
    snprintf(expr, sizeof expr, "(assert (order (wf-id v5) (line-no %zu)))",
             i + 1);
    act(SHOP, expr);
    EXPECT(cycle(cases[i].expr, log, sizeof log) == 1);
    snprintf(want, sizeof want,
             "clpdaemon: sync: shop@1::order: table 'shop_order': %s\n"
             "clpdaemon: cycle transaction failed: %s; retrying in 100 ms\n",
             cases[i].reason, cases[i].reason);
    isolated(want, sizeof want, cases[i].wf, cases[i].reason);
    not_reloaded(want, sizeof want, cases[i].wf);
    EXPECT(same(cases[i].expr, log, want));
    EXPECT(strstr(stmts, "INSERT INTO") != NULL);
    fingerprint(after, sizeof after);
    EXPECT(same("unchanged after a value error", after, before));
    EXPECT(clpd_db_in_transaction(db) == 0);
    EXPECT(cycle(NULL, log, sizeof log) == 0);
    EXPECT(same("retry after a value error", stmts,
                expect_sql(INS_ORDER INSTANCE)));
  }
}

/* An INTEGER written to a PostgreSQL int4 column that cannot hold it is a
 * database error: rolled back. SQLite stores 64 bits. */
static void test_int4_overflow(void) {
  char log[4096], want[4096], got[8192], before[16384], after[16384];
  fingerprint(before, sizeof before);
  if (pg) {
    EXPECT(cycle("(assert (small (wf-id w6o) (n 3000000000)))", log,
                 sizeof log) == 1);
    refused(want, sizeof want, "shop@1::small: table 'shop_small'",
            "integer out of range (SQLSTATE 22003)");
    isolated(want, sizeof want, "w6o", "integer out of range (SQLSTATE 22003)");
    not_reloaded(want, sizeof want, "w6o");
    EXPECT(same("int4 overflow", log, want));
    fingerprint(after, sizeof after);
    EXPECT(same("unchanged after the overflow", after, before));
    act(SHOP, "(assert (small (wf-id w6) (n 7)))");
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
  EXPECT(cycle("(do-for-fact ((?f small)) (eq ?f:wf-id w6) "
               "(modify ?f (n nil)))", log, sizeof log) == 0);
  EXPECT(same("small update", stmts,
              expect_sql("UPDATE @\"shop_small\" SET \"n\" = $1 WHERE "
                         "\"wf_id\" = $2\n" INSTANCE)));
  rows_of("shop_small", got, sizeof got);
  EXPECT(same("small null", got, "'w6' NULL"));
}

/* Any statement of the flush that fails rolls the whole cycle back (the
 * mapped rows written before it, other aggregates included); the instance
 * of the failed write is isolated and evicted (T10c), and the retry writes
 * the other one, whose facts were asserted before the cycle (so it is no
 * instance the cycle touched). */
static void test_failed_flush(void) {
  char log[4096], want[4096], got[8192], before[16384], after[16384], e[128];
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
    actf(SHOP, "(assert (order (wf-id w7%d) (line-no 1)))", k);
    actf(CRM, "(assert (party (wf-id c7%d)))", k);
    begin_cycle();
    fail_at = k;
    EXPECT(end_cycle(log, sizeof log) == 1);
    refused(want, sizeof want, what[k - 1], injected);
    snprintf(e, sizeof e, "%c7%d", k <= 2 ? 'w' : 'c', k);
    isolated(want, sizeof want, e, injected);
    not_reloaded(want, sizeof want, e);
    EXPECT(same("injected failure", log, want));
    fingerprint(after, sizeof after);
    EXPECT(same("unchanged after a failed flush", after, before));
    /* Undo the facts: the next attempt starts from the same state. */
    actf(SHOP, "(do-for-fact ((?f order)) (eq ?f:wf-id w7%d) (retract ?f))", k);
    actf(CRM, "(do-for-fact ((?f party)) (eq ?f:wf-id c7%d) (retract ?f))", k);
    EXPECT(cycle(NULL, log, sizeof log) == 0);
    EXPECT(same("nothing pending", stmts, ""));
  }
  act(SHOP, "(assert (order (wf-id w7) (line-no 1)))");
  act(CRM, "(assert (party (wf-id c7)))");
  begin_cycle();
  fail_at = 3;
  EXPECT(end_cycle(log, sizeof log) == 1);
  EXPECT(cycle(NULL, log, sizeof log) == 0);
  EXPECT(same("retry writes the other instance", stmts,
              expect_sql(INS_ORDER INSTANCE)));
  query("SELECT wf_id FROM workflow_instance WHERE wf_id IN ('w7', 'c7') "
        "ORDER BY 1",
        got, sizeof got);
  EXPECT(same("only w7", got, "'w7'"));
}

/* After a statement of the cycle failed (a rule's db-nextval), the flush
 * sends nothing and the commit reports that failure. The cycle touched
 * the instance (its go fact, the order the rule asserted), so its facts
 * are retracted and the retry reloads it from the database (T10c):
 * nothing is stored for it, so its facts are lost and nothing is
 * written. */
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
  facts_of("w8", got, sizeof got);
  EXPECT(same("evicted", got, ""));
  EXPECT(cycle(NULL, log, sizeof log) == 0);
  EXPECT(same("retry log", log, ""));
  EXPECT(same("reloaded on the retry", stmts, expect_sql(RELOAD_SHOP)));
  facts_of("w8", got, sizeof got);
  EXPECT(same("nothing stored", got, ""));
  query("SELECT count(*) FROM shop_order WHERE wf_id = 'w8'", got, sizeof got);
  EXPECT(same("still not written", got, "0"));
}

/* T11b: the clock fact (asserted before a cycle, modified at its start
 * like the daemon does) is never written, and a rolled-back cycle keeps it
 * (it carries no wf-id): no statement, no log line, nothing stored. */
static void test_clock_not_written(void) {
  char log[4096], before[16384], after[16384];
  clpd_clock *c = clpd_clock_create(env);
  EXPECT(c != NULL);
  if (c == NULL) return;
  fingerprint(before, sizeof before);
  EXPECT(clpd_clock_update(c) == 0);
  begin_cycle();
  EXPECT(clpd_clock_update(c) == 0);
  EXPECT(end_cycle(log, sizeof log) == 0);
  EXPECT(same("clock statements", stmts, ""));
  EXPECT(same("clock log", log, ""));
  fingerprint(after, sizeof after);
  EXPECT(same("clock: nothing written", after, before));
  begin_cycle();
  EXPECT(clpd_clock_update(c) == 0);
  act(SHOP, "(assert (go (wf-id k9) (act fail)))");
  EXPECT(end_cycle(log, sizeof log) == 1);
  EXPECT(clpd_clock_fact(c) != NULL && FactExistp(clpd_clock_fact(c)));
  begin_cycle();
  EXPECT(clpd_clock_update(c) == 0);
  EXPECT(end_cycle(log, sizeof log) == 0);
  EXPECT(same("clock retry log", log, ""));
  EXPECT(strstr(stmts, "clock") == NULL);
  EXPECT(clpd_clock_fact(c) != NULL && FactExistp(clpd_clock_fact(c)));
  fingerprint(after, sizeof after);
  EXPECT(same("clock: still nothing written", after, before));
  if (clpd_clock_fact(c) != NULL) Retract(clpd_clock_fact(c));
  clpd_clock_destroy(c);
}

/* --- loading and rehydration (T10c) ------------------------------------- */

/* The schema prefix the loader takes ("public". on PostgreSQL). */
static const char *schema_prefix(void) { return pg ? "\"public\"." : ""; }

/* Retracts every fact of the instances (symbol or string wf-id) while the
 * synchronization is suspended, so nothing is recorded. */
static void forget(const char *wf) {
  char e[256];
  clpd_orm_sync_suspend(syncer);
  snprintf(e, sizeof e,
           "(do-for-all-facts ((?f order note small item tmp go)) "
           "(eq (str-cat ?f:wf-id) \"%s\") (retract ?f))",
           wf);
  act(SHOP, e);
  clpd_orm_sync_resume(syncer);
}

/* The loader: the rows of an instance become facts of their template, in
 * mapping and key order, every column per its CLIPS type and null policy
 * (nil: the symbol nil; default: the slot's default; error: the row is
 * rejected with one line and not asserted); a value the slot's
 * constraints refuse, a TEXT in an integer column (SQLite) and a bytea
 * with a NUL byte for a string slot are rejected the same way. The facts
 * are not written back (echo suppression). Any equality filter, IS NULL
 * for a NULL value. */
static void test_load(void) {
  char log[4096], want[4096], got[8192], before[16384], after[16384];
  const clpd_orm_template *failed = NULL;
  const char *line =
      "clpdaemon: load: shop@1::%s: table '%s': instance 'n1': row "
      "rejected: %s\n";
  clpd_orm_filter f[2];
  size_t loaded = 99, len;
  clpd_db_status st;
  FILE *lf;
  sql("INSERT INTO shop_item (wf_id, n, qty, label, req, price) VALUES "
      "('n1', 1, NULL, NULL, 'r', NULL), ('n1', 2, 3, 'abc', NULL, 2.5), "
      "('n1', 3, 4, 'zzz', 'r', 2.5), ('n1', 4, 5, 'abc', 'r', 3.0), "
      "('n2', 1, 9, 'xyz', 'q', 9.5)");
  if (!pg)
    sql("INSERT INTO shop_item (wf_id, n, qty, label, req, price) VALUES "
        "('n1', 5, 'many', 'abc', 'r', 1.0)");
  sql(pg ? "INSERT INTO shop_order (wf_id, line_no, status, amount, note, "
           "paid, data) VALUES ('n1', 1, NULL, NULL, NULL, NULL, NULL), "
           "('n1', 2, 'open', 1.25, 'x', TRUE, '\\x4142'), "
           "('n1', 3, 'open', NULL, NULL, NULL, '\\x4100')"
         : "INSERT INTO shop_order (wf_id, line_no, status, amount, note, "
           "paid, data) VALUES ('n1', 1, NULL, NULL, NULL, NULL, NULL), "
           "('n1', 2, 'open', 1.25, 'x', 1, X'4142'), "
           "('n1', 3, 'open', NULL, NULL, NULL, X'4100')");
  fingerprint(before, sizeof before);
  lf = tmpfile();
  if (lf == NULL) exit(2);
  open_log();
  st = clpd_orm_load_instance(env, db, syncer, &pkgs.items[SHOP],
                              schema_prefix(), "n1", NULL, 0, lf, &loaded,
                              &failed);
  close_log(log, sizeof log);
  EXPECT(st == CLPD_DB_OK);
  EXPECT(loaded == 4);
  EXPECT(failed == NULL);
  EXPECT(same("load statements", stmts, expect_sql(RELOAD_SHOP)));
  rewind(lf);
  len = fread(log, 1, sizeof log - 1, lf);
  log[len] = '\0';
  fclose(lf);
  want[0] = '\0';
  len = strlen(want);
  snprintf(want + len, sizeof want - len, line, "order", "shop_order",
           "column 'data' (slot 'data'): the value holds a NUL byte");
  len = strlen(want);
  snprintf(want + len, sizeof want - len, line, "item", "shop_item",
           "column 'req' (slot 'req') is NULL and its null policy is error");
  len = strlen(want);
  snprintf(want + len, sizeof want - len, line, "item", "shop_item",
           "column 'label' (slot 'label'): the slot's allowed values do not "
           "include the value");
  if (!pg) {
    len = strlen(want);
    snprintf(want + len, sizeof want - len, line, "item", "shop_item",
             "column 'qty' (slot 'qty'): expected integer, got TEXT");
  }
  EXPECT(same("rejected rows", log, want));
  facts_of("n1", got, sizeof got);
  EXPECT(same("loaded facts", got,
              "order(wf-id \"n1\" line-no 1 status nil amount nil note nil "
              "paid 0 data \"\"); "
              "order(wf-id \"n1\" line-no 2 status open amount 1.25 note "
              "\"x\" paid 1 data \"AB\"); "
              "item(wf-id n1 n 1 qty 7 label nil req \"r\" price 1.5); "
              "item(wf-id n1 n 4 qty 5 label abc req \"r\" price 3)"));
  facts_of("n2", got, sizeof got);
  EXPECT(same("other instance not loaded", got, ""));
  /* Echo suppression: nothing to write. */
  EXPECT(cycle(NULL, log, sizeof log) == 0);
  EXPECT(same("loaded facts are not written", stmts, ""));
  fingerprint(after, sizeof after);
  EXPECT(same("unchanged by the load", after, before));
  /* Other filters: an equality on a non-key column, IS NULL. */
  forget("n1");
  f[0].column = 3;
  f[0].value = clpd_db_text("xyz");
  open_log();
  st = clpd_orm_load(env, db, syncer, &pkgs.items[SHOP], 3, schema_prefix(),
                     f, 1, stderr, &loaded);
  close_log(log, sizeof log);
  EXPECT(st == CLPD_DB_OK && loaded == 1);
  EXPECT(same("equality filter", stmts,
              expect_sql("SELECT \"wf_id\", \"n\", \"qty\", \"label\", "
                         "\"req\", \"price\" FROM @\"shop_item\" WHERE "
                         "\"label\" = $1 ORDER BY \"wf_id\", \"n\"\n")));
  facts_of("n2", got, sizeof got);
  EXPECT(same("filtered fact", got,
              "item(wf-id n2 n 1 qty 9 label xyz req \"q\" price 9.5)"));
  f[0].column = 2;
  f[0].value = clpd_db_null();
  f[1].column = 0;
  f[1].value = clpd_db_text("n1");
  open_log();
  st = clpd_orm_load(env, db, syncer, &pkgs.items[SHOP], 3, schema_prefix(),
                     f, 2, stderr, &loaded);
  close_log(log, sizeof log);
  EXPECT(st == CLPD_DB_OK && loaded == 1);
  EXPECT(same("IS NULL filter", stmts,
              expect_sql("SELECT \"wf_id\", \"n\", \"qty\", \"label\", "
                         "\"req\", \"price\" FROM @\"shop_item\" WHERE "
                         "\"qty\" IS NULL AND \"wf_id\" = $1 ORDER BY "
                         "\"wf_id\", \"n\"\n")));
  facts_of("n1", got, sizeof got);
  EXPECT(same("IS NULL fact", got,
              "item(wf-id n1 n 1 qty 7 label nil req \"r\" price 1.5)"));
  /* A failed SELECT: its status, nothing asserted. */
  forget("n1");
  forget("n2");
  open_log();
  fail_at = 1;
  st = clpd_orm_load_instance(env, db, syncer, &pkgs.items[SHOP],
                              schema_prefix(), "n1", NULL, 0, stderr, &loaded,
                              &failed);
  close_log(log, sizeof log);
  EXPECT(st == CLPD_DB_ERROR);
  EXPECT(failed == &pkgs.items[SHOP].orm->templates[0]);
  facts_of("n1", got, sizeof got);
  EXPECT(same("nothing loaded after a failed SELECT", got, ""));
  sql("DELETE FROM shop_item");
  sql("DELETE FROM shop_order WHERE wf_id = 'n1'");
}

/* A rolled-back cycle (FR-26): every fact of each instance it touched
 * (asserted, modified, retracted), mapped or not, is retracted; when the
 * retry begins the instance is reloaded from the database (each mapped
 * template of its package, manual ones included, one SELECT by wf_id) and
 * the facts are asserted without echo (the retry writes nothing); a row
 * the loader rejects is logged. Unmapped facts are transient (a fact
 * asserted before the cycle too); the facts of an instance the cycle did
 * not touch stay. A reloaded fact is a stored one: its modify is an
 * UPDATE. */
static void test_rehydrate(void) {
  char log[4096], want[4096], got[8192], before[16384], after[16384];
  const char *missing =
      pg ? "relation \"public.shop_missing_seq\" does not exist (SQLSTATE "
           "42P01)"
         : "no such table: clpd_sequence";
  EXPECT(cycle("(progn (assert (order (wf-id r1) (line-no 1) (status new) "
               "(note \"n\"))) (assert (small (wf-id r1) (n 5))) "
               "(assert (order (wf-id r2) (line-no 1))))",
               log, sizeof log) == 0);
  /* Rows of r1 another writer stored (a manual template too). */
  sql("INSERT INTO shop_note (wf_id, body) VALUES ('r1', 'memo')");
  sql("INSERT INTO shop_item (wf_id, n, qty, label, req, price) VALUES "
      "('r1', 1, 2, 'abc', NULL, 1.0)");
  sql("INSERT INTO crm_party (wf_id, name) VALUES ('c9', 'Bo')");
  act(SHOP, "(assert (tmp (wf-id r1) (v before)))");
  fingerprint(before, sizeof before);
  begin_cycle();
  act(SHOP, "(do-for-fact ((?f order)) (eq ?f:wf-id r1) "
            "(modify ?f (status changed)))");
  act(SHOP, "(do-for-fact ((?f small)) (eq ?f:wf-id r1) (retract ?f))");
  act(SHOP, "(assert (order (wf-id r1) (line-no 2)))");
  act(SHOP, "(assert (tmp (wf-id r1) (v during)))");
  act(CRM, "(assert (party (wf-id c9) (name \"new\")))");
  act(SHOP, "(assert (go (wf-id r1) (act fail)))");
  EXPECT(end_cycle(log, sizeof log) == 1);
  snprintf(want, sizeof want,
           "clpdaemon: db-nextval: sequence alias 'missing_no': sequence "
           "'shop_missing_seq': %s\n"
           "clpdaemon: cycle transaction failed: %s; retrying in 100 ms\n",
           missing, missing);
  EXPECT(same("rolled-back cycle log", log, want));
  facts_of("r1", got, sizeof got);
  EXPECT(same("touched instance evicted", got, ""));
  facts_of("c9", got, sizeof got);
  EXPECT(same("touched crm instance evicted", got, ""));
  facts_of("r2", got, sizeof got);
  EXPECT(same("untouched instance kept", got,
              "order(wf-id r2 line-no 1 status nil amount nil note nil "
              "paid 0 data \"\")"));
  fingerprint(after, sizeof after);
  EXPECT(same("unchanged after the rollback", after, before));
  /* The retry: reloaded, nothing written. */
  begin_cycle();
  EXPECT(end_cycle(log, sizeof log) == 0);
  EXPECT(same("rehydration log", log,
              "clpdaemon: load: shop@1::item: table 'shop_item': instance "
              "'r1': row rejected: column 'req' (slot 'req') is NULL and "
              "its null policy is error\n"));
  EXPECT(same("rehydration statements", stmts,
              expect_sql(RELOAD_SHOP
                         "SELECT \"wf_id\", \"name\" FROM @\"crm_party\" "
                         "WHERE \"wf_id\" = $1 ORDER BY \"wf_id\"\n")));
  facts_of("r1", got, sizeof got);
  EXPECT(same("reloaded instance", got,
              "order(wf-id \"r1\" line-no 1 status new amount nil note \"n\" "
              "paid 0 data \"\"); note(wf-id \"r1\" text \"memo\"); "
              "small(wf-id \"r1\" n 5)"));
  facts_of("c9", got, sizeof got);
  EXPECT(same("reloaded crm instance", got, "party(wf-id \"c9\" name \"Bo\")"));
  facts_of("r2", got, sizeof got);
  EXPECT(same("untouched instance still there", got,
              "order(wf-id r2 line-no 1 status nil amount nil note nil "
              "paid 0 data \"\")"));
  fingerprint(after, sizeof after);
  EXPECT(same("unchanged after the rehydration", after, before));
  EXPECT(tx.retry_ms == 0);
  EXPECT(cycle("(do-for-fact ((?f small)) (eq ?f:wf-id \"r1\") "
               "(modify ?f (n 6)))",
               log, sizeof log) == 0);
  EXPECT(same("a reloaded fact is stored", stmts,
              expect_sql("UPDATE @\"shop_small\" SET \"n\" = $1 WHERE "
                         "\"wf_id\" = $2\n" INSTANCE)));
  forget("r1");
  forget("r2");
  clpd_orm_sync_suspend(syncer);
  act(CRM, "(do-for-all-facts ((?f party)) (eq (str-cat ?f:wf-id) \"c9\") "
           "(retract ?f))");
  clpd_orm_sync_resume(syncer);
  sql("DELETE FROM shop_item");
}

/* A fact drained from the ingest queue in a rolled-back cycle is lost:
 * its instance is touched, so it is retracted with it and not queued
 * again; the retry reloads the instance's stored rows (none here). */
static void test_rehydrate_ingest(void) {
  static const char json[] =
      "{\"template\":\"go\",\"slots\":{\"wf-id\":{\"sym\":\"r3\"},"
      "\"act\":{\"sym\":\"fail\"}}}";
  char log[4096], got[8192];
  begin_cycle();
  EXPECT(clpd_ingest_post(NULL, json, sizeof json - 1) == 0);
  EXPECT(clpd_ingest_drain(env, &pkgs, NULL, NULL) == 1);
  facts_of("r3", got, sizeof got);
  EXPECT(same("drained", got, "go(wf-id r3 act fail)"));
  EXPECT(end_cycle(log, sizeof log) == 1);
  facts_of("r3", got, sizeof got);
  EXPECT(same("drained fact lost", got, ""));
  EXPECT(clpd_ingest_queue_length() == 0);
  EXPECT(cycle(NULL, log, sizeof log) == 0);
  EXPECT(same("ingest retry log", log, ""));
  EXPECT(same("ingest retry", stmts, expect_sql(RELOAD_SHOP)));
  facts_of("r3", got, sizeof got);
  EXPECT(same("not queued again", got, ""));
}

/* A reload whose SELECT fails with a database error isolates the instance
 * (it stays evicted, its later facts are not written) and the cycle runs;
 * a lost connection fails the start of the cycle (the cycle transaction
 * line, the backoff, a reconnect first), and the next start reloads the
 * instance. */
static void test_rehydrate_failures(void) {
  char log[4096], want[4096], got[8192];
  const char *injected =
      pg ? "relation \"clpd_injected_failure\" does not exist (SQLSTATE "
           "42P01)"
         : "no such table: clpd_injected_failure";
  sql("INSERT INTO shop_order (wf_id, line_no, status) VALUES ('r4', 1, 's'), "
      "('r6', 1, 't')");
  EXPECT(cycle("(progn (assert (go (wf-id r4) (act none))) "
               "(assert (go (wf-id r5) (act none))) "
               "(assert (go (wf-id r9) (act fail))))",
               log, sizeof log) == 1);
  open_log();
  fail_at = 1;
  EXPECT(clpd_orm_sync_begin_cycle(syncer, &tx, cycle_log) == 0);
  EXPECT(end_cycle(log, sizeof log) == 0);
  snprintf(want, sizeof want,
           "clpdaemon: rehydrate: instance 'r4': shop@1::order: table "
           "'shop_order': %s\n",
           injected);
  isolated(want, sizeof want, "r4", injected);
  EXPECT(same("failed reload log", log, want));
  EXPECT(same("failed reload statements", stmts,
              expect_sql(SEL_ORDER RELOAD_SHOP RELOAD_SHOP)));
  facts_of("r4", got, sizeof got);
  EXPECT(same("not reloaded", got, ""));
  EXPECT(cycle("(assert (order (wf-id r4) (line-no 2)))", log, sizeof log) ==
         0);
  EXPECT(same("isolated after a failed reload", stmts, ""));
  forget("r4");
  /* Lost while reloading. */
  EXPECT(cycle("(progn (assert (go (wf-id r6) (act none))) "
               "(assert (go (wf-id r9) (act fail))))",
               log, sizeof log) == 1);
  lost_at = 1;
  EXPECT(try_begin(log, sizeof log) == -1);
  EXPECT(same("lost reload log", log,
              "clpdaemon: rehydrate: instance 'r6': shop@1::order: table "
              "'shop_order': injected lost connection\n"
              "clpdaemon: cycle transaction failed: injected lost "
              "connection; retrying in 200 ms\n"));
  EXPECT(tx.reconnect == 1);
  EXPECT(clpd_db_in_transaction(db) == 0);
  facts_of("r6", got, sizeof got);
  EXPECT(same("nothing reloaded yet", got, ""));
  begin_cycle();
  EXPECT(end_cycle(log, sizeof log) == 0);
  EXPECT(same("reload after the reconnect log", log, ""));
  EXPECT(same("reload after the reconnect", stmts,
              expect_sql(RELOAD_SHOP RELOAD_SHOP)));
  EXPECT(tx.reconnect == 0 && tx.retry_ms == 0);
  facts_of("r6", got, sizeof got);
  EXPECT(same("reloaded after the reconnect", got,
              "order(wf-id \"r6\" line-no 1 status t amount nil note nil "
              "paid 0 data \"\")"));
  forget("r6");
  sql("DELETE FROM shop_order WHERE wf_id IN ('r4', 'r6')");
}

/* The poison retry (T10e, A1): a rolled-back cycle whose first failure is
 * a statement of a rule counts for the rule's instance (the wf-id of its
 * first basis fact that has one); after instance_isolate_after such
 * cycles in a row (no commit between them) the instance is isolated like
 * a failed write: its facts retracted, not reloaded, never written; the
 * retry then commits the other instances. Here a stored row that a rule
 * always fails on (status poison) comes back with every reload. A commit
 * resets the counts; a failure of a rule without a wf-id counts for no
 * instance. */
static void test_poison_retry(void) {
  char log[4096], want[4096], got[8192], line[1024], reason[1024];
  const char *missing =
      pg ? "relation \"public.shop_missing_seq\" does not exist (SQLSTATE "
           "42P01)"
         : "no such table: clpd_sequence";
  int k;
  /* The failure lines; the wait doubles from 100 ms while no cycle
   * commits. */
#define POISON_LINE(ms)                                                     \
  snprintf(line, sizeof line,                                              \
           "clpdaemon: db-nextval: sequence alias 'missing_no': sequence "  \
           "'shop_missing_seq': %s\n"                                       \
           "clpdaemon: cycle transaction failed: %s; retrying in %d ms\n",  \
           missing, missing, (ms))
  POISON_LINE(100);
  snprintf(reason, sizeof reason, "%s (3 rolled-back cycles in a row)",
           missing);
  sql("INSERT INTO shop_order (wf_id, line_no, status) VALUES ('p1', 1, "
      "'poison')");
  /* A change of p2 made before the cycles: they never touch it. */
  act(SHOP, "(assert (order (wf-id p2) (line-no 1)))");
  EXPECT(cycle("(assert (go (wf-id p1) (act fail)))", log, sizeof log) == 1);
  EXPECT(same("poison: first failure", log, line));
  for (k = 2; k <= 3; k++) {
    begin_cycle();
    EXPECT(end_cycle(log, sizeof log) == 1);
    POISON_LINE(k == 2 ? 200 : 400);
    snprintf(want, sizeof want, "%s", line);
    if (k == 3) {
      isolated(want, sizeof want, "p1", reason);
      not_reloaded(want, sizeof want, "p1");
    }
    EXPECT(same(k == 2 ? "poison: second failure" : "poison: isolated", log,
                want));
  }
  facts_of("p1", got, sizeof got);
  EXPECT(same("poison: evicted", got, ""));
  EXPECT(cycle(NULL, log, sizeof log) == 0);
  EXPECT(same("poison: the retry log", log, ""));
  EXPECT(same("poison: the retry writes p2", stmts,
              expect_sql(INS_ORDER INSTANCE)));
  query("SELECT wf_id, status FROM shop_order WHERE wf_id IN ('p1', 'p2') "
        "ORDER BY 1",
        got, sizeof got);
  EXPECT(same("poison: rows", got, "'p1' 'poison'; 'p2' 'nil'"));
  /* Isolated: its later facts are not written. */
  EXPECT(cycle("(assert (order (wf-id p1) (line-no 2)))", log, sizeof log) ==
         0);
  EXPECT(same("poison: p1 not written", stmts, ""));
  forget("p1");
  forget("p2");

  /* A commit between two failures resets the count. */
  POISON_LINE(100);
#undef POISON_LINE
  clpd_orm_sync_set_isolate_after(syncer, 2);
  for (k = 0; k < 2; k++) {
    EXPECT(cycle("(assert (go (wf-id p3) (act fail)))", log, sizeof log) ==
           1);
    EXPECT(same("poison: reset by a commit", log, line));
    EXPECT(cycle(NULL, log, sizeof log) == 0);
    EXPECT(same("poison: committed", log, ""));
  }
  /* A rule whose basis has no wf-id (the slot left nil): the failure
   * counts for nobody. */
  clpd_orm_sync_set_isolate_after(syncer, 1);
  EXPECT(cycle("(assert (go (act anon)))", log, sizeof log) == 1);
  EXPECT(same("poison: unattributed", log, line));
  EXPECT(cycle(NULL, log, sizeof log) == 0);
  EXPECT(same("poison: unattributed retry", log, ""));
  clpd_orm_sync_set_isolate_after(syncer, 3);
  EXPECT(cycle(NULL, log, sizeof log) == 0);
  sql("DELETE FROM shop_order WHERE wf_id IN ('p1', 'p2')");
}

/* T10f: transient contention (CLPD_DB_TRANSIENT: a lock, deadlock,
 * serialization or timeout failure) and a lost connection never isolate an
 * instance. A rule statement failing that way rolls the cycle back and is
 * retried, counted for no instance even with instance_isolate_after = 1;
 * a transient failure between two attributed ones neither counts nor
 * resets the count. A sync write failing that way isolates nothing: the
 * retry writes the row. A reload SELECT failing that way fails the start
 * of the cycle (no reconnect) and the next start reloads the instance. */
static void test_transient(void) {
  char log[4096], want[4096], got[8192], line[1024], reason[1024];
  const char *missing =
      pg ? "relation \"public.shop_missing_seq\" does not exist (SQLSTATE "
           "42P01)"
         : "no such table: clpd_sequence";
  const char *tr = "injected transient failure";
  int rc;
#define NEXTVAL_LINE(msg, ms)                                               \
  snprintf(line, sizeof line,                                              \
           "clpdaemon: db-nextval: sequence alias 'missing_no': sequence "  \
           "'shop_missing_seq': %s\n"                                       \
           "clpdaemon: cycle transaction failed: %s; retrying in %d ms\n",  \
           (msg), (msg), (ms))

  /* A rule statement, transient or lost, with isolate_after 1. */
  clpd_orm_sync_set_isolate_after(syncer, 1);
  begin_cycle();
  act(SHOP, "(assert (go (wf-id t1) (act fail)))");
  transient_at = seen + 1;
  EXPECT(end_cycle(log, sizeof log) == 1);
  NEXTVAL_LINE(tr, 100);
  EXPECT(same("transient rule statement", log, line));
  EXPECT(tx.reconnect == 0);
  facts_of("t1", got, sizeof got);
  EXPECT(same("transient: evicted", got, ""));
  EXPECT(cycle(NULL, log, sizeof log) == 0);
  EXPECT(same("transient: the retry log", log, ""));
  EXPECT(same("transient: reloaded, not isolated", stmts,
              expect_sql(RELOAD_SHOP)));
  begin_cycle();
  act(SHOP, "(assert (go (wf-id l1) (act fail)))");
  lost_at = seen + 1;
  EXPECT(end_cycle(log, sizeof log) == 1);
  NEXTVAL_LINE("injected lost connection", 100);
  EXPECT(same("lost rule statement", log, line));
  EXPECT(tx.reconnect == 1);
  EXPECT(cycle(NULL, log, sizeof log) == 0);
  EXPECT(same("lost: the retry log", log, ""));
  EXPECT(same("lost: reloaded, not isolated", stmts,
              expect_sql(RELOAD_SHOP)));
  EXPECT(tx.reconnect == 0 && tx.retry_ms == 0);
  forget("t1");
  forget("l1");

  /* Between two attributed failures a transient one neither counts nor
   * resets: the second attributed failure isolates with isolate_after 2. */
  clpd_orm_sync_set_isolate_after(syncer, 2);
  sql("INSERT INTO shop_order (wf_id, line_no, status) VALUES ('q1', 1, "
      "'poison')");
  EXPECT(cycle("(assert (go (wf-id q1) (act fail)))", log, sizeof log) == 1);
  NEXTVAL_LINE(missing, 100);
  EXPECT(same("q1: first attributed failure", log, line));
  begin_cycle();
  transient_at = seen + 1;
  EXPECT(end_cycle(log, sizeof log) == 1);
  NEXTVAL_LINE(tr, 200);
  EXPECT(same("q1: transient, not counted", log, line));
  begin_cycle();
  EXPECT(end_cycle(log, sizeof log) == 1);
  NEXTVAL_LINE(missing, 400);
  snprintf(want, sizeof want, "%s", line);
  snprintf(reason, sizeof reason, "%s (2 rolled-back cycles in a row)",
           missing);
  isolated(want, sizeof want, "q1", reason);
  not_reloaded(want, sizeof want, "q1");
  EXPECT(same("q1: the second attributed failure isolates", log, want));
  EXPECT(cycle(NULL, log, sizeof log) == 0);
  EXPECT(same("q1: committed", log, ""));
  forget("q1");
  sql("DELETE FROM shop_order WHERE wf_id = 'q1'");
  clpd_orm_sync_set_isolate_after(syncer, 3);

  /* A sync write. */
  act(SHOP, "(assert (order (wf-id t2) (line-no 1)))");
  begin_cycle();
  transient_at = seen + 1;
  EXPECT(end_cycle(log, sizeof log) == 1);
  refused(want, sizeof want, "shop@1::order: table 'shop_order'", tr);
  EXPECT(same("transient sync write", log, want));
  EXPECT(cycle(NULL, log, sizeof log) == 0);
  EXPECT(same("transient sync write: the retry log", log, ""));
  EXPECT(same("transient sync write: the retry writes t2", stmts,
              expect_sql(INS_ORDER INSTANCE)));
  query("SELECT wf_id, line_no FROM shop_order WHERE wf_id = 't2'", got,
        sizeof got);
  EXPECT(same("transient sync write: the row", got, "'t2' 1"));
  forget("t2");

  /* A reload SELECT. */
  sql("INSERT INTO shop_order (wf_id, line_no, status) VALUES ('t3', 1, "
      "'u')");
  EXPECT(cycle("(progn (assert (go (wf-id t3) (act none))) "
               "(assert (go (wf-id t9) (act fail))))",
               log, sizeof log) == 1);
  transient_at = 1;
  rc = try_begin(log, sizeof log);
  if (rc == 0) end_cycle(log, sizeof log);
  EXPECT(rc == -1);
  EXPECT(same("transient reload log", log,
              "clpdaemon: rehydrate: instance 't3': shop@1::order: table "
              "'shop_order': injected transient failure\n"
              "clpdaemon: cycle transaction failed: injected transient "
              "failure; retrying in 200 ms\n"));
  EXPECT(tx.reconnect == 0);
  EXPECT(clpd_db_in_transaction(db) == 0);
  facts_of("t3", got, sizeof got);
  EXPECT(same("transient reload: nothing loaded yet", got, ""));
  begin_cycle();
  EXPECT(end_cycle(log, sizeof log) == 0);
  EXPECT(same("transient reload: the retry log", log, ""));
  EXPECT(same("transient reload: reloaded on the retry", stmts,
              expect_sql(RELOAD_SHOP RELOAD_SHOP)));
  facts_of("t3", got, sizeof got);
  EXPECT(same("transient reload: reloaded", got,
              "order(wf-id \"t3\" line-no 1 status u amount nil note nil "
              "paid 0 data \"\")"));
  forget("t3");
  forget("t9");
  sql("DELETE FROM shop_order WHERE wf_id IN ('t2', 't3')");
#undef NEXTVAL_LINE
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
  EXPECT(clpd_orm_sync_begin_cycle(NULL, &none, stderr) == 0);
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
  test_clock_not_written();
  test_poison_retry();
  test_transient();
  test_load();
  test_rehydrate();
  test_rehydrate_ingest();
  test_rehydrate_failures();
  test_existing_instance();
  test_attach();
  test_clear_and_detach();
  clpd_packages_free(&pkgs);
  DestroyEnvironment(env);
  {
    const char *const drop[] = {"DROP TABLE shop_order", "DROP TABLE shop_note",
                                "DROP TABLE shop_small", "DROP TABLE shop_item",
                                "DROP TABLE crm_party",
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
