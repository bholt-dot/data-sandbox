---
# repo-cklx
title: Seeded RNG streams
status: completed
type: task
priority: normal
created_at: 2026-09-24T23:46:47Z
updated_at: 2026-09-24T23:59:15Z
parent: repo-zclh
---

Per-system streams derived from world seed; no global RNG.

## Summary of Changes

Added header-only `core/include/simcore/rng.hpp` (namespace `sim`) and `core/tests/rng_tests.cpp`.

- `sim::Rng`: xoshiro256** 1.0, a trivially copyable aggregate whose public `std::array<uint64_t,4> s`
  is the complete state (save/load/state hashing). Models `std::uniform_random_bit_generator`,
  but the header documents that `<random>` distributions must not feed sim results.
- Seeding: `Rng::from_seed(u64)` expands via SplitMix64 (never yields the all-zero state).
- Streams: `rng_for(world_seed, "economy", index = 0)` (or an integer key): FNV-1a 64 of the
  key, each component combined through the bijective SplitMix64 mixer, then seeded via SplitMix64.
  Streams depend only on their own (seed, key, index), so adding systems never perturbs others.
- Own distributions: `below(n)` / `uniform_int(lo, hi)` (Lemire multiply-shift with rejection,
  portable 64x64->128 multiply, handles the full int64 range), `uniform01()` (top 53 bits),
  `uniform_real`, `chance(p)`, `pick_weighted` (exact integer weights and double weights),
  `normal()` (Marsaglia polar), `shuffle` (Fisher-Yates).
- Tests: known-answer vectors for xoshiro256** (state {1,2,3,4}) and SplitMix64 (from Vigna's
  reference C, as used in rust-random/rand_xoshiro's tests), FNV-1a reference vectors, pinned
  `rng_for` golden outputs cross-checked against an independent Python implementation,
  stream independence (no shared values, ~32 differing bits per paired output, 10k distinct
  entity sub-streams), state round-trip, bounds/full range, coarse deterministic bias checks
  (including bound 3*2^62, where modulo would give 50% instead of the correct 33%),
  distribution moments, shuffle permutation/uniformity.

Research done before committing:
- Vigna's PRNG shootout (prng.di.unimi.it) and comparison articles: xoshiro256** is the
  recommended general-purpose 64-bit generator; seed it via SplitMix64 because initialising with a
  generator of a different nature avoids correlations between similar seeds. Chose it over PCG32
  because we want 64-bit output (one call per 53-bit double) and hash-derived streams rather than
  PCG increment-based streams.
- Lemire's nearly-divisionless bounded ints (lemire.me, dotat.at write-ups): adopted. Used a
  portable 128-bit multiply because `__int128` trips `-Wpedantic` and doesn't exist on MSVC.
- `<random>` distribution algorithms are unspecified and differ across libstdc++/libc++/MSVC
  (libc++'s normal uses Marsaglia polar, others Box-Muller), and they are *stateful* (they cache
  the second normal variate). This changed the design: our `normal()` discards the second variate
  so `Rng::s` alone is the full state. `std::log` isn't correctly rounded, so normals are
  reproducible per build/platform (the project's guarantee), not bit-identical across libms.
- Reference vectors came from rust-random/rngs, because the egress proxy blocked
  prng.di.unimi.it. The Python cross-check reproduces them from the published algorithm.

Deferred: xoshiro jump()/long_jump() (not needed with hash-derived streams). Folding the RNG
state into the world state hash waits for the state-hash work.
