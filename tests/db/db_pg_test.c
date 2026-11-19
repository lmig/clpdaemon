/* Tests for the PostgreSQL backend (T8c, src/db_pg.c), run by
 * tests/db/pg_cluster.sh against a throwaway cluster.
 * usage: db_pg_test <conninfo> <password>
 * <conninfo> has no password: it comes from the PGPASSFILE the script
 * wrote; <password> is that role's password, only used to check that it is
 * never echoed. */
#define _POSIX_C_SOURCE 200809L

#include "db.h"
#include "db_suite.h"

#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "expect.h"
#include "log.h"

#define PASSWORD_MSG                                                       \
  "database: the connection string must not contain a password (use a "  \
  "password file, see PGPASSFILE)"

static const char *conninfo, *password;

static int one_line(const char *s) {
  return s != NULL && *s != '\0' && strchr(s, '\n') == NULL &&
         strchr(s, '\r') == NULL;
}

static int contains(const char *s, const char *needle) {
  return s != NULL && strstr(s, needle) != NULL;
}

static clpd_db *open_db(const char *spec) {
  clpd_db *db = NULL;
  char msg[512];
  if (clpd_db_open(spec, &db, msg, sizeof msg) != CLPD_DB_OK) {
    fprintf(stderr, "cannot open the database: %s\n", msg);
    return NULL;
  }
  return db;
}

static clpd_db_status run(clpd_db *db, const char *sql) {
  return clpd_db_exec(db, sql, NULL, 0, NULL);
}

/* Runs a one-row, one-column query and returns its value (copied into *out);
 * 0 on failure. */
static int scalar(clpd_db *db, const char *sql, const clpd_db_value *params,
                  size_t n, clpd_db_value *out, char *text, size_t text_size) {
  clpd_db_result *r = NULL;
  const clpd_db_value *v;
  int ok = 0;
  if (clpd_db_exec(db, sql, params, n, &r) == CLPD_DB_OK &&
      clpd_db_result_rows(r) == 1 && clpd_db_result_columns(r) == 1) {
    v = clpd_db_result_value(r, 0, 0);
    *out = *v;
    out->data = NULL;
    if (text != NULL && (v->type == CLPD_DB_TEXT || v->type == CLPD_DB_BLOB))
      snprintf(text, text_size, "%.*s", (int)v->size, (const char *)v->data);
    ok = 1;
  }
  clpd_db_result_free(r);
  return ok;
}

/* The password file connects; without it the password is required. */
static void test_password_file(void) {
  clpd_db *db = open_db(conninfo), *none = NULL;
  clpd_db_value v;
  char text[128] = "", msg[512] = "", missing[4096];
  const char *passfile = getenv("PGPASSFILE");

  EXPECT(db != NULL);
  if (db != NULL) {
    EXPECT(strcmp(clpd_db_backend(db), "postgresql") == 0);
    EXPECT(scalar(db, "SELECT current_user::text", NULL, 0, &v, text,
                  sizeof text));
    EXPECT(v.type == CLPD_DB_TEXT && strcmp(text, "clpd_test") == 0);
    EXPECT(scalar(db, "SHOW password_encryption", NULL, 0, &v, text,
                  sizeof text));
    EXPECT(strcmp(text, "scram-sha-256") == 0);
  }
  clpd_db_close(db);

  EXPECT(passfile != NULL);
  if (passfile == NULL) return;
  snprintf(missing, sizeof missing, "%s.missing", passfile);
  setenv("PGPASSFILE", missing, 1);
  EXPECT(clpd_db_open(conninfo, &none, msg, sizeof msg) == CLPD_DB_ERROR);
  EXPECT(none == NULL);
  EXPECT(one_line(msg));
  EXPECT(contains(msg, "password"));
  EXPECT(!contains(msg, conninfo));
  setenv("PGPASSFILE", passfile, 1);
  clpd_db_close(none);
}

