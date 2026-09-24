# Shared compile settings, applied to our own targets via sim_configure_target().
# Third-party targets are left alone so their warnings don't break our build.

add_library(sim_options INTERFACE)
add_library(sim::options ALIAS sim_options)

target_compile_features(sim_options INTERFACE cxx_std_20)

if(MSVC)
  target_compile_options(sim_options INTERFACE /W4 /permissive-
    $<$<BOOL:${SIM_WARNINGS_AS_ERRORS}>:/WX>)
else()
  target_compile_options(sim_options INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual -Wnull-dereference
    -Wdouble-promotion -Wimplicit-fallthrough
    $<$<BOOL:${SIM_WARNINGS_AS_ERRORS}>:-Werror>)
endif()

if(SIM_SANITIZE AND NOT MSVC)
  target_compile_options(sim_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(sim_options INTERFACE -fsanitize=address,undefined)
endif()

function(sim_configure_target target)
  target_link_libraries(${target} PRIVATE sim::options)
endfunction()

# sim_add_test(<name> SOURCES ... LIBS ...): a doctest executable registered with CTest.
function(sim_add_test name)
  if(NOT SIM_BUILD_TESTS)
    return()
  endif()
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE ${ARG_LIBS} doctest::doctest)
  sim_configure_target(${name})
  doctest_discover_tests(${name})
endfunction()
