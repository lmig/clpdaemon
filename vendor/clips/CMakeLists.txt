# CLIPS 6.4.2 core, built from unmodified upstream sources (see README.md).
# Mirrors the upstream core/makefile "release" target for Linux:
#   CFLAGS = -std=c99 -O3 -fno-strict-aliasing, -DLINUX, LDLIBS = -lm
# and its OBJS list (every core .c file except main.c).

file(GLOB CLIPS_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
list(REMOVE_ITEM CLIPS_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c)

add_library(clips STATIC ${CLIPS_SOURCES})
add_library(clips::clips ALIAS clips)

set_target_properties(clips PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  C_STANDARD 99
  C_EXTENSIONS OFF)

target_compile_definitions(clips PUBLIC LINUX)
target_compile_options(clips PRIVATE -fno-strict-aliasing -w)
target_include_directories(clips SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(clips PUBLIC m)