/* A conninfo with a password is rejected by the spec check, no echo. */
static void test_password_rejected(void) {
  clpd_db *db = (clpd_db *)&db;
  char spec[1024], msg[512] = "";
  snprintf(spec, sizeof spec, "%s password=%s", conninfo, password);
  EXPECT(clpd_db_check_spec(spec, msg, sizeof msg) == 0);
  EXPECT(strcmp(msg, PASSWORD_MSG) == 0);
  msg[0] = '\0';
  EXPECT(clpd_db_open(spec, &db, msg, sizeof msg) == CLPD_DB_ERROR);
  EXPECT(db == NULL);
  EXPECT(strcmp(msg, PASSWORD_MSG) == 0);
  EXPECT(!contains(msg, password));
}

/* Column types beyond the shared suite, client encoding, text parameters
 * typed by the server, and messages without parameter values. */
static void test_types_and_messages(void) {
  char spec[1024], text[128];
  clpd_db *db;
  clpd_db_result *r = NULL;
  clpd_db_value v, p[3];

  snprintf(spec, sizeof spec, "%s client_encoding=LATIN1", conninfo);
  db = open_db(spec);
  EXPECT(db != NULL);
  if (db == NULL) return;
  EXPECT(scalar(db, "SHOW client_encoding", NULL, 0, &v, text, sizeof text));
  EXPECT(strcmp(text, "UTF8") == 0);

  EXPECT(clpd_db_exec(db,
                      "SELECT true, false, 7::int2, -8::int4, 9::int8, "
                      "0.5::float4, 2.5::float8, 1.25::numeric, "
                      "'\\x00ff'::bytea, NULL::bool, 'x'::varchar",
                      NULL, 0, &r) == CLPD_DB_OK);
  EXPECT(r != NULL && clpd_db_result_rows(r) == 1 &&
         clpd_db_result_columns(r) == 11);
  if (r != NULL && clpd_db_result_rows(r) == 1 &&
      clpd_db_result_columns(r) == 11) {
    const clpd_db_value *c[11];
    size_t k;
    for (k = 0; k < 11; k++) c[k] = clpd_db_result_value(r, 0, k);
    EXPECT(c[0]->type == CLPD_DB_INTEGER && c[0]->integer == 1);
    EXPECT(c[1]->type == CLPD_DB_INTEGER && c[1]->integer == 0);
    EXPECT(c[2]->type == CLPD_DB_INTEGER && c[2]->integer == 7);
    EXPECT(c[3]->type == CLPD_DB_INTEGER && c[3]->integer == -8);
    EXPECT(c[4]->type == CLPD_DB_INTEGER && c[4]->integer == 9);
    EXPECT(c[5]->type == CLPD_DB_FLOAT && c[5]->real == 0.5);
    EXPECT(c[6]->type == CLPD_DB_FLOAT && c[6]->real == 2.5);
    EXPECT(c[7]->type == CLPD_DB_TEXT && c[7]->size == 4 &&
           strcmp(c[7]->data, "1.25") == 0);
    EXPECT(c[8]->type == CLPD_DB_BLOB && c[8]->size == 2 &&
           memcmp(c[8]->data, "\x00\xff", 2) == 0);
    EXPECT(c[9]->type == CLPD_DB_NULL);
    EXPECT(c[10]->type == CLPD_DB_TEXT && strcmp(c[10]->data, "x") == 0);
  }
  clpd_db_result_free(r);

  /* Doubles round-trip exactly, infinities and negative zero included;
   * parameters keep their type in a bare SELECT. */
  {
    static const double values[] = {0.1, 1.0 / 3.0, 5e-324, -0.0, INFINITY,
                                    -INFINITY, 1.7976931348623157e308};
    size_t k;
    for (k = 0; k < sizeof values / sizeof values[0]; k++) {
      p[0] = clpd_db_float(values[k]);
      EXPECT(scalar(db, "SELECT $1", p, 1, &v, NULL, 0));
      EXPECT(v.type == CLPD_DB_FLOAT && v.real == values[k] &&
             signbit(v.real) == signbit(values[k]));
    }
    p[0] = clpd_db_int(INT64_MIN);
    EXPECT(scalar(db, "SELECT $1", p, 1, &v, NULL, 0));
    EXPECT(v.type == CLPD_DB_INTEGER && v.integer == INT64_MIN);
    p[0] = clpd_db_blob(NULL, 0); /* empty, not NULL */
    EXPECT(scalar(db, "SELECT $1", p, 1, &v, NULL, 0));
    EXPECT(v.type == CLPD_DB_BLOB && v.size == 0);
  }

  /* TEXT parameters take the type of their context (timestamptz, jsonb);
   * a TEXT value need not be NUL-terminated. */
  EXPECT(run(db, "CREATE TEMP TABLE pg_only (id BIGINT PRIMARY KEY, "
                 "ts TIMESTAMPTZ, doc JSONB, name TEXT UNIQUE)") == CLPD_DB_OK);
  p[0] = clpd_db_int(1);
  p[1] = clpd_db_text("2026-01-02 03:04:05+00");
  p[2] = clpd_db_text("{\"a\": 1}xyz");
  p[2].size -= 3;
  EXPECT(clpd_db_exec(db, "INSERT INTO pg_only (id, ts, doc) "
                          "VALUES ($1, $2, $3)", p, 3, NULL) == CLPD_DB_OK);
  EXPECT(scalar(db, "SELECT (doc->>'a')::int8 FROM pg_only WHERE ts = $1",
                &p[1], 1, &v, NULL, 0));
  EXPECT(v.type == CLPD_DB_INTEGER && v.integer == 1);

  /* Messages never carry parameter values (constraint details, invalid
   * input) and stay one line. */
  p[0] = clpd_db_int(2);
  p[1] = clpd_db_text("unique-secret-1");
  EXPECT(clpd_db_exec(db, "INSERT INTO pg_only (id, name) VALUES ($1, $2)", p,
                      2, NULL) == CLPD_DB_OK);
  p[0] = clpd_db_int(3);
  EXPECT(clpd_db_exec(db, "INSERT INTO pg_only (id, name) VALUES ($1, $2)", p,
                      2, NULL) == CLPD_DB_UNIQUE);
  EXPECT(one_line(clpd_db_error(db)));
  EXPECT(!contains(clpd_db_error(db), "unique-secret-1"));
  p[0] = clpd_db_text("bad\"input-secret-2\"x");
  EXPECT(clpd_db_exec(db, "SELECT id FROM pg_only WHERE id = $1", p, 1,
                      NULL) == CLPD_DB_ERROR);
  EXPECT(one_line(clpd_db_error(db)));
  EXPECT(contains(clpd_db_error(db), "22P02"));
  EXPECT(!contains(clpd_db_error(db), "secret"));
  EXPECT(run(db, "SELECT nothing FROM missing_table") == CLPD_DB_ERROR);
  EXPECT(contains(clpd_db_error(db), "missing_table"));

  /* COPY is refused and the connection stays usable. */
  EXPECT(run(db, "COPY pg_only FROM STDIN") == CLPD_DB_ERROR);
  EXPECT(one_line(clpd_db_error(db)));
  EXPECT(run(db, "COPY pg_only TO STDOUT") == CLPD_DB_ERROR);
  EXPECT(scalar(db, "SELECT count(*) FROM pg_only", NULL, 0, &v, NULL, 0));
  EXPECT(v.type == CLPD_DB_INTEGER && v.integer == 2);

  /* Affected rows: INSERT ... RETURNING counts, other statements are 0. */
  r = NULL;
  p[0] = clpd_db_int(4);
  EXPECT(clpd_db_exec(db, "INSERT INTO pg_only (id) VALUES ($1) RETURNING id",
                      p, 1, &r) == CLPD_DB_OK);
  EXPECT(r != NULL && clpd_db_result_affected(r) == 1 &&
         clpd_db_result_rows(r) == 1);
  clpd_db_result_free(r);
  r = NULL;
  EXPECT(clpd_db_exec(db, "CREATE TEMP TABLE pg_only2 AS SELECT * FROM pg_only",
                      NULL, 0, &r) == CLPD_DB_OK);
  EXPECT(r != NULL && clpd_db_result_affected(r) == 0);
  clpd_db_result_free(r);
  clpd_db_close(db);
}

