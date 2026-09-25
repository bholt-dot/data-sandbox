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

## Layout & layering

- `core/` → library `sim::core`, namespace `sim`. Reusable across sim projects.
  **Must never include anything from `expanse/`.**
- `expanse/` → library `sim::expanse`, namespace `expanse`. Game rules and data.
- `apps/` → executables (`belter`).
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
