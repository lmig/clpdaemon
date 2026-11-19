/* Backend-neutral database interface (T8a, S7, L74): SQLite for fast tests,
 * PostgreSQL in production, both behind the same calls and semantics.
 *
 * Contract:
 * - Engine thread only: a clpd_db and its results are not thread-safe and
 *   must be used from the thread that opened them.
 * - Spec: "sqlite:<path>" or a libpq conninfo (key=value pairs or a
 *   postgresql:// / postgres:// URI) without a password; see
 *   clpd_db_check_spec.
 * - SQL is one statement (one trailing ';' allowed) whose parameters are
 *   written exactly $1..$n for n parameters, each number used at least once
 *   ($1 may repeat). Any other placeholder form (?, ?1, :name, @name,
 *   $name, $0, $01), a number above n, a missing number or a second
 *   statement is CLPD_DB_ERROR, and nothing is executed. Quotes ('...',
 *   E'...', "..."), line comments (--) and block comments are skipped by
 *   that check; an unterminated one is CLPD_DB_ERROR.
 * - Parameters: TEXT must be valid UTF-8 without NUL bytes, a FLOAT must
 *   not be NaN; otherwise CLPD_DB_ERROR. BLOB may contain NUL bytes.
 * - Transactions: clpd_db_begin inside a transaction and clpd_db_commit or
 *   clpd_db_rollback outside one are CLPD_DB_ERROR. Commit ends the
 *   transaction even when it fails (a failed commit is rolled back). After
 *   an error inside a transaction the only portable call is rollback (on
 *   PostgreSQL the transaction is aborted). Transaction-control SQL must
 *   not be passed to clpd_db_exec.
 * - Errors: clpd_db_error returns the last message, one line, never
 *   containing the spec. CLPD_DB_UNIQUE is a unique or primary-key
 *   violation; CLPD_DB_LOST means the connection is gone (the retry policy
 *   belongs to the caller, FR-26) and any open transaction is over.
 * - Results: clpd_db_exec copies every row into a clpd_db_result owned by
 *   the caller, freed with clpd_db_result_free. Names and values returned
 *   by the accessors stay valid until that result is freed, independent of
 *   later calls on the connection. */
#ifndef CLPD_DB_H
#define CLPD_DB_H

#include <stddef.h>
#include <stdint.h>

typedef struct clpd_db clpd_db;
typedef struct clpd_db_result clpd_db_result;

typedef enum {
  CLPD_DB_OK = 0,
  CLPD_DB_ERROR,  /* any other failure; see clpd_db_error */
  CLPD_DB_UNIQUE, /* unique or primary-key violation */
  CLPD_DB_LOST    /* connection lost */
} clpd_db_status;

typedef enum {
  CLPD_DB_NULL = 0,
  CLPD_DB_INTEGER, /* integer */
  CLPD_DB_FLOAT,   /* real */
  CLPD_DB_TEXT,    /* data, size: UTF-8 bytes; NUL-terminated in results */
  CLPD_DB_BLOB     /* data, size: any bytes, NUL included */
} clpd_db_type;

typedef struct {
  clpd_db_type type;
  int64_t integer;
  double real;
  const void *data; /* TEXT and BLOB; may be NULL when size is 0 */
  size_t size;      /* bytes, without the terminator of a result TEXT */
} clpd_db_value;

/* Parameter constructors. clpd_db_text takes strlen(s) bytes. */
clpd_db_value clpd_db_null(void);
clpd_db_value clpd_db_int(int64_t v);
clpd_db_value clpd_db_float(double v);
clpd_db_value clpd_db_text(const char *s);
clpd_db_value clpd_db_blob(const void *data, size_t size);

/* Validates a spec without connecting. Returns 1 when valid; otherwise 0
 * with one of these messages in msg (never the value itself):
 *   database: expected a connection string
 *   database: expected sqlite:<path>
 *   database: invalid PostgreSQL connection string
 *   database: the connection string must not contain a password (use a
 *   password file, see PGPASSFILE)            (password or sslpassword) */
int clpd_db_check_spec(const char *value, char *msg, size_t n);

/* Opens a connection. On CLPD_DB_OK *db is the handle; otherwise *db is
 * NULL and msg holds a one-line message (never the spec). */
clpd_db_status clpd_db_open(const char *spec, clpd_db **db, char *msg,
                            size_t n);
void clpd_db_close(clpd_db *db); /* NULL is accepted; open tx rolled back */

/* "sqlite" or "postgresql". */
const char *clpd_db_backend(const clpd_db *db);

/* Runs one statement with n parameters (params may be NULL when n is 0).
 * When result is not NULL it receives the rows on CLPD_DB_OK and NULL
 * otherwise. */
clpd_db_status clpd_db_exec(clpd_db *db, const char *sql,
                            const clpd_db_value *params, size_t n,
                            clpd_db_result **result);

clpd_db_status clpd_db_begin(clpd_db *db);
clpd_db_status clpd_db_commit(clpd_db *db);
clpd_db_status clpd_db_rollback(clpd_db *db);
int clpd_db_in_transaction(const clpd_db *db);

/* Last error message, one line ("" when none). */
const char *clpd_db_error(const clpd_db *db);

size_t clpd_db_result_rows(const clpd_db_result *r);
size_t clpd_db_result_columns(const clpd_db_result *r);
/* NULL when col is out of range. */
const char *clpd_db_result_column_name(const clpd_db_result *r, size_t col);
/* NULL when row or col is out of range. */
const clpd_db_value *clpd_db_result_value(const clpd_db_result *r, size_t row,
                                          size_t col);
/* Rows changed by INSERT, UPDATE or DELETE; 0 for any other statement. */
int64_t clpd_db_result_affected(const clpd_db_result *r);
void clpd_db_result_free(clpd_db_result *r); /* NULL is accepted */

#endif
