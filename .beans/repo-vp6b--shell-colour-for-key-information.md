---
# repo-vp6b
title: Shell colour for key information
status: completed
type: feature
priority: low
created_at: 2026-09-25T00:45:36Z
updated_at: 2026-09-25T01:38:44Z
parent: repo-89dz
blocked_by:
    - repo-7v8a
---

Trade Wars-style ANSI colour so urgent messages, money and GO/NO GO stand out while text scrolls. Only when stdout is a TTY (and respect NO_COLOR); scripts/tests stay plain.

## Summary of Changes

Covered by repo-7v8a (styled output layer + TUI). Commands emit semantic styles; on a TTY the
`--plain` REPL renders them as 16-colour ANSI (money yellow, gains green, losses red, GO bold
green / NO GO bold red, warnings bold yellow, urgent bold white on red, keys cyan), and the
full-screen TUI uses the same theme. Scripts, pipes and tests stay plain text. NO_COLOR (non-empty,
per no-color.org) switches to a bold/dim/inverse-only theme; TERM=dumb gets no escape codes.
Research: no-color.org (empty value ignored; bold/underline still allowed; flags/config may
override — no flag added yet).
