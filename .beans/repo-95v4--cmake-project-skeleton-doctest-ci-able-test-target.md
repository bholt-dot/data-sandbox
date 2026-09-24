---
# repo-95v4
title: CMake project skeleton + doctest + CI-able test target
status: completed
type: task
priority: normal
created_at: 2026-09-24T23:46:46Z
updated_at: 2026-09-24T23:54:53Z
parent: repo-zclh
---

core/, expanse/, apps/ layout; warnings-as-errors; sanitizer build option.

## Summary of Changes

- Root CMake (3.24+, C++20), `cmake/ProjectOptions.cmake` (strict warnings, -Werror toggle, ASan/UBSan option, `sim_add_test()`), `cmake/Dependencies.cmake` (doctest v2.5.3 via FetchContent, pinned tag).
- Libraries `sim::core` and `sim::expanse`, app `belter`; CMakePresets (dev = Debug+sanitizers, release).
- Shared `sim::Time`/`sim::Duration` (integral seconds) so all systems agree on time.
- GitHub Actions CI: GCC+sanitizers and Clang release.
- CLAUDE.md with layering, reproducibility and workflow rules.

Verified: GCC 13 (sanitizers) and Clang 18 build warning-free, tests pass.
Best-practice check: modern CMake target-based layout, FetchContent pinned to tags rather than branches, tests as separate targets (HSF 'More Modern CMake', foonathan FetchContent guide).