/* Class 22: the quotes are found on the whole message, so a value longer
 * than the error buffer with quotes inside leaks nothing. */
static void test_long_class22(void) {
  clpd_db *db = open_db(conninfo);
  char value[1200];
  clpd_db_value p;
  EXPECT(db != NULL);
  if (db == NULL) return;
  memset(value, 'z', sizeof value - 1);
  value[sizeof value - 1] = '\0';
  memcpy(value, "a\"LEAKMARK-T8F", 14);
  memcpy(value + 600, "\"LEAKMARK-T8F\"", 14);
  p = clpd_db_text(value);
  EXPECT(clpd_db_exec(db, "SELECT $1::int8", &p, 1, NULL) == CLPD_DB_ERROR);
  EXPECT(one_line(clpd_db_error(db)));
  EXPECT(!contains(clpd_db_error(db), "LEAKMARK"));
  EXPECT(!contains(clpd_db_error(db), "zz"));
  EXPECT(strcmp(clpd_db_error(db), "invalid input syntax for type bigint: "
                                   "\"?\" (SQLSTATE 22P02)") == 0);
  clpd_db_close(db);
}

static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Runs fn in a child process bounded by `limit` seconds, so a missing
 * timeout fails the case instead of hanging the test. The child opens its
 * own connections and exits with its failed check count. */
