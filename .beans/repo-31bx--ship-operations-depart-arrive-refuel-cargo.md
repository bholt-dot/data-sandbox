---
# repo-31bx
title: 'Ship operations: depart, arrive, refuel, cargo'
status: todo
type: feature
priority: normal
created_at: 2026-09-25T00:26:38Z
updated_at: 2026-09-25T00:27:51Z
parent: repo-tydy
---

Player/NPC ship actions on the World: depart for a station (plot_transit, deduct reaction mass, schedule ShipArrives, set Underway), arrival (dock, post message), hull wear per transit, cancel/redirect. Used by the shell 'order'/'plot' commands and NPC traders. (Refuel and cargo buy/sell are market transactions owned by the economy bean.)
