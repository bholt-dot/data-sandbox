# data-sandbox

A data-oriented simulation framework (`core/`) and a hard-SF game built on it (`expanse/`):
a headless, shell-played Belter sim — the player is a dirt-poor ship captain trying to make a living.

## Build & test

```bash
cmake --preset dev && cmake --build --preset dev -j && ctest --preset dev   # Debug + ASan/UBSan
cmake --preset release && cmake --build --preset release -j                  # Optimized
./build/dev/apps/belter
```

Toolchain floor: GCC 13 / Clang 18, C++20. `<print>` and other C++23 library features are
not available on GCC 13 — use `<format>` + streams.

### Viewer (optional, `SIM_VIEWER=ON`)

The SDL3 GPU system viewer (`apps/viewer`, `belter-view`) is off by default; headless builds
need neither SDL3 nor glslang. Vulkan only, shaders are GLSL 450 compiled to SPIR-V at build time
and embedded (`cmake/Shaders.cmake`).

```bash
sudo pacman -S sdl3 sdl3_ttf glslang vulkan-icd-loader   # Arch; plus your Vulkan driver
                                   # (vulkan-radeon, vulkan-intel, nvidia-utils) or vulkan-swrast
cmake --preset viewer && cmake --build --preset viewer -j && ctest --preset viewer
./build/viewer/apps/viewer/belter-view                     # controls below
cmake --preset viewer-release && cmake --build --preset viewer-release -j   # for playing
./build/viewer-release/apps/belter --view                  # shell + window
SDL_VIDEO_DRIVER=offscreen ./build/viewer/apps/viewer/belter-view --screenshot out.png  # headless
```

Without a system SDL3 >= 3.4 the build fetches and statically builds a pinned SDL; likewise
SDL3_ttf >= 3.2 (vendored FreeType only, no HarfBuzz). The UI font (Fira Sans, OFL,
`apps/viewer/assets/fonts/`) is embedded like the shaders (`cmake/Embed.cmake`). Shader
bindings follow SDL's SPIR-V layout: vertex set 0 = textures/storage, set 1 = uniforms;
fragment set 2 / set 3 (see `apps/viewer/shaders/frame.glsl`).

Viewer controls: left-drag orbit, click to focus (and open its info panel), shift/right/middle-drag
pan, wheel or +/- zoom (log scale), F player's ship, Home Sun, 1-9 Earth Luna Mars Ceres-cluster
Vesta Jupiter Ganymede Saturn Titan, Tab/Shift+Tab cycle stations, `[` `]` cycle ships, I info
panel, H hotkey hints, R reset, Esc closes the panel (then the window), Q close. `belter-view
--focus KEY --distance-au X --yaw DEG --pitch DEG [--info]` sets the opening view (screenshot
tests use it).

Viewer rendering rules: world positions stay double on the CPU; every draw is made camera-relative
(subtract the eye in double, then narrow to float, `camera_relative()` in `scene.hpp`). Depth is
reversed-Z (D32_FLOAT, clear 0, compare GREATER, infinite far plane). Input handling, camera and
picking are SDL-free (`nav.hpp`, `pick.hpp`) and unit-tested. So are the overlay's words and
layout: `info.hpp` (panel/tooltip text from the snapshot only, scale bar) and `overlay.hpp` (label
priority, fade and greedy declutter, HUD and panel layout as a draw list, measured through a
callback); `text.hpp` only rasterises (SDL_ttf GPU text engine) and draws it, after the scene.

## Layout & layering

- `core/` → library `sim::core`, namespace `sim`. Reusable across sim projects.
  **Must never include anything from `expanse/`.**
- `expanse/` → library `sim::expanse`, namespace `expanse`. Game rules and data.
- `apps/` → executables (`belter`; `belter-view` + the `belter_viewer` library when `SIM_VIEWER=ON`).
- `data/` → TOML definitions (bodies, stations, commodities, ship classes).
- Tests live next to the code they test (`core/tests`, `expanse/tests`), doctest, registered via
  `sim_add_test()`. Test-case names must not contain `,` `[` `]` `*` or `\` — they break
  `doctest_discover_tests` (CTest filter syntax).

## Simulation rules

- **State lives in tables**, not object graphs. Entities are referenced by generational handles, never
  raw pointers or references held across ticks.
- **Reproducibility**: same build + same seed + same command script ⇒ identical state hash.
  - Time is `sim::Time` / `sim::Duration` (integral seconds). Convert to double only inside
    continuous models (orbits).
  - No global RNG; use per-system seeded streams.
  - Never let iteration order of an unordered container affect results.
  - No wall-clock time, no pointer-address ordering, no uninitialized reads.
  - Player input that changes state is a `sim::CommandBus` *action* (queued, logged, replayable as
    a script); *queries* get a const context. Handlers write styled output to the given `sim::Doc`
    (`simcore/doc.hpp`: headings, money, warnings, tables), never std::cout.
- Every persistent row/state type lists its fields once for save + load + hash:
  `static auto fields(auto& self) { return std::tie(self.a, self.b); }` (see `simcore/serialize.hpp`).
  Use `<cstdint>` fixed-width integers in state; no unordered containers or pointers in state.
- Orbits are analytic (position is a pure function of time); transits are scheduled arrival events,
  not per-tick movement.
- Doubles are fine (no multiplayer lockstep).

## Code style

- Warnings are errors (`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow ...`).
  Use explicit `static_cast` rather than disabling warnings.
- `snake_case` for functions/variables, `PascalCase` for types, 4-space indent.
- Keep comments sparse and about *why*.
- Keep dependencies minimal and pinned (see `cmake/Dependencies.cmake`).

## Workflow

- Work is tracked with **beans** (`beans list`, `beans show <id>`); see the SessionStart primer.
  Commit bean files together with the code they describe.
- **Before every commit, do a brief online check of best practices** for the approach taken
  (algorithms, data structures, library usage) and adjust if the implementation doesn't hold up.
  Mention what was checked in the bean's summary.
- Build and run tests under the `dev` preset (sanitizers) before committing.