static void bounded(const char *name, void (*fn)(void), int limit) {
  struct timespec ts = {0, 20000000};
  double start = now();
  int status = 0;
  pid_t pid, done = 0;
  fflush(stdout);
  fflush(stderr);
  pid = fork();
  if (pid == 0) {
    failures = 0;
    fn();
    fflush(stderr);
    _exit(failures > 100 ? 100 : failures);
  }
  EXPECT(pid > 0);
  if (pid <= 0) return;
  while ((done = waitpid(pid, &status, WNOHANG)) == 0 &&
         now() - start < limit)
    nanosleep(&ts, NULL);
  if (done == 0) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    failures++;
    fprintf(stderr, "FAIL %s: still running after %d s\n", name, limit);
    return;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    failures += WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    fprintf(stderr, "FAIL %s: child status %d\n", name, status);
  }
}

static int show_is(clpd_db *db, const char *setting, const char *want) {
  char sql[64], text[64] = "";
  clpd_db_value v;
  snprintf(sql, sizeof sql, "SHOW %s", setting);
  return scalar(db, sql, NULL, 0, &v, text, sizeof text) &&
         strcmp(text, want) == 0;
}

/* lock_timeout 5 s and statement_timeout 30 s unless the session already
 * has a non-zero value (options, role or database settings). */
