add_library(expanse
  src/game.cpp
  src/orbit.cpp
  src/transit.cpp
)
add_library(sim::expanse ALIAS expanse)
target_include_directories(expanse PUBLIC include)
target_link_libraries(expanse PUBLIC sim::core)
sim_configure_target(expanse)

sim_add_test(expanse_tests
  SOURCES
    tests/main.cpp
    tests/orbit_tests.cpp
    tests/transit_tests.cpp
  LIBS sim::expanse)
