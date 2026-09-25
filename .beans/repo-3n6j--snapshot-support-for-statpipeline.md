---
# repo-3n6j
title: Snapshot support for StatPipeline
status: completed
type: task
priority: normal
created_at: 2026-09-25T00:08:02Z
updated_at: 2026-09-25T00:17:38Z
parent: repo-zclh
blocked_by:
    - repo-dtzo
---

Serialize modifier table, bases and scope lists via the snapshot field-visitor pattern; add rebuild_indices() after load (derived buckets/caches are not saved). Round-trip test: get() results and breakdowns identical after load.

## Summary of Changes

- `StatPipeline::snapshot()` / `restore(StatPipelineState)` (Scheduler-style) plus `Codec<StatPipeline>` in `snapshot.hpp`, so a pipeline is saved, loaded and hashed like other state.
- Persisted (authoritative): the modifier `Table` including slot generations, dense order and free list (so ids allocated after a load match an uninterrupted run); bases as `std::map<StatKey, double>` (global defaults are the global scope's bases); non-empty scope lists as `std::map<EntityKey, std::vector<EntityKey>>`.
- Rebuilt on load by the private `rebuild_indices()`: per-bucket modifier lists, by-source and by-expiry indexes, and change epochs. The cache starts empty and `recompute_count()` restarts at 0.
- `restore()` validates its input (finite values, no -0.0, known `ModOp`, scope lists non-empty, sorted and unique, with no global scope and no self-reference) and leaves the pipeline unchanged on failure (strong guarantee). The codec turns failures into `SerializeError`.
- Hash choice: the hash covers the exact authoritative state, including handle allocation. This follows "same seed + same commands => same hash", and divergent allocation would change future ModifierIds. Bases and scopes are encoded canonically. Caches, epochs and empty scope lists do not affect the hash. Documented in `stats.hpp`.
- Added `fields()` to `StatId`, `EntityKey` and `Modifier`. The private `BucketKey` became the public `StatKey`.
- Tests in `core/tests/snapshot_tests.cpp`: randomized churn with a save/load mid-run; after load, identical get(), breakdown(), scopes and next_expiry; the next ModifierId matches the uninterrupted run; identical continuation log, expiry order and hash; hash semantics; invalid states rejected through restore() and through bytes; truncated input and checksum mismatches rejected.
- Best-practice check: save authoritative state only and recompute derived data on load (for example the mightyprofessionalgaming.com save-game tutorial and the SPUD approach it describes). Validate untrusted input on load and keep the target unchanged on failure. The implementation already did this, so nothing changed.