static void test_session_timeouts(void) {
  char spec[1024];
  clpd_db *db = open_db(conninfo), *other;
  EXPECT(db != NULL);
  if (db == NULL) return;
  EXPECT(show_is(db, "lock_timeout", "5s"));
  EXPECT(show_is(db, "statement_timeout", "30s"));

  snprintf(spec, sizeof spec,
           "%s options='-c lock_timeout=200ms -c statement_timeout=300ms'",
           conninfo);
  other = open_db(spec);
  EXPECT(other != NULL);
  if (other != NULL) {
    EXPECT(show_is(other, "lock_timeout", "200ms"));
    EXPECT(show_is(other, "statement_timeout", "300ms"));
  }
  clpd_db_close(other);

  /* A role setting wins too; the other one keeps the default. */
  EXPECT(run(db, "ALTER ROLE CURRENT_USER SET statement_timeout = '7s'") ==
         CLPD_DB_OK);
  other = open_db(conninfo);
  EXPECT(other != NULL);
  if (other != NULL) {
    EXPECT(show_is(other, "statement_timeout", "7s"));
    EXPECT(show_is(other, "lock_timeout", "5s"));
  }
  clpd_db_close(other);
  EXPECT(run(db, "ALTER ROLE CURRENT_USER RESET statement_timeout") ==
         CLPD_DB_OK);
  clpd_db_close(db);
}

/* Waits on a row another connection holds: CLPD_DB_ERROR (55P03) after the
 * lock timeout, and the connection stays usable. */
static void wait_for_lock(clpd_db *db, double min, double max) {
  double start;
  clpd_db_value v;
  start = now();
  EXPECT(run(db, "UPDATE pg_lock SET n = 2 WHERE id = 1") == CLPD_DB_ERROR);
  start = now() - start;
  EXPECT(start >= min && start <= max);
  EXPECT(one_line(clpd_db_error(db)));
  EXPECT(contains(clpd_db_error(db), "lock timeout"));
  EXPECT(contains(clpd_db_error(db), "(SQLSTATE 55P03)"));
  EXPECT(scalar(db, "SELECT 1", NULL, 0, &v, NULL, 0));
  EXPECT(v.type == CLPD_DB_INTEGER && v.integer == 1);
}

static void case_lock_timeout(void) {
  char spec[1024];
  clpd_db *holder = open_db(conninfo), *fast, *slow;
  EXPECT(holder != NULL);
  if (holder == NULL) return;
  EXPECT(run(holder, "CREATE TABLE pg_lock (id BIGINT PRIMARY KEY, "
                     "n BIGINT)") == CLPD_DB_OK);
  EXPECT(run(holder, "INSERT INTO pg_lock VALUES (1, 0)") == CLPD_DB_OK);
  EXPECT(clpd_db_begin(holder) == CLPD_DB_OK);
  EXPECT(run(holder, "UPDATE pg_lock SET n = 1 WHERE id = 1") == CLPD_DB_OK);

  /* Override through options. */
  snprintf(spec, sizeof spec, "%s options='-c lock_timeout=200ms'", conninfo);
  fast = open_db(spec);
  EXPECT(fast != NULL);
  if (fast != NULL) {
    wait_for_lock(fast, 0.15, 3.0);
    /* Inside a transaction: the statement fails, rollback ends it. */
    EXPECT(clpd_db_begin(fast) == CLPD_DB_OK);
    EXPECT(run(fast, "UPDATE pg_lock SET n = 2 WHERE id = 1") ==
           CLPD_DB_ERROR);
    EXPECT(contains(clpd_db_error(fast), "55P03"));
    EXPECT(clpd_db_rollback(fast) == CLPD_DB_OK);
    wait_for_lock(fast, 0.15, 3.0);
  }
  clpd_db_close(fast);

  /* The 5 s default. */
  slow = open_db(conninfo);
  EXPECT(slow != NULL);
  if (slow != NULL) wait_for_lock(slow, 4.5, 8.0);
  clpd_db_close(slow);

  EXPECT(clpd_db_rollback(holder) == CLPD_DB_OK);
  EXPECT(run(holder, "DROP TABLE pg_lock") == CLPD_DB_OK);
  clpd_db_close(holder);
}

/* A statement over statement_timeout is CLPD_DB_ERROR (57014); the
 * connection stays usable. */
