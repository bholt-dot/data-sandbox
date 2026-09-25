---
# repo-jo7k
title: Station production/consumption + local pricing
status: completed
type: task
priority: normal
created_at: 2026-09-24T23:46:47Z
updated_at: 2026-09-25T00:44:47Z
parent: repo-xcm0
---

Price responds to stock vs. target; population drives consumption.

Also owns market transactions: buy/sell cargo between ship and station (price impact, capacity and cash checks, whole-credit rounding) and refuel (reaction mass = water bought at the station's water price).

## Summary of Changes

Public API in `expanse/include/expanse/economy.hpp` (namespace `expanse::economy`), implemented in
`expanse/src/economy.cpp` together with the `systems::daily_economy` hook.

**Daily tick** (per station, per market entry, in data order; `world.rng("economy")`):
production/consumption from the StationDef entry with +-10% noise (consumption rates in the data
are authored per station and already reflect population, so there is no extra multiplier); stock
never goes negative and unmet demand is recorded in `StationState::unmet`. Then unsimulated
traffic closes 1/30 of the gap to the target stock (Elite-style "return to natural equilibrium"),
so consumers settle below target (dear) and producers above (cheap). Rarely (1/365 per entry per
day) a supply disruption stops production and inbound traffic for 10-45 days; buffers are ~30 days,
so long ones empty a station ("Tycho Station has run out of Foodstuffs"). Messages are posted for
disruption start/end and stock-outs.

**Price**: `mid = base * clamp((target/stock)^0.5, 0.25, 4)`, target = 60 days of the larger of
production/consumption. Equilibrium: pure consumer 1.41x, pure producer 0.82x. Station sells at
mid*1.03, buys at mid*0.97. Stations refuse commodities they don't list.

**Transactions**: `buy`, `sell`, `refuel(ship, optional tonnes)` (nullopt = fill as far as tank,
stock and cash allow). Checks: ship/owner exist, commodity valid, quantity positive, docked, station
trades it, station stock, cargo space / tank space, cash, cargo aboard. Failures return a
`TradeResult` with a `TradeStatus` + reason and change nothing. Cost is the exact integral of the
price curve over the stock moved (closed-form antiderivative of the clamped power law), so
splitting a trade costs the same and buy-then-sell-back loses exactly the spread. Whole credits:
buyer rounds up, seller rounds down (no free micro-trades). Cargo lots of the same commodity merge
and carry cost basis; sells remove basis pro rata and report profit. Each success posts a market
message.

**Tuning check** (test `secondhand has a thin but real starter trade route`, 0.1 g, dv capped at
300 km/s, full-hold optimised quantity, laden out + empty back, reaction mass at the origin's water
ask): Ceres -> Vesta water, 600 t for 16,666 cr, sold for 20,585 cr, 71 t reaction mass (1,960 cr):
margin ~1,960 cr (10.5%) over a 38-day round trip; ~1,700 cr once markets settle. With only the
starting 1,850 cr the same run loses ~770 cr: fuel eats a 66 t trade, so the first income has to
come from contracts. Capitalised routes are richer (Vesta -> Tycho metals ~33k cr / 30 days on
155k; Mars -> Ceres food ~96k / 83 days on 239k).

**Research** (web): X4 scales price inversely with stock between ware min/max, average at half
storage; Elite Dangerous stocks recover to a natural equilibrium (demand slower than supply) and
large deliveries depress price; Patrician III prices are piecewise in weeks of consumption with the
buy/sell gap narrowing near a sufficient stock; bonding-curve literature: trade cost = integral of
the price curve (slippage). What it changed: used a days-of-cover target (Patrician) with a smooth
power law instead of piecewise-linear, added the equilibrium relaxation (Elite) instead of letting
consumer stations starve until NPC traders exist, and replaced a per-tonne step approximation of
price impact with the exact integral. Tuning pass after the route check: cover 30 -> 60 days and
tau 10 -> 30 days (deeper markets, a full hold no longer floods a small station), half-spread
4% -> 3%.

Not changed: data/*.toml (no market numbers needed tuning). Deferred: storage caps, dynamic
population, light-lag on market news (repo-yfv8), NPC traders carrying the flow now handled by the
offscreen relaxation (repo-e8bt), `world_schema_version` left at 1 (StationState grew two fields).
