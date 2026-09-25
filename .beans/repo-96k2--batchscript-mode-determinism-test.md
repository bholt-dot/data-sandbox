---
# repo-96k2
title: Batch/script mode + determinism test
status: completed
type: task
priority: normal
created_at: 2026-09-24T23:46:47Z
updated_at: 2026-09-25T00:35:18Z
parent: repo-9zts
---

sim --seed N --script f.txt; test runs 20 years twice and compares hashes.

## Summary of Changes
- `belter --script FILE [--echo]` runs commands in batch mode (stops at the first error with file:line, exits non-zero).
- CTest `belter_replay_twenty_years_deterministic`: runs a 20-year script in two separate processes and requires identical output incl. state hashes (separate processes also catch address-dependent ordering). `belter_script_error_exits_nonzero` covers the error path.
- Research: headless command-log replay + canonical state hashes in CI is the standard determinism regression check; a committed golden hash is deferred until game systems stabilize (follow-up bean).
