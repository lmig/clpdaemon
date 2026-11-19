/* Shared backend suite for src/db.h (T8a). Uses PostgreSQL DDL type names
 * (BIGINT, DOUBLE PRECISION, TEXT, BYTEA) that SQLite also accepts. */
#include "db_suite.h"

#include "db.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fails;
static clpd_db *db;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      fails++;                                                             \
      fprintf(stderr, "FAIL %s:%d: %s (last error: %s)\n", __FILE__,       \
              __LINE__, #cond, db != NULL ? clpd_db_error(db) : "-");      \
    }                                                                      \
  } while (0)

/* Runs sql without parameters, discarding the rows. */
static clpd_db_status run(clpd_db *c, const char *sql) {
  return clpd_db_exec(c, sql, NULL, 0, NULL);
}

static clpd_db_status run1(clpd_db *c, const char *sql, clpd_db_value p) {
  return clpd_db_exec(c, sql, &p, 1, NULL);
}

/* SELECT count(*) FROM table, or -1. */
static int64_t count(clpd_db *c, const char *table) {
  char sql[128];
  clpd_db_result *r = NULL;
  const clpd_db_value *v;
  int64_t n = -1;
  snprintf(sql, sizeof sql, "SELECT count(*) FROM %s", table);
  if (clpd_db_exec(c, sql, NULL, 0, &r) == CLPD_DB_OK &&
      clpd_db_result_rows(r) == 1 && clpd_db_result_columns(r) == 1) {
    v = clpd_db_result_value(r, 0, 0);
    if (v != NULL && v->type == CLPD_DB_INTEGER) n = v->integer;
  }
  clpd_db_result_free(r);
  return n;
}

/* The last error is one non-empty line. */
static int one_line_error(void) {
  const char *e = clpd_db_error(db);
  return e != NULL && *e != '\0' && strchr(e, '\n') == NULL &&
         strchr(e, '\r') == NULL;
}

static int is_null(const clpd_db_value *v) {
  return v != NULL && v->type == CLPD_DB_NULL;
}

static int is_int(const clpd_db_value *v, int64_t x) {
  return v != NULL && v->type == CLPD_DB_INTEGER && v->integer == x;
}

static int is_float(const clpd_db_value *v, double x) {
  return v != NULL && v->type == CLPD_DB_FLOAT && v->real == x;
}

/* TEXT results are NUL-terminated after `size` bytes. */
static int is_text(const clpd_db_value *v, const char *s) {
  size_t n = strlen(s);
  return v != NULL && v->type == CLPD_DB_TEXT && v->size == n &&
         v->data != NULL && memcmp(v->data, s, n) == 0 &&
         ((const char *)v->data)[n] == '\0';
}

static int is_blob(const clpd_db_value *v, const void *data, size_t size) {
  return v != NULL && v->type == CLPD_DB_BLOB && v->size == size &&
         (size == 0 || (v->data != NULL && memcmp(v->data, data, size) == 0));
}

static const char injection[] = "O'Reilly'); DROP TABLE clpd_suite_types; --";
static const char utf8[] = "\xc3\xb1" "and\xc3\xba \xe2\x80\x94 "
                           "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e \xf0\x9f\x8e\x89";
static const char quotes[] = "quotes \" ' \\ $1 ? :x @y ; -- /*";
static const unsigned char blob_nul[] = {0, 1, 0, 255, 0};
static unsigned char blob_all[256];

static void insert_types(int64_t id, clpd_db_value i, clpd_db_value f,
                         clpd_db_value t, clpd_db_value b) {
  clpd_db_value p[5];
  clpd_db_result *r = NULL;
  p[0] = clpd_db_int(id);
  p[1] = i;
  p[2] = f;
  p[3] = t;
  p[4] = b;
  CHECK(clpd_db_exec(db,
                     "INSERT INTO clpd_suite_types (id, i, f, t, b) "
                     "VALUES ($1, $2, $3, $4, $5)",
                     p, 5, &r) == CLPD_DB_OK);
  CHECK(r != NULL && clpd_db_result_affected(r) == 1);
  CHECK(r != NULL && clpd_db_result_rows(r) == 0);
  clpd_db_result_free(r);
}

