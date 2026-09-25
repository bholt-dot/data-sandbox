---
# repo-d2ta
title: Viewer labels and info panel
status: completed
type: feature
priority: normal
created_at: 2026-09-25T01:08:58Z
updated_at: 2026-09-25T15:47:38Z
parent: repo-89dz
blocked_by:
    - repo-s00q
---

SDL3_ttf GPU text labels for bodies/stations/ships with declutter, hover/click info panel (name, faction, key prices, light-lag from player), flip-point marker on courses, corner HUD (date, focus, scale), plot preview styling, time-jump replay animation.

## Summary of Changes

- **Text**: SDL3_ttf (system `sdl3_ttf` >= 3.2 when the system SDL3 is used, else pinned
  `release-3.2.2` built static with its vendored FreeType only: HarfBuzz and plutosvg off).
  Its GPU text engine (`TTF_CreateGPUTextEngine` / `TTF_GetGPUTextDrawData`) fills the glyph
  atlas; `text.hpp` batches the atlas triangles plus solid quads (a 1x1 white texel, same
  pipeline, `overlay.vert/.frag`) into one vertex/index buffer and draws them in a pass after the
  scene (display space; with HDR it goes after the tonemap). Text objects are cached per string
  and evicted after ~4 s unused; fonts reopen when the display scale changes (HiDPI).
  Font: Fira Sans Medium (SIL OFL 1.1, `apps/viewer/assets/fonts/` with `OFL.txt`), embedded at
  build time by `cmake/Embed.cmake` (a CMake script, no external tools).
- **Labels** (`overlay.hpp`, SDL-free): tiers focus/hover > player ship and its courses >
  Sun/planets > stations/moons/dwarf planets > asteroids > other ships; visibility fades with
  scale (hosted objects once they stand clear of their host on screen, free stations and
  asteroids by camera distance); occluded objects (the pick occlusion test) get none. Greedy
  placement in priority order, four candidate sides (right, left, above, below), rejecting
  rectangles that leave the screen or overlap placed labels, other markers or the HUD/panels;
  labels placed last frame keep precedence among equals (no flicker).
- **Info panel** (`info.hpp`, from the snapshot only): click focuses and pins it, I toggles, Esc
  closes it before the window. Bodies: kind, distance + light-lag from the ship, distance from
  the Sun, radius, stations. Stations: faction/host, distance + light-lag, population, docking
  fee (and tab), job offers on the board, your contracts to/from it, market table (commodity,
  stock, ask, bid via `economy::quote`, entries furthest from base price first, cheap/dear
  tones). Ship: class, status, ETA, speed, reaction mass, hull, cargo, cash, hold by commodity,
  contracts aboard with deadlines, passengers. Hover tooltip: name, kind, distance. Clicks and
  hover over the panel don't reach the scene.
- **HUD**: date/time, focus, 1-2-5 scale bar (AU from 0.01 AU, km below), hotkey hint line that
  wraps at its separators; H toggles it.
- **Courses**: `SceneCourse` with the flip point (`flip_point()`, from the burn profile) for
  in-flight transits and the plot preview; flip tick across the line and a "flip" label when the
  course is long enough on screen; the preview is dashed (a LINELIST pipeline) with a
  "destination · arr date · Δv" label.
- **Tests**: overlay_tests (declutter, tiers, fade, system-view labels without overlaps, panel and
  tooltip layout, station and ship panels from a new game incl. market quotes and an underway
  ship, flip point vs `transit::ship_position`, scale bars, formatting); nav tests for H/I/Esc
  and the panel region; three 1280x720 offscreen screenshot tests (`labels`, `info`, `plot`
  checks) whose images are `docs/screenshots/labels-system.png`, `info-station.png`,
  `plot-preview.png`. All four presets green, no warnings, leak checking on.

Research: SDL3_ttf wiki and `examples/testgputext.c` (engine owns the atlas, y-up vertex data,
linear sampling; indexed draws per atlas); Christensen/Marks/Shieber's empirical study of
point-feature label placement and the greedy priority labellers of interactive maps (process by
priority, first free candidate position, a feature stays unlabelled only when higher priorities
hold all its positions) - so markers are obstacles too and positions are tried in the
cartographic preference order; HUD legibility (Frontier forum halo thread, Red Blob Games SDF
halos, Xbox Accessibility Guideline 102: 4.5:1 contrast) - so labels over the sky get a dark
outline rather than a drop shadow, panels a near-opaque backplate, and the dimmest panel text is
~7.7:1 on it.

Deferred: time-jump replay animation (interpolating between snapshots after `advance`), label
fade-in animation, SDF text for smooth zoom-scaled labels, a `--hover` option for tooltip
screenshots.