static void case_statement_timeout(void) {
  char spec[1024];
  clpd_db *db;
  clpd_db_value v;
  double start;
  snprintf(spec, sizeof spec, "%s options='-c statement_timeout=300ms'",
           conninfo);
  db = open_db(spec);
  EXPECT(db != NULL);
  if (db == NULL) return;
  start = now();
  EXPECT(run(db, "SELECT pg_sleep(20)") == CLPD_DB_ERROR);
  start = now() - start;
  EXPECT(start >= 0.25 && start <= 3.0);
  EXPECT(one_line(clpd_db_error(db)));
  EXPECT(contains(clpd_db_error(db), "statement timeout"));
  EXPECT(contains(clpd_db_error(db), "(SQLSTATE 57014)"));
  EXPECT(scalar(db, "SELECT 2", NULL, 0, &v, NULL, 0));
  EXPECT(v.type == CLPD_DB_INTEGER && v.integer == 2);
  clpd_db_close(db);
}

/* connect_timeout: a TCP listener that never answers (the kernel accepts
 * the connection, nobody reads the startup packet). */
static int silent_port;
static const char *connect_extra;
static double connect_min, connect_max;

static void case_connect_timeout(void) {
  char spec[256], msg[512] = "";
  clpd_db *db = (clpd_db *)&db;
  double start;
  snprintf(spec, sizeof spec,
           "host=127.0.0.1 port=%d dbname=clpd_test user=clpd_test "
           "sslmode=disable gssencmode=disable%s",
           silent_port, connect_extra);
  start = now();
  EXPECT(clpd_db_open(spec, &db, msg, sizeof msg) == CLPD_DB_ERROR);
  start = now() - start;
  EXPECT(db == NULL);
  EXPECT(one_line(msg));
  EXPECT(contains(msg, "timeout expired"));
  if (!(start >= connect_min && start <= connect_max)) {
    failures++;
    fprintf(stderr, "FAIL connect timeout%s: %.1f s, expected %.0f-%.0f\n",
            connect_extra, start, connect_min, connect_max);
  }
}

static void case_connect_timeout_env(void) {
  setenv("PGCONNECT_TIMEOUT", "3", 1);
  case_connect_timeout();
}

static void test_connect_timeout(void) {
  struct sockaddr_in a;
  socklen_t len = sizeof a;
  int s = socket(AF_INET, SOCK_STREAM, 0), ok;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ok = s >= 0 && bind(s, (struct sockaddr *)&a, sizeof a) == 0 &&
       listen(s, 16) == 0 &&
       getsockname(s, (struct sockaddr *)&a, &len) == 0;
  EXPECT(ok);
  if (!ok) {
    if (s >= 0) close(s);
    return;
  }
  silent_port = ntohs(a.sin_port);
  connect_extra = ""; /* the 10 s default */
  connect_min = 9.0;
  connect_max = 13.0;
  bounded("connect_timeout default", case_connect_timeout, 20);
  connect_extra = " connect_timeout=2"; /* the spec's value wins */
  connect_min = 1.5;
  connect_max = 4.5;
  bounded("connect_timeout=2", case_connect_timeout, 10);
  connect_extra = ""; /* PGCONNECT_TIMEOUT wins over the default */
  connect_min = 2.5;
  connect_max = 5.5;
  bounded("PGCONNECT_TIMEOUT=3", case_connect_timeout_env, 10);
  close(s);
}

/* Link seam (-Wl,--wrap=PQunescapeBytea, this test binary only). */
static int fail_unescape;
unsigned char *__real_PQunescapeBytea(const unsigned char *from, size_t *n);
unsigned char *__wrap_PQunescapeBytea(const unsigned char *from, size_t *n);
unsigned char *__wrap_PQunescapeBytea(const unsigned char *from, size_t *n) {
  if (fail_unescape) return NULL;
  return __real_PQunescapeBytea(from, n);
}

static char oom_log[64], oom_err[64];