/* Every type round-trips, NULL included; text is stored literally. */
static void test_round_trip(void) {
  clpd_db_result *r = NULL, *r2 = NULL;
  size_t k;

  for (k = 0; k < sizeof blob_all; k++) blob_all[k] = (unsigned char)k;
  CHECK(run(db, "CREATE TABLE clpd_suite_types (id BIGINT PRIMARY KEY, "
                "i BIGINT, f DOUBLE PRECISION, t TEXT, b BYTEA)") == CLPD_DB_OK);
  insert_types(1, clpd_db_null(), clpd_db_null(), clpd_db_null(),
               clpd_db_null());
  insert_types(2, clpd_db_int(INT64_MIN), clpd_db_float(-2.5e-300),
               clpd_db_text(""), clpd_db_blob(NULL, 0));
  insert_types(3, clpd_db_int(INT64_MAX), clpd_db_float(1.7976931348623157e308),
               clpd_db_text(utf8), clpd_db_blob(blob_nul, sizeof blob_nul));
  insert_types(4, clpd_db_int(0), clpd_db_float(0.1), clpd_db_text(injection),
               clpd_db_blob("", 1));
  insert_types(5, clpd_db_int(-1), clpd_db_float(3.141592653589793),
               clpd_db_text(quotes), clpd_db_blob(blob_all, sizeof blob_all));
  CHECK(count(db, "clpd_suite_types") == 5); /* the injection did nothing */

  CHECK(clpd_db_exec(db, "SELECT id, i, f, t, b FROM clpd_suite_types "
                         "ORDER BY id", NULL, 0, &r) == CLPD_DB_OK);
  /* Results stay valid across later calls on the connection. */
  CHECK(clpd_db_exec(db, "SELECT t FROM clpd_suite_types WHERE id = $1",
                     (clpd_db_value[]){clpd_db_int(3)}, 1, &r2) == CLPD_DB_OK);
  CHECK(r2 != NULL && clpd_db_result_rows(r2) == 1 &&
        is_text(clpd_db_result_value(r2, 0, 0), utf8));
  clpd_db_result_free(r2);
  if (r == NULL) {
    CHECK(r != NULL);
    return;
  }
  CHECK(clpd_db_result_rows(r) == 5);
  CHECK(clpd_db_result_columns(r) == 5);
  CHECK(clpd_db_result_affected(r) == 0);
  if (clpd_db_result_rows(r) == 5 && clpd_db_result_columns(r) == 5) {
    CHECK(is_int(clpd_db_result_value(r, 0, 0), 1));
    for (k = 1; k < 5; k++) CHECK(is_null(clpd_db_result_value(r, 0, k)));

    CHECK(is_int(clpd_db_result_value(r, 1, 1), INT64_MIN));
    CHECK(is_float(clpd_db_result_value(r, 1, 2), -2.5e-300));
    CHECK(is_text(clpd_db_result_value(r, 1, 3), ""));
    CHECK(is_blob(clpd_db_result_value(r, 1, 4), NULL, 0));

    CHECK(is_int(clpd_db_result_value(r, 2, 1), INT64_MAX));
    CHECK(is_float(clpd_db_result_value(r, 2, 2), 1.7976931348623157e308));
    CHECK(is_text(clpd_db_result_value(r, 2, 3), utf8));
    CHECK(is_blob(clpd_db_result_value(r, 2, 4), blob_nul, sizeof blob_nul));

    CHECK(is_int(clpd_db_result_value(r, 3, 1), 0));
    CHECK(is_float(clpd_db_result_value(r, 3, 2), 0.1));
    CHECK(is_text(clpd_db_result_value(r, 3, 3), injection));
    CHECK(is_blob(clpd_db_result_value(r, 3, 4), "", 1));

    CHECK(is_int(clpd_db_result_value(r, 4, 1), -1));
    CHECK(is_float(clpd_db_result_value(r, 4, 2), 3.141592653589793));
    CHECK(is_text(clpd_db_result_value(r, 4, 3), quotes));
    CHECK(is_blob(clpd_db_result_value(r, 4, 4), blob_all, sizeof blob_all));
  }
  CHECK(clpd_db_result_value(r, 5, 0) == NULL);
  CHECK(clpd_db_result_value(r, 0, 5) == NULL);
  CHECK(clpd_db_result_column_name(r, 5) == NULL);
  clpd_db_result_free(r);

  /* A parameter compares literally. */
  r = NULL;
  CHECK(clpd_db_exec(db, "SELECT id FROM clpd_suite_types WHERE t = $1",
                     (clpd_db_value[]){clpd_db_text(injection)}, 1,
                     &r) == CLPD_DB_OK);
  CHECK(r != NULL && clpd_db_result_rows(r) == 1 &&
        is_int(clpd_db_result_value(r, 0, 0), 4));
  clpd_db_result_free(r);

  /* $n, ';' and '?' inside literals and comments are not placeholders. */
  r = NULL;
  CHECK(clpd_db_exec(db,
                     "SELECT id FROM clpd_suite_types WHERE id = $1 "
                     "AND t <> 'it''s $2; not ? a param' /* $3 ; */ "
                     "AND \"t\" <> 'x' -- $5 ;\n",
                     (clpd_db_value[]){clpd_db_int(3)}, 1, &r) == CLPD_DB_OK);
  CHECK(r != NULL && clpd_db_result_rows(r) == 1 &&
        is_int(clpd_db_result_value(r, 0, 0), 3));
  clpd_db_result_free(r);
}

