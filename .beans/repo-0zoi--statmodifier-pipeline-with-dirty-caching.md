---
# repo-0zoi
title: Stat/modifier pipeline with dirty caching
status: completed
type: task
priority: normal
created_at: 2026-09-24T23:46:47Z
updated_at: 2026-09-25T00:07:16Z
parent: repo-zclh
---

(source, target, stat, add/mul, expiry) modifiers; cached totals recomputed only when dirty.

## Summary of Changes

Added `sim::StatPipeline` (`core/include/simcore/stats.hpp`, `core/src/stats.cpp`, tests in
`core/tests/stats_tests.cpp`).

- Plain data: `StatId{u32}`, `EntityKey{kind, index, generation}` (targets, scopes and sources;
  `entity_key(kind, handle)` helper; `global_scope` = `{}`), `Modifier{source, target, stat, op,
  value, expires}` stored in a `sim::Table` (`ModifierId` handles).
- Ops and formula: `(base + Σadd) × (1 + Σincreased) × Π(1 + more)`, then the highest `floor`
  and the lowest `cap` (cap wins a conflict). No implicit clamp at zero. Base is the target's own,
  else the global per-stat default, else 0.
- Order independence: each op group is sorted by value before summing/multiplying, so a total
  depends only on the multiset of values, bit-for-bit, across different insert/erase histories.
  Non-finite values are rejected; -0.0 is normalised to +0.0.
- Scopes: every target sees global modifiers and modifiers on scopes set with
  `set_scopes(target, ...)` (flat, one level).
- Caching: pull-based epoch validation. A mutation stamps a new epoch on one (target, stat)
  bucket (or on a target's scope list), so a global/station modifier costs O(log B), not
  O(N entities). `get()` checks own + global + scope buckets (O((S+2) log B)) and recomputes
  (O(k log k)) only if one changed. `evaluate()` (uncached) and `breakdown()` give identical bits.
- Removal: `remove(id)`, `remove_source(source)` (ordered source index), `set_value(id, v)` for
  wear, `forget_target(key)` for despawns (clears the whole cache, which is rare).
- Expiry: half-open (`expires = T` applies while now < T); `expire_until(now)` removes in
  (expires, id) order and can report what expired; `next_expiry()` lets the scheduler schedule
  one event instead of polling. Explicit calls rather than wiring into the scheduler keep the
  pipeline independent of it.
- `breakdown(target, stat)` returns base, per-op aggregates, clamps, value and every contributing
  modifier (with the scope it came from), sorted by (op, source, target, id).
- Tests: formula, clamps, global base default, bit-identical order independence under shuffles
  and churn (plus a check that naive summation *would* differ), dirty-only recompute counts,
  removal by source/handle, expiry, global + station + entity stacking, forget_target, breakdown.

### Research
Checked the Unreal GAS aggregator (`((base + add) × mul) / div`, override short-circuit; mul
magnitudes stacked additively with a bias of 1), Path of Exile's `(base + added) × (1 + Σincreased)
× Π(1 + more)`, Paradox forum discussions on additive percentage stacking (reductions to ~0
break balance), and reproducible floating-point summation (Demmel/Nguyen binned summation).
What it changed:
- Split multiplicative ops into `increased` (additive %, the Paradox/PoE/GAS default) and `more`
  (compounding) instead of a single `mul`.
- Left out GAS-style `override`: with several overrides it must pick by application order,
  which breaks order independence; `floor` + `cap` with the same value covers it.
- No implicit clamp at zero (the Paradox reductions problem is a per-stat design choice); games
  add a `floor` modifier where needed.
- Exact/binned summation is overkill here: sorting the small value group before summing gives
  bit-identical results for the same multiset at O(k log k), on a cache miss only.
- Push-based dirty flags would make a global modifier O(N); used pull-based epoch validation
  so scope-wide changes touch one bucket.

### Deferred
- Nested scopes (station → faction → global); scopes are one level for now.
- Snapshot support: persistent state is the modifier table + bases + scope lists; bucket lists,
  source/expiry indexes and the cache are derived and need a `rebuild_indices()` on restore.
- Per-stat default floors/caps defined in data (currently expressed as global modifiers).
