---
# repo-dtzo
title: Snapshot save/load + state hashing
status: todo
type: task
priority: normal
created_at: 2026-09-24T23:46:47Z
updated_at: 2026-09-25T00:01:48Z
parent: repo-zclh
---

Binary snapshot of all tables; hash for determinism tests.

## Notes from dependencies
- Scheduler: needs a restore API that re-inserts pending events/periodics with their original ordinals (pending_events()/periodics() already expose the data).
- Table: consider a test hook to start a slot near generation exhaustion to cover slot retirement.
- Rng: state is the public std::array<uint64_t,4> s — hash/serialize directly.
