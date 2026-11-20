add_executable(clips_embed_test smoke/clips_embed_test.c)
target_link_libraries(clips_embed_test PRIVATE clips::clips)
target_compile_options(clips_embed_test PRIVATE ${CLPD_WARNINGS})

add_test(NAME clips_embed_smoke
  COMMAND clips_embed_test ${CMAKE_CURRENT_SOURCE_DIR}/smoke/fixtures/smoke.clp)

add_subdirectory(plugins)

# Talks to the admin socket for the CLI test (socat and nc -U may be missing).
add_executable(admin_client admin/admin_client.c)
target_compile_options(admin_client PRIVATE ${CLPD_WARNINGS})

add_test(NAME clpdaemon_cli
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/cli/cli_test.sh $<TARGET_FILE:clpdaemon>
    $<TARGET_FILE_DIR:clpd_example> $<TARGET_FILE:plugin_desc_size>
    $<TARGET_FILE:admin_client>)

add_executable(config_test config/config_test.c)
target_link_libraries(config_test PRIVATE clpd_core)
target_compile_options(config_test PRIVATE ${CLPD_WARNINGS})

add_test(NAME clpdaemon_config COMMAND config_test)

foreach(t scheduler engine loop lint manifest packages trace clips_trace fdcapture plugins json ingest admin orm)
  add_executable(${t}_test ${t}/${t}_test.c)
  target_link_libraries(${t}_test PRIVATE clpd_core)
  target_compile_options(${t}_test PRIVATE ${CLPD_WARNINGS})
  add_test(NAME clpdaemon_${t} COMMAND ${t}_test)
endforeach()

