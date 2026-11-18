add_library(clpd_core STATIC kvfile.c config.c manifest.c scheduler.c engine.c loop.c lint.c)
target_include_directories(clpd_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(clpd_core PUBLIC clips::clips)
target_compile_options(clpd_core PRIVATE ${CLPD_WARNINGS})

add_executable(clpdaemon main.c)
target_link_libraries(clpdaemon PRIVATE clpd_core)
target_compile_options(clpdaemon PRIVATE ${CLPD_WARNINGS})
