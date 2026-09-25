add_library(expanse
  src/game.cpp
  src/orbit.cpp
  src/transit.cpp
  src/calendar.cpp
  src/content.cpp
  src/world.cpp
  src/scenario.cpp
  src/simulation.cpp
  src/economy.cpp
  src/finance.cpp
  src/crew.cpp
  src/ships.cpp
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
    tests/world_tests.cpp
    tests/crew_tests.cpp
  LIBS sim::expanse)
if(TARGET expanse_tests)
  target_compile_definitions(expanse_tests PRIVATE EXPANSE_DATA_DIR="${PROJECT_SOURCE_DIR}/data")
endif()
