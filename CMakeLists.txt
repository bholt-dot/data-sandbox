cmake_minimum_required(VERSION 3.24)

project(data_sandbox
  VERSION 0.1.0
  DESCRIPTION "Data-oriented simulation framework + hard-SF belter game"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SIM_BUILD_TESTS "Build unit tests" ON)
option(SIM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(SIM_SANITIZE "Build with address + undefined-behaviour sanitizers" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProjectOptions)
include(Dependencies)

if(SIM_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(core)     # reusable sim framework; must never depend on game code
add_subdirectory(expanse)  # the belter game built on core
add_subdirectory(apps)     # executables
