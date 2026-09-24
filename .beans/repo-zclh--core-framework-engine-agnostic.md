---
# repo-zclh
title: Core framework (engine-agnostic)
status: todo
type: epic
created_at: 2026-09-24T23:46:46Z
updated_at: 2026-09-24T23:46:46Z
parent: repo-lbrq
---

Reusable data-oriented sim core. Rule: core/ never includes game code.

C++20, CMake, doctest, toml++, replxx/linenoise. Doubles are fine (no MP); keep same-build reproducibility via seeded RNG streams + stable iteration order.
