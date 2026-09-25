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

# Full-screen interface (FTXUI). sim_tui draws sim::Doc output; belter_tui is the game's screen.
if(SIM_TUI)
  add_library(sim_tui STATIC tui/doc_view.cpp)
  target_include_directories(sim_tui PUBLIC tui)
  target_link_libraries(sim_tui PUBLIC sim::core ftxui::screen ftxui::dom ftxui::component)
  sim_configure_target(sim_tui)

  add_library(belter_tui STATIC belter/tui.cpp)
  target_include_directories(belter_tui PUBLIC belter)
  target_link_libraries(belter_tui PUBLIC sim_tui sim::expanse)
  sim_configure_target(belter_tui)

  target_link_libraries(belter PRIVATE belter_tui)
  target_compile_definitions(belter PRIVATE SIM_HAVE_TUI=1)

  sim_add_test(belter_tui_tests SOURCES tests/tui_tests.cpp LIBS belter_tui)
  if(TARGET belter_tui_tests)
    target_compile_definitions(belter_tui_tests PRIVATE BELTER_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/data")
  endif()
endif()
