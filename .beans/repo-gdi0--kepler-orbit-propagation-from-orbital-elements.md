---
# repo-gdi0
title: Kepler orbit propagation from orbital elements
status: completed
type: task
priority: normal
created_at: 2026-09-24T23:46:47Z
updated_at: 2026-09-25T00:03:46Z
parent: repo-bc5e
---

Solve Kepler's equation; hierarchical frames (moons around planets). Validate vs. known positions.

## Summary of Changes

New code in `expanse/` (namespace `expanse::orbit`, plus `expanse::Vec3` and `expanse::units`):

- `expanse/vec3.hpp`: minimal double `Vec3` (arithmetic, dot, cross, length, distance, normalized).
- `expanse/units.hpp`: SI constants and helpers (`au()`, `deg()`, `km()`, `days()`, `to_au()`,
  `to_deg()`, `mu_sun`, `mu_earth`, Julian year/century).
- `expanse/orbit.hpp` / `src/orbit.cpp`:
  - `Elements` (a, e, i, Omega, omega, M0, `sim::Time` epoch, parent mu), SI units.
  - `solve_kepler_elliptic[_detailed](M, e)`: M reduced to [-pi, pi] and folded to [0, pi] by
    symmetry; root bracketed in [M, min(M+e, pi)]; Danby starting guess M + 0.85e; Newton steps
    clamped to the bracket. f(E) = E - e sin E - M is convex on [0, pi], so after at most one
    overshoot (clamped to hi, where f >= 0) iterates converge monotonically for any e < 1.
    Stops when the step falls below what rounding in f can resolve (~4 eps max(1,E)/f'), capped
    at 50 iterations. Measured: residual <= 4.4e-16, 4-5 iterations typical, <= 8 on the test
    grid up to e = 0.999, <= 12 on a 10^5-point sweep.
  - `Orbit` = elements preprocessed to perifocal basis vectors P, Q and mean motion;
    `make_orbit()` validates (throws `std::invalid_argument`, incl. e >= 1 for now).
  - `state_at()` / `position_at()`: state straight from E (x = a(cos E - e), y = b sin E,
    v = sqrt(mu a)/r * (-sin E, sqrt(1-e^2) cos E)), no true anomaly needed. dt is computed in
    integer seconds before converting to double.
  - `OrbitSystem`: data-oriented table (parallel arrays: parent, fixed flag, fixed offset,
    orbit) indexed by `BodyId`. Parents must be added before children, so index order is a
    parent-before-child order: `evaluate(t, span)` fills all world states in one forward pass;
    `world_position(id, t)` / `world_state(id, t)` walk the short parent chain for single-body
    queries (intended for the intercept solver's inner loop). `add_fixed()` covers the Sun and
    bodies/markers fixed relative to a parent.
- `expanse/tests/orbit_tests.cpp` (new `expanse_tests` target): Kepler residual sweep over
  e in [0, 0.999] and M in [-2pi, 4pi] (< 1e-12); M -> 0 / e -> 1 corner; periodicity; vis-viva
  and angular momentum; periapsis/apoapsis distances, direction and speed; velocity vs finite
  difference; J2000 mean motion vs published rates (Earth-Moon bary, Mars); Earth's distance at
  2024 perihelion/aphelion (off by 1-2e-5 AU; test tolerance 2e-4 relative) and longitude at the 2024 March equinox (within
  0.03 deg of 180 deg minus precession); hierarchical Sun -> Earth -> Moon -> station and a fixed
  offset under Mars (world = parent + local, bulk vs single-body agreement).

### Research

- Kepler solvers: Charles & Tatum (CeMDA 1997), "The Convergence of Newton-Raphson Iteration
  with Kepler's Equation": with E0 = M, Newton converges slowly or erratically for some (e, M)
  at high e; E0 = pi always converges. Danby/Burkardt's M + 0.85 e is the common practical
  starter; Halley/Danby higher-order iterations save iterations but aren't needed here.
  Changed because of this: instead of trusting the starter, the solver brackets the root and
  clamps Newton steps, relying on convexity for guaranteed monotone convergence.
- Testing the first draft (bisection fallback) exposed two issues: (1) Newton from the left of
  a convex root overshoots, and falling back to bisection made it converge linearly (11-16
  iterations), so it now clamps to the upper bracket instead; (2) near e -> 1, M -> 0 the
  derivative 1 - e cos E is ~1e-2..1e-4, so a fixed 4-ulp step tolerance cycled between
  neighbouring doubles until the iteration cap. The tolerance is now scaled by 1/f' (the
  problem's conditioning).
- Elements -> state vector: standard perifocal (PQW) formulation with the
  R3(-Omega) R1(-i) R3(-omega) rotation (Wikipedia "Orbital state vectors", orbital-mechanics.space
  notes, poliastro `rv_pqw`); used the eccentric-anomaly form to skip the true anomaly.
- JPL/Standish "Keplerian Elements for Approximate Positions of the Major Planets", Table 1
  (1800-2050): ssd.jpl.nasa.gov is blocked by the sandbox proxy, so values were taken from a
  mirror (gitlab.com/mechgl/phyz `keplerian.go`), where they match the published table.
  omega = varpi - Omega, M = L - varpi, frame = J2000 mean ecliptic. The mean-motion test
  compares n against dL/dt - dvarpi/dt (comparing against dL/dt alone was off by 2e-5).

### Deferred

- Hyperbolic/parabolic orbits (e >= 1): `Elements` keeps generic a/e; add
  `solve_kepler_hyperbolic` and a branch in `state_at` when needed (`make_orbit` rejects now).
- Element rates (secular drift of a, e, varpi, ...) and the full solar-system dataset (separate
  bean). `BodyId` is a dense index since bodies are static scenario data; switch to core
  generational handles if bodies ever become removable.
