---
# repo-0z5p
title: Brachistochrone transit + reaction mass model
status: completed
type: task
priority: normal
created_at: 2026-09-24T23:46:47Z
updated_at: 2026-09-25T00:12:12Z
parent: repo-bc5e
---

t = 2*sqrt(d/a); delta-v and rmass via rocket equation; acceleration limits by crew g-tolerance.



## Summary of Changes

- New `expanse/include/expanse/transit.hpp` + `src/transit.cpp` (namespace `expanse::transit`), pure
  functions and small structs:
  - `BurnProfile` (accel -> coast -> decel along a straight line), `brachistochrone(d, a)`,
    `capped_profile(d, a, max_dv)` (coast when the dv cap binds), `transit_time(d, a, cap)`,
    `profile_for_duration` / `delta_v_for_duration` (inverse: cheapest dv to arrive in time T, via the
    cancellation-safe root of v^2 - aT v + a d = 0), `state_along(profile, elapsed)`.
  - `MassBudget` (dry, cargo, reaction mass, ve), `available_delta_v`, `reaction_mass_from_initial`,
    `reaction_mass_from_final`, `reaction_mass_for(ship, dv)` (Tsiolkovsky, expm1 for accuracy).
  - Crew g-tolerance is just the `accel` parameter (no juice model).
  - `units.hpp`: `g0`, `gees()`, `tonnes()`, `km_per_s()`.
- Approximations (documented in the header): straight-line path, solar gravity ignored during the
  burn (drive out-pulls the Sun by 100-1000x); velocity match to the destination added to the dv
  budget as |v_dest(t1) - v_origin(t0)| (conservative upper bound on a blended burn; 5-40 km/s vs
  hundreds of km/s peak speed, so no time correction).
- Epstein ve = 1.0e7 m/s (~3.3% c, Isp ~1.02e6 s): about half the Expanse-wiki figure (~1.9e7 m/s).
  Hauler with tank = 0.5 x (dry+cargo) has 4055 km/s full-tank dv; 1 AU at 0.3 g = 5.2 d,
  1327 km/s, 37% of tank; 1 AU at 0.1 g = 9.0 d, 22%; 0.5 AU at 0.3 g = 27%; a 300 km/s-capped
  slow boat over 3.4 AU = 45 d, 9%.
- Tests (`expanse/tests/transit_tests.cpp`): 1 AU at 1 g = 2.859 d closed form, coast monotonicity
  and distance conservation, inverse round trip, rocket-equation round trip, tank-share tuning,
  path state at start/mid/end and phase continuity.

Research checked: brachistochrone / flip-and-burn formulas (Atomic Rockets torchships, trajectory
calculators) confirmed t = 2 sqrt(d/a), dv = a t; Expanse wiki / ToughSF / KSP-forum Epstein
estimates (ve ~1.9e7 m/s canon, Rocinante dv ~4000 km/s) - chose ve at ~half canon so reaction mass
is a real constraint while a hauler's full-tank dv (~4000 km/s) matches the fan Rocinante estimate.
No standard game treatment of the velocity mismatch turned up, so the conservative additive term was
kept.
