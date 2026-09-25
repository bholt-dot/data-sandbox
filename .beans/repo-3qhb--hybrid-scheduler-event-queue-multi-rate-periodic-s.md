---
# repo-3qhb
title: 'Hybrid scheduler: event queue + multi-rate periodic systems'
status: completed
type: task
priority: normal
created_at: 2026-09-24T23:46:47Z
updated_at: 2026-09-25T00:01:04Z
parent: repo-zclh
---

Priority queue keyed by sim time (tie-break by sequence no.); periodic systems (daily/weekly/monthly); advance(until) jumps event-to-event.


## Summary of Changes

Added `core/include/simcore/scheduler.hpp` (header-only `sim::Scheduler<Payload>`) and
`core/tests/scheduler_tests.cpp` (16 doctest cases), registered in `core/CMakeLists.txt`.

- One timeline for one-shot events and periodic systems. Total order is
  `(time, priority, ordinal)`: lower priority value first, then a global ordinal from a
  monotonically increasing counter (events: at scheduling; periodic systems: at registration,
  kept on every recurrence so the order between systems is stable).
- Events are plain data (`Payload`, e.g. a `std::variant` of small structs). No closures are
  stored: the caller passes the handler to each `advance_*` call and dispatches on the payload.
  Periodic systems carry a payload too, so all scheduler state is snapshot-able
  (`pending_events()`, `periodics()`).
- Periodic cadence is grid-based (`anchor + k*period`), so it never drifts and long jumps fire
  the exact count (365 daily ticks for `advance_by(365d)`).
- Cancellation via `EventId {slot, seq}`: the slot is freed immediately (seq mismatch makes the
  heap entry a tombstone, skipped lazily). Heap is compacted when tombstones exceed half the heap.
  `remove_periodic()` works the same way, also from the system's own handler.
- `advance_to` / `advance_by` / `advance_until(limit, handler, flag_mask | predicate)` /
  `advance_until_next_event`. Advancing is half-open `[now, target)`; stopping on a flagged event
  leaves the clock at its time with later same-instant occurrences still pending.
- Handlers may schedule (including at the current instant) and cancel. A per-instant fire limit
  guards against livelock (checked before popping, so nothing is lost); re-entrant advance and
  scheduling in the past throw.

### Research
Checked DES event-list literature and tools (Wikipedia DES, OMNeT++ simultaneous-events paper,
arXiv 2105.00069 on deterministic ordering, simmer), lazy-deletion/cancellation discussions
(DiscreteEvents.jl issue #34, ladder queue paper), Paradox/Clausewitz-style daily-tick +
event-queue designs, and events-as-data vs std::function (gamedev.net, save-system articles).
What it changed:
- Confirmed insertion-counter FIFO tie-breaking, but added an explicit priority tier before it
  (as simulators with event-type priorities do) and gave periodic systems a fixed ordinal so
  multi-rate systems keep registration order at shared timestamps.
- Tombstone cancellation is standard but bloats the heap under mass cancellation, so added
  compaction (safe because the key is a total order).
- Captured `std::function` state defeats save games and small-buffer optimization, so payloads
  are plain data and handlers are not stored.
- Binary heap kept over calendar/ladder queues: pending set is small (ships + a few systems).

### Deferred
- Snapshot restore API (re-inserting pending events/periodics with their original ordinals)
  until the save-game work needs it.
