---
# repo-s00q
title: Viewer camera, navigation and scene
status: completed
type: feature
priority: normal
created_at: 2026-09-25T01:08:58Z
updated_at: 2026-09-25T12:14:46Z
parent: repo-89dz
blocked_by:
    - repo-sp5l
---

Double-precision world positions made camera-relative per frame, reversed-Z depth, orbit line strips, bodies as impostors -> lit spheres up close, stations/ships/transit lines. Camera: orbit a focus target with smooth fly-to transitions and log-space zoom (Ceres cluster to Saturn). Hotkeys: F follow player ship, Home = Sun, 1-9 bookmarks (Earth, Mars, Ceres cluster, Jupiter, Saturn...), Tab/Shift+Tab cycle stations, [ ] cycle ships, R reset. Picking (click/hover -> hovered object id) exposed for the overlays bean.

## Summary of Changes

**Precision and depth.** World positions stay double; `prepare_frame` subtracts the eye in double and
narrows every draw (spheres, sprites, line vertices) to float camera-relative AU
(`camera_relative()` in `scene.hpp`). Line strips are no longer static: orbits are re-sampled per
frame directly in camera-relative coordinates. Depth is reversed-Z with an infinite far plane
(`reversed_z_projection`: depth = near / distance), D32_FLOAT chosen via
`SDL_GPUTextureSupportsFormat` (fallbacks D32_FLOAT_S8_UINT, D24), clear 0, compare GREATER,
near plane 1 km. Pass order: spheres (depth write), lines (depth tested), sprites (CPU occlusion).

**Bodies.** Dots with a minimum pixel size far away; up close a ray-traced sphere impostor
(`sphere.vert/.frag`): a camera-facing quad sized to the tangent cone, exact ray-sphere hit in the
stable geometric form, `gl_FragDepth`, 1-px analytic silhouette AA, Lambert from the Sun with a
dark night side, limb darkening for the Sun. Dot and sphere crossfade over projected radii
min_px..2*min_px; planet glow fades with the dot. Real `radius_km`. Markers (dots, stations,
ships, rings) hidden behind a body are culled on the CPU (`occluded`), surface ports at a body's
centre stay visible. `BodyDef` gained an optional `color = "#rrggbb"` (`parse_hex_color`, schema
check, test) set for the Sun, planets, major moons, Ceres and Vesta; the old palette is the fallback.

**Camera and controls** (`nav.hpp`, SDL-free): `OrbitCamera` orbits a focus object and tracks it
as it moves; fly-to eases look-at point and log(distance) over 0.8 s (smootherstep) and zooms out
mid-flight on long jumps so both ends stay in view (van Wijk & Nuij); wheel / +/- zoom in log space
with a short exponential ease; per-focus limits (bodies >= 2 radii, stations/ships clear of their
host body, 200 AU max); per-focus framing distances (planets with their regular moons). `Navigator`
maps `InputEvent`s: left-drag orbit, click focus, shift/right/middle-drag pan, F player ship, Home
Sun, 1-9 bookmarks by content key (Ceres cluster = ceres_station at 0.8 AU), Tab/Shift+Tab stations,
[ ] ships, R reset (animated), Esc/Q close. Controls print on launch (`belter --view` prints them
before the shell takes the terminal) and are in CLAUDE.md. Hovered object name shows in the title.

**Picking** (`pick.hpp`): `pick(scene, camera, x, y, w, h, max_px)` -> `PickHit{ObjectRef, distance_px}`
on the double-precision scene, skipping occluded objects; `Scene::objects` carry ref/key/name/position/
radius for the labels bean; `Navigator::hovered()`.

**Orbit lines.** Adaptive sampling in eccentric anomaly: chord sag <= 0.3 px at the point's distance
and segments <= half their distance to the eye, so an orbit passing by the camera stays smooth with
O(log(size/distance)) extra vertices; orbits smaller than ~3-30 px fade out.

**Tests.** Camera/nav: easing, fly-to convergence and mid-flight zoom-out, tracking a moving focus,
log zoom steps and limits, orbit/pan, hotkeys/bookmarks, cycling, click vs drag, vanished focus.
Scene: reversed-Z near->1 / far->0 and float-distinct at 40 AU, sub-metre camera-relative precision
at 30 AU (and that naive float loses 1 km), adaptive sampling, dot->sphere, occlusion, picking,
frame buffer reuse. GPU (offscreen lavapipe): system, Earth close-up (lit limb vs dark night side
checked), Ceres cluster (player ring + Belt stations checked). belter-view gained
`--focus/--distance-au/--yaw/--pitch`. Screenshots: docs/screenshots/{system,close-up-planet,ceres-cluster}.png.

**Research (and what it changed).** Reversed-Z vs log depth (NVIDIA "Depth precision visualized",
Outerra posts): float reversed-Z gives near-uniform relative precision without writing depth in
every shader, so no log depth; infinite far plane. SDL3 GPU: D32_FLOAT is not guaranteed ->
format query with fallbacks; clear_depth 0 + COMPAREOP_GREATER; found by rendering that SDL's
default rasterizer state clamps depth instead of clipping (enable_depth_clip = false), which let
lines to points behind the eye smear across the screen -> enable_depth_clip = true. Floating
origin / camera-relative rendering (Godot large-world article, Babylon.js floating origin):
subtract in double per frame, never ship absolute positions. Sphere impostors (gltut "Lies and
Impostors", Ben Golus "Rendering a Sphere on a Quad"): enlarge the quad for perspective (tangent
cone), write depth, analytic AA -> adopted instead of a tessellated mesh. Fly-to (van Wijk & Nuij
"Smooth and efficient zooming and panning"): interpolate zoom in log space and bump the zoom out on
long pans -> implemented as a log-distance bump with C2 smootherstep easing.

Deferred: labels/info panel/HUD (repo-d2ta, which will use `pick`/`hovered`); atmosphere rim,
rings, HDR/bloom, textures (repo-zvv9); 1-px aliased lines (no wide-line support in SDL GPU);
no drag inertia.
