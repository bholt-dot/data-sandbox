# The optional 3D system viewer (SIM_VIEWER=ON). Two libraries:
#   belter_viewer_core  snapshot handoff + scene building; plain C++, no SDL (unit-testable anywhere)
#   belter_viewer       the SDL3 GPU window and renderer, with SPIR-V shaders embedded
include(Shaders)
include(Embed)

add_library(belter_viewer_core STATIC
  src/viewer_link.cpp
  src/view_snapshot.cpp
  src/scene.cpp
  src/nav.cpp
  src/pick.cpp
  src/info.cpp
  src/overlay.cpp
)
target_include_directories(belter_viewer_core PUBLIC include PRIVATE src)
target_link_libraries(belter_viewer_core PUBLIC sim::expanse Threads::Threads)
sim_configure_target(belter_viewer_core)

add_library(belter_viewer STATIC
  src/gpu.cpp
  src/module_pins.cpp
  src/renderer.cpp
  src/text.cpp
  src/viewer.cpp
)
target_include_directories(belter_viewer PUBLIC include PRIVATE src)
target_link_libraries(belter_viewer PUBLIC belter_viewer_core PRIVATE SDL3_ttf::SDL3_ttf SDL3::SDL3 ${CMAKE_DL_LIBS})
sim_configure_target(belter_viewer)
sim_add_spirv_shaders(belter_viewer NAMESPACE viewer SHADERS
  shaders/sphere.vert
  shaders/sphere.frag
  shaders/sprite.vert
  shaders/sprite.frag
  shaders/line.vert
  shaders/line.frag
  shaders/overlay.vert
  shaders/overlay.frag
)
# Fira Sans (SIL Open Font License 1.1, assets/fonts/OFL.txt) for labels and panels.
sim_embed_files(belter_viewer NAMESPACE viewer FILES assets/fonts/FiraSans-Medium.ttf)

add_executable(belter-view main.cpp)
target_link_libraries(belter-view PRIVATE belter_viewer)
sim_configure_target(belter-view)
target_compile_definitions(belter-view PRIVATE BELTER_DEFAULT_DATA_DIR="${PROJECT_SOURCE_DIR}/data")

