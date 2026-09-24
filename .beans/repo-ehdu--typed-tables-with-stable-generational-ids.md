---
# repo-ehdu
title: Typed tables with stable generational IDs
status: completed
type: task
priority: normal
created_at: 2026-09-24T23:46:47Z
updated_at: 2026-09-24T23:55:21Z
parent: repo-zclh
---

table<T> struct-of-arrays, handle = (index, generation), free-list, deterministic iteration.

## Summary of Changes

- `core/include/simcore/table.hpp` (header-only):
  - `sim::Handle<Tag>`: `{uint32 index, uint32 generation}`, 8 bytes, trivially copyable, `<=>`/`==`,
    `std::hash` specialisation, default-constructed = null (generation 0 is never live).
    Tag makes handles of different tables distinct types.
  - `sim::Table<Tag, Row>`: slot map with sparse `slots_` (generation, dense index, free-list link),
    dense AoS `rows_` and parallel `dense_to_slot_`. O(1) `insert`/`emplace`, `erase` (swap-and-pop),
    `contains`, `get` (nullptr on stale), `at` (throws on stale), `size`/`empty`/`clear`/`reserve`,
    `erase_if` (back-to-front, deterministic), range-for yielding `{handle, row}`, `rows()` span,
    `handle_at(i)`, and raw `slots()`/`dense_slots()`/`free_list_head()` for future snapshot/hash.
  - Deterministic by construction: iteration = dense order, which depends only on the op sequence;
    `clear()` bumps all generations and rebuilds the free list in slot order.
  - AoS chosen over SoA (justified in header comment): small tables, systems touch most fields;
    storage is private so SoA/hot-cold split can come later without changing handles.
- `core/tests/table_tests.cpp`: null handles, stale detection, generation bump on reuse, FIFO reuse,
  swap-and-pop correctness, iteration (mutable + const), identical-op-sequence determinism (rows,
  slot metadata, free list), `erase_if` and collect-then-erase patterns, `clear`, hashing,
  move-only rows, and static_asserts that ShipId/StationId don't convert.

Verified: dev preset (GCC 13, ASan/UBSan) and Clang build warning-free, all tests pass.

Best-practice check: WG21 P0661 slot_map (Deutsch) — confirmed the slots + dense values +
reverse-index layout and swap-and-pop erase; P0661 accepts silent generation wrap by default.
Generational-index write-ups (Lucas Sardois, studyplan.dev, jodyhagins/slotmap, electrp slotmap
post) recommend a FIFO free list so churn on one entity doesn't burn one slot's generation. Changes
made as a result: free list is FIFO (intrusive head/tail) instead of the LIFO stack I first planned,
and slots whose generation would wrap are **retired** (never reused) instead of silently wrapping, so
a stale handle can never validate again. `clear()` rebuilds the free list front-to-back as in P0661.

Deferred: the retirement path (needs ~4e9 erases) is not unit-tested — a snapshot restore API
(repo-dtzo) would allow seeding a near-exhausted generation to test it. No SoA storage yet.
