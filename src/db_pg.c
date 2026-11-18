/* PostgreSQL backend (src/db_backend.h). Stub until T8c: opening fails. */
#include "db_backend.h"

static clpd_db_status pg_open(clpd_db *db, const char *target) {
  (void)target;
  clpd_db_set_error(db, "the postgresql backend is not implemented yet");
  return CLPD_DB_ERROR;
}

static void pg_close(clpd_db *db) { (void)db; }

static clpd_db_status pg_exec(clpd_db *db, const char *sql,
                              const clpd_db_value *params, size_t n,
                              clpd_db_result *result) {
  (void)sql;
  (void)params;
  (void)n;
  (void)result;
  clpd_db_set_error(db, "the postgresql backend is not implemented yet");
  return CLPD_DB_ERROR;
}

static clpd_db_status pg_tx(clpd_db *db) {
  clpd_db_set_error(db, "the postgresql backend is not implemented yet");
  return CLPD_DB_ERROR;
}

const clpd_db_backend_ops clpd_db_pg_ops = {
    "postgresql", pg_open, pg_close, pg_exec, pg_tx, pg_tx, pg_tx,
};
