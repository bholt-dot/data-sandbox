---
# repo-7v8a
title: Styled output layer + FTXUI shell TUI
status: todo
type: feature
created_at: 2026-09-25T01:08:58Z
updated_at: 2026-09-25T01:08:58Z
parent: repo-89dz
---

Commands emit semantic output (heading, money, warning, table...) through a core output layer rendered as plain text (scripts/tests/pipes), ANSI colour, or FTXUI panels. TUI: always-visible status banner, scrolling output, journal panel with pinned urgent items, input line with completion and history. Full-screen only on an interactive TTY; --plain/--script keep current behaviour. Subsumes the shell colour bean.
