/* SQLite backend (src/db_backend.h). Stub until T8b: opening fails. */
#include "db_backend.h"

static clpd_db_status sq_open(clpd_db *db, const char *target) {
  (void)target;
  clpd_db_set_error(db, "the sqlite backend is not implemented yet");
  return CLPD_DB_ERROR;
}

static void sq_close(clpd_db *db) { (void)db; }

static clpd_db_status sq_exec(clpd_db *db, const char *sql,
                              const clpd_db_value *params, size_t n,
                              clpd_db_result *result) {
  (void)sql;
  (void)params;
  (void)n;
  (void)result;
  clpd_db_set_error(db, "the sqlite backend is not implemented yet");
  return CLPD_DB_ERROR;
}

static clpd_db_status sq_tx(clpd_db *db) {
  clpd_db_set_error(db, "the sqlite backend is not implemented yet");
  return CLPD_DB_ERROR;
}

const clpd_db_backend_ops clpd_db_sqlite_ops = {
    "sqlite", sq_open, sq_close, sq_exec, sq_tx, sq_tx, sq_tx,
};
