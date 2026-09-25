# Third-party dependencies, pinned to release tags.
include(FetchContent)

if(SIM_BUILD_TESTS)
  FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.5.3
    GIT_SHALLOW    TRUE
    SYSTEM)
  set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(doctest)
  include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")
endif()

# TOML parser for data definitions. Only included from core/src (never public headers).
FetchContent_Declare(tomlplusplus
  GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
  GIT_TAG        v3.4.0
  GIT_SHALLOW    TRUE
  SYSTEM)
FetchContent_MakeAvailable(tomlplusplus)

# Line editing (history, tab completion) for interactive apps only; core never links it.
# isocline: MIT, pure C, one translation unit. Its own CMakeLists also builds a shared library
# and samples, so SOURCE_SUBDIR points at a directory without one and we build the TU ourselves.
option(SIM_LINE_EDITING "Interactive line editing via isocline" ON)
if(SIM_LINE_EDITING)
  FetchContent_Declare(isocline
    GIT_REPOSITORY https://github.com/daanx/isocline.git
    GIT_TAG        v1.1.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  do-not-build
    SYSTEM)
  FetchContent_MakeAvailable(isocline)
  enable_language(C)
  add_library(isocline STATIC "${isocline_SOURCE_DIR}/src/isocline.c")
  target_include_directories(isocline SYSTEM PUBLIC "${isocline_SOURCE_DIR}/include")
  set_target_properties(isocline PROPERTIES C_STANDARD 99 C_EXTENSIONS OFF)
endif()

# Full-screen terminal UI for interactive apps only; core and expanse never link it.
# FTXUI: MIT, no runtime dependencies (examples, docs and tests are off by default).
option(SIM_TUI "Full-screen terminal UI via FTXUI" ON)
if(SIM_TUI)
  set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(FTXUI_QUIET ON CACHE BOOL "" FORCE)
  FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG        v7.0.3
    GIT_SHALLOW    TRUE
    SYSTEM)
  FetchContent_MakeAvailable(ftxui)
endif()
