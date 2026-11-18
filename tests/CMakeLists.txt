add_executable(clips_embed_test smoke/clips_embed_test.c)
target_link_libraries(clips_embed_test PRIVATE clips::clips)
target_compile_options(clips_embed_test PRIVATE ${CLPD_WARNINGS})

add_test(NAME clips_embed_smoke
  COMMAND clips_embed_test ${CMAKE_CURRENT_SOURCE_DIR}/smoke/fixtures/smoke.clp)

add_subdirectory(plugins)

add_test(NAME clpdaemon_cli
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/cli/cli_test.sh $<TARGET_FILE:clpdaemon>
    $<TARGET_FILE_DIR:clpd_example> $<TARGET_FILE:plugin_desc_size>)

add_executable(config_test config/config_test.c)
target_link_libraries(config_test PRIVATE clpd_core)
target_compile_options(config_test PRIVATE ${CLPD_WARNINGS})

add_test(NAME clpdaemon_config COMMAND config_test)

foreach(t scheduler engine loop lint manifest packages trace clips_trace fdcapture plugins)
  add_executable(${t}_test ${t}/${t}_test.c)
  target_link_libraries(${t}_test PRIVATE clpd_core)
  target_compile_options(${t}_test PRIVATE ${CLPD_WARNINGS})
  add_test(NAME clpdaemon_${t} COMMAND ${t}_test)
endforeach()

# Link seam for the reader-failure cases, this test binary only: the test
# wraps poll and read (production objects are unchanged).
target_link_options(fdcapture_test PRIVATE -Wl,--wrap=poll -Wl,--wrap=read)

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
  clpdaemon_lint clpdaemon_manifest clpdaemon_packages clpdaemon_trace clpdaemon_clips_trace clpdaemon_fdcapture clpdaemon_plugins clpdaemon_config PROPERTIES TIMEOUT 30)
set_tests_properties(clpdaemon_cli PROPERTIES TIMEOUT 120)
