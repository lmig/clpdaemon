/* Tests for src/db.h (T8a) and the SQLite backend (T8b).
 * usage: db_test                 spec checks (clpd_db_check_spec, open) and
 *                                SQLite-specific cases
 *        db_test sqlite-suite    shared suite on sqlite:<temp file> */
#define _POSIX_C_SOURCE 200809L

#include "db.h"
#include "db_suite.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "expect.h"

#define PASSWORD_MSG                                                       \
  "database: the connection string must not contain a password (use a "  \
  "password file, see PGPASSFILE)"

static void test_check_spec(void) {
  static const char *const good[] = {
      "sqlite:/tmp/x.db", "sqlite:x.db", "sqlite:a:b",
      "host=/tmp dbname=clpd", "dbname=clpd user=clpd sslmode=disable",
      "host=/tmp password=", /* an empty password is no password */
      "postgresql://clpd@localhost/clpd", "postgres://localhost:5432/clpd",
      "postgresql:///clpd?host=/tmp",
      /* A dbname that is itself a connection string, without a password. */
      "host=/tmp dbname='host=h dbname=clpd'",
      "host=/tmp dbname='postgresql://u@h/d'",
      "postgresql:///?dbname=host%3Dh%20dbname%3Dclpd"};
  static const struct {
    const char *value, *message;
  } bad[] = {
      {"", "database: expected a connection string"},
      {"sqlite:", "database: expected sqlite:<path>"},
      {"sqlite", "database: expected sqlite:<path>"},
      {"not a conninfo", "database: invalid PostgreSQL connection string"},
      {"host='s3cr3t", "database: invalid PostgreSQL connection string"},
      {"s3cr3t=1", "database: invalid PostgreSQL connection string"},
      {"postgresql://[::1", "database: invalid PostgreSQL connection string"},
      {"host=/tmp password=s3cr3t", PASSWORD_MSG},
      {"password='s3cr3t' host=/tmp", PASSWORD_MSG},
      {"host=/tmp sslpassword=s3cr3t", PASSWORD_MSG},
      {"postgresql://clpd:s3cr3t@localhost/clpd", PASSWORD_MSG},
      {"postgres://localhost/clpd?password=s3cr3t", PASSWORD_MSG},
      {"postgresql://localhost/clpd?sslpassword=s3cr3t", PASSWORD_MSG},
      /* A password inside a dbname that is a connection string (L78). */
      {"host=/tmp dbname='postgresql://u:s3cr3t@h/d'", PASSWORD_MSG},
      {"host=/tmp dbname='host=h password=s3cr3t'", PASSWORD_MSG},
      {"host=/tmp dbname=postgres://u:s3cr3t@h/d", PASSWORD_MSG},
      {"host=/tmp dbname='host=h sslpassword=s3cr3t'", PASSWORD_MSG},
      {"postgresql:///?dbname=host%3Dh%20password%3Ds3cr3t", PASSWORD_MSG},
      {"postgresql:///?dbname=postgresql%3A%2F%2Fu%3As3cr3t%40h%2Fd",
       PASSWORD_MSG},
      {"dbname='dbname=password=s3cr3t'", PASSWORD_MSG}, /* two levels */
      {"host=/tmp dbname='s3cr3t=1'",
       "database: invalid PostgreSQL connection string"},
      {"host=/tmp dbname='postgresql://[::1'",
       "database: invalid PostgreSQL connection string"},
  };
  char msg[256];
  size_t i;

  for (i = 0; i < sizeof good / sizeof good[0]; i++) {
    strcpy(msg, "untouched");
    if (clpd_db_check_spec(good[i], msg, sizeof msg) != 1) {
      fprintf(stderr, "spec rejected: %s (%s)\n", good[i], msg);
      EXPECT(0);
    }
  }
  for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    msg[0] = '\0';
    EXPECT(clpd_db_check_spec(bad[i].value, msg, sizeof msg) == 0);
    if (strcmp(msg, bad[i].message) != 0) {
      fprintf(stderr, "spec %zu: got '%s'\n", i, msg);
      EXPECT(0);
    }
    EXPECT(strstr(msg, "s3cr3t") == NULL);
  }
  /* Nested dbname connection strings are followed up to a bound. */
  {
    char deep[256] = "";
    int k;
    for (k = 0; k < 8; k++) strcat(deep, "dbname=");
    strcat(deep, "x");
    EXPECT(clpd_db_check_spec(deep, msg, sizeof msg) == 1);
    deep[0] = '\0';
    for (k = 0; k < 20; k++) strcat(deep, "dbname=");
    strcat(deep, "x");
    EXPECT(clpd_db_check_spec(deep, msg, sizeof msg) == 0);
    EXPECT(strcmp(msg, "database: invalid PostgreSQL connection string") ==
           0);
  }
  /* A short buffer is truncated, not overrun. */
  memset(msg, 'X', sizeof msg);
  EXPECT(clpd_db_check_spec("", msg, 9) == 0);
  EXPECT(strcmp(msg, "database") == 0 && msg[9] == 'X');
}

