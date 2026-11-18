/* Internal backend interface behind src/db.h (T8a). db.c owns the shared
 * behavior: spec checks, placeholder and parameter validation, transaction
 * state, one-line error messages and result storage. A backend only talks
 * to its library. */
#ifndef CLPD_DB_BACKEND_H
#define CLPD_DB_BACKEND_H

#include "db.h"

#define CLPD_DB_ERROR_MAX 512

typedef struct {
  const char *name; /* clpd_db_backend: "sqlite" or "postgresql" */

  /* target: the path after "sqlite:" or the whole conninfo. On failure the
   * backend sets the error (clpd_db_set_error) and releases db->impl. */
  clpd_db_status (*open)(clpd_db *db, const char *target);
  void (*close)(clpd_db *db); /* releases db->impl */

  /* sql and params are already validated (one statement, $1..$n). Fills
   * `result` (never NULL, empty on entry) with clpd_db_result_init_columns,
   * clpd_db_result_add_row and clpd_db_result_set, and sets its affected
   * count for INSERT, UPDATE and DELETE. */
  clpd_db_status (*exec)(clpd_db *db, const char *sql,
                         const clpd_db_value *params, size_t n,
                         clpd_db_result *result);

  /* Called only in the right transaction state. When commit returns
   * anything but CLPD_DB_OK, db.c calls rollback and keeps the commit
   * error; a commit the server turns into a rollback must not return OK.
   * CLPD_DB_LOST from any call ends the transaction. */
  clpd_db_status (*begin)(clpd_db *db);
  clpd_db_status (*commit)(clpd_db *db);
  clpd_db_status (*rollback)(clpd_db *db);
} clpd_db_backend_ops;

struct clpd_db {
  const clpd_db_backend_ops *ops;
  void *impl; /* backend connection state */
  int in_tx;
  char error[CLPD_DB_ERROR_MAX];
};

extern const clpd_db_backend_ops clpd_db_sqlite_ops;
extern const clpd_db_backend_ops clpd_db_pg_ops;

/* Sets the last error from a format; newlines and other control
 * characters become spaces and trailing spaces are dropped (one line). */
void clpd_db_set_error(clpd_db *db, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* Result building (memory is copied; exits on allocation failure like
 * clpd_strdup). Columns are declared once, before any row. */
void clpd_db_result_init_columns(clpd_db_result *r, size_t columns);
void clpd_db_result_set_name(clpd_db_result *r, size_t col, const char *name);
size_t clpd_db_result_add_row(clpd_db_result *r); /* returns its index */
/* Copies v (TEXT and BLOB bytes included) into row, col. */
void clpd_db_result_set(clpd_db_result *r, size_t row, size_t col,
                        const clpd_db_value *v);
void clpd_db_result_set_affected(clpd_db_result *r, int64_t affected);

#endif
