---
# repo-3ge8
title: 'Contracts: cargo haul & passenger jobs'
status: completed
type: task
priority: normal
created_at: 2026-09-24T23:46:47Z
updated_at: 2026-09-25T12:39:25Z
parent: repo-tydy
---

Generated from market needs; deadlines as scheduled events; payment on delivery.

## Summary of Changes

**Data model (World, schema version 3; saved, loaded and hashed through `fields()`).**

- `sim::Table<ContractTag, Contract> World::contracts`. A contract has: kind (cargo or passengers), origin, destination, commodity + tonnes or a passenger count, reward, deposit, min_standing, time_allowed, deadline, posted_at, expires_at, status (offered / accepted / delivered / failed / expired), holder, ship, accepted_at, closed_at, paid, aboard, and warned flags.
- `Ship::consignments`: the client's cargo aboard. It counts toward `cargo_mass_t`, so it takes hold space and adds mass to transits. It is kept apart from `Ship::cargo`, so it can't be sold or eaten.
- `World::standings`: (company, faction, value) rows with values in -10..+10.
- Closed contracts are pruned after 30 days and stale offers are withdrawn, so the table stays bounded. After a year the table holds no more rows than the total board capacity.

**Generation** (`expanse/src/contracts.cpp`, daily after economy/finance/crew, stream `world.rng("contracts")`, stations in key order):

- Board capacity is clamp(round(1.5 log10(pop) - 1.5), 2, 10): 3 on Hollow Nail, 9 on Ceres. Each open slot is filled with probability 0.5 per day. Offers stay up for 3-6 days.
- Cargo (65% of offers): the origin has the commodity to spare (net producer, or stock above 1.2x target), and the destination consumes more than it produces. The weight is proximity (1/(1+(d/0.3 AU)^2)) x the destination's price multiplier, doubled while it has unmet demand. Size is 3-12 days of the destination's consumption, capped at 1-450 t and at half the origin's stock. The cargo leaves the origin's stock on acceptance and joins the destination's stock on delivery.
- Passengers (35%): the destination is weighted by proximity x log10(pop). The group size is 1..clamp(round(log10(min pop) - 2), 1, 6), so there are more people between big ports. The Mule has 3 spare berths, so a group of 4 to Ganymede needs a bigger ship.
- Time allowed = T x U(1.3, 2.0) + 12 h, where T is the flip-and-burn transit time at 0.1 g (a real `plot_transit` intercept at posting time). The deadline is set on acceptance.
- 20% of offers are premium: they need standing 3+ with the origin's faction, pay x1.25 and ask half the deposit.

**Reward** (rounded to 10 cr; T in days, t in tonnes, V = t x base_price):

- cargo = S x P x U x K x (60 + 40 T + 0.2 t T + 0.01 V)
- passengers = S x P x U x n x (50 + 30 T)
- U (urgency) = 1 + 0.5 (2.0 - slack)/0.7, so 1.0 to 1.5.
- K (risk) is medical 1.3, luxury 1.2, food 1.1, otherwise 1.0, and x1.15 when the job crosses faction lines.
- S (standing) = 1 + 0.03 x standing.
- P = 1.25 on premium offers.
- Deposit (cargo only, EVE-style collateral) = 25% of V x clamp(1 - 0.06 x standing, 0.4, 1.6). It is zero for cargo worth under 1,500 cr.
- Examples: 45 t water Ceres -> Sakai pays 540 cr with no deposit. 90 t water pays 620 cr with a 680 cr deposit. 220 t metals Sakai -> Ganymede pays 2,840 cr with a 14,300 cr deposit (the constraint a broke captain faces). One passenger inside the cluster pays 160-260 cr.

**Delivery, lateness, failure and abandoning:**