/* clpd_db_open checks the spec first: no handle, the same message. */
static void test_open_rejects(void) {
  clpd_db *db = (clpd_db *)&db;
  char msg[256];
  EXPECT(clpd_db_open("host=/tmp password=s3cr3t", &db, msg, sizeof msg) ==
         CLPD_DB_ERROR);
  EXPECT(db == NULL);
  EXPECT(strcmp(msg, PASSWORD_MSG) == 0);
  db = (clpd_db *)&db;
  EXPECT(clpd_db_open("sqlite:", &db, msg, sizeof msg) == CLPD_DB_ERROR);
  EXPECT(db == NULL);
  EXPECT(strcmp(msg, "database: expected sqlite:<path>") == 0);
  clpd_db_close(NULL); /* accepted */
  clpd_db_result_free(NULL);
}

/* SQLite-specific cases the shared suite does not cover. */

static int one_line(const char *s) {
  return s != NULL && *s != '\0' && strchr(s, '\n') == NULL &&
         strchr(s, '\r') == NULL;
}

static int64_t sq_count(clpd_db *db, const char *sql) {
  clpd_db_result *r = NULL;
  const clpd_db_value *v;
  int64_t n = -1;
  if (clpd_db_exec(db, sql, NULL, 0, &r) == CLPD_DB_OK &&
      clpd_db_result_rows(r) == 1) {
    v = clpd_db_result_value(r, 0, 0);
    if (v != NULL && v->type == CLPD_DB_INTEGER) n = v->integer;
  }
  clpd_db_result_free(r);
  return n;
}

static int is_text_value(const clpd_db_value *v, const char *s) {
  size_t n = strlen(s);
  return v != NULL && v->type == CLPD_DB_TEXT && v->size == n &&
         memcmp(v->data, s, n) == 0;
}

/* A missing directory or a file that is not a database fails at open with
 * one line that names neither the path nor the spec. */
static void test_sqlite_open_failures(const char *dir) {
  char spec[320], path[256], msg[256];
  clpd_db *db = (clpd_db *)&db;
  struct stat sb;
  FILE *f;
  size_t i;

  snprintf(path, sizeof path, "%s/missing", dir);
  snprintf(spec, sizeof spec, "sqlite:%s/missing/x.db", dir);
  EXPECT(clpd_db_open(spec, &db, msg, sizeof msg) == CLPD_DB_ERROR);
  EXPECT(db == NULL);
  EXPECT(one_line(msg));
  EXPECT(strstr(msg, dir) == NULL);
  EXPECT(stat(path, &sb) != 0); /* nothing created */

  snprintf(path, sizeof path, "%s/junk.db", dir);
  f = fopen(path, "w");
  EXPECT(f != NULL);
  if (f == NULL) return;
  for (i = 0; i < 1024; i++) fputc('x', f);
  fclose(f);
  snprintf(spec, sizeof spec, "sqlite:%s", path);
  db = (clpd_db *)&db;
  EXPECT(clpd_db_open(spec, &db, msg, sizeof msg) == CLPD_DB_ERROR);
  EXPECT(db == NULL);
  EXPECT(one_line(msg));
  EXPECT(strstr(msg, dir) == NULL);
  EXPECT(stat(path, &sb) == 0 && sb.st_size == 1024); /* left untouched */
  unlink(path);
}