static void case_out_of_memory(void) {
  FILE *log = fopen(oom_log, "w"), *err = freopen(oom_err, "w", stderr);
  clpd_db *db;
  if (log == NULL || err == NULL) _exit(4);
  clpd_log_set(log);
  db = open_db(conninfo);
  if (db == NULL) _exit(5);
  fail_unescape = 1;
  run(db, "SELECT '\\x00ff'::bytea");
  _exit(3);
}

/* Out of memory decoding a bytea: a fixed message on the daemon log
 * stream (not stdio stderr, no errno text) and exit 1. */
static void test_out_of_memory(void) {
  char dir[] = "/tmp/clpd_db_pg_test.XXXXXX", buf[256];
  int status = 0;
  pid_t pid;
  FILE *f;
  size_t n;
  if (mkdtemp(dir) == NULL) {
    EXPECT(0);
    return;
  }
  snprintf(oom_log, sizeof oom_log, "%s/log", dir);
  snprintf(oom_err, sizeof oom_err, "%s/err", dir);
  fflush(stdout);
  fflush(stderr);
  pid = fork();
  if (pid == 0) case_out_of_memory();
  EXPECT(pid > 0 && waitpid(pid, &status, 0) == pid);
  EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 1);
  buf[0] = '\0';
  if ((f = fopen(oom_log, "r")) != NULL) {
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
  }
  EXPECT(strcmp(buf, "clpdaemon: out of memory\n") == 0);
  n = 1;
  if ((f = fopen(oom_err, "r")) != NULL) {
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
  }
  EXPECT(n == 0);
  unlink(oom_log);
  unlink(oom_err);
  rmdir(dir);
}

/* A commit the server turns into a rollback is an error. */
static void test_aborted_commit(void) {
  clpd_db *db = open_db(conninfo);
  clpd_db_value v;
  EXPECT(db != NULL);
  if (db == NULL) return;
  EXPECT(run(db, "CREATE TEMP TABLE pg_abort (id BIGINT PRIMARY KEY)") ==
         CLPD_DB_OK);
  EXPECT(clpd_db_begin(db) == CLPD_DB_OK);
  EXPECT(run(db, "INSERT INTO pg_abort (id) VALUES (1)") == CLPD_DB_OK);
  EXPECT(run(db, "SELECT nothing FROM missing_table") == CLPD_DB_ERROR);
  EXPECT(clpd_db_commit(db) == CLPD_DB_ERROR);
  EXPECT(one_line(clpd_db_error(db)));
  EXPECT(clpd_db_in_transaction(db) == 0);
  EXPECT(scalar(db, "SELECT count(*) FROM pg_abort", NULL, 0, &v, NULL, 0));
  EXPECT(v.type == CLPD_DB_INTEGER && v.integer == 0);
  /* The connection is usable again. */
  EXPECT(clpd_db_begin(db) == CLPD_DB_OK);
  EXPECT(run(db, "INSERT INTO pg_abort (id) VALUES (2)") == CLPD_DB_OK);
  EXPECT(clpd_db_commit(db) == CLPD_DB_OK);
  EXPECT(scalar(db, "SELECT count(*) FROM pg_abort", NULL, 0, &v, NULL, 0));
  EXPECT(v.type == CLPD_DB_INTEGER && v.integer == 1);
  clpd_db_close(db);
}

/* Terminates the backend of `victim` from `admin` and waits until it is
 * gone. */
static void terminate(clpd_db *admin, clpd_db *victim) {
  clpd_db_value v, pid;
  struct timespec ts = {0, 20000000};
  int i;
  EXPECT(scalar(victim, "SELECT pg_backend_pid()", NULL, 0, &v, NULL, 0));
  EXPECT(v.type == CLPD_DB_INTEGER);
  pid = clpd_db_int(v.integer);
  EXPECT(scalar(admin, "SELECT pg_terminate_backend($1::int4)", &pid, 1, &v,
                NULL, 0));
  EXPECT(v.type == CLPD_DB_INTEGER && v.integer == 1);
  for (i = 0; i < 500; i++) { /* at most 10 s */
    if (scalar(admin, "SELECT count(*) FROM pg_stat_activity "
                      "WHERE pid = $1::int4", &pid, 1, &v, NULL, 0) &&
        v.type == CLPD_DB_INTEGER && v.integer == 0)
      return;
    nanosleep(&ts, NULL);
  }
  failures++;
  fprintf(stderr, "FAIL %s:%d: the terminated backend is still listed\n",
          __FILE__, __LINE__);
}

