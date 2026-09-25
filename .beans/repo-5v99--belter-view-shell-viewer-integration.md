---
# repo-5v99
title: 'belter --view: shell + viewer integration'
status: completed
type: feature
priority: normal
created_at: 2026-09-25T01:58:25Z
updated_at: 2026-09-25T01:58:25Z
parent: repo-89dz
---

Run the shell (TUI or plain) on a second thread and the SDL3 viewer on the main thread; publish a snapshot after every line.

## Summary of Changes
- core: `CommandBus::on_after_line()` — a presentation-only observer called after every non-empty line (const context), so any front end can react without touching the TUI/REPL loops.
- expanse: `Session::last_plot` (presentation-only, mutable) holds the last feasible `plot`; cleared by go/new/load/advance.
- apps/belter: `--view` runs the window on the main thread (SDL: window, events and swapchain on the creating thread) and the shell on a worker; quitting the shell closes the window; closing the window leaves the shell running headless. `--screenshot F` (with --view --script) renders a script's end state headless.
- CTest belter_view_script_screenshot (+ image check) runs a scripted session through the viewer on SDL's offscreen driver.
- Fixed: infeasible plots were published with zeroed positions (a target ring on the Sun).
Research: SDL3 threading guidance (wiki FAQ, SDL issue #12959) — render and swapchain on the window's thread, game logic elsewhere, matches this design.
