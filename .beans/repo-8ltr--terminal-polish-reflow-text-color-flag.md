---
# repo-8ltr
title: 'Terminal polish: reflow text, --color flag'
status: completed
type: task
priority: normal
created_at: 2026-09-25T12:21:16Z
updated_at: 2026-09-25T12:21:16Z
parent: repo-89dz
---

Deferred from the TUI bean.

## Summary of Changes
- sim::reflow() (core/text): joins hard-wrapped data text into paragraphs so each front end wraps to its own width; used for scenario descriptions.
- belter --color auto|always|never: an explicit flag overrides detection and NO_COLOR (no-color.org: user-level flags win over the environment); applies to the TUI theme, the plain REPL and --script output.