static void test_lost(void) {
  clpd_db *admin = open_db(conninfo), *db = open_db(conninfo);
  clpd_db_value v;
  EXPECT(admin != NULL && db != NULL);
  if (admin == NULL || db == NULL) {
    clpd_db_close(admin);
    clpd_db_close(db);
    return;
  }
  terminate(admin, db);
  EXPECT(run(db, "SELECT 1") == CLPD_DB_LOST);
  EXPECT(one_line(clpd_db_error(db)));
  EXPECT(!contains(clpd_db_error(db), conninfo));
  EXPECT(run(db, "SELECT 1") == CLPD_DB_LOST); /* stays lost */
  EXPECT(clpd_db_begin(db) == CLPD_DB_LOST);
  EXPECT(clpd_db_in_transaction(db) == 0);
  clpd_db_close(db);

  /* Inside a transaction: LOST ends it and nothing was committed. */
  db = open_db(conninfo);
  EXPECT(db != NULL);
  if (db != NULL) {
    EXPECT(run(admin, "CREATE TABLE pg_lost (id BIGINT PRIMARY KEY)") ==
           CLPD_DB_OK);
    EXPECT(clpd_db_begin(db) == CLPD_DB_OK);
    EXPECT(run(db, "INSERT INTO pg_lost (id) VALUES (1)") == CLPD_DB_OK);
    terminate(admin, db);
    EXPECT(run(db, "INSERT INTO pg_lost (id) VALUES (2)") == CLPD_DB_LOST);
    EXPECT(clpd_db_in_transaction(db) == 0);
    EXPECT(clpd_db_rollback(db) == CLPD_DB_ERROR); /* no transaction */
    EXPECT(scalar(admin, "SELECT count(*) FROM pg_lost", NULL, 0, &v, NULL,
                  0));
    EXPECT(v.type == CLPD_DB_INTEGER && v.integer == 0);
    EXPECT(run(admin, "DROP TABLE pg_lost") == CLPD_DB_OK);
    clpd_db_close(db);
  }

  /* A lost connection inside a transaction is also LOST on commit. */
  db = open_db(conninfo);
  EXPECT(db != NULL);
  if (db != NULL) {
    EXPECT(clpd_db_begin(db) == CLPD_DB_OK);
    terminate(admin, db);
    EXPECT(clpd_db_commit(db) == CLPD_DB_LOST);
    EXPECT(clpd_db_in_transaction(db) == 0);
    clpd_db_close(db);
  }
  clpd_db_close(admin);
}

int main(int argc, char **argv) {
  int suite;
  if (argc != 3) {
    fprintf(stderr, "usage: db_pg_test <conninfo> <password>\n");
    return 2;
  }
  conninfo = argv[1];
  password = argv[2];
  test_password_rejected();
  test_password_file();
  test_types_and_messages();
  test_aborted_commit();
  test_lost();
  test_long_class22();
  test_out_of_memory();
  test_session_timeouts();
  bounded("lock_timeout", case_lock_timeout, 25);
  bounded("statement_timeout", case_statement_timeout, 10);
  test_connect_timeout();
  suite = clpd_db_suite(conninfo);
  if (suite != 0) fprintf(stderr, "shared suite: %d failed checks\n", suite);
  failures += suite;
  if (failures != 0) {
    fprintf(stderr, "%d failed checks\n", failures);
    return 1;
  }
  return 0;
}