static void test_unique(void) {
  clpd_db_value p[2];
  clpd_db_result *r = (clpd_db_result *)&r; /* must become NULL */
  const char *ins = "INSERT INTO clpd_suite_uniq (id, name) VALUES ($1, $2)";

  CHECK(run(db, "CREATE TABLE clpd_suite_uniq (id BIGINT PRIMARY KEY, "
                "name TEXT UNIQUE)") == CLPD_DB_OK);
  p[0] = clpd_db_int(1);
  p[1] = clpd_db_text("a");
  CHECK(clpd_db_exec(db, ins, p, 2, NULL) == CLPD_DB_OK);
  p[1] = clpd_db_text("b"); /* same primary key */
  CHECK(clpd_db_exec(db, ins, p, 2, &r) == CLPD_DB_UNIQUE);
  CHECK(r == NULL);
  CHECK(one_line_error());
  p[0] = clpd_db_int(2);
  p[1] = clpd_db_text("a"); /* same unique column */
  CHECK(clpd_db_exec(db, ins, p, 2, NULL) == CLPD_DB_UNIQUE);
  CHECK(one_line_error());
  CHECK(count(db, "clpd_suite_uniq") == 1);
}

static void test_affected_names_empty(void) {
  clpd_db_value p[2];
  clpd_db_result *r = NULL;
  const char *ins = "INSERT INTO clpd_suite_uniq (id, name) VALUES ($1, $2)";

  p[0] = clpd_db_int(2);
  p[1] = clpd_db_text("b");
  CHECK(clpd_db_exec(db, ins, p, 2, NULL) == CLPD_DB_OK);
  p[0] = clpd_db_int(3);
  p[1] = clpd_db_text("c");
  CHECK(clpd_db_exec(db, ins, p, 2, NULL) == CLPD_DB_OK);

  CHECK(clpd_db_exec(db, "UPDATE clpd_suite_uniq SET name = name || 'x' "
                         "WHERE id >= $1",
                     (clpd_db_value[]){clpd_db_int(2)}, 1, &r) == CLPD_DB_OK);
  CHECK(r != NULL && clpd_db_result_affected(r) == 2);
  clpd_db_result_free(r);
  r = NULL;
  CHECK(clpd_db_exec(db, "UPDATE clpd_suite_uniq SET name = 'z' WHERE id = $1",
                     (clpd_db_value[]){clpd_db_int(99)}, 1, &r) == CLPD_DB_OK);
  CHECK(r != NULL && clpd_db_result_affected(r) == 0);
  clpd_db_result_free(r);
  r = NULL;
  CHECK(clpd_db_exec(db, "DELETE FROM clpd_suite_uniq WHERE id = $1",
                     (clpd_db_value[]){clpd_db_int(3)}, 1, &r) == CLPD_DB_OK);
  CHECK(r != NULL && clpd_db_result_affected(r) == 1);
  clpd_db_result_free(r);
  CHECK(count(db, "clpd_suite_uniq") == 2);

  /* Column names, including a quoted mixed-case alias. */
  r = NULL;
  CHECK(clpd_db_exec(db, "SELECT id AS first_col, name AS \"Second\" "
                         "FROM clpd_suite_uniq WHERE id = $1",
                     (clpd_db_value[]){clpd_db_int(2)}, 1, &r) == CLPD_DB_OK);
  CHECK(r != NULL && clpd_db_result_columns(r) == 2 &&
        clpd_db_result_rows(r) == 1);
  CHECK(r != NULL && clpd_db_result_column_name(r, 0) != NULL &&
        strcmp(clpd_db_result_column_name(r, 0), "first_col") == 0);
  CHECK(r != NULL && clpd_db_result_column_name(r, 1) != NULL &&
        strcmp(clpd_db_result_column_name(r, 1), "Second") == 0);
  CHECK(r != NULL && is_text(clpd_db_result_value(r, 0, 1), "bx"));
  CHECK(r != NULL && clpd_db_result_affected(r) == 0);
  clpd_db_result_free(r);

  /* An empty result still has its columns. */
  r = NULL;
  CHECK(clpd_db_exec(db, "SELECT id, name FROM clpd_suite_uniq WHERE id < 0",
                     NULL, 0, &r) == CLPD_DB_OK);
  CHECK(r != NULL && clpd_db_result_rows(r) == 0 &&
        clpd_db_result_columns(r) == 2);
  CHECK(r != NULL && clpd_db_result_column_name(r, 0) != NULL &&
        strcmp(clpd_db_result_column_name(r, 0), "id") == 0);
  CHECK(r != NULL && clpd_db_result_column_name(r, 1) != NULL &&
        strcmp(clpd_db_result_column_name(r, 1), "name") == 0);
  CHECK(r != NULL && clpd_db_result_value(r, 0, 0) == NULL);
  clpd_db_result_free(r);
}

