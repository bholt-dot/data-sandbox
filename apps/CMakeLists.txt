add_executable(belter belter/main.cpp)
target_link_libraries(belter PRIVATE sim::expanse)
sim_configure_target(belter)
