cmake_minimum_required(VERSION 3.20)

project(clpdaemon
  VERSION 0.1.0
  DESCRIPTION "CLIPS workflow engine daemon"
  LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Warnings for project-owned targets (vendored code sets its own flags).
set(CLPD_WARNINGS -Wall -Wextra -Werror)

enable_testing()

add_subdirectory(vendor/clips)
add_subdirectory(tests)
