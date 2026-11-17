add_executable(clips_embed_test smoke/clips_embed_test.c)
target_link_libraries(clips_embed_test PRIVATE clips::clips)
target_compile_options(clips_embed_test PRIVATE ${CLPD_WARNINGS})

add_test(NAME clips_embed_smoke
  COMMAND clips_embed_test ${CMAKE_CURRENT_SOURCE_DIR}/smoke/fixtures/smoke.clp)

add_test(NAME clpdaemon_cli
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/cli/cli_test.sh $<TARGET_FILE:clpdaemon>)