/* Foreign keys are enforced; constraint errors carry no parameter value. */
static void test_sqlite_constraints(clpd_db *db) {
  clpd_db_value p[2];
  const char *ins = "INSERT INTO parent (id, name) VALUES ($1, $2)";

  EXPECT(clpd_db_exec(db, "CREATE TABLE parent (id BIGINT PRIMARY KEY, "
                          "name TEXT UNIQUE)", NULL, 0, NULL) == CLPD_DB_OK);
  EXPECT(clpd_db_exec(db, "CREATE TABLE child (id BIGINT PRIMARY KEY, "
                          "parent_id BIGINT REFERENCES parent (id))",
                      NULL, 0, NULL) == CLPD_DB_OK);
  p[0] = clpd_db_int(1);
  p[1] = clpd_db_text("s3cr3t-value");
  EXPECT(clpd_db_exec(db, ins, p, 2, NULL) == CLPD_DB_OK);
  p[0] = clpd_db_int(2);
  EXPECT(clpd_db_exec(db, ins, p, 2, NULL) == CLPD_DB_UNIQUE);
  EXPECT(one_line(clpd_db_error(db)));
  EXPECT(strstr(clpd_db_error(db), "s3cr3t") == NULL);

  p[0] = clpd_db_int(10);
  p[1] = clpd_db_int(424242); /* no such parent */
  EXPECT(clpd_db_exec(db, "INSERT INTO child (id, parent_id) VALUES ($1, $2)",
                      p, 2, NULL) == CLPD_DB_ERROR);
  EXPECT(one_line(clpd_db_error(db)));
  EXPECT(strstr(clpd_db_error(db), "424242") == NULL);
  EXPECT(sq_count(db, "SELECT count(*) FROM child") == 0);
  p[1] = clpd_db_int(1);
  EXPECT(clpd_db_exec(db, "INSERT INTO child (id, parent_id) VALUES ($1, $2)",
                      p, 2, NULL) == CLPD_DB_OK);
  EXPECT(clpd_db_exec(db, "DELETE FROM parent WHERE id = $1",
                      (clpd_db_value[]){clpd_db_int(1)}, 1, NULL) ==
         CLPD_DB_ERROR); /* still referenced */
  EXPECT(sq_count(db, "SELECT count(*) FROM parent") == 1);
}

/* PostgreSQL type names: TIMESTAMPTZ and JSONB read back as TEXT even when
 * the text looks like a number, BYTEA keeps BLOB bytes. */
static void test_sqlite_pg_types(clpd_db *db) {
  static const unsigned char raw[] = {'1', '2', '3'};
  clpd_db_value p[4];
  clpd_db_result *r = NULL;
  const char *ins = "INSERT INTO pgt (id, ts, doc, raw) "
                    "VALUES ($1, $2, $3, $4)";

  EXPECT(clpd_db_exec(db, "CREATE TABLE pgt (id BIGINT PRIMARY KEY, "
                          "ts TIMESTAMPTZ, doc JSONB, raw BYTEA)",
                      NULL, 0, NULL) == CLPD_DB_OK);
  p[0] = clpd_db_int(1);
  p[1] = clpd_db_text("2024-05-01 10:00:00.123+00");
  p[2] = clpd_db_text("{\"a\": [1, 2]}");
  p[3] = clpd_db_blob("\0\1", 2);
  EXPECT(clpd_db_exec(db, ins, p, 4, NULL) == CLPD_DB_OK);
  p[0] = clpd_db_int(2);
  p[1] = clpd_db_text("1714557600");
  p[2] = clpd_db_text("123");
  p[3] = clpd_db_blob(raw, sizeof raw);
  EXPECT(clpd_db_exec(db, ins, p, 4, NULL) == CLPD_DB_OK);
  p[0] = clpd_db_int(3);
  p[1] = clpd_db_null();
  p[2] = clpd_db_null();
  p[3] = clpd_db_null();
  EXPECT(clpd_db_exec(db, ins, p, 4, NULL) == CLPD_DB_OK);

  EXPECT(clpd_db_exec(db, "SELECT ts, doc, raw FROM pgt ORDER BY id", NULL, 0,
                      &r) == CLPD_DB_OK);
  EXPECT(r != NULL && clpd_db_result_rows(r) == 3);
  if (r != NULL && clpd_db_result_rows(r) == 3) {
    const clpd_db_value *v;
    EXPECT(is_text_value(clpd_db_result_value(r, 0, 0),
                         "2024-05-01 10:00:00.123+00"));
    EXPECT(is_text_value(clpd_db_result_value(r, 0, 1), "{\"a\": [1, 2]}"));
    v = clpd_db_result_value(r, 0, 2);
    EXPECT(v->type == CLPD_DB_BLOB && v->size == 2 &&
           memcmp(v->data, "\0\1", 2) == 0);
    EXPECT(is_text_value(clpd_db_result_value(r, 1, 0), "1714557600"));
    EXPECT(is_text_value(clpd_db_result_value(r, 1, 1), "123"));
    v = clpd_db_result_value(r, 1, 2);
    EXPECT(v->type == CLPD_DB_BLOB && v->size == sizeof raw &&
           memcmp(v->data, raw, sizeof raw) == 0);
    EXPECT(clpd_db_result_value(r, 2, 0)->type == CLPD_DB_NULL);
    EXPECT(clpd_db_result_value(r, 2, 1)->type == CLPD_DB_NULL);
    EXPECT(clpd_db_result_value(r, 2, 2)->type == CLPD_DB_NULL);
  }
  clpd_db_result_free(r);
}

