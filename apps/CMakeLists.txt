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
