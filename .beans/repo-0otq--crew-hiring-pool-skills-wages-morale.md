---
# repo-0otq
title: 'Crew: hiring pool, skills, wages, morale'
status: completed
type: task
priority: normal
created_at: 2026-09-24T23:46:47Z
updated_at: 2026-09-25T00:43:37Z
parent: repo-tydy
---

Crew as table rows; roles (pilot, engineer, medic...), skills affect outcomes; payroll each period.

## Summary of Changes

- `expanse/include/expanse/crew.hpp`, `expanse/src/crew.cpp`: crew system.
  - Hiring pool per station (job-seekers are `CrewMember` rows with null ship), sized
    `round(1.5·log10(pop) − 2)` clamped to 1..12 (Ceres 8, Mars Highport 6, Tycho 4), from
    `world.rng("crew")`. Weekly: each seeker leaves with p=0.35, newcomers refill to target.
    Seekers come mostly (8:1 weight) from origins of the dock's faction.
  - Roles pilot/engineer/medic/deckhand, skill 5..95 (normal around a role mean). Asking wage =
    role base × (0.5 + 1.5·skill/100) × origin multiplier × ±10%, rounded to 10 cr
    (`crew::expected_wage`). Belters ×0.8, Earthers ×1.25, Martians ×1.35.
  - `crew::hire` / `crew::fire` return status + reason (never throw). Hire needs the ship docked
    at the seeker's dock, a free berth (`crew_berths`, captain included) and a one-week signing
    bonus. Fire needs the ship docked; settles arrears if the owner can pay.
  - Weekly payroll from the owner's cash, all-or-nothing per person incl. arrears. Missed pay:
    morale −(0.1 + 0.1·consecutive weeks); paid crew drift 25%/week toward neutral 0.6.
    Morale < 0.4 posts a "grumbling" warning; < 0.25 → walks off at the next port (daily check
    while docked), with an urgent message.
  - Life support (`systems::daily_crew`): per person per day 0.4 kg water, 1.8 kg food,
    0.42 kg oxygen make-up (NASA BVAD: 0.84 kg O2, ~1.8 kg packaged food, ~3.9 kg water gross;
    90% water / ~50% O2 recovery on a worn hauler) plus hull-condition leakage; oxygen only
    underway (docked ships breathe station air). Drawn from "water"/"food"/"oxygen" cargo lots;
    reaction mass untouched. Urgent warning when crossing 3 days left; urgent daily message while
    out; shortages cost morale and health. `crew::on_health_depleted` is the hook for a future
    injury/death system (currently an urgent message).
  - Skills → StatPipeline (`expanse/include/expanse/stat_ids.hpp`, ids 100–104): best
    pilot (captain counts) → `accel_tolerance` +20%·s, `reaction_mass_use` −10%·s; best engineer
    → `hull_wear` −40%·s, `life_support_use` −25%·s; best medic → `health_loss` −50%·s. All are
    ship multipliers with global base 1.0; rebuilt on every roster change from source
    `crew_complement_key(ship)`.
  - Crew of a ship that no longer exists are put ashore at its last dock.
  - `pay()` adjusts `Company::cash` directly — `TODO(finance)` to route through `transact()`.
- Captain created in `new_game` via `crew::start_new_game` (also stocks `provision_days` of
  rations, sets stat bases, fills pools). `ScenarioDef.provision_days` (secondhand: 21).
- `CrewOriginDef` (`[crew_origin.*]`, `data/crew_names.toml`): original Belter/Martian/Earther
  name lists, backgrounds, wage multiplier. `CrewMember` gained origin, background, wages_owed,
  unpaid_weeks, health.
- Tests: `expanse/tests/crew_tests.cpp` (16 cases).

Checked online: NASA BVAD consumables and ISS ECLSS recovery rates (98% water with BPA, partial
O2 closure via Sabatier); crew design in Star Traders: Frontiers (desertion only on landing
below a morale threshold, escalating unpaid-wage penalty; players complain morale problems are
noticed too late → added the early "grumbling" warning) and FTL (a manned station gives a small
skill-scaled bonus → only the best crew member per role counts, bonuses stay modest).

Deferred: labour claims for walked-off arrears; injury/death on health 0; captain replacement
if the captain is lost; wage renegotiation, skill growth, traits affecting play.
