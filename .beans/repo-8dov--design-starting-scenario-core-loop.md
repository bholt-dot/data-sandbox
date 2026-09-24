---
# repo-8dov
title: 'Design: starting scenario & core loop'
status: todo
type: task
priority: normal
created_at: 2026-09-24T23:46:47Z
updated_at: 2026-09-24T23:51:56Z
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