if(SIM_BUILD_TESTS)
  sim_add_test(viewer_tests
    SOURCES tests/main.cpp tests/viewer_link_tests.cpp tests/scene_tests.cpp tests/nav_tests.cpp
            tests/overlay_tests.cpp
    LIBS belter_viewer_core)
  target_include_directories(viewer_tests PRIVATE src)
  target_compile_definitions(viewer_tests PRIVATE BELTER_DATA_DIR="${PROJECT_SOURCE_DIR}/data")

  add_executable(check_screenshot tests/check_screenshot.cpp)
  target_link_libraries(check_screenshot PRIVATE SDL3::SDL3)
  sim_configure_target(check_screenshot)

  # Headless render on whatever Vulkan driver is present (lavapipe works): a screenshot of the
  # default view, then image checks. `offscreen` is SDL's windowless video driver; its Vulkan
  # surface is VK_EXT_headless_surface, so no display server is needed. With xvfb-run available
  # the same run goes through a real X11 window and swapchain as well.
  # Leak checking stays on: GPU driver libraries are pinned in sanitizer builds (module_pins.hpp)
  # so their process-lifetime allocations aren't misreported after the loader unloads them.
  set(shot_size 640x360)
  set(shot_args --size ${shot_size} --frames 3)
  set(offscreen_png "${CMAKE_CURRENT_BINARY_DIR}/screenshot_offscreen.png")
  add_test(NAME viewer_screenshot_offscreen
           COMMAND belter-view ${shot_args} --screenshot ${offscreen_png})
  set_tests_properties(viewer_screenshot_offscreen PROPERTIES
    ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen"
    FIXTURES_SETUP viewer_offscreen_png)
  add_test(NAME viewer_screenshot_offscreen_check
           COMMAND check_screenshot ${offscreen_png} 640 360 system)
  set_tests_properties(viewer_screenshot_offscreen_check PROPERTIES FIXTURES_REQUIRED viewer_offscreen_png)

  # Close-ups through --focus: a planet as a lit sphere (day and night side), and the Ceres
  # cluster with the player's ship.
  set(planet_png "${CMAKE_CURRENT_BINARY_DIR}/screenshot_planet.png")
  add_test(NAME viewer_screenshot_planet
           COMMAND belter-view ${shot_args} --focus earth --distance-au 0.00016 --pitch 20
                   --screenshot ${planet_png})
  set_tests_properties(viewer_screenshot_planet PROPERTIES
    ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen"
    FIXTURES_SETUP viewer_planet_png)
  add_test(NAME viewer_screenshot_planet_check COMMAND check_screenshot ${planet_png} 640 360 sphere)
  set_tests_properties(viewer_screenshot_planet_check PROPERTIES FIXTURES_REQUIRED viewer_planet_png)

  set(cluster_png "${CMAKE_CURRENT_BINARY_DIR}/screenshot_cluster.png")
  add_test(NAME viewer_screenshot_cluster
           COMMAND belter-view ${shot_args} --focus ceres_station --distance-au 0.8 --pitch 40
                   --screenshot ${cluster_png})
  set_tests_properties(viewer_screenshot_cluster PROPERTIES
    ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen"
    FIXTURES_SETUP viewer_cluster_png)
  add_test(NAME viewer_screenshot_cluster_check COMMAND check_screenshot ${cluster_png} 640 360 cluster)
  set_tests_properties(viewer_screenshot_cluster_check PROPERTIES FIXTURES_REQUIRED viewer_cluster_png)

  # The overlay at 1280x720 (text needs the room): the labelled system view, a station's info
  # panel and a plotted course with its flip marker. The same shots are docs/screenshots/
  # labels-system.png, info-station.png and plot-preview.png.
  function(viewer_overlay_shot name mode)
    set(png "${CMAKE_CURRENT_BINARY_DIR}/screenshot_${name}.png")
    add_test(NAME viewer_screenshot_${name}
             COMMAND belter-view --size 1280x720 --frames 3 ${ARGN} --screenshot ${png})
    set_tests_properties(viewer_screenshot_${name} PROPERTIES
      ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen"
      FIXTURES_SETUP viewer_${name}_png)
    add_test(NAME viewer_screenshot_${name}_check COMMAND check_screenshot ${png} 1280 720 ${mode})
    set_tests_properties(viewer_screenshot_${name}_check PROPERTIES FIXTURES_REQUIRED viewer_${name}_png)
  endfunction()
  viewer_overlay_shot(labels labels)
  viewer_overlay_shot(info info --focus ceres_station --info)
  viewer_overlay_shot(plot plot --plot mars_highport --focus earth --distance-au 5.5 --pitch 80)

  # The shell's pattern: a second thread advances the game and publishes while the window renders.
  add_test(NAME viewer_animate_offscreen COMMAND belter-view --size 320x180 --animate 200 --frames 40)
  set_tests_properties(viewer_animate_offscreen PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen")

  find_program(XVFB_RUN xvfb-run)
  if(XVFB_RUN)
    set(x11_png "${CMAKE_CURRENT_BINARY_DIR}/screenshot_x11.png")
    add_test(NAME viewer_screenshot_x11
             COMMAND ${XVFB_RUN} -a -s "-screen 0 1024x768x24"
                     $<TARGET_FILE:belter-view> ${shot_args} --screenshot ${x11_png})
    set_tests_properties(viewer_screenshot_x11 PROPERTIES
      ENVIRONMENT "SDL_VIDEO_DRIVER=x11"
      FIXTURES_SETUP viewer_x11_png)
    add_test(NAME viewer_screenshot_x11_check COMMAND check_screenshot ${x11_png} 640 360)
    set_tests_properties(viewer_screenshot_x11_check PROPERTIES FIXTURES_REQUIRED viewer_x11_png)
  endif()
endif()
