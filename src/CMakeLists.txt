add_library(clpd_core STATIC kvfile.c config.c manifest.c packages.c scheduler.c engine.c loop.c lint.c trace.c clips_trace.c fdcapture.c log.c plugins.c json.c ingest.c admin.c db.c db_sqlite.c db_pg.c orm.c orm_ddl.c orm_dml.c orm_tx.c orm_sync.c orm_load.c orm_life.c)
target_include_directories(clpd_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include)
find_package(Threads REQUIRED)
# Database backends (src/db.h, T8): both required. libpq from Homebrew is
# keg-only: put its lib/pkgconfig on PKG_CONFIG_PATH.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(LIBPQ REQUIRED IMPORTED_TARGET libpq)
target_link_libraries(clpd_core PUBLIC clips::clips Threads::Threads ${CMAKE_DL_LIBS})
target_link_libraries(clpd_core PRIVATE PkgConfig::SQLITE3 PkgConfig::LIBPQ)
target_compile_options(clpd_core PRIVATE ${CLPD_WARNINGS})

# Plugin contract (include/clpd_plugin.h, docs/plugins.md): a plugin
# compiles against the contract and CLIPS headers but does not link CLIPS
# (FR-18); it resolves the CLIPS symbols from the executable at dlopen.
add_library(clpd_plugin_api INTERFACE)
target_include_directories(clpd_plugin_api INTERFACE ${PROJECT_SOURCE_DIR}/include)
target_include_directories(clpd_plugin_api SYSTEM INTERFACE
  $<TARGET_PROPERTY:clips,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(clpd_plugin_api INTERFACE
  $<TARGET_PROPERTY:clips,INTERFACE_COMPILE_DEFINITIONS>)

# clpd_export_clips(<executable>): links every CLIPS object (whole archive,
# not only the ones the daemon calls) and exports the global symbols to the
# dynamic symbol table (-rdynamic), so plugins can call any CLIPS API. The
# version script clpd_exports.ver keeps the clpd_* internals out of that
# table (GNU ld). CMake 3.20 has no $<LINK_LIBRARY:WHOLE_ARCHIVE> (3.24),
# hence the explicit linker options.
function(clpd_export_clips target)
  set_target_properties(${target} PROPERTIES ENABLE_EXPORTS ON
    LINK_DEPENDS ${PROJECT_SOURCE_DIR}/src/clpd_exports.ver)
  target_link_options(${target} PRIVATE
    "LINKER:--whole-archive" "$<TARGET_FILE:clips>" "LINKER:--no-whole-archive"
    "LINKER:--version-script=${PROJECT_SOURCE_DIR}/src/clpd_exports.ver")
endfunction()

# clpd_plugin_module(<target> <sources...>): builds a plugin as
# <build>/plugins/<target>.so.
function(clpd_plugin_module target)
  add_library(${target} MODULE ${ARGN})
  target_link_libraries(${target} PRIVATE clpd_plugin_api)
  target_compile_options(${target} PRIVATE ${CLPD_WARNINGS})
  set_target_properties(${target} PROPERTIES PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/plugins)
endfunction()

add_executable(clpdaemon main.c)
target_link_libraries(clpdaemon PRIVATE clpd_core)
target_compile_options(clpdaemon PRIVATE ${CLPD_WARNINGS})
clpd_export_clips(clpdaemon)
