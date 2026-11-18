/* Tests for src/db.h (T8a).
 * usage: db_test                 spec checks (clpd_db_check_spec, open)
 *        db_test sqlite-suite    shared suite on sqlite:<temp file> */
#define _POSIX_C_SOURCE 200809L

#include "db.h"
#include "db_suite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
      "postgresql:///clpd?host=/tmp"};
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
  if (failures != 0) {
    fprintf(stderr, "%d db checks failed\n", failures);
    return 1;
  }
  puts("db checks passed");
  return 0;
}
