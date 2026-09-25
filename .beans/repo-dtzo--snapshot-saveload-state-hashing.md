---
# repo-dtzo
title: Snapshot save/load + state hashing
status: completed
type: task
priority: normal
created_at: 2026-09-24T23:46:47Z
updated_at: 2026-09-25T00:10:36Z
parent: repo-zclh
---

Binary snapshot of all tables; hash for determinism tests.

## Notes from dependencies
- Scheduler: needs a restore API that re-inserts pending events/periodics with their original ordinals (pending_events()/periodics() already expose the data).
- Table: consider a test hook to start a slot near generation exhaustion to cover slot retirement.
- Rng: state is the public std::array<uint64_t,4> s — hash/serialize directly.

## Summary of Changes

New headers in `core/include/simcore/`:
- `serialize.hpp`: `Writer` / bounds-checked `Reader` (throws `SerializeError` with offset), and the
  `sim::Codec<T>` customization point with `sim::encode(sink, v)` / `sim::decode(reader, v)`.
  Canonical encoding: fixed-width little-endian integers, IEEE bit patterns for float/double,
  u64 counts for string/vector/map, u8 flag for optional, u32 index for variant. Nothing is memcpy'd
  as a struct, so padding never leaks. Supported: arithmetic, enums, bool, string, vector, array,
  optional, variant, monostate, pair, tuple, std::map (keys must be strictly ascending on load).
  Unordered containers are deliberately unsupported.
- **Field-list pattern** (one list drives save, load and hash):
  `static auto fields(auto& self) { return std::tie(self.a, self.b); }`. `self` deduces const for
  encode and non-const for decode. Added to `Time`, `Duration`, `Handle`, `Rng`, `EventId`,
  `PeriodicId`, `PendingEvent`. Full `Codec<T>` specialization is the escape hatch.
- `hash.hpp`: in-repo streaming XXH64 (`Hasher`, `xxh64`, `hash_state(parts...)`), checked against
  reference vectors (python-xxhash) and chunked streaming. It is a `ByteSink`, so it takes exactly
  the save byte stream: `hash_state(x) == hash_state(load(save(x)))` holds by construction.
  Doubles are hashed by bit pattern with no normalization, so -0.0 != 0.0.
- `snapshot.hpp`: `Codec<Table>` (generations, dense order + rows, free list in reuse order),
  `Codec<Scheduler>`, and the envelope `save_snapshot(schema, state|fn)` /
  `load_snapshot(bytes, current_schema, state|fn)`: magic "SIMS", u32 format version, u32 game
  schema version (exposed as `Reader::schema_version()` for migrations; newer saves are rejected),
  u64 body size, body, u64 XXH64 checksum. Loading into a state object decodes into a temporary
  first, so a failed load leaves the target untouched.

Core API additions:
- `Table::free_list()` and `Table::restore(generations, dense_slots, rows, free_list)`. It validates
  everything (no gen 0, live/free/retired partition, no duplicates) and gives the strong
  guarantee. It rebuilds `free_tail_`, so later inserts allocate identical handles. It also serves
  as the hook for tests near generation exhaustion.
- `Scheduler::snapshot()` -> `SchedulerState<Payload>` (now, next_seq, fires_at_instant,
  slot_count, free-slot stack, pending events by slot, periodics with ordinals) and
  `Scheduler::restore(state)`. It validates (unique seqs/ordinals < next_seq, slot partition, no
  past occurrences) and rebuilds the heap. Firing order and future EventIds match an uninterrupted
  run.

Tests (`serialize_tests.cpp`, `snapshot_tests.cpp`): byte layout, extremes, NaN/-0.0, nested
struct round trip, single-field hash sensitivity, XXH64 vectors, truncation at every cut point,
corrupt bool/variant/length/map order, table churn round trip plus identical future handles,
slot retirement via restore, restore validation, rng round trip, scheduler mid-run round trip vs
an uninterrupted run (event log incl. allocated ids), early-stop round trip, envelope corruption
(truncation, bit flip, magic, version, schema, trailing bytes, semantically invalid body). A
mutation check (reversed scheduler free-slot stack) was caught by the tests.

Research checked before committing:
- Reflection-lite field visitors (cbloom "reflection visitor", visit_struct, cista, reflect-cpp).
  A hand-written field list is the macro-free, dependency-free choice. Boost.PFR-style automatic
  aggregate reflection would remove even that line, but it adds new fields to saves silently,
  so it is deferred.
- Versioning: a global game schema version plus decoder branching (not per-field tags) is enough
  for single-player saves. The envelope carries a separate core format version.
- Endianness: explicit byte-wise LE encoding, never raw struct writes (padding, endianness).
- Hash: XXH64 was chosen over wyhash/rapidhash because rapidhash has no streaming form and wyhash
  has known entropy-loss edge cases. XXH64 is portable and bit-identical across platforms.
  FNV-1a is too slow and weak for multi-MB states.

Deferred: automatic aggregate reflection (PFR-style); compression; per-type version tags and
field-level migration helpers; enum range validation on load (the codec can't know valid values);
unordered_map support (needs sort-on-save); streaming a Writer directly to a file.
