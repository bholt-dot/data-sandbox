---
# repo-3n6j
title: Snapshot support for StatPipeline
status: todo
type: task
created_at: 2026-09-25T00:08:02Z
updated_at: 2026-09-25T00:08:02Z
parent: repo-zclh
blocked_by:
    - repo-dtzo
---

Serialize modifier table, bases and scope lists via the snapshot field-visitor pattern; add rebuild_indices() after load (derived buckets/caches are not saved). Round-trip test: get() results and breakdowns identical after load.
