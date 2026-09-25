---
# repo-9ly7
title: Game world data model, content loading and Secondhand scenario
status: completed
type: feature
priority: normal
created_at: 2026-09-25T00:19:59Z
updated_at: 2026-09-25T00:25:41Z
parent: repo-tydy
---

The glue every game system builds on.

- [x] Content definitions (commodity, body, station, ship class, scenario) read from data/*.toml
- [x] Content build: orbit system from body/station defs (parent-before-child), validation
- [x] Game calendar (sim::Time <-> Gregorian date, J2000 element epoch)
- [x] World: serializable runtime tables (companies, ships, crew, loans, station markets), scheduler event payloads, stat pipeline
- [x] new_game(): Secondhand scenario from data
- [x] Starter data set (small; full solar system is repo-pl7w)
- [x] Tests: content loads, world save/load/hash round trip, scenario reproducible by seed

## Summary of Changes

- `expanse/content.hpp`: CommodityDef, BodyDef (+ OrbitDef), StationDef (+ market entries), ShipClassDef, ScenarioDef with schema validation; `Content::load` builds the OrbitSystem in parent-before-child order, detects cycles / missing GM / duplicate market entries, and fingerprints the data for save compatibility.
- `expanse/calendar.hpp`: game epoch 2350-01-01 = sim::Time{0}; Hinnant civil-date conversion; J2000 constant for element epochs; parse/format.
- `expanse/world.hpp`: World = fingerprint, seed, Scheduler<Event>, named persistent RNG streams, tables (companies, ships, crew, loans), per-station stock, StatPipeline, player id. Event payloads: ShipArrives, LoanPaymentDue, DailyTick, WeeklyTick. Ship location = Docked | Underway (committed straight-line transit). save_world/load_world/world_hash.
- `expanse/scenario.hpp`: new_game() — Secondhand: 1,850 cr, 35% tank, 62% hull, 380k loan at 4,200/week, one-person crew, docked at Ceres.
- `data/`: starter commodities, 8 bodies (planets from Standish J2000 table), 5 stations, 1 ship class, 1 scenario.

Best-practice check: static-definition vs runtime-state separation (standard save architecture), integer currency (avoids float rounding; int64 ample). Economy code should round to whole credits at transaction boundaries.
