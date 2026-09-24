add_library(expanse
  src/game.cpp
)
add_library(sim::expanse ALIAS expanse)
target_include_directories(expanse PUBLIC include)
target_link_libraries(expanse PUBLIC sim::core)
sim_configure_target(expanse)
