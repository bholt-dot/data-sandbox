# Terminal line editing adapter; falls back to plain getline when SIM_LINE_EDITING is off.
add_library(sim_lineedit STATIC lineedit/line_editor.cpp)
target_include_directories(sim_lineedit PUBLIC lineedit)
target_link_libraries(sim_lineedit PUBLIC sim::core)
if(SIM_LINE_EDITING)
  target_link_libraries(sim_lineedit PRIVATE isocline)
  target_compile_definitions(sim_lineedit PRIVATE SIM_HAVE_ISOCLINE=1)
endif()
sim_configure_target(sim_lineedit)

add_executable(belter belter/main.cpp)
target_link_libraries(belter PRIVATE sim::expanse sim_lineedit)
sim_configure_target(belter)
target_compile_definitions(belter PRIVATE BELTER_DEFAULT_DATA_DIR="${PROJECT_SOURCE_DIR}/data")

if(SIM_BUILD_TESTS)
  add_test(NAME belter_replay_twenty_years_deterministic
           COMMAND ${CMAKE_COMMAND} -DBELTER=$<TARGET_FILE:belter>
                   -DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/twenty_years.txt
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/compare_runs.cmake)
  add_test(NAME belter_script_error_exits_nonzero
           COMMAND belter --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/bad_command.txt)
  set_tests_properties(belter_script_error_exits_nonzero PROPERTIES WILL_FAIL TRUE)
endif()

if(SIM_VIEWER)
  add_subdirectory(viewer)  # belter_viewer library + belter-view
endif()