static void test_transactions(const char *spec) {
  clpd_db *other = NULL;
  char msg[512];
  const char *ins = "INSERT INTO clpd_suite_tx (id) VALUES ($1)";

  CHECK(run(db, "CREATE TABLE clpd_suite_tx (id BIGINT PRIMARY KEY)") ==
        CLPD_DB_OK);
  CHECK(clpd_db_open(spec, &other, msg, sizeof msg) == CLPD_DB_OK);
  CHECK(other != NULL);

  CHECK(clpd_db_in_transaction(db) == 0);
  CHECK(clpd_db_commit(db) == CLPD_DB_ERROR);
  CHECK(one_line_error());
  CHECK(clpd_db_rollback(db) == CLPD_DB_ERROR);
  CHECK(one_line_error());

  CHECK(clpd_db_begin(db) == CLPD_DB_OK);
  CHECK(clpd_db_in_transaction(db) == 1);
  CHECK(clpd_db_begin(db) == CLPD_DB_ERROR);
  CHECK(one_line_error());
  CHECK(clpd_db_in_transaction(db) == 1); /* the first one is still open */
  CHECK(run1(db, ins, clpd_db_int(1)) == CLPD_DB_OK);
  CHECK(count(db, "clpd_suite_tx") == 1);
  if (other != NULL) CHECK(count(other, "clpd_suite_tx") == 0);
  CHECK(clpd_db_commit(db) == CLPD_DB_OK);
  CHECK(clpd_db_in_transaction(db) == 0);
  CHECK(count(db, "clpd_suite_tx") == 1);
  if (other != NULL) CHECK(count(other, "clpd_suite_tx") == 1);

  CHECK(clpd_db_begin(db) == CLPD_DB_OK);
  CHECK(run1(db, ins, clpd_db_int(2)) == CLPD_DB_OK);
  CHECK(count(db, "clpd_suite_tx") == 2);
  CHECK(clpd_db_rollback(db) == CLPD_DB_OK);
  CHECK(clpd_db_in_transaction(db) == 0);
  CHECK(count(db, "clpd_suite_tx") == 1);
  if (other != NULL) CHECK(count(other, "clpd_suite_tx") == 1);

  CHECK(clpd_db_commit(db) == CLPD_DB_ERROR);
  CHECK(clpd_db_rollback(db) == CLPD_DB_ERROR);
  clpd_db_close(other);
}

