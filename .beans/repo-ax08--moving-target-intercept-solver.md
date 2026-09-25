---
# repo-ax08
title: Moving-target intercept solver
status: completed
type: task
priority: normal
created_at: 2026-09-24T23:46:47Z
updated_at: 2026-09-25T00:12:14Z
parent: repo-bc5e
---

Iterate on arrival time so destination position at arrival is used. Enables launch windows / 'Ceres is behind the sun'.



## Summary of Changes

- `expanse::transit::plot_transit(OrbitSystem, PlotRequest) -> Plot`: origin is an OrbitSystem body
  (`Origin::at_body`) or a free ship state (`Origin::at_state`); destination is a body. Solves for
  the earliest whole-second arrival t1 such that the ship leaving at t0 reaches the destination's
  position at t1: f(T) = transit_time(|p_dest(t0+T) - p0|, a, cap(T)) - T on the integer-second grid.
  - f(0) > 0; scan outward (steps of brachistochrone(d0)/32, min 60 s, for 64 steps, then x1.5) to the
    first sign change so the earliest intercept is found, then refine the integer bracket with
    Illinois regula falsi plus a bisection fallback whenever a step fails to halve the bracket.
    Bounded by `intercept_max_evaluations` (256); observed ~35 evals for torch plots and ~80 for
    slow capped plots. Deterministic; exceeding the horizon/bound reports `no_intercept`.
  - The flown profile is `profile_for_duration(d, a, T)` so ship and destination coincide exactly at
    arrival (the ship never arrives before the destination does).
  - `Plot`: status (ok / invalid_request / no_intercept / insufficient_reaction_mass /
    path_obstructed, `describe()` text), departure, arrival, duration, start/end points, profile,
    distance, match dv, total dv, reaction mass used/left, peak speed, flip time, closest approach to
    an optional keep-out body (e.g. the Sun: "Ceres is behind the sun"), evaluation count.
  - `best_departure(...)` scans a departure window for earliest arrival or least dv (launch windows).
  - `ship_state` / `ship_position(plot, t)` give the ship along the path (for "where is my ship" and
    light lag).
- Tests: Earth->Mars (35 departure dates, 1 g) and Tycho-like->Ceres-like (50 dates x 3 accels):
  ship position at arrival equals the destination, arrival is the earliest second (one second earlier
  is unreachable), agreement with an independent fixed-point iteration within 2 s, evaluation bound,
  slow capped plots, insufficient rmass, no-intercept, invalid requests, ship-state origin, Sun
  obstruction, best-departure window, determinism.

Research checked: root-finding robustness (bisection/Brent guarantee convergence given a bracket;
fixed-point iteration T <- transit_time(d(T)) only converges while the destination's closing speed
is below the ship's average speed, which fails for slow coast plots). Chose a bracketed method; since
sim time is integral seconds, a bracket on the integer grid with Illinois + bisection safeguard
(Brent-like guarantee, <= ~2 log2 steps) instead of full Brent on doubles. Intercept/pursuit
literature (time-to-go iteration, rendezvous boundary conditions) confirmed iterating on arrival time
as the standard approach.
