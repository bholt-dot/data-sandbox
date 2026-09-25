add_library(simcore
  src/time.cpp
  src/text.cpp
  src/stats.cpp
  src/data_node.cpp
  src/defs.cpp
  src/command.cpp
)
add_library(sim::core ALIAS simcore)
target_include_directories(simcore PUBLIC include)
sim_configure_target(simcore)
target_link_libraries(simcore PRIVATE tomlplusplus::tomlplusplus)  # confined to src/data_node.cpp

sim_add_test(simcore_tests
  SOURCES
    tests/main.cpp
    tests/time_tests.cpp
    tests/table_tests.cpp
    tests/rng_tests.cpp
    tests/scheduler_tests.cpp
    tests/stats_tests.cpp
    tests/serialize_tests.cpp
    tests/snapshot_tests.cpp
    tests/defs_tests.cpp
    tests/command_tests.cpp
  LIBS sim::core)
