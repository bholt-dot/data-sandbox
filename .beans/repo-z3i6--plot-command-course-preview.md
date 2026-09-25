---
# repo-z3i6
title: '''plot'' command: course preview'
status: completed
type: task
priority: normal
created_at: 2026-09-24T23:46:47Z
updated_at: 2026-09-25T00:39:35Z
parent: repo-bc5e
---

Show transit time, rmass cost, arrival date for a given accel.

## Summary of Changes

Game-side API for the course preview, in `expanse/include/expanse/ships.hpp` (the shell command that prints it belongs to the REPL work):

- `plot_course(content, world, ship, destination, CourseOptions{accel_g, max_delta_v_km_s})` → `CoursePreview`: departure (now), arrival, turnover (flip), duration, distance (AU), Δv total and velocity-match share, peak speed, the accel actually flown (capped by the drive at current mass), reaction mass needed / aboard / left, % of tank used and left, expected hull wear, one-way light lag, a feasibility status with a player-facing reason, and the raw `transit::Plot`. Numbers are still filled in when the tank is too small, so the shell can show the shortfall.
- `plot_cheapest_within(..., accel_g, max_travel)`: the least-Δv course that departs now and arrives within N days (bisects on the Δv cap); `too_slow` if even full burn misses the deadline.
- `plot_best_departure(..., window, step, Objective)`: wraps `transit::best_departure`; `wait` says how long to stay docked.
- `plot_all_destinations(...)`: a preview for every other station (a reachable-destinations list).

Best-practice check: looked at the route/transfer planners in Elite Dangerous, EVE, Aurora 4X, Terra Invicta and KSP (porkchop plots). That is where the fastest/economical/deadline/window variants, the explicit wait and tank-after figures, and the always-shown go/no-go reason came from.