static void test_rejected_sql(void) {
  static const char *const bad_one[] = {
      "SELECT id FROM clpd_suite_tx WHERE id = $1 OR id = $2",
      "SELECT id FROM clpd_suite_tx WHERE id = ?",
      "SELECT id FROM clpd_suite_tx WHERE id = ?1",
      "SELECT id FROM clpd_suite_tx WHERE id = :id",
      "SELECT id FROM clpd_suite_tx WHERE id = @id",
      "SELECT id FROM clpd_suite_tx WHERE id = $id",
      "SELECT id FROM clpd_suite_tx WHERE id = $0",
      "SELECT id FROM clpd_suite_tx WHERE id = $01",
      "SELECT id FROM clpd_suite_tx WHERE id = $1 AND 'open",
      "SELECT id FROM clpd_suite_tx WHERE id = $1 /* open",
      "SELECT id FROM clpd_suite_tx WHERE id = $1; SELECT 2",
  };
  clpd_db_value two[2];
  clpd_db_result *r = (clpd_db_result *)&r;
  size_t i;

  /* Multiple statements: nothing runs. */
  CHECK(run(db, "SELECT 1; SELECT 2") == CLPD_DB_ERROR);
  CHECK(one_line_error());
  CHECK(run(db, "INSERT INTO clpd_suite_tx (id) VALUES (10); "
                "DROP TABLE clpd_suite_tx") == CLPD_DB_ERROR);
  CHECK(count(db, "clpd_suite_tx") == 1);
  CHECK(run(db, "SELECT 1;") == CLPD_DB_OK);
  CHECK(run(db, "SELECT 1 ;  -- trailing comment\n") == CLPD_DB_OK);
  /* A carriage return ends a -- comment too, as on the PostgreSQL server. */
  CHECK(run(db, "SELECT 1 --\r; SELECT 2") == CLPD_DB_ERROR);
  CHECK(run(db, "SELECT 1 --\r") == CLPD_DB_OK);

  /* Placeholder forms and counts. */
  for (i = 0; i < sizeof bad_one / sizeof bad_one[0]; i++) {
    r = (clpd_db_result *)&r;
    CHECK(clpd_db_exec(db, bad_one[i], (clpd_db_value[]){clpd_db_int(1)}, 1,
                       &r) == CLPD_DB_ERROR);
    CHECK(r == NULL);
    CHECK(one_line_error());
  }
  two[0] = clpd_db_int(1);
  two[1] = clpd_db_int(2);
  CHECK(clpd_db_exec(db, "SELECT id FROM clpd_suite_tx WHERE id = $1", two, 2,
                     NULL) == CLPD_DB_ERROR); /* $2 missing */
  CHECK(run(db, "SELECT id FROM clpd_suite_tx WHERE id = $1") == CLPD_DB_ERROR);

  r = NULL;
  CHECK(clpd_db_exec(db, "SELECT id FROM clpd_suite_tx WHERE id = $1 OR id = $1",
                     two, 1, &r) == CLPD_DB_OK);
  CHECK(r != NULL && clpd_db_result_rows(r) == 1);
  clpd_db_result_free(r);
  r = NULL;
  CHECK(clpd_db_exec(db, "SELECT id FROM clpd_suite_tx WHERE id = $2 OR id = $1",
                     two, 2, &r) == CLPD_DB_OK);
  CHECK(r != NULL && clpd_db_result_rows(r) == 1 &&
        is_int(clpd_db_result_value(r, 0, 0), 1));
  clpd_db_result_free(r);

  /* Parameters that cannot behave the same on both backends. */
  {
    clpd_db_value nul = clpd_db_text("a");
    nul.data = "a\0b";
    nul.size = 3;
    CHECK(run1(db, "SELECT id FROM clpd_suite_tx WHERE id = $1", nul) ==
          CLPD_DB_ERROR);
    CHECK(run1(db, "SELECT id FROM clpd_suite_tx WHERE id = $1",
               clpd_db_text("bad \xff utf-8")) == CLPD_DB_ERROR);
    CHECK(run1(db, "SELECT id FROM clpd_suite_tx WHERE id = $1",
               clpd_db_float(NAN)) == CLPD_DB_ERROR);
    CHECK(one_line_error());
  }
}

static void drop_tables(void) {
  CHECK(run(db, "DROP TABLE IF EXISTS clpd_suite_types") == CLPD_DB_OK);
  CHECK(run(db, "DROP TABLE IF EXISTS clpd_suite_uniq") == CLPD_DB_OK);
  CHECK(run(db, "DROP TABLE IF EXISTS clpd_suite_tx") == CLPD_DB_OK);
}

int clpd_db_suite(const char *spec) {
  char msg[512];
  const char *want = strncmp(spec, "sqlite:", 7) == 0 ? "sqlite" : "postgresql";

  fails = 0;
  db = NULL;
  if (clpd_db_open(spec, &db, msg, sizeof msg) != CLPD_DB_OK || db == NULL) {
    fprintf(stderr, "FAIL %s:%d: cannot open the database: %s\n", __FILE__,
            __LINE__, msg);
    db = NULL;
    return 1;
  }
  CHECK(strcmp(clpd_db_backend(db), want) == 0);
  drop_tables();
  test_round_trip();
  test_unique();
  test_affected_names_empty();
  test_transactions(spec);
  test_rejected_sql();
  drop_tables();
  clpd_db_close(db);
  db = NULL;
  return fails;
}
