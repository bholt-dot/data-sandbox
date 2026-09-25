# The optional 3D system viewer (SIM_VIEWER=ON). Two libraries:
#   belter_viewer_core  snapshot handoff + scene building; plain C++, no SDL (unit-testable anywhere)
#   belter_viewer       the SDL3 GPU window and renderer, with SPIR-V shaders embedded
include(Shaders)

add_library(belter_viewer_core STATIC
  src/viewer_link.cpp
  src/view_snapshot.cpp
  src/scene.cpp
)
target_include_directories(belter_viewer_core PUBLIC include PRIVATE src)
target_link_libraries(belter_viewer_core PUBLIC sim::expanse Threads::Threads)
sim_configure_target(belter_viewer_core)

add_library(belter_viewer STATIC
  src/gpu.cpp
  src/renderer.cpp
  src/viewer.cpp
)
target_include_directories(belter_viewer PUBLIC include PRIVATE src)
target_link_libraries(belter_viewer PUBLIC belter_viewer_core PRIVATE SDL3::SDL3)
sim_configure_target(belter_viewer)
sim_add_spirv_shaders(belter_viewer NAMESPACE viewer SHADERS
  shaders/sprite.vert
  shaders/sprite.frag
  shaders/line.vert
  shaders/line.frag
)

add_executable(belter-view main.cpp)
target_link_libraries(belter-view PRIVATE belter_viewer)
sim_configure_target(belter-view)
target_compile_definitions(belter-view PRIVATE BELTER_DEFAULT_DATA_DIR="${PROJECT_SOURCE_DIR}/data")

if(SIM_BUILD_TESTS)
  sim_add_test(viewer_tests
    SOURCES tests/main.cpp tests/viewer_link_tests.cpp tests/scene_tests.cpp
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
  # Leak checking is off for these runs only: Vulkan drivers (lavapipe's LLVM) and libX11 keep
  # process-lifetime allocations that LSan reports after they are unloaded. Our own code is
  # leak-checked by viewer_tests.
  set(gpu_env "ASAN_OPTIONS=detect_leaks=0")
  set(shot_size 640x360)
  set(shot_args --size ${shot_size} --frames 3)
  set(offscreen_png "${CMAKE_CURRENT_BINARY_DIR}/screenshot_offscreen.png")
  add_test(NAME viewer_screenshot_offscreen
           COMMAND belter-view ${shot_args} --screenshot ${offscreen_png})
  set_tests_properties(viewer_screenshot_offscreen PROPERTIES
    ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${gpu_env}"
    FIXTURES_SETUP viewer_offscreen_png)
  add_test(NAME viewer_screenshot_offscreen_check
           COMMAND check_screenshot ${offscreen_png} 640 360)
  set_tests_properties(viewer_screenshot_offscreen_check PROPERTIES FIXTURES_REQUIRED viewer_offscreen_png)

  # The shell's pattern: a second thread advances the game and publishes while the window renders.
  add_test(NAME viewer_animate_offscreen COMMAND belter-view --size 320x180 --animate 200 --frames 40)
  set_tests_properties(viewer_animate_offscreen PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen;${gpu_env}")

  find_program(XVFB_RUN xvfb-run)
  if(XVFB_RUN)
    set(x11_png "${CMAKE_CURRENT_BINARY_DIR}/screenshot_x11.png")
    add_test(NAME viewer_screenshot_x11
             COMMAND ${XVFB_RUN} -a -s "-screen 0 1024x768x24"
                     $<TARGET_FILE:belter-view> ${shot_args} --screenshot ${x11_png})
    set_tests_properties(viewer_screenshot_x11 PROPERTIES
      ENVIRONMENT "SDL_VIDEO_DRIVER=x11;${gpu_env}"
      FIXTURES_SETUP viewer_x11_png)
    add_test(NAME viewer_screenshot_x11_check COMMAND check_screenshot ${x11_png} 640 360)
    set_tests_properties(viewer_screenshot_x11_check PROPERTIES FIXTURES_REQUIRED viewer_x11_png)
  endif()
endif()
