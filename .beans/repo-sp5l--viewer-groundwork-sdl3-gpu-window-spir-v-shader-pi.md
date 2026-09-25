---
# repo-sp5l
title: 'Viewer groundwork: SDL3 GPU window, SPIR-V shader pipeline, snapshot handoff'
status: completed
type: feature
priority: normal
created_at: 2026-09-25T01:08:58Z
updated_at: 2026-09-25T01:37:30Z
parent: repo-89dz
---

SDL3 (FetchContent, system package preferred when present), GLSL -> SPIR-V at build time via glslangValidator, GPU device + window + swapchain, a minimal pipeline drawing something, headless screenshot test on lavapipe + Xvfb, and the thread/snapshot handoff API for the shell to publish read-only World snapshots. Main thread owns the window.



## Summary of Changes

**Build.** New option `SIM_VIEWER` (default OFF; the dev/release presets are unchanged) and a
`viewer` preset (dev + sanitizers + `SIM_VIEWER=ON`, `build/viewer`). When ON,
`cmake/Dependencies.cmake` uses `find_package(SDL3 3.4.0 CONFIG)` and otherwise FetchContents
`release-3.4.16` (static, SYSTEM; tests/examples/install and the audio, camera, joystick, haptic,
hidapi, sensor, power, dialog and tray subsystems off). `glslangValidator` is required.
`cmake/Shaders.cmake` adds `sim_add_spirv_shaders(target NAMESPACE ns SHADERS ...)`: each
`.vert`/`.frag` is compiled with `glslangValidator -V --target-env vulkan1.0 --vn <ident>` into a
`uint32_t` array header (SPIR-V words, correctly aligned, no extra embed tool), glslang's depfile
tracks `#include`s, and a generated `embedded_shader(name)` lookup is compiled into the target.

**Code** (`apps/viewer/`):
- `belter_viewer_core` (no SDL): `ViewerLink` (mutex-guarded `shared_ptr<const ViewSnapshot>` +
  generation counter; atomic close flags both ways, contract documented in `viewer_link.hpp`),
  `ViewSnapshot` {content, optional full World copy (World is copyable; static_assert), time,
  hints: focus ship, plot preview (`CoursePreview`)}, `make_snapshot(Session|content+world)`,
  and the scene builder: `build_scene` (per snapshot: bodies, stations, ships, orbit line strips
  in parent-local AU, transit and plot lines) and `prepare_frame` (per frame: subtracts the eye
  in double, narrows camera-relative positions to float AU, culls strips under 3 px, builds
  view-proj = rotation x infinite-far RH [0,1] projection).
- `belter_viewer` (SDL3 GPU): Vulkan/SPIR-V device, resizable HiDPI window, VSYNC swapchain, frame
  loop that renders into an offscreen R8G8B8A8 scene target and blits to the swapchain (so the
  scene format is independent of the swapchain's and screenshots use the same path), two
  pipelines (instanced sprite quads pulled from a storage buffer; line strips with per-draw
  origin/colour uniforms), `DynamicBuffer` uploads with cycling, screenshot via
  `SDL_DownloadFromGPUTexture` + fence + `SDL_SavePNG`, drag/wheel/R/Esc controls, clean
  shutdown (wait idle, release, unclaim, destroy) on every exit path, `notify_viewer_closed()`.
- `belter-view`: loads content, new game (`--scenario/--seed`) or `--load SAVE`, focuses the
  player's ship, `--plot STATION` preview, `--animate DAYS` (a second thread advances and
  publishes, the shell's pattern), `--size`, `--frames`, `--screenshot`, `--gpu-debug`.
  `apps/belter/main.cpp` is untouched; `belter --view` integration is left to the shell work.

**Tests.** `viewer_tests` (doctest, no GPU): ViewerLink publish/read across threads, close
signals, snapshot isolation, scene counts, orbit geometry, camera centring and projection,
hints. GPU (CTest): `viewer_screenshot_offscreen` (SDL `offscreen` video driver, Vulkan
`VK_EXT_headless_surface`, no display needed) + `check_screenshot` (size, >= 16 colours, bright
Sun at the centre, dark corner); the same through `xvfb-run` + X11 when available;
`viewer_animate_offscreen` (publisher thread during rendering). Leak checking is off only for the
GPU runs (lavapipe/libX11 allocations are reported after unload). All pass on Mesa lavapipe
under the viewer preset (ASan/UBSan), Release, and clang builds; `viewer_tests` is also clean
under TSan. Screenshot: `docs/screenshots/viewer-groundwork.png` (`--plot mars_highport`).

**Research.** SDL 3.4.16 headers (the wiki is generated from them; wiki.libsdl.org is blocked
here): `SDL_CreateGPUShader` SPIR-V sets (vertex 0/1, fragment 2/3),
`SDL_WaitAndAcquireGPUSwapchainTexture` (NULL texture is not an error; never cancel a command
buffer after acquiring, so failures after acquire submit instead), `SDL_PushGPU*UniformData`
and storage buffers want std140 (so the sprite SSBO is std140 with vec4-only members),
VSYNC+SDR always supported, clip space is y-up/[0,1] depth on Vulkan (negative viewport). SDL's
`test/testgpu_spinning_cube.c` and TheSpydog/SDL_gpu_examples (PullSpriteBatch: storage-buffer
instancing with cycled transfer buffers; CopyAndReadback: download + fence) for the frame loop,
uploads and readback. SDL's offscreen driver source confirms Vulkan support via headless
surfaces. Arch `extra/sdl3` is 3.4.16, matching the pin.

**Arch packages.** `sdl3 glslang vulkan-icd-loader` + a Vulkan driver (`vulkan-radeon`,
`vulkan-intel`, `nvidia-utils`, or `vulkan-swrast` for lavapipe); `xorg-server-xvfb` for the
optional X11 screenshot test. Without the `sdl3` package a fetched build also needs the usual
SDL build deps (`libx11 libxext libxcursor libxi libxrandr libxss wayland libxkbcommon libdecor`).

**Deferred** (to repo-s00q / repo-zvv9 / repo-d2ta): depth buffer + reversed-Z, sphere impostors,
orbit/follow camera with log zoom, anti-aliased/wide lines, data-driven body colours, HDR target
+ tonemap, text labels. Snapshots copy the whole World including the ever-growing journal and
ledger; if that shows up in profiles, snapshot only what the viewer draws.