/* Affected rows: DML only (never a stale count from an earlier statement),
 * INSERT ... RETURNING and WITH-prefixed DML included. */
static void test_sqlite_affected(clpd_db *db) {
  clpd_db_result *r = NULL;

  EXPECT(clpd_db_exec(db, "UPDATE pgt SET doc = 'x' WHERE id >= $1",
                      (clpd_db_value[]){clpd_db_int(1)}, 1, &r) == CLPD_DB_OK);
  EXPECT(r != NULL && clpd_db_result_affected(r) == 3);
  clpd_db_result_free(r);
  r = NULL;
  EXPECT(clpd_db_exec(db, "CREATE TABLE t2 (x BIGINT)", NULL, 0, &r) ==
         CLPD_DB_OK);
  EXPECT(r != NULL && clpd_db_result_affected(r) == 0 &&
         clpd_db_result_columns(r) == 0);
  clpd_db_result_free(r);
  r = NULL;
  EXPECT(clpd_db_exec(db, "INSERT INTO t2 (x) VALUES ($1) RETURNING x + 1 "
                          "AS next",
                      (clpd_db_value[]){clpd_db_int(41)}, 1, &r) == CLPD_DB_OK);
  EXPECT(r != NULL && clpd_db_result_affected(r) == 1 &&
         clpd_db_result_rows(r) == 1);
  EXPECT(r != NULL && clpd_db_result_value(r, 0, 0) != NULL &&
         clpd_db_result_value(r, 0, 0)->integer == 42);
  clpd_db_result_free(r);
  r = NULL;
  EXPECT(clpd_db_exec(db, "WITH d AS (SELECT $1 AS x) SELECT x FROM d",
                      (clpd_db_value[]){clpd_db_int(41)}, 1, &r) == CLPD_DB_OK);
  EXPECT(r != NULL && clpd_db_result_affected(r) == 0);
  clpd_db_result_free(r);
  r = NULL;
  EXPECT(clpd_db_exec(db, "/* c */ WITH d AS (SELECT $1 AS x) "
                          "DELETE FROM t2 WHERE x IN (SELECT x FROM d)",
                      (clpd_db_value[]){clpd_db_int(41)}, 1, &r) == CLPD_DB_OK);
  EXPECT(r != NULL && clpd_db_result_affected(r) == 1);
  clpd_db_result_free(r);
  r = NULL;
  EXPECT(clpd_db_exec(db, "DROP TABLE t2", NULL, 0, &r) == CLPD_DB_OK);
  EXPECT(r != NULL && clpd_db_result_affected(r) == 0);
  clpd_db_result_free(r);
}

