---
# repo-ug8l
title: Commodity definitions + station stockpiles
status: completed
type: task
priority: normal
created_at: 2026-09-24T23:46:47Z
updated_at: 2026-09-25T00:44:47Z
parent: repo-xcm0
---

Starting set: water, reaction mass, food, spare parts.

## Summary of Changes

- Definitions already existed (`CommodityDef` + `data/commodities.toml`: water, oxygen, food, ore,
  metals, parts, medical). Reaction mass is not its own commodity: it is water, exposed as
  `economy::reaction_mass_commodity()`.
- Stockpiles: `StationState` (world.hpp) now holds `stock`, `unmet` (demand not met on the last
  daily tick, i.e. a visible shortage) and `disrupted_days` per commodity; all three are in
  `fields()` and initialised in `new_game`. `load_world` rejects saves whose per-station vectors
  don't match the loaded commodity count.
- `economy::market_entry()` answers "does this station trade X" (only listed commodities trade).
- Tests (`expanse/tests/economy_tests.cpp`): the shipped basics exist, stockpiles start from the
  data, and every station sells water (so refuelling works everywhere).
- Research/design notes are in repo-jo7k.
