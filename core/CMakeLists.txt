add_library(simcore
  src/time.cpp
  src/stats.cpp
)
add_library(sim::core ALIAS simcore)
target_include_directories(simcore PUBLIC include)
sim_configure_target(simcore)

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
  LIBS sim::core)
