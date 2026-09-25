---
# repo-31bx
title: 'Ship operations: depart, arrive, refuel, cargo'
status: completed
type: feature
priority: normal
created_at: 2026-09-25T00:26:38Z
updated_at: 2026-09-25T00:39:35Z
parent: repo-tydy
---

Player/NPC ship actions on the World: depart for a station (plot_transit, deduct reaction mass, schedule ShipArrives, set Underway), arrival (dock, post message), refuel (buy water as reaction mass at station price), load/unload cargo (capacity checks). Hull wear per transit. Used by the shell 'order'/'plot' commands and NPC traders.

## Summary of Changes

New `expanse/include/expanse/ships.hpp` + `expanse/src/ships.cpp` (tests: `expanse/tests/ships_tests.cpp`).

- `depart(content, world, ship, destination, CourseOptions{accel_g, max_delta_v_km_s})` → `DepartResult{status, reason, preview}`. Validates (known ship, docked, destination known and not the current dock, accel/dv budget sane, cargo within the hold, intercept found, clears the Sun keep-out, enough reaction mass), then deducts reaction mass, sets `Underway` (all fields, incl. heliocentric start/end from the plot) and schedules `ShipArrives` at `priority_arrivals` with `player_event` for the player's ships, and journals a departure message. Failures change nothing. The captain is implicit, so there is no crew check yet. `// TODO(finance): also require finance::can_depart(...)` marks the spot in `depart()` for the dock-fee check (`CourseStatus::dock_fees_owed` is reserved for it).
- `systems::ship_arrives`: docks at the destination, applies hull wear (`hull_wear()` = 0.5% per 1000 km/s of profile dv + 0.05% per day; `TODO(crew)` hook for engineer skill), posts "Dustkicker docked at Vesta Dock after 6d 5h." and an urgent warning when the hull first drops below 25%. Stale/null ShipIds, ships no longer underway, and events not at the transit's arrival time are ignored. NPC arrivals are not journaled.
- Acceleration is capped by the drive: `max_accel_g` is quoted at dry mass, so thrust = max_accel_g·dry and the limit at departure is max_accel_g·dry/wet (the profile is constant-accel, so departure mass binds). The preview says when this cap applies (`accel_limited`).
- Status queries: `ship_status()` (location text, ETA/time remaining, progress, speed, reaction mass t/%, dv available, cargo t/capacity, hull %, drive limit), `separation()` (distance + one-way light lag between stations), `light_lag()`, `format_trip()`. Added `units::speed_of_light`.
- Not done here: refuel and cargo load/unload belong to the economy bean. Abort or redirect while underway is not implemented: a transit is committed until arrival (documented in ships.hpp).

Best-practice check: read how Elite (economical vs fastest plot, clear fuel go/no-go), EVE (route preference), Aurora (auto-route lists only reachable systems), Terra Invicta (launch date / arrival date / dv trade-off with loiter time) and KSP porkchop planners present routes. That led to the fastest / cheapest-within-deadline / best-departure-window variants, an explicit `wait`, tank-after %, and a destinations list. The bisection on the dv cap in `plot_cheapest_within` relies on arrival time being monotone in the cap (a larger cap never slows any leg of the intercept function).