# Database interface (T8a): spec checks, and the shared backend suite on a
# temporary SQLite file (RED until the SQLite backend, T8b).
add_executable(db_test db/db_test.c db/db_suite.c)
target_link_libraries(db_test PRIVATE clpd_core)
target_include_directories(db_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_compile_options(db_test PRIVATE ${CLPD_WARNINGS})
add_test(NAME clpdaemon_db COMMAND db_test)
add_test(NAME clpdaemon_db_sqlite_suite COMMAND db_test sqlite-suite)
set_tests_properties(clpdaemon_db clpdaemon_db_sqlite_suite PROPERTIES TIMEOUT 30)

# PostgreSQL backend (T8c): the shared suite and PostgreSQL-only cases on a
# throwaway cluster; SKIPPED (exit 77) without initdb/pg_ctl/psql in PATH
# or CLPD_PG_BINDIR.
add_executable(db_pg_test db/db_pg_test.c db/db_suite.c)
target_link_libraries(db_pg_test PRIVATE clpd_core)
target_include_directories(db_pg_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_compile_options(db_pg_test PRIVATE ${CLPD_WARNINGS})
add_test(NAME clpdaemon_db_pg
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/db/pg_cluster.sh $<TARGET_FILE:db_pg_test>)
set_tests_properties(clpdaemon_db_pg PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)

# ORM DDL functions (T9b): on a temporary SQLite file, and on a throwaway
# PostgreSQL cluster (SKIPPED, exit 77, without initdb/pg_ctl/psql). The
# PostgreSQL tests share one lock so their clusters never run at once.
add_executable(orm_ddl_test orm/orm_ddl_test.c)
target_link_libraries(orm_ddl_test PRIVATE clpd_core)
target_include_directories(orm_ddl_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_compile_options(orm_ddl_test PRIVATE ${CLPD_WARNINGS})
add_test(NAME clpdaemon_orm_ddl COMMAND orm_ddl_test)
set_tests_properties(clpdaemon_orm_ddl PROPERTIES TIMEOUT 30)
add_test(NAME clpdaemon_orm_ddl_pg
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/db/pg_cluster.sh $<TARGET_FILE:orm_ddl_test>)
set_tests_properties(clpdaemon_orm_ddl_pg PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
# Cycle transaction (T10a): the same pair, SQLite and PostgreSQL.
add_executable(orm_tx_test orm/orm_tx_test.c)
target_link_libraries(orm_tx_test PRIVATE clpd_core)
target_include_directories(orm_tx_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_compile_options(orm_tx_test PRIVATE ${CLPD_WARNINGS})
add_test(NAME clpdaemon_orm_tx COMMAND orm_tx_test)
set_tests_properties(clpdaemon_orm_tx PROPERTIES TIMEOUT 60)
add_test(NAME clpdaemon_orm_tx_pg
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/db/pg_cluster.sh $<TARGET_FILE:orm_tx_test>)
set_tests_properties(clpdaemon_orm_tx_pg PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
# Automatic synchronization (T10b): the same pair, SQLite and PostgreSQL.
add_executable(orm_sync_test orm/orm_sync_test.c)
target_link_libraries(orm_sync_test PRIVATE clpd_core)
target_include_directories(orm_sync_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_compile_options(orm_sync_test PRIVATE ${CLPD_WARNINGS})
add_test(NAME clpdaemon_orm_sync COMMAND orm_sync_test)
set_tests_properties(clpdaemon_orm_sync PROPERTIES TIMEOUT 60)
add_test(NAME clpdaemon_orm_sync_pg
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/db/pg_cluster.sh $<TARGET_FILE:orm_sync_test>)
set_tests_properties(clpdaemon_orm_sync_pg PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
# DML functions (T10d): the same pair, SQLite and PostgreSQL.
add_executable(orm_dml_test orm/orm_dml_test.c)
target_link_libraries(orm_dml_test PRIVATE clpd_core)
target_include_directories(orm_dml_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_compile_options(orm_dml_test PRIVATE ${CLPD_WARNINGS})
add_test(NAME clpdaemon_orm_dml COMMAND orm_dml_test)
set_tests_properties(clpdaemon_orm_dml PROPERTIES TIMEOUT 60)
add_test(NAME clpdaemon_orm_dml_pg
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/db/pg_cluster.sh $<TARGET_FILE:orm_dml_test>)
set_tests_properties(clpdaemon_orm_dml_pg PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
# A running daemon against PostgreSQL (T10b): the synchronization, and the
# admin socket served during the backoff after a lost server.
add_test(NAME clpdaemon_cli_pg
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/db/pg_cluster.sh /bin/sh
    ${CMAKE_CURRENT_SOURCE_DIR}/cli/cli_pg_test.sh $<TARGET_FILE:clpdaemon>
    $<TARGET_FILE:admin_client>)
set_tests_properties(clpdaemon_cli_pg PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
set_tests_properties(clpdaemon_db_pg clpdaemon_orm_ddl_pg clpdaemon_orm_tx_pg
  clpdaemon_orm_sync_pg clpdaemon_orm_dml_pg clpdaemon_cli_pg PROPERTIES
  RESOURCE_LOCK clpd_pg_cluster)

# Link seam for the reader-failure cases, this test binary only: the test
# wraps poll and read (production objects are unchanged).
target_link_options(fdcapture_test PRIVATE -Wl,--wrap=poll -Wl,--wrap=read)
# Out-of-memory cases (T8f): db_test wraps the allocators (src/db.c),
# db_pg_test wraps PQunescapeBytea (src/db_pg.c).
target_link_options(db_test PRIVATE
  -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc)
target_link_options(db_pg_test PRIVATE -Wl,--wrap=PQunescapeBytea)
# Database-error paths of the ORM DDL functions (T9d): orm_ddl_test wraps
# clpd_db_exec to make one generated statement fail.
target_link_options(orm_ddl_test PRIVATE -Wl,--wrap=clpd_db_exec)
# Cycle transaction failures the database cannot produce on demand (T10a):
# orm_tx_test wraps begin, commit and reconnect.
target_link_options(orm_tx_test PRIVATE -Wl,--wrap=clpd_db_begin_atomic
  -Wl,--wrap=clpd_db_commit -Wl,--wrap=clpd_db_reconnect)
# The statements of the automatic synchronization (T10b): orm_sync_test
# wraps clpd_db_exec to record them and to make one fail.
target_link_options(orm_sync_test PRIVATE -Wl,--wrap=clpd_db_exec)
# The statements of the DML functions (T10d): orm_dml_test wraps
# clpd_db_exec to record them and to make one fail.
target_link_options(orm_dml_test PRIVATE -Wl,--wrap=clpd_db_exec)

# The loader test dlopens the plugins built in <build>/plugins, which take
# their CLIPS symbols from the test executable.
clpd_export_clips(plugins_test)
set_tests_properties(clpdaemon_plugins PROPERTIES
  ENVIRONMENT CLPD_PLUGIN_DIR=$<TARGET_FILE_DIR:clpd_example>)

# The executables export the CLIPS API and hide the clpd_* internals.
add_test(NAME clpdaemon_exports
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/plugins/exports_test.sh ${CMAKE_NM}
    $<TARGET_FILE:clpdaemon> $<TARGET_FILE:clips>)
add_test(NAME clpdaemon_plugins_test_exports
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/plugins/exports_test.sh ${CMAKE_NM}
    $<TARGET_FILE:plugins_test> $<TARGET_FILE:clips>)

# A lost stop signal must fail a test, not hang it until the default timeout.
set_tests_properties(clpdaemon_scheduler clpdaemon_engine clpdaemon_loop
  clpdaemon_lint clpdaemon_manifest clpdaemon_packages clpdaemon_trace clpdaemon_clips_trace clpdaemon_fdcapture clpdaemon_plugins clpdaemon_json clpdaemon_ingest clpdaemon_admin clpdaemon_config clpdaemon_orm PROPERTIES TIMEOUT 30)
set_tests_properties(clpdaemon_cli PROPERTIES TIMEOUT 120)
