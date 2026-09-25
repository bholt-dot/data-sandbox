---
# repo-i3bf
title: Wire belter shell to the game world
status: completed
type: feature
priority: normal
created_at: 2026-09-25T00:32:37Z
updated_at: 2026-09-25T00:35:18Z
parent: repo-9zts
---

Replace the placeholder REPL world with the real Content + World: new game from a scenario and seed, date, advance (duration / until event), messages, status, save/load. Game action commands (plot/order/buy/sell/hire) are added as their systems land.

## Summary of Changes
- `expanse/shell.hpp`: Session (content + optional world + unseen-message cursor) and register_game_commands(): new, save, load, hash, date, advance [--force], wait [--max], messages, status, stations (distance + light-lag), station <key>. Tab completion for station/scenario keys.
- apps/belter: loads data (--data), interactive REPL or --script batch mode.
Game action commands (plot/order/buy/sell/hire) will be added as their systems merge.