- Delivery is automatic when the carrying ship docks at the destination (`contracts::on_docked`, called from `systems::ship_arrives`), so a forgotten command never loses a job.
- Grace = max(1 d, 25% of the time allowed). On time: full pay, deposit back, standing +1. Late but within grace: pay x (1 - 0.5 L/grace), deposit back, standing -1. Beyond grace: failed, deposit forfeited, standing -3. The daily system and the arrival hook apply the same rules.
- A contract also fails if its ship is lost (for example, repossessed).
- `abandon`: the deposit is forfeited, or, when there is no deposit, a fee of 10% of the reward is charged if the captain can pay. Standing -2. Cargo or passengers of a failed or abandoned job leave the ship at its next port, or at once if it is docked.
- Standing of -5 or below: that faction's boards refuse you.
- Every payment and refund goes through `transact()` with `LedgerCategory::contract`.
- An urgent journal entry is posted once per contract when the ship is underway to the destination with an ETA past the deadline (it states the expected pay share, or that the job will fail), or within one day of the deadline when the ship is not on the way.
- Passengers take berths (`hire` counts them) and draw life support like crew (`crew::daily_need`).

**Shell:**

- `jobs` (query): the board at your dock, with reward and deposit after standing, time allowed, the hold or berths needed, and either "ok" or the reason you can't take the job.
- `accept <id>` and `abandon <id>` (actions).
- `contracts` (query): contracts held, with deadline, ETA and a verdict (on time with the slack left, late with a pay cut, will fail, or time left if not en route), plus your standings.
- `status` lists consignments and passengers.

**Tests:**

- New `expanse/tests/contracts_tests.cpp` (15 cases): boards fill and are deterministic by seed; the cluster always has local work; deadlines are feasible (between 1.3 and 2x T); offers expire; accept checks (location, hold, berths, deposit, premium, a refused accept changes nothing, passengers block hiring); cargo is loaded on accept, can't be sold, and delivery on docking pays and refunds through the ledger and moves market stock; the late pay cut; failure with deposit forfeiture and standing loss; the ETA warning; abandon penalties; passengers' life support; standing terms; save/load mid-contract equals an uninterrupted run; closed contracts are pruned and the table stays bounded; shell commands and replay.

**Balance** (`balance_tests.cpp`, new *contractor* bot):

- The contractor flies the careful trader's coasting courses, buys fuel before cargo and pays instalments early. It also roams the cluster: at each port it weighs every other cluster station by (rewards of acceptable jobs bound there + best own trade in the space left - reaction mass - docking fee) per day, and it never takes a leg that misses the earliest deadline.
- Asserted over 8 seeds: never repossessed, at most 1 missed payment, more than 10 jobs delivered, at most 1 failed, median 26-week gain at least 1.5x the pure ore trader's and at most 5x, and median week-4 liquid below 8 instalments.
- 16-seed survey (net-worth gain at week 26): trader median **20.2k**, contractor median **53.8k** (range 20.4k-111.6k). The contractor delivers 38-94 jobs worth 29-53k cr, fails none, and misses 0-1 payments.
- The start is still lean. Week-4 liquid median is 6.7k. On seed 1 cash is 66 cr at week 4 and 78 cr at week 9, because it is all tied up in cargo and deposits.
- Seed 1 trajectory (week: cash / net worth): 0: 1,850 / -118k; 4: 66 / -118k; 8: 8,299 / -110k; 13: 45 / -86k; 17: 29.6k / -84k; 26: 31.3k / -76k.
- The idle bot is unchanged (repossessed on day 28). The careful and careless bots are unchanged: the contracts stream is independent, and markets only move when a job is taken.

**Research** (checked before committing):

- EVE couriers: collateral is escrowed on acceptance and returned on delivery. Reward scales with distance, volume and collateral. The gap between reward and collateral is what protects both sides. Adopted as a fractional deposit (full value would lock out a broke captain), reduced by standing.
- Elite Dangerous: reward = f(distance, difficulty) x reputation. Reputation gates better offers, and passenger fares scale with distance but are capped. Adopted as the S and P factors and premium offers.
- Star Traders: Frontiers: standing unlocks better-paying missions. Cancelling costs reputation but less than letting a job fail. Adopted: abandon is -2, failure is -3.
- Space Rangers and Escape Velocity: strict time limits, and late delivery damages relations. Adopted as a grace window with a linear pay cut, then failure.

**Deferred:**

- No NPC haulers take offers; boards only turn over by expiry.
- Standing affects only contracts, not prices or loans.
- Passengers don't suffer from deprivation; only crew health does.
- Deadline warnings are checked at the daily tick, so they can come up to a day after departure.
- The status banner and the viewer don't show contracts yet.
- There is no jettisoning of contract cargo.
