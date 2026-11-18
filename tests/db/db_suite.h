/* Shared backend suite for src/db.h (T8a): the same cases run against
 * every backend (sqlite:<path> and a PostgreSQL conninfo). */
#ifndef CLPD_DB_SUITE_H
#define CLPD_DB_SUITE_H

/* Runs every case against a fresh connection to `spec`; the tables it
 * creates are named clpd_suite_* and dropped before and after. Reports
 * each failure on stderr and returns the failure count. */
int clpd_db_suite(const char *spec);

#endif
