---
# repo-8dov
title: 'Design: starting scenario & core loop'
status: todo
type: task
priority: normal
created_at: 2026-09-24T23:46:47Z
updated_at: 2026-09-25T00:54:26Z
parent: repo-tydy
---

Decide how 'start from nothing' works (work passage as crew? loan on a beat-up hauler? salvage?), win/fail pressure, first hour of play.

## Decision (initial scenario): Debt start

Goal: an authentic *dirt poor belter* experience. Other starts (working passage, salvage claim) can be added later as alternate scenarios; the data model shouldn't assume one.

Initial scenario, 'Secondhand':
- Player is a Belter with a used, underpowered ice/cargo hauler bought on a predatory loan from a station lender (weekly payments, repossession if 3 missed).
- Near-zero cash; reaction mass tank partly full; one worn subsystem (e.g. degraded drive efficiency or leaky water reclamation).
- No crew beyond the captain. Hiring pool on the dock: cheap-but-unskilled vs. skilled-but-expensive; crew expect wages and air/water/food.
- Survival pressure is physical, not abstract: air, water and food are line items; stations charge for water and docking. Running dry is lethal.
- First hour: take a low-paying short haul (e.g. water or parts within the Ceres/Vesta neighbourhood), make a loan payment, hire a first crew member, discover that fuel and water eat the margin.

Design principle: margins should be thin enough that a bad decision hurts, but the player can always find *some* work (no hard soft-locks).

## Data after dataset + Ceres cluster (2350-03-14, seed 1)
- Real belt geometry: Vesta/Pallas/Hygiea/Psyche are 4.5-5.3 AU from Ceres at the start (synodic periods of years). Added the Ceres cluster (Hollow Nail, Dagu Rock, Sakai Drift) co-orbital with Ceres at 0.14-0.33 AU so short runs always exist.
- Starter route (full 600 t hold, unlimited capital): Hollow Nail -> Sakai Drift ore, +3.7k (9%) per 8-day round trip; after 60 days settled +9.2k (26%).
- With the actual 1,850 cr, a first trade nets a few hundred cr at best: the 4,200/week instalment is unreachable in week 1 by trading. Needs the first-payment fix and/or contracts.
- Long hauls: equilibrium consumer 1.41x vs producer 0.82x base => ~60% gross on any producer->consumer pair (Earth -> Ceres food +131k on 228k capital, 50 days). Fine as a mid-game goal; revisit if it makes the game trivial once capitalised.
