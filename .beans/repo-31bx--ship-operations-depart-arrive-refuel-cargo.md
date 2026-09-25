---
# repo-31bx
title: 'Ship operations: depart, arrive, refuel, cargo'
status: todo
type: feature
created_at: 2026-09-25T00:26:38Z
updated_at: 2026-09-25T00:26:38Z
parent: repo-tydy
---

Player/NPC ship actions on the World: depart for a station (plot_transit, deduct reaction mass, schedule ShipArrives, set Underway), arrival (dock, post message), refuel (buy water as reaction mass at station price), load/unload cargo (capacity checks). Hull wear per transit. Used by the shell 'order'/'plot' commands and NPC traders.