/* Placeholders SQLite would read as another parameter name (Tcl-style
 * $1::x and $1(x)) are rejected and nothing runs. */
static void test_sqlite_placeholders(clpd_db *db) {
  static const char *const bad[] = {
      "INSERT INTO pgt (id, doc) VALUES (99, $1::text)",
      "INSERT INTO pgt (id, doc) VALUES (99, $1(x))",
  };
  clpd_db_result *r;
  size_t i;
  for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    r = (clpd_db_result *)&r;
    EXPECT(clpd_db_exec(db, bad[i], (clpd_db_value[]){clpd_db_text("v")}, 1,
                        &r) == CLPD_DB_ERROR);
    EXPECT(r == NULL);
    EXPECT(one_line(clpd_db_error(db)));
  }
  EXPECT(sq_count(db, "SELECT count(*) FROM pgt") == 3);
}

/* Closing with an open transaction rolls it back. */
static void test_sqlite_close_rolls_back(clpd_db **db, const char *spec) {
  char msg[256];
  EXPECT(clpd_db_begin(*db) == CLPD_DB_OK);
  EXPECT(clpd_db_exec(*db, "DELETE FROM pgt", NULL, 0, NULL) == CLPD_DB_OK);
  EXPECT(sq_count(*db, "SELECT count(*) FROM pgt") == 0);
  clpd_db_close(*db);
  *db = NULL;
  EXPECT(clpd_db_open(spec, db, msg, sizeof msg) == CLPD_DB_OK);
  if (*db != NULL) EXPECT(sq_count(*db, "SELECT count(*) FROM pgt") == 3);
}

static void test_sqlite(void) {
  char dir[] = "/tmp/clpd_db_test.XXXXXX", file[64], spec[80], extra[80];
  static const char *const suffixes[] = {"", "-journal", "-wal", "-shm"};
  clpd_db *db = NULL;
  char msg[256];
  size_t i;

  if (mkdtemp(dir) == NULL) {
    perror("db_test");
    EXPECT(0);
    return;
  }
  test_sqlite_open_failures(dir);
  snprintf(file, sizeof file, "%s/extra.db", dir);
  snprintf(spec, sizeof spec, "sqlite:%s", file);
  EXPECT(clpd_db_open(spec, &db, msg, sizeof msg) == CLPD_DB_OK);
  if (db != NULL) {
    EXPECT(strcmp(clpd_db_backend(db), "sqlite") == 0);
    test_sqlite_constraints(db);
    test_sqlite_pg_types(db);
    test_sqlite_placeholders(db);
    test_sqlite_affected(db);
    test_sqlite_close_rolls_back(&db, spec);
  }
  clpd_db_close(db);
  for (i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++) {
    snprintf(extra, sizeof extra, "%s%s", file, suffixes[i]);
    unlink(extra);
  }
  rmdir(dir);
}

/* Shared suite on a fresh SQLite file. */
static int sqlite_suite(void) {
  char dir[] = "/tmp/clpd_db_test.XXXXXX", file[64], spec[80], extra[80];
  static const char *const suffixes[] = {"", "-journal", "-wal", "-shm"};
  size_t i;
  int n;

  if (mkdtemp(dir) == NULL) {
    perror("db_test");
    return 2;
  }
  snprintf(file, sizeof file, "%s/suite.db", dir);
  snprintf(spec, sizeof spec, "sqlite:%s", file);
  n = clpd_db_suite(spec);
  for (i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++) {
    snprintf(extra, sizeof extra, "%s%s", file, suffixes[i]);
    unlink(extra);
  }
  rmdir(dir);
  if (n != 0) {
    fprintf(stderr, "%d db suite checks failed (sqlite)\n", n);
    return 1;
  }
  puts("db suite checks passed (sqlite)");
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "sqlite-suite") == 0) return sqlite_suite();
  if (argc != 1) {
    fputs("usage: db_test [sqlite-suite]\n", stderr);
    return 2;
  }
  test_check_spec();
  test_open_rejects();
  test_sqlite();
  if (failures != 0) {
    fprintf(stderr, "%d db checks failed\n", failures);
    return 1;
  }
  puts("db checks passed");
  return 0;
}
